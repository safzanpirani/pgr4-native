// pgr4-native PGR4 native renderer (M2 owner #5): host render targets and Clear. See
// render_targets.h for the model (one host target per guest surface, no EDRAM emulation).
#include "render_targets.h"

#include "renderer.h"

#include <rex/logging.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pgr4::native {
namespace {

constexpr uint32_t kMaxViews = 256;   // RTVs and DSVs each; PGR4 uses ~20 surfaces per frame
constexpr uint32_t kNullRtvs = 3;     // null RTVs for 1, 2 and 4 samples, at the heap's start
constexpr uint32_t kEdramTiles = 2048;  // 10 MiB of 80x16-sample tiles
constexpr uint32_t kMaxHeight = 2048;

// Same translation as the generated code: the 0xE0000000 view sits 4 KiB higher on the host.
inline uint32_t Load32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a + (a >= 0xE0000000u ? 0x1000u : 0u), 4);
  return __builtin_bswap32(v);
}
inline float LoadF(const uint8_t* base, uint32_t a) {
  uint32_t v = Load32(base, a);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

uint32_t SamplesOf(uint32_t surface_info) {
  uint32_t m = (surface_info >> 16) & 3;
  return m >= 2 ? 4 : m == 1 ? 2 : 1;
}

bool Is64bpp(uint32_t color_format) {
  // 16_16_16_16, 16_16_16_16_FLOAT, 32_32_FLOAT (xenos::IsColorRenderTargetFormat64bpp).
  return color_format == 5 || color_format == 7 || color_format == 15;
}

// Rows of pixels the EDRAM holds from `base` for this surface (what a non-tiling guest could
// render); the host target is at least this tall so it rarely has to grow.
uint32_t EdramRows(uint32_t base, uint32_t pitch, uint32_t samples, bool wide) {
  const uint32_t sx = samples >= 4 ? 2 : 1, sy = samples >= 2 ? 2 : 1;
  const uint32_t tiles_x = std::max(1u, (pitch * sx * (wide ? 2 : 1) + 79) / 80);
  const uint32_t free_tiles = base < kEdramTiles ? kEdramTiles - base : 0;
  return (free_tiles / tiles_x) * 16 / sy;
}

D3D12_RESOURCE_BARRIER TransitionBarrier(ID3D12Resource* r, D3D12_RESOURCE_STATES before,
                                         D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b = {};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = r;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = before;
  b.Transition.StateAfter = after;
  return b;
}

}  // namespace

SurfaceRegs SurfaceRegsFromRegisters(const uint32_t* regs) {
  SurfaceRegs s;
  s.surface_info = regs[0x2000];
  s.color_info[0] = regs[0x2001];
  s.depth_info = regs[0x2002];
  s.color_info[1] = regs[0x2003];
  s.color_info[2] = regs[0x2004];
  s.color_info[3] = regs[0x2005];
  return s;
}

SurfaceRegs SurfaceRegsFromDevice(const uint8_t* base, uint32_t device) {
  const uint32_t b = device + kDevShadow2000;
  SurfaceRegs s;
  s.surface_info = Load32(base, b + 0 * 4);
  s.color_info[0] = Load32(base, b + 1 * 4);
  s.depth_info = Load32(base, b + 2 * 4);
  s.color_info[1] = Load32(base, b + 3 * 4);
  s.color_info[2] = Load32(base, b + 4 * 4);
  s.color_info[3] = Load32(base, b + 5 * 4);
  return s;
}

DXGI_FORMAT RenderTargets::ColorFormat(uint32_t f) {
  switch (f) {
    case 0:   // k_8_8_8_8
    case 1:   // k_8_8_8_8_GAMMA (gamma not applied on the host yet)
      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case 2:   // k_2_10_10_10
    case 10:  // k_2_10_10_10_AS_10_10_10_10
      return DXGI_FORMAT_R10G10B10A2_UNORM;
    case 3:   // k_2_10_10_10_FLOAT (7e3)
    case 12:  // k_2_10_10_10_FLOAT_AS_16_16_16_16: same EDRAM data, 16-bit blending
    case 7:   // k_16_16_16_16_FLOAT
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 4: return DXGI_FORMAT_R16G16_SNORM;        // k_16_16 (-32..32 on Xenos; scaled)
    case 5: return DXGI_FORMAT_R16G16B16A16_SNORM;  // k_16_16_16_16
    case 6: return DXGI_FORMAT_R16G16_FLOAT;
    case 14: return DXGI_FORMAT_R32_FLOAT;
    case 15: return DXGI_FORMAT_R32G32_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}

RenderTargets::RenderTargets() = default;
RenderTargets::~RenderTargets() = default;

bool RenderTargets::Initialize(ID3D12Device* device) {
  device_ = device;
  D3D12_DESCRIPTOR_HEAP_DESC hd = {};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  hd.NumDescriptors = kMaxViews + kNullRtvs;
  if (Failed(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap_)), "RT CreateDescriptorHeap(RTV)"))
    return false;
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  hd.NumDescriptors = kMaxViews;
  if (Failed(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsv_heap_)), "RT CreateDescriptorHeap(DSV)"))
    return false;
  rtv_stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  dsv_stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
  rtv_start_ = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
  for (uint32_t i = kMaxViews; i-- > 0;) {
    free_rtv_.push_back(kNullRtvs + i);
    free_dsv_.push_back(i);
  }
  // Null RTVs fill the gaps below the highest bound color target (D3D12 needs a descriptor in
  // every slot of OMSetRenderTargets; the sample count of a null view must match the others).
  for (uint32_t i = 0; i < kNullRtvs; ++i) {
    D3D12_RENDER_TARGET_VIEW_DESC v = {};
    v.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    v.ViewDimension = i == 0 ? D3D12_RTV_DIMENSION_TEXTURE2D : D3D12_RTV_DIMENSION_TEXTURE2DMS;
    D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += size_t(i) * rtv_stride_;
    device->CreateRenderTargetView(nullptr, &v, h);
  }
  return true;
}

uint32_t RenderTargets::AllocView(bool depth) {
  auto& list = depth ? free_dsv_ : free_rtv_;
  if (list.empty()) return UINT32_MAX;
  uint32_t i = list.back();
  list.pop_back();
  return i;
}

void RenderTargets::Retire(ComPtr<ID3D12Resource> resource) {
  if (resource) retired_.push_back({std::move(resource), frame_fence_});
}

bool RenderTargets::Create(RenderTarget* rt, uint32_t height) {
  D3D12_RESOURCE_DESC d = {};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = std::max(1u, rt->pitch);
  d.Height = height;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.Format = rt->resource_format;
  d.SampleDesc.Count = rt->samples;
  d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  d.Flags = rt->depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  const D3D12_RESOURCE_STATES initial =
      rt->depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
  ComPtr<ID3D12Resource> res;
  HRESULT hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, initial, nullptr,
                                                IID_PPV_ARGS(&res));
  if (FAILED(hr)) {
    REXLOG_ERROR("PGR4 native RT: CreateCommittedResource {}x{} fmt {} x{} failed: {:08X}", d.Width,
                 d.Height, int(d.Format), rt->samples, uint32_t(hr));
    return false;
  }
  Retire(std::move(rt->resource));
  rt->resource = std::move(res);
  rt->state = initial;
  rt->width = uint32_t(d.Width);
  rt->height = height;
  if (rt->view_index == UINT32_MAX) {
    rt->view_index = AllocView(rt->depth);
    if (rt->view_index == UINT32_MAX) {
      REXLOG_ERROR("PGR4 native RT: out of {} views", rt->depth ? "depth" : "color");
      return false;
    }
  }
  if (rt->depth) {
    D3D12_DEPTH_STENCIL_VIEW_DESC v = {};
    v.Format = rt->view_format;
    v.ViewDimension = rt->samples > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
    rt->view = dsv_heap_->GetCPUDescriptorHandleForHeapStart();
    rt->view.ptr += size_t(rt->view_index) * dsv_stride_;
    device_->CreateDepthStencilView(rt->resource.Get(), &v, rt->view);
  } else {
    D3D12_RENDER_TARGET_VIEW_DESC v = {};
    v.Format = rt->view_format;
    v.ViewDimension = rt->samples > 1 ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D;
    rt->view = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    rt->view.ptr += size_t(rt->view_index) * rtv_stride_;
    device_->CreateRenderTargetView(rt->resource.Get(), &v, rt->view);
  }
  return true;
}

RenderTarget* RenderTargets::Get(bool depth, uint32_t surface_info, uint32_t info, uint32_t height) {
  const uint32_t pitch = surface_info & 0x3FFF;
  const uint32_t samples = SamplesOf(surface_info);
  const uint32_t base = info & 0xFFF;
  DXGI_FORMAT resource, view, srv, stencil_srv = DXGI_FORMAT_UNKNOWN;
  uint32_t guest_format;
  bool wide = false;
  if (depth) {
    guest_format = (info >> 16) & 1;
    if (guest_format) {  // kD24FS8: 20e4 float depth, kept as 32-bit float on the host
      resource = DXGI_FORMAT_R32G8X24_TYPELESS;
      view = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
      srv = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
      stencil_srv = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
    } else {  // kD24S8
      resource = DXGI_FORMAT_R24G8_TYPELESS;
      view = DXGI_FORMAT_D24_UNORM_S8_UINT;
      srv = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
      stencil_srv = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
    }
  } else {
    guest_format = (info >> 16) & 0xF;
    view = ColorFormat(guest_format);
    if (view == DXGI_FORMAT_UNKNOWN) {
      static uint32_t logged = 0;
      if (logged++ < 8) REXLOG_WARN("PGR4 native RT: unsupported color format {}", guest_format);
      return nullptr;
    }
    resource = srv = view;
    wide = Is64bpp(guest_format);
  }
  if (pitch == 0) return nullptr;
  const uint64_t key = (uint64_t(depth) << 63) | (uint64_t(samples >> 1) << 60) | (uint64_t(base) << 44) |
                       (uint64_t(pitch) << 30) | uint64_t(view);
  GetMemo& memo = get_memo_[(key ^ (key >> 30) ^ (key >> 44) ^ (key >> 60)) & 15];
  if (memo.rt && memo.key == key && std::min(height, kMaxHeight) <= memo.rt->height) {
    memo.rt->guest_format = guest_format;
    memo.rt->last_frame = frame_;
    return memo.rt;
  }
  uint32_t want = std::max(std::min(height, kMaxHeight), EdramRows(base, pitch, samples, wide));
  want = std::min((std::max(want, 8u) + 7) & ~7u, kMaxHeight);
  auto it = targets_.find(key);
  RenderTarget* rt;
  if (it == targets_.end()) {
    auto owned = std::make_unique<RenderTarget>();
    rt = owned.get();
    rt->key = key;
    rt->depth = depth;
    rt->edram_base = base;
    rt->pitch = pitch;
    rt->samples = samples;
    rt->resource_format = resource;
    rt->view_format = view;
    rt->srv_format = srv;
    rt->stencil_srv_format = stencil_srv;
    rt->view_index = UINT32_MAX;
    if (!Create(rt, want)) {
      if (rt->view_index != UINT32_MAX) (depth ? free_dsv_ : free_rtv_).push_back(rt->view_index);
      return nullptr;
    }
    targets_.emplace(key, std::move(owned));
  } else {
    rt = it->second.get();
    if (std::min(height, kMaxHeight) > rt->height) {
      // Taller than anything this surface had: a new, taller target. The old content is lost
      // (not seen in PGR4's traces: every surface is created at its EDRAM height or more).
      ++recreated_;
      REXLOG_WARN("PGR4 native RT: growing {} base {} pitch {} x{} from {} to {} rows", depth ? "depth" : "color",
                  base, pitch, samples, rt->height, want);
      if (!Create(rt, want)) return nullptr;
    }
  }
  rt->guest_format = guest_format;
  rt->last_frame = frame_;
  memo = {key, rt};
  return rt;
}

RenderTarget* RenderTargets::GetColor(const SurfaceRegs& regs, uint32_t index, uint32_t height) {
  return Get(false, regs.surface_info, regs.color_info[index & 3], height);
}

RenderTarget* RenderTargets::GetDepth(const SurfaceRegs& regs, uint32_t height) {
  return Get(true, regs.surface_info, regs.depth_info, height);
}

void RenderTargets::Transition(ID3D12GraphicsCommandList* list, RenderTarget* rt,
                               D3D12_RESOURCE_STATES state) {
  if (!rt || rt->state == state) return;
  D3D12_RESOURCE_BARRIER b = TransitionBarrier(rt->resource.Get(), rt->state, state);
  list->ResourceBarrier(1, &b);
  rt->state = state;
}

bool RenderTargets::Bind(ID3D12GraphicsCommandList* list, const SurfaceRegs& regs, uint32_t color_mask,
                         bool depth_used, uint32_t height, BoundTargets* out) {
  *out = BoundTargets{};
  const uint32_t samples = SamplesOf(regs.surface_info);
  out->samples = samples;
  out->width = UINT32_MAX;
  out->height = UINT32_MAX;
  D3D12_CPU_DESCRIPTOR_HANDLE null_rtv = rtv_start_;
  null_rtv.ptr += size_t(samples == 4 ? 2 : samples == 2 ? 1 : 0) * rtv_stride_;
  for (uint32_t i = 0; i < 4; ++i) {
    out->rtv[i] = null_rtv;
    if (!(color_mask >> i & 1)) continue;
    RenderTarget* rt = GetColor(regs, i, height);
    if (!rt) return false;
    Transition(list, rt, D3D12_RESOURCE_STATE_RENDER_TARGET);
    out->color[i] = rt;
    out->rtv[i] = rt->view;
    out->color_count = i + 1;
    out->width = std::min(out->width, rt->width);
    out->height = std::min(out->height, rt->height);
  }
  if (depth_used) {
    RenderTarget* rt = GetDepth(regs, height);
    if (!rt) return false;
    Transition(list, rt, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    out->depth = rt;
    out->dsv = rt->view;
    out->width = std::min(out->width, rt->width);
    out->height = std::min(out->height, rt->height);
  }
  if (out->width == UINT32_MAX) out->width = out->height = 0;
  return true;
}

void RenderTargets::OnClear(ID3D12GraphicsCommandList* list, const uint8_t* base, const HookArgs& args) {
  ClearParams p;
  auto read_rect = [&](uint32_t a) {
    // D3DRECT {x1, y1, x2, y2}, big-endian LONGs.
    D3D12_RECT r;
    r.left = LONG(int32_t(Load32(base, a)));
    r.top = LONG(int32_t(Load32(base, a + 4)));
    r.right = LONG(int32_t(Load32(base, a + 8)));
    r.bottom = LONG(int32_t(Load32(base, a + 12)));
    if (r.right > r.left && r.bottom > r.top) p.rects.push_back(r);
  };
  if (args.fn == HookFn::kClear) {
    // Clear(dev, Count r4, pRects r5, Flags r6, Color r7, Z f1, Stencil r8)
    const uint32_t count = args.r[1], rects = args.r[2], color = args.r[4];
    p.flags = args.r[3];
    p.color[0] = float((color >> 16) & 0xFF) / 255.0f;
    p.color[1] = float((color >> 8) & 0xFF) / 255.0f;
    p.color[2] = float(color & 0xFF) / 255.0f;
    p.color[3] = float(color >> 24) / 255.0f;
    p.depth = float(args.f[0]);
    p.stencil = uint8_t(args.r[5]);
    if (rects)
      for (uint32_t i = 0; i < count && i < 64; ++i) read_rect(rects + i * 16);
    if (rects && count && p.rects.empty()) return;  // only empty rectangles
  } else if (args.fn == HookFn::kClearF) {
    // ClearF(dev, Flags r4, pRect r5, pColor r6 (D3DVECTOR4), Z f1, Stencil r7). Also reached
    // from inside BeginTiling (fn_8269C6D0), which clears the tiled scene surfaces through it.
    p.flags = args.r[1];
    if (args.r[3])
      for (int i = 0; i < 4; ++i) p.color[i] = LoadF(base, args.r[3] + 4 * i);
    p.depth = float(args.f[0]);
    p.stencil = uint8_t(args.r[4]);
    if (args.r[2]) {
      read_rect(args.r[2]);
      if (p.rects.empty()) return;
    }
  } else {
    return;
  }
  const uint32_t device = args.device;
  const SurfaceRegs regs = SurfaceRegsFromDevice(base, device);
  uint32_t bound = 0;
  for (uint32_t i = 0; i < 4; ++i)
    if (Load32(base, device + kDevColorSurfaces + 4 * i)) bound |= 1u << i;
  const bool depth_bound = Load32(base, device + kDevDepthSurface) != 0;
  // The surfaces' height is not in the registers; the viewport bottom is the best hint
  // (PA_CL_VPORT_YSCALE 0x2111, YOFFSET 0x2112). Targets are at least their EDRAM height.
  const float ys = LoadF(base, device + kDevShadow2100 + 0x11 * 4);
  const float yo = LoadF(base, device + kDevShadow2100 + 0x12 * 4);
  float bottom = std::fabs(ys) + yo;
  uint32_t height = (bottom > 0.0f && bottom <= float(kMaxHeight)) ? uint32_t(std::ceil(bottom)) : 0;
  for (const D3D12_RECT& r : p.rects) height = std::max(height, uint32_t(std::max<LONG>(r.bottom, 0)));
  Clear(list, regs, bound, depth_bound, height, p);
}

void RenderTargets::Clear(ID3D12GraphicsCommandList* list, const SurfaceRegs& regs, uint32_t bound_colors,
                          bool depth_bound, uint32_t height, const ClearParams& p) {
  const UINT nrects = UINT(p.rects.size());
  const D3D12_RECT* rects = nrects ? p.rects.data() : nullptr;
  for (uint32_t i = 0; i < 4; ++i) {
    if (!(p.flags >> i & 1) || !(bound_colors >> i & 1)) continue;
    RenderTarget* rt = GetColor(regs, i, height);
    if (!rt) continue;
    Transition(list, rt, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ClearRenderTargetView(rt->view, p.color, nrects, rects);
  }
  D3D12_CLEAR_FLAGS df = D3D12_CLEAR_FLAGS(0);
  if (p.flags & 0x10) df |= D3D12_CLEAR_FLAG_DEPTH;
  if (p.flags & 0x20) df |= D3D12_CLEAR_FLAG_STENCIL;
  if (df && depth_bound) {
    RenderTarget* rt = GetDepth(regs, height);
    if (rt) {
      Transition(list, rt, D3D12_RESOURCE_STATE_DEPTH_WRITE);
      list->ClearDepthStencilView(rt->view, df, std::clamp(p.depth, 0.0f, 1.0f), p.stencil, nrects, rects);
    }
  }
}

void RenderTargets::BeginFrame(uint64_t frame, uint64_t frame_fence, uint64_t completed_fence) {
  frame_ = frame;
  frame_fence_ = frame_fence;
  retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
                                [&](const Retired& r) { return r.fence <= completed_fence; }),
                 retired_.end());
  for (auto it = targets_.begin(); it != targets_.end();) {
    RenderTarget* rt = it->second.get();
    if (rt->last_frame + kMaxAge < frame) {
      // Last used kMaxAge frames ago: no list in flight references it, but retire it with the
      // current fence anyway (cheap) so the rule is the same everywhere.
      Retire(std::move(rt->resource));
      (rt->depth ? free_dsv_ : free_rtv_).push_back(rt->view_index);
      it = targets_.erase(it);
      for (GetMemo& m : get_memo_) m = {};
    } else {
      ++it;
    }
  }
}

}  // namespace pgr4::native
