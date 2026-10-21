// pgr4-native PGR4 native renderer (design A, milestone 2): host of the native D3D12 path.
//
// The guest D3D hooks in pgr4_d3d_trace.cpp forward every call to the guest D3D (the Xenos
// plugin keeps rendering as the oracle) and, with --pgr4_native, then call the NativeRenderer
// entry points below with the guest device and the call's arguments. Only the outermost
// draw/clear/resolve/Swap call is passed on (the D3D's internal clear, copy and Swap quads are
// not game draws); a top-level BeginVertices reaches OnDraw at the entry of the next hooked
// call, once the game has written its vertices. The renderer owns its
// own D3D12 queue, a debug window and a swapchain, separate from ReXGlue's presenter.
//
// Threading: the guest D3D device is used by one guest thread at a time; all On* entry points
// are called on that thread, after the original guest function returned, and are serialized
// by the renderer. Modules may assume single-threaded access from the On* calls.
//
// Owner: Integrator (M2 owner #1). Other modules (d3d_state, shader_*, textures,
// render_targets, resolve, draw, present) plug in behind these calls; ask the owner for
// changes to this header.
#pragma once

#include <rex/cvar.h>

#include <cstdint>
#include <memory>
#include <string>

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12GraphicsCommandList;

REXCVAR_DECLARE(bool, pgr4_native);             // master switch, off by default
REXCVAR_DECLARE(std::string, pgr4_native_csv);  // per-frame CSV of the native path ("" = off)

namespace pgr4::native {

class D3D12Device;
class DrawRenderer;
class Presenter;
class RenderTargets;
class Resolver;
class TextureCache;

// Guest D3D entry points the native path receives (addresses in the base-disc default.xex;
// names from native/src/d3d_surface.inc).
enum class HookFn : uint32_t {
  kBeginVertices = 0x8269A4F8,        // draw; the game writes the vertices after it returns
  kDrawVerticesUP = 0x8269A9A8,       // draw
  kDrawVertices = 0x8269A9F0,         // draw
  kDrawIndexedVertices = 0x8269ADD8,  // draw
  kDrawIndx2Inline = 0x8269B230,      // draw, called only from inside the D3D library
  kDrawIndx2Split = 0x826A7318,       // draw, called only from inside the D3D library
  kClearF = 0x8269C378,
  kClear = 0x8269C4A0,
  kResolve = 0x82696978,
  kSwap = 0x82695BE0,
};

// Raw PPC arguments of one hooked call. Integer arguments in r3..r10 and float arguments in
// f1..f4 are captured at entry, before the guest function runs (it clobbers them).
// Argument meaning follows the XDK D3DDevice_* signatures [INFERENCE, check against the
// M1 traces' `C` lines before relying on one]:
//   DrawIndexedVertices(dev, prim, base_vertex, start_index, index_count)
//   DrawVertices(dev, prim, start_vertex, vertex_count)
//   DrawVerticesUP(dev, prim, vertex_count, data_ptr, stride)
//   BeginVertices(dev, prim, vertex_count, stride) -> r3 = pointer the game writes into
//   Clear(dev, count, rects_ptr, flags, color; z in f1, stencil)
//   Resolve(dev, flags, src_rect_ptr, dest_texture, dest_point_ptr, level, slice,
//           clear_color_ptr; clear_z in f1, clear_stencil, params_ptr)
//   Swap(dev, front_buffer_texture, scaler_params_ptr)
struct HookArgs {
  HookFn fn;
  uint32_t device;  // r3 at entry: the guest D3D device (a guest virtual address)
  uint32_t r[8];    // r3..r10 at entry; r[0] == device
  uint32_t sp;      // r1 at entry: arguments past r10 are on the guest stack (Resolve's
                    // ClearStencil and pParameters; offsets in resolve.h)
  double f[4];      // f1..f4 at entry
  uint64_t ret;     // r3 after the guest function returned
};

// Per-frame counters of the native path (also the frame CSV's columns).
struct FrameStats {
  uint64_t draws = 0, clears = 0, resolves = 0;
  uint64_t state_fail = 0;                             // draws ReadDrawState rejected
  uint64_t draw_ns = 0, clear_ns = 0, resolve_ns = 0;  // CPU time inside the On* calls
  uint64_t swap_ns = 0;                                // OnSwap: record, submit, present
};

class NativeRenderer {
 public:
  // The renderer, created on first use when --pgr4_native is on; nullptr when it is off or
  // when D3D12 initialization failed (logged once). Cheap enough to call per hook.
  static NativeRenderer* Get();

  // Hook entry points. `base` is the guest memory base the hook received (guest virtual
  // address a is at base + a, plus 0x1000 for a >= 0xE0000000 like the generated code).
  void OnDraw(const uint8_t* base, const HookArgs& args);
  void OnClear(const uint8_t* base, const HookArgs& args);
  void OnResolve(const uint8_t* base, const HookArgs& args);
  void OnSwap(const uint8_t* base, const HookArgs& args);

  // For modules.
  D3D12Device& device() { return *device_; }
  ID3D12Device* d3d12() const;
  ID3D12CommandQueue* queue() const;
  // The frame's direct command list: open from the previous Swap until OnSwap closes and
  // submits it. Record into it from the On* calls only.
  ID3D12GraphicsCommandList* command_list() const;
  // Swaps seen so far (= index of the frame being recorded).
  uint64_t frame_index() const { return frame_index_; }
  // Fence value OnSwap will signal for the frame being recorded; a resource last used in this
  // frame may be freed once device().completed_fence() >= this.
  uint64_t frame_fence() const;
  // Modules owned by the renderer.
  TextureCache& textures() { return *textures_; }
  RenderTargets& render_targets() { return *rt_; }
  Resolver& resolver() { return *resolver_; }

  ~NativeRenderer();

 private:
  NativeRenderer() = default;
  bool Initialize();
  void WriteCsv(const FrameStats& s, uint64_t frame_ns);
  // Starts a frame's module state (RenderTargets/Resolver BeginFrame) at its first On* call.
  void BeginFrame(const uint8_t* base) {
    if (!frame_begun_) BeginFrameModules(base);
  }
  void BeginFrameModules(const uint8_t* base);

  std::unique_ptr<D3D12Device> device_;
  std::unique_ptr<RenderTargets> rt_;
  std::unique_ptr<Resolver> resolver_;
  std::unique_ptr<TextureCache> textures_;
  std::unique_ptr<DrawRenderer> draw_;   // draw.h (DrawPresent)
  std::unique_ptr<Presenter> present_;   // present.h (DrawPresent)
  bool frame_begun_ = false;
  uint64_t frame_index_ = 0;
  uint64_t frame_start_ns_ = 0;
  FrameStats stats_;
  struct Csv;
  std::unique_ptr<Csv> csv_;
};

// Monotonic CPU time in nanoseconds (QueryPerformanceCounter).
uint64_t NowNs();

}  // namespace pgr4::native
