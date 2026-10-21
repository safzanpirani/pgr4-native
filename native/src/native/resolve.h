// pgr4-native PGR4 native renderer (M2 owner #5): guest Resolve -> host texture copies.
//
// A guest Resolve(Flags, pSourceRect, pDestTexture, pDestPoint, DestLevel, DestSliceOrFace,
// pClearColor, ClearZ, ClearStencil, pParameters) copies a rectangle of a render target
// (color RT 0-3 or depth) into a texture level, converting the format, averaging MSAA samples,
// and optionally clears the source surfaces afterwards. Here every resolve destination
// (guest physical address of the level/slice) gets a host Texture2D that holds exactly the
// texels the Xenos plugin writes into guest memory, untiled and endian-unswapped, in
// GetHostFormat(dest format)->resource (texture_layout.h), so the texture cache can copy it
// into its own textures in place of the guest-memory upload. The copy runs as a compute
// shader (format packing as ReXGlue's resolve shaders do it) into a linear scratch buffer,
// then CopyTextureRegion into the destination.
//
// Arguments are decoded the way the guest D3D does it (read from its generated code,
// sub_82696978): NULL source rect = the destination level's size, right/bottom rounded up to
// 8; NULL dest point = (0, 0); sample select from Flags bits 4-6 (0 = by the surface's MSAA:
// 1x sample 0, 2x average of 2, 4x average of 4); exponent bias from Flags bits 26-31;
// red/blue swap when the destination's swizzle X is not R; raw copies for depth, conversion
// for color. During M2 the guest Resolve also runs (Xenos is the oracle), so OnResolve
// compares the decoded copy registers with the ones the D3D wrote into its register shadow
// and counts mismatches (shadow_mismatches()).
//
// Oracle dumps (--pgr4_native_resolve_dump=<dir>): creating <dir>\dump.request arms a dump of
// the next frame's resolves: for each, the native texels (`f<F>-r<i>-native.bin`, tight rows),
// the guest memory Xenos wrote at the same address in the same frame (`-guest.bin`, tiled,
// big-endian; read in that frame's Swap with the guest thread held and the plugin on full
// readback, see HoldFullReadback), and one JSON line in `f<F>-resolves.jsonl`. When a frame
// resolves twice into one address, every entry's guest copy is the last write.
// tools/compare_resolves.py untiles and compares them.
//
// Pipeline state: OnResolve sets its own compute root signature, PSO and shader-visible
// descriptor heap on the command list; callers that draw afterwards must set theirs again.
#pragma once

#include "render_targets.h"

#include <rex/graphics/xenos.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace pgr4::native {

// What the texture cache needs to use a resolve result (TextureMod contract).
struct ResolvedImage {
  uint32_t guest_base = 0;  // physical address of the destination level/slice
  rex::graphics::xenos::TextureFormat format{};  // destination format (RB_COPY_DEST_INFO)
  uint32_t width = 0, height = 0;  // texels written from (0, 0), dest point included
  ID3D12Resource* texture = nullptr;  // state: COPY_SOURCE | ALL_SHADER_RESOURCE
  // Depth resolves only: R32_FLOAT = float20e4(d >> 8) (or unorm24 for D24S8), what the
  // texture cache's k_24_8(_FLOAT) conversion gives when the game samples the k_8_8_8_8
  // destination as depth. Same size and state as `texture`.
  ID3D12Resource* depth_r32f = nullptr;
  uint64_t seq = 0;  // increases with every write (global resolve counter)
};

// One decoded Resolve call.
struct ResolveParams {
  SurfaceRegs regs;            // the source surfaces (register shadow at the call)
  uint32_t source = 0;         // 0-3 color render target, 4 depth
  uint32_t sample_select = 0;  // xenos::CopySampleSelect
  uint32_t copy_command = 1;   // xenos::CopyCommand: 0 raw, 1 convert
  int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // source rectangle, surface pixels, exclusive end
  int32_t dest_x = 0, dest_y = 0;          // destination point
  uint32_t dest_base = 0;      // physical address of the destination level/slice
  uint32_t dest_format = 0;    // xenos::TextureFormat (copy destination format)
  uint32_t dest_width = 0, dest_height = 0;  // destination level size, texels
  uint32_t dest_endian = 0;    // xenos::Endian (dumps only; the host image is unswapped)
  uint32_t dest_number = 0;    // xenos::SurfaceNumberFormat: 0 unorm, 1 snorm, 2 uint, 3 sint, 7 float
  bool dest_swap = false;      // swap red and blue
  int32_t exp_bias = 0;        // color *= 2^exp_bias (convert only)
  // Guest layout of the destination (dumps only).
  bool dest_tiled = false;
  uint32_t dest_pitch_blocks = 0;  // row pitch in texels (tiled: 32-aligned)
  uint32_t dest_extent_bytes = 0;  // guest memory the level/slice occupies
  // Clears after the copy (Flags 0x100 color, 0x200 depth+stencil) over the source rect.
  uint32_t clear_flags = 0;
  float clear_color[4] = {};
  float clear_depth = 1.0f;
  uint8_t clear_stencil = 0;
};

// Decodes a Resolve hook call (`args` = r3..r10, f1, sp at entry; guest memory at `base`).
// Returns false (and logs once per reason) for what is not supported yet: packed mip tails,
// 3D destinations, copy commands other than raw/convert.
bool DecodeResolve(const uint8_t* base, const HookArgs& args, ResolveParams* out);

// The Xenos plugin's --readback_resolve "fast"/"some" copy the previous resolve of the same
// address range into guest memory (one frame late), so guest memory never holds the frame the
// guest just swapped. Oracles (captures, dumps) call HoldFullReadback(true) a frame ahead, which
// switches the plugin to "full" (synchronous, exact) through the cvar registry, and
// HoldFullReadback(false) after reading guest memory, which restores the previous value. Nested
// holds are counted. Guest thread only.
void HoldFullReadback(bool hold);

class Resolver {
 public:
  Resolver();
  ~Resolver();

  // `queue` runs SelfTest at start when --pgr4_native_resolve_selftest is set.
  bool Initialize(ID3D12Device* device, RenderTargets* targets, ID3D12CommandQueue* queue);

  // Once per frame before recording it, like RenderTargets::BeginFrame (handles dumps too).
  void BeginFrame(const uint8_t* base, uint64_t frame, uint64_t frame_fence, uint64_t completed_fence);

  // In NativeRenderer::OnSwap after EndFrame: for a dumped frame, holds the guest thread for
  // --pgr4_native_capture_hold_ms and copies the guest memory of its resolves.
  void AfterSwap(const uint8_t* base);

  // After the guest Resolve returned.
  void OnResolve(ID3D12GraphicsCommandList* list, const uint8_t* base, const HookArgs& args);
  bool Resolve(ID3D12GraphicsCommandList* list, const ResolveParams& p);

  // The last resolve into this guest physical address, or nullptr.
  const ResolvedImage* Find(uint32_t guest_phys_base) const;
  // Called with a guest physical address whenever its Find result may change (an image is
  // resolved into, created, replaced or erased), so a cache can recheck only the textures
  // that sample that address.
  void SetChangedCallback(std::function<void(uint32_t guest_phys_base)> f) { changed_ = std::move(f); }

  uint64_t resolves() const { return resolves_; }
  uint64_t failures() const { return failures_; }
  uint64_t shadow_mismatches() const { return shadow_mismatches_; }

  // Clears host targets of the formats PGR4 resolves from (8888, 7e3, 16F, 32F, D24FS8/D24S8;
  // 1x/2x/4x) to known values, resolves them into the destination formats PGR4 uses on its own
  // command list, and checks the read-back texels against hand-computed Xenos encodings. Logs
  // each case; returns true when all pass. Initialize runs it with --pgr4_native_resolve_selftest.
  bool SelfTest(ID3D12CommandQueue* queue);

 private:
  struct Image;
  struct Dump;
  bool CreatePipelines();
  Image* GetImage(const ResolveParams& p, uint32_t width, uint32_t height);
  bool EnsureScratch(uint64_t bytes);
  void CheckAgainstShadow(const uint8_t* base, uint32_t device, const ResolveParams& p);
  void DumpRecord(ID3D12GraphicsCommandList* list, const ResolveParams& p, Image* img,
                  uint32_t width, uint32_t height);
  void DumpBeginFrame(const uint8_t* base, uint64_t completed_fence);

  ID3D12Device* device_ = nullptr;
  RenderTargets* targets_ = nullptr;
  ComPtr<ID3D12RootSignature> root_signature_;
  ComPtr<ID3D12PipelineState> pso_[4];  // [depth][msaa]
  ComPtr<ID3D12DescriptorHeap> heap_;   // shader visible: per-frame rings of SRV pairs
  uint32_t heap_stride_ = 0;
  uint32_t heap_next_ = 0;
  ComPtr<ID3D12Resource> scratch_;
  uint64_t scratch_size_ = 0;
  D3D12_RESOURCE_STATES scratch_state_ = D3D12_RESOURCE_STATE_COMMON;
  std::unordered_map<uint32_t, std::unique_ptr<Image>> images_;
  struct Retired {
    ComPtr<ID3D12Resource> resource;
    uint64_t fence;
  };
  std::vector<Retired> retired_;
  uint64_t frame_ = 0, frame_fence_ = 0;
  uint64_t resolves_ = 0, failures_ = 0, shadow_mismatches_ = 0, seq_ = 0;
  std::function<void(uint32_t)> changed_;
  std::unique_ptr<Dump> dump_;
};

}  // namespace pgr4::native
