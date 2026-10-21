// pgr4-native PGR4 native renderer (M2 owner #6): one guest draw -> one host D3D12 draw.
//
// Per draw (DrawRenderer::OnDraw, after ReadDrawState):
//   - shaders: ShaderCache::Load of the bound VS/PS ucode, SelectModifications (the Xenos
//     plugin's variant selection), translations with the DXBC translator (host RT path);
//   - pipeline: blend, depth/stencil, rasterizer, primitive topology, RT/DSV formats from the
//     register file, the geometry shader for point/rectangle/quad lists; PSOs cached by a hash
//     of that description (ports of d3d12::PipelineCache::GetCurrentStateDescription and
//     CreateD3D12Pipeline, BSD);
//   - vertices: the translator reads them in the vertex shader from "shared memory", a GPU
//     buffer mirroring guest physical memory (t0). Ranges named by the vertex fetch constants
//     the VS uses, and the index buffer, are uploaded at their guest offsets; uploaded pages
//     stay valid until a CPU write hits them (physical memory access watches, like the
//     plugin's shared memory). The inline vertices of BeginVertices/DrawVerticesUP live in the
//     push buffer and are uploaded on every draw, never watched;
//   - indices: DMA index buffers are bound straight from shared memory (big-endian, the VS
//     swaps them by SystemConstants::vertex_index_endian); auto-indexed draws are non-indexed;
//   - constants: the translator's b0 system constants (port of
//     D3D12CommandProcessor::UpdateSystemConstantValues, host RT path), b1 the VS/PS float
//     constants packed by the shaders' constant_register_map, b2 bool/loop, b3 fetch, b4
//     bindless descriptor indices; uploaded to per-frame ring buffers, reused while unchanged;
//   - textures from TextureCache (TextureMod), samplers from the sampler bindings (port of
//     D3D12TextureCache::GetSamplerParameters/WriteSampler), bindless heaps;
//   - render targets from RenderTargets::Bind (RTResolve); viewport, scissor, blend factor
//     and stencil reference (port of draw_util::GetHostViewportInfo / GetScissor).
//
// Not handled (counted in Stats and skipped): memexport, tessellation, host-converted index
// buffers (triangle fans, line loops, primitive reset with an index other than 0xFFFF).
//
// Threading: the renderer's On* calls only (one guest thread).
#pragma once

#include "d3d12_device.h"
#include "d3d_state.h"

#include <cstdint>
#include <memory>

namespace pgr4::native {

class NativeRenderer;

class DrawRenderer {
 public:
  struct Stats {
    uint64_t draws = 0;           // issued host draws
    uint64_t skipped = 0;         // dropped like the plugin does (EDRAM copy, nothing rasterizes)
    uint64_t unsupported = 0;     // see the header comment; logged once per reason
    uint64_t failed = 0;          // shader/PSO/RT/upload failures
    uint64_t pso_created = 0;
    uint64_t pso_pending = 0;     // draws skipped while their pipeline compiles (async)
    uint64_t upload_bytes = 0;    // guest memory copied into shared memory
    uint64_t uploads = 0;         // copy regions recorded
    uint64_t textures = 0, textures_missing = 0;  // bindings; missing = null SRV bound
    uint64_t srv_lookups = 0, srv_created = 0, samplers_created = 0;
    uint64_t bind_memo_hits = 0, bind_memo_misses = 0;  // whole-shader texture binding memo
  };

  explicit DrawRenderer(NativeRenderer& renderer);
  ~DrawRenderer();
  DrawRenderer(const DrawRenderer&) = delete;
  DrawRenderer& operator=(const DrawRenderer&) = delete;

  // Root signature, descriptor heaps, shared memory buffer, the memory watch. Logs and
  // returns false on failure.
  bool Initialize();

  // After a successful ReadDrawState for the draw. Records into renderer.command_list().
  void OnDraw(const uint8_t* base, const DrawState& state);

  // Something else set graphics/compute state or descriptor heaps on the command list (the
  // Resolver does): set everything again at the next draw. A new frame's list is detected
  // without this call.
  void InvalidateListState();

  // Per frame, with the fence values of NativeRenderer (once per Swap, after EndFrame).
  void EndFrame(uint64_t completed_fence);

  const Stats& stats() const { return stats_; }
  void ResetStats() { stats_ = Stats{}; }

 private:
  struct Impl;
  NativeRenderer& renderer_;
  std::unique_ptr<Impl> impl_;
  Stats stats_;
};

}  // namespace pgr4::native
