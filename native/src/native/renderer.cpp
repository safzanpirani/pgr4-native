// pgr4-native PGR4 native renderer (design A, milestone 2): NativeRenderer host.
#include "renderer.h"

#include "d3d12_device.h"
#include "d3d_state.h"
#include "draw.h"
#include "present.h"
#include "profile.h"
#include "render_targets.h"
#include "resolve.h"
#include "textures.h"

#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include <algorithm>
#include <cstdio>

REXCVAR_DEFINE_BOOL(pgr4_native, false, "PGR4 native",
                    "Run the native D3D12 renderer beside the Xenos plugin (debug window)");
REXCVAR_DEFINE_BOOL(pgr4_native_primary, false, "PGR4 native",
                    "The native renderer is the game's output: it presents into the game window "
                    "and the Xenos plugin skips all draws, resolves and swaps (gpu_skip_draws, "
                    "native/rexglue-native-primary.patch); implies --pgr4_native");
REXCVAR_DEFINE_STRING(pgr4_native_csv, "", "PGR4 native",
                      "Per-frame CSV of the native path: counts and CPU time per hook class");
REXCVAR_DEFINE_INT32(pgr4_native_width, 1280, "PGR4 native", "Debug window width");
REXCVAR_DEFINE_INT32(pgr4_native_height, 720, "PGR4 native", "Debug window height");
REXCVAR_DEFINE_BOOL(pgr4_native_profile, false, "PGR4 native",
                    "Per-stage CPU time of the draw path (RDTSC), appended to the native CSV as "
                    "st_<stage>_us columns and to the stats log line");
REXCVAR_DECLARE(int32_t, pgr4_d3d_stats_frames);

namespace pgr4::native {

uint64_t NowNs() {
  static const uint64_t freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return uint64_t(f.QuadPart);
  }();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  // Split to avoid overflow: whole seconds plus the remainder.
  uint64_t t = uint64_t(c.QuadPart);
  return (t / freq) * 1000000000ull + (t % freq) * 1000000000ull / freq;
}

struct NativeRenderer::Csv {
  std::FILE* file = nullptr;
  // Window for the log summary.
  uint64_t frames = 0, draws = 0, draw_ns = 0, swap_ns = 0, frame_ns = 0;
  // Profile: RDTSC calibration (ticks and ns at start-up) and the window's stage ticks.
  uint64_t tsc0 = 0, ns0 = 0;
  uint64_t stage_ticks[profile::kStageCount] = {}, track_calls = 0;
  TextureCache::Stats tex_prev;  // texture counters at the previous CSV row
  ~Csv() {
    if (file) std::fclose(file);
  }
};

namespace {
bool g_failed = false;  // set once the device or swapchain failed; the native path stops

// The game window: the process's visible, unowned top-level window (the SDK's), not ours.
HWND FindGameWindow() {
  struct Ctx {
    DWORD pid;
    HWND found;
  } ctx = {GetCurrentProcessId(), nullptr};
  EnumWindows(
      [](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid != c->pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
        wchar_t cls[64] = {};
        GetClassNameW(h, cls, 64);
        if (!wcscmp(cls, L"pgr4_native_debug")) return TRUE;
        c->found = h;
        return FALSE;
      },
      reinterpret_cast<LPARAM>(&ctx));
  return ctx.found;
}

bool FlagOn(const char* name) {
  const std::string v = rex::cvar::GetFlagByName(name);
  return v == "true" || v == "1";
}
}  // namespace

NativeRenderer* NativeRenderer::Get() {
  if (!(REXCVAR_GET(pgr4_native) || REXCVAR_GET(pgr4_native_primary)) || g_failed)
    return nullptr;
  // Never destroyed: the process exits with the game, and the guest thread may still be in a
  // hook during static destruction.
  static NativeRenderer* r = [] {
    auto* n = new NativeRenderer();
    if (!n->Initialize()) {
      REXLOG_ERROR("PGR4 native: initialization failed; native path off for this run");
      g_failed = true;
      delete n;
      return static_cast<NativeRenderer*>(nullptr);
    }
    return n;
  }();
  return g_failed ? nullptr : r;
}

NativeRenderer::~NativeRenderer() = default;

bool NativeRenderer::Initialize() {
  device_ = std::make_unique<D3D12Device>();
  uint32_t w = uint32_t((std::max)(64, REXCVAR_GET(pgr4_native_width)));
  uint32_t h = uint32_t((std::max)(64, REXCVAR_GET(pgr4_native_height)));
  const bool primary = REXCVAR_GET(pgr4_native_primary);
  HWND game_window = nullptr;
  if (primary) {
    // The first D3D call comes after the SDK created its window; wait briefly in case not.
    for (int i = 0; i < 100 && !(game_window = FindGameWindow()); ++i) Sleep(50);
    if (!game_window) {
      REXLOG_ERROR("PGR4 native: --pgr4_native_primary: no game window found");
      return false;
    }
    // Same pacing switch as the SDK presenter's.
    device_->set_vsync(FlagOn("d3d12_present_vsync"));
  }
  if (!device_->Initialize(w, h, primary ? L"PGR4 native" : L"PGR4 native (debug)", game_window))
    return false;
  rt_ = std::make_unique<RenderTargets>();
  if (!rt_->Initialize(device_->device())) return false;
  resolver_ = std::make_unique<Resolver>();
  if (!resolver_->Initialize(device_->device(), rt_.get(), device_->queue())) return false;
  textures_ = std::make_unique<TextureCache>(device_->device(), rex::system::kernel_memory());
  textures_->SetResolvedLookup([this](uint32_t a) { return resolver_->Find(a); });
  resolver_->SetChangedCallback([this](uint32_t a) { textures_->OnResolved(a); });
  draw_ = std::make_unique<DrawRenderer>(*this);
  if (!draw_->Initialize()) return false;
  present_ = std::make_unique<Presenter>(*this);
  if (!present_->Initialize()) return false;
  // Only once native is up: from here the Xenos plugin does no draw, copy or swap work, and its
  // command processor only runs fences, write-backs and interrupts.
  if (primary && !rex::cvar::SetFlagByName("gpu_skip_draws", "true"))
    REXLOG_ERROR("PGR4 native: could not set gpu_skip_draws (rexglue-native-primary.patch missing?); "
                 "Xenos keeps drawing");
  csv_ = std::make_unique<Csv>();
  profile::g.on = REXCVAR_GET(pgr4_native_profile);
  csv_->tsc0 = profile::Now();
  csv_->ns0 = NowNs();
  const std::string& path = REXCVAR_GET(pgr4_native_csv);
  if (!path.empty()) {
    csv_->file = std::fopen(path.c_str(), "w");
    if (csv_->file) {
      std::fprintf(csv_->file,
                   "frame,draws,clears,resolves,state_fail,draw_us,clear_us,resolve_us,swap_us,"
                   "us_per_draw,frame_ms,host_draws,skipped,unsupported,failed,pso_created,"
                   "upload_kb,textures,textures_missing,front_buffer");
      if (profile::g.on) {
        for (const char* n : profile::kStageNames) std::fprintf(csv_->file, ",st_%s_us", n);
        std::fprintf(csv_->file, ",track_calls,tex_memo_miss,tex_created,tex_create_us,"
                                 "tex_uploads,tex_upload_kb,tex_upload_us,tex_untile_us,"
                                 "tex_resolved_copies,tex_copy_resolved_us,srv_lookups,"
                                 "srv_created,samplers_created,bind_hits,bind_misses,pso_pending");
        for (const char* n : profile::kCounterNames) std::fprintf(csv_->file, ",%s", n);
      }
      std::fprintf(csv_->file, "\n");
    } else {
      REXLOG_ERROR("PGR4 native: cannot open {}", path);
    }
  }
  frame_start_ns_ = NowNs();
  return true;
}

ID3D12Device* NativeRenderer::d3d12() const { return device_->device(); }
ID3D12CommandQueue* NativeRenderer::queue() const { return device_->queue(); }
ID3D12GraphicsCommandList* NativeRenderer::command_list() const { return device_->command_list(); }
uint64_t NativeRenderer::frame_fence() const { return device_->frame_fence(); }

void NativeRenderer::BeginFrameModules(const uint8_t* base) {
  const uint64_t fence = device_->frame_fence(), done = device_->completed_fence();
  rt_->BeginFrame(frame_index_, fence, done);
  resolver_->BeginFrame(base, frame_index_, fence, done);
  frame_begun_ = true;
}

void NativeRenderer::OnDraw(const uint8_t* base, const HookArgs& args) {
  const uint64_t t0 = NowNs();
  profile::Begin();
  BeginFrame(base);
  ++stats_.draws;
  DrawState state;
  const bool ok = ReadDrawState(base, args.device, args, &state);
  profile::Mark(profile::kReadState);
  if (ok)
    draw_->OnDraw(base, state);
  else
    ++stats_.state_fail;
  stats_.draw_ns += NowNs() - t0;
}

void NativeRenderer::OnClear(const uint8_t* base, const HookArgs& args) {
  const uint64_t t0 = NowNs();
  BeginFrame(base);
  ++stats_.clears;
  rt_->OnClear(device_->command_list(), base, args);
  stats_.clear_ns += NowNs() - t0;
}

void NativeRenderer::OnResolve(const uint8_t* base, const HookArgs& args) {
  const uint64_t t0 = NowNs();
  BeginFrame(base);
  ++stats_.resolves;
  // Sets its own compute root signature, pipeline and descriptor heap on the list.
  resolver_->OnResolve(device_->command_list(), base, args);
  draw_->InvalidateListState();
  stats_.resolve_ns += NowNs() - t0;
}

void NativeRenderer::OnSwap(const uint8_t* base, const HookArgs& args) {
  const uint64_t t0 = NowNs();
  BeginFrame(base);
  D3D12Device& d = *device_;
  // The resolved front buffer into the debug window (back buffer PRESENT in and out).
  present_->OnSwap(base, args);
  // The frame's texture copies read upload memory the untile workers fill.
  textures_->FinishUploads();
  if (!d.EndFrame()) {
    g_failed = true;
    REXLOG_ERROR("PGR4 native: frame {} failed; native path off for the rest of the run",
                 frame_index_);
    return;
  }
  ++frame_index_;
  frame_begun_ = false;
  d.SetTitleFrame(frame_index_);
  textures_->Retire(d.completed_fence());
  draw_->EndFrame(d.completed_fence());
  resolver_->AfterSwap(base);
  present_->AfterEndFrame(base);
  const uint64_t t1 = NowNs();
  stats_.swap_ns = t1 - t0;
  WriteCsv(stats_, t1 - frame_start_ns_);
  frame_start_ns_ = t1;
  stats_ = FrameStats{};
}

void NativeRenderer::WriteCsv(const FrameStats& s, uint64_t frame_ns) {
  const double us_per_draw = s.draws ? double(s.draw_ns) / 1000.0 / double(s.draws) : 0.0;
  // RDTSC ticks per µs, from start-up to now (QPC-backed NowNs).
  double ticks_per_us = 1.0;
  if (profile::g.on) {
    const uint64_t dns = NowNs() - csv_->ns0, dt = profile::Now() - csv_->tsc0;
    if (dns) ticks_per_us = double(dt) * 1000.0 / double(dns);
  }
  if (csv_->file) {
    const DrawRenderer::Stats& ds = draw_->stats();
    std::fprintf(csv_->file,
                 "%llu,%llu,%llu,%llu,%llu,%.1f,%.1f,%.1f,%.1f,%.4f,%.3f,%llu,%llu,%llu,%llu,%llu,"
                 "%.1f,%llu,%llu,%d",
                 (unsigned long long)frame_index_ - 1, (unsigned long long)s.draws,
                 (unsigned long long)s.clears, (unsigned long long)s.resolves,
                 (unsigned long long)s.state_fail, s.draw_ns / 1000.0, s.clear_ns / 1000.0,
                 s.resolve_ns / 1000.0, s.swap_ns / 1000.0, us_per_draw, frame_ns / 1e6,
                 (unsigned long long)ds.draws, (unsigned long long)ds.skipped,
                 (unsigned long long)ds.unsupported, (unsigned long long)ds.failed,
                 (unsigned long long)ds.pso_created, ds.upload_bytes / 1024.0,
                 (unsigned long long)ds.textures, (unsigned long long)ds.textures_missing,
                 present_->front_buffer_found() ? 1 : 0);
    if (profile::g.on) {
      for (uint64_t t : profile::g.ticks) std::fprintf(csv_->file, ",%.1f", double(t) / ticks_per_us);
      std::fprintf(csv_->file, ",%llu", (unsigned long long)profile::g.track_calls);
      const TextureCache::Stats& t = textures_->stats();
      TextureCache::Stats& p = csv_->tex_prev;
      std::fprintf(csv_->file, ",%llu,%llu,%.1f,%llu,%.1f,%.1f,%.1f,%llu,%.1f,%llu,%llu,%llu,%llu,%llu,%llu",
                   (unsigned long long)(t.memo_misses - p.memo_misses),
                   (unsigned long long)(t.created - p.created), (t.create_ns - p.create_ns) / 1000.0,
                   (unsigned long long)(t.uploads - p.uploads),
                   (t.upload_bytes - p.upload_bytes) / 1024.0, (t.upload_ns - p.upload_ns) / 1000.0,
                   (t.untile_ns - p.untile_ns) / 1000.0,
                   (unsigned long long)(t.resolved_copies - p.resolved_copies),
                   (t.copy_resolved_ns - p.copy_resolved_ns) / 1000.0,
                   (unsigned long long)ds.srv_lookups, (unsigned long long)ds.srv_created,
                   (unsigned long long)ds.samplers_created, (unsigned long long)ds.bind_memo_hits,
                   (unsigned long long)ds.bind_memo_misses, (unsigned long long)ds.pso_pending);
      p = t;
      for (uint64_t c : profile::g.counters) std::fprintf(csv_->file, ",%llu", (unsigned long long)c);
    }
    std::fprintf(csv_->file, "\n");
    std::fflush(csv_->file);  // per frame, so a killed run keeps every row
  }
  draw_->ResetStats();
  Csv& w = *csv_;
  ++w.frames;
  w.draws += s.draws;
  w.draw_ns += s.draw_ns;
  w.swap_ns += s.swap_ns;
  w.frame_ns += frame_ns;
  if (profile::g.on) {
    for (uint32_t i = 0; i < profile::kStageCount; ++i) w.stage_ticks[i] += profile::g.ticks[i];
    w.track_calls += profile::g.track_calls;
    for (uint64_t& t : profile::g.ticks) t = 0;
    profile::g.track_calls = 0;
    for (uint64_t& c : profile::g.counters) c = 0;
  }
  const int32_t every = REXCVAR_GET(pgr4_d3d_stats_frames);
  if (every > 0 && w.frames >= uint64_t(every)) {
    const double n = double(w.frames);
    REXLOG_INFO("PGR4 native: frames {} | per frame: draws {:.1f}, draw CPU {:.1f} us "
                "({:.4f} us/draw), swap {:.1f} us, frame {:.2f} ms",
                frame_index_, w.draws / n, w.draw_ns / 1000.0 / n,
                w.draws ? w.draw_ns / 1000.0 / double(w.draws) : 0.0, w.swap_ns / 1000.0 / n,
                w.frame_ns / 1e6 / n);
    if (profile::g.on && w.draws) {
      // ns per draw for each stage (the hook-side ones are per draw too, for comparison).
      std::string line;
      char item[64];
      for (uint32_t i = 0; i < profile::kStageCount; ++i) {
        std::snprintf(item, sizeof(item), " %s %.0f", profile::kStageNames[i],
                      double(w.stage_ticks[i]) * 1000.0 / ticks_per_us / double(w.draws));
        line += item;
      }
      REXLOG_INFO("PGR4 native profile: ns/draw{} | track calls/frame {:.0f}", line,
                  double(w.track_calls) / n);
      for (uint64_t& t : w.stage_ticks) t = 0;
      w.track_calls = 0;
    }
    w.frames = w.draws = w.draw_ns = w.swap_ns = w.frame_ns = 0;
  }
}

}  // namespace pgr4::native
