// pgr4-native PGR4 native renderer (M2 owner #5): host render targets for the guest's EDRAM
// surfaces, and the guest Clear calls.
//
// No EDRAM emulation and no tiles: every guest surface (RB_SURFACE_INFO pitch and MSAA,
// RB_COLOR_INFO / RB_DEPTH_INFO base and format) gets one host render target at full size,
// whatever predicated tiling the guest D3D does behind the API.
//
// Aliasing (tools/rt_passes.py over the M1 menu and race traces): PGR4 reuses EDRAM base 0
// for most passes of a frame (shadow-map depth, the 128x128 mip chain, the 1280x720 2x scene,
// the 1x post chain, 80-pitch luminance reductions, the UI), and the 256x256 4x cube faces
// sit at color base 224 over depth base 0. Every surface use starts with a Clear/ClearF of
// that surface (BeginTiling's ClearF for the scene), a Resolve with clear flags on the same
// surface, or a full-screen draw that overwrites it; no pass reads EDRAM another surface
// left behind. So surfaces at the same base are independent host targets, and no data moves
// between them. The only format aliasing that must share storage is 2_10_10_10_FLOAT vs
// 2_10_10_10_FLOAT_AS_16_16_16_16 (PGR4 flips between them inside one pass for blending
// precision); both map to one R16G16B16A16_FLOAT target, as in ReXGlue's host RT path.
// The D3D's own clear quads at half pitch with 4x MSAA (inside Clear/ClearF) are not game
// draws and never reach this module.
//
// Threading: called from the NativeRenderer's On* calls only (single guest thread).
#pragma once

#include "d3d12_device.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace pgr4::native {

struct HookArgs;

// Raw EDRAM surface registers of the current render targets.
struct SurfaceRegs {
  uint32_t surface_info = 0;    // RB_SURFACE_INFO (0x2000)
  uint32_t color_info[4] = {};  // RB_COLOR_INFO (0x2001), RB_COLOR1..3_INFO (0x2003..0x2005)
  uint32_t depth_info = 0;      // RB_DEPTH_INFO (0x2002)
};

// From a register file indexed by register number (DrawState::regs).
SurfaceRegs SurfaceRegsFromRegisters(const uint32_t* regs);
// From the guest device's register shadow (block 0x2000 at device+10368).
SurfaceRegs SurfaceRegsFromDevice(const uint8_t* base, uint32_t device);

// Guest D3D device fields this module reads (tools/scan_d3d_surface.py, d3d_state.h).
constexpr uint32_t kDevShadow2000 = 10368;     // registers 0x2000..0x2012
constexpr uint32_t kDevShadow2100 = 10444;     // registers 0x2100..0x2114
constexpr uint32_t kDevColorSurfaces = 12432;  // 4 x guest D3D surface object
constexpr uint32_t kDevDepthSurface = 12448;

// One host render target.
struct RenderTarget {
  uint64_t key = 0;
  bool depth = false;
  uint32_t edram_base = 0;   // tiles
  uint32_t pitch = 0;        // pixels (RB_SURFACE_INFO surface_pitch)
  uint32_t samples = 1;      // 1, 2 or 4
  uint32_t width = 0, height = 0;
  uint32_t guest_format = 0;  // last xenos::ColorRenderTargetFormat / DepthRenderTargetFormat
  DXGI_FORMAT resource_format = DXGI_FORMAT_UNKNOWN;
  DXGI_FORMAT view_format = DXGI_FORMAT_UNKNOWN;     // RTV or DSV
  DXGI_FORMAT srv_format = DXGI_FORMAT_UNKNOWN;      // color, or the depth plane
  DXGI_FORMAT stencil_srv_format = DXGI_FORMAT_UNKNOWN;  // depth only: the stencil plane
  ComPtr<ID3D12Resource> resource;
  D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
  D3D12_CPU_DESCRIPTOR_HANDLE view{};  // RTV or DSV
  uint32_t view_index = 0;
  uint64_t last_frame = 0;  // frame of the last use, for eviction
};

// Host targets for one draw.
struct BoundTargets {
  RenderTarget* color[4] = {};
  D3D12_CPU_DESCRIPTOR_HANDLE rtv[4] = {};
  uint32_t color_count = 0;  // highest bound color index + 1 (null RTVs below it)
  RenderTarget* depth = nullptr;
  D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
  uint32_t samples = 1;
  uint32_t width = 0, height = 0;  // smallest bound target (the draw's maximum extent)
};

// Decoded Clear / ClearF arguments.
struct ClearParams {
  uint32_t flags = 0;          // D3DCLEAR_TARGET0..3 = 1..8, ZBUFFER = 0x10, STENCIL = 0x20
  float color[4] = {};         // RGBA
  float depth = 1.0f;
  uint8_t stencil = 0;
  std::vector<D3D12_RECT> rects;  // empty: whole surface
};

class RenderTargets {
 public:
  RenderTargets();
  ~RenderTargets();

  bool Initialize(ID3D12Device* device);

  // Host targets for a draw. color_mask bit i: guest RT i is used (its RB_COLOR_MASK nibble is
  // non-zero); depth_used: depth or stencil test/write enabled. height: rows the draw can
  // touch (bottom of its scissor/viewport, clamped to 2048). Records the barriers to
  // RENDER_TARGET / DEPTH_WRITE into `list`; the caller sets the RTVs/DSV from `out`.
  bool Bind(ID3D12GraphicsCommandList* list, const SurfaceRegs& regs, uint32_t color_mask, bool depth_used, uint32_t height, BoundTargets* out);

  // Guest Clear / ClearF (HookFn kClear / kClearF), after the guest call returned: clears the
  // host targets of the surfaces bound in the device's register shadow.
  void OnClear(ID3D12GraphicsCommandList* list, const uint8_t* base, const HookArgs& args);
  void Clear(ID3D12GraphicsCommandList* list, const SurfaceRegs& regs,
             uint32_t bound_colors, bool depth_bound, uint32_t height, const ClearParams& p);

  // The host target of guest color RT `index` (0..3) or of the depth surface, created if
  // missing (resolve sources). nullptr for an invalid format.
  RenderTarget* GetColor(const SurfaceRegs& regs, uint32_t index, uint32_t height);
  RenderTarget* GetDepth(const SurfaceRegs& regs, uint32_t height);

  static void Transition(ID3D12GraphicsCommandList* list, RenderTarget* rt,
                         D3D12_RESOURCE_STATES state);

  // Once per frame before recording it (renderer start and after each Swap): `frame_fence`
  // is the fence the frame being recorded will signal, `completed_fence` the GPU's progress.
  // Frees replaced targets the GPU is done with and drops targets unused for kMaxAge frames.
  void BeginFrame(uint64_t frame, uint64_t frame_fence, uint64_t completed_fence);
  static constexpr uint64_t kMaxAge = 120;

  size_t count() const { return targets_.size(); }
  uint64_t recreated() const { return recreated_; }

  // Host DXGI formats of a guest color / depth render target format (UNKNOWN if unsupported).
  static DXGI_FORMAT ColorFormat(uint32_t color_format);

 private:
  RenderTarget* Get(bool depth, uint32_t surface_info, uint32_t info, uint32_t height);
  void Retire(ComPtr<ID3D12Resource> resource);
  bool Create(RenderTarget* rt, uint32_t height);
  uint32_t AllocView(bool depth);

  ID3D12Device* device_ = nullptr;
  ComPtr<ID3D12DescriptorHeap> rtv_heap_, dsv_heap_;
  uint32_t rtv_stride_ = 0, dsv_stride_ = 0;
  std::vector<uint32_t> free_rtv_, free_dsv_;
  std::unordered_map<uint64_t, std::unique_ptr<RenderTarget>> targets_;
  // Get's last hits, direct-mapped by key, in front of targets_ (cleared when targets are
  // dropped); and the RTV heap start (null RTVs).
  struct GetMemo {
    uint64_t key = 0;
    RenderTarget* rt = nullptr;
  };
  GetMemo get_memo_[16];
  D3D12_CPU_DESCRIPTOR_HANDLE rtv_start_{};
  struct Retired {
    ComPtr<ID3D12Resource> resource;
    uint64_t fence;
  };
  std::vector<Retired> retired_;
  uint64_t recreated_ = 0;
  uint64_t frame_ = 0, frame_fence_ = 0;
};

}  // namespace pgr4::native
