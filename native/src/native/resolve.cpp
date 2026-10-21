// pgr4-native PGR4 native renderer (M2 owner #5): guest Resolve -> host texture. See resolve.h.
//
// Format packing follows ReXGlue/Xenia's resolve shaders and xenos.h conversions (BSD-3,
// Copyright Xenia/ReXGlue developers): 7e3 and 20e4 float encodings, unorm rounding, the
// red/blue swap of RB_COPY_DEST_INFO and the sample selections of RB_COPY_CONTROL.
#include "resolve.h"

#include "renderer.h"
#include "texture_layout.h"

#include <rex/cvar.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/logging.h>

#include <d3dcompiler.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

REXCVAR_DEFINE_STRING(pgr4_native_resolve_dump, "", "PGR4 native",
                      "Directory for resolve oracle dumps; create <dir>\\dump.request to dump the next frame");
REXCVAR_DEFINE_BOOL(pgr4_native_resolve_selftest, false, "PGR4 native",
                    "Check the resolve shader's format packing against known encodings at start");
REXCVAR_DECLARE(int32_t, pgr4_native_capture_hold_ms);

namespace pgr4::native {
namespace {

namespace xenos = rex::graphics::xenos;
namespace texture_util = rex::graphics::texture_util;

constexpr uint32_t kHeapPerFrame = 256;  // SRV descriptors per frame slot (2 per resolve)
constexpr uint32_t kHeapSlots = 4;       // > frames in flight
constexpr uint64_t kPlacement = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
constexpr uint32_t kPitchAlign = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
constexpr D3D12_RESOURCE_STATES kImageReadState =
    D3D12_RESOURCE_STATE_COPY_SOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

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

#define PGR4_LOG_ONCE(counter, ...)        \
  do {                                     \
    if ((counter)++ < 4) REXLOG_WARN(__VA_ARGS__); \
  } while (0)

// log2 of the bytes per texel of a copy destination format; -1 if the resolve can't write it.
int DestBppLog2(uint32_t f) {
  switch (xenos::TextureFormat(f)) {
    case xenos::TextureFormat::k_8: return 0;
    case xenos::TextureFormat::k_8_8:
    case xenos::TextureFormat::k_5_6_5:
    case xenos::TextureFormat::k_6_5_5:
    case xenos::TextureFormat::k_16_FLOAT: return 1;
    case xenos::TextureFormat::k_8_8_8_8:
    case xenos::TextureFormat::k_2_10_10_10:
    case xenos::TextureFormat::k_16_16_FLOAT:
    case xenos::TextureFormat::k_32_FLOAT: return 2;
    case xenos::TextureFormat::k_16_16_16_16_FLOAT:
    case xenos::TextureFormat::k_32_32_FLOAT: return 3;
    case xenos::TextureFormat::k_32_32_32_32_FLOAT: return 4;
    default: return -1;
  }
}

// Bytes per pixel of a color render target format (raw copies keep the EDRAM bits).
uint32_t ColorRtBppLog2(uint32_t f) { return (f == 5 || f == 7 || f == 15) ? 3 : 2; }

// The guest D3D's mapping from the destination texture's format to RB_COPY_DEST_INFO's
// (sub_82696978: the cmplwi chain on the fetch format).
uint32_t D3DCopyDestFormat(uint32_t f) {
  switch (f) {
    case 54: return 7;   // k_2_10_10_10_AS_16_16_16_16 -> k_2_10_10_10
    case 55: return 16;  // k_10_11_11_AS_16_16_16_16 -> k_10_11_11
    case 56: return 17;  // k_11_11_10_AS_16_16_16_16 -> k_11_11_10
    case 27: return 30;  // k_16_EXPAND -> k_16_FLOAT
    case 28: return 31;  // k_16_16_EXPAND -> k_16_16_FLOAT
    case 29: return 32;  // k_16_16_16_16_EXPAND -> k_16_16_16_16_FLOAT
    case 22:             // k_24_8
    case 23: return 6;   // k_24_8_FLOAT -> k_8_8_8_8
    default: return f;
  }
}

bool IsFloatFormat(uint32_t f) {
  switch (xenos::TextureFormat(f)) {
    case xenos::TextureFormat::k_16_FLOAT:
    case xenos::TextureFormat::k_16_16_FLOAT:
    case xenos::TextureFormat::k_16_16_16_16_FLOAT:
    case xenos::TextureFormat::k_32_FLOAT:
    case xenos::TextureFormat::k_32_32_FLOAT:
    case xenos::TextureFormat::k_32_32_32_32_FLOAT: return true;
    default: return false;
  }
}

const char kShader[] = R"(
cbuffer Params : register(b0) {
  int2 src_origin;   // source rectangle origin in the render target
  uint2 size;        // rectangle size
  uint row_pitch;    // bytes per row in the output buffer
  uint dest_format;  // xenos::TextureFormat
  uint src_format;   // xenos::ColorRenderTargetFormat, or the depth format
  uint flags;        // 0: convert, 1: swap, 4-6: sample select, 8-10: samples, 12-14: number
  int exp_bias;
  uint r32f_offset;  // depth: byte offset of the R32_FLOAT image
  uint bpp_log2;
  uint pad;
};
#if DEPTH
#if MSAA
Texture2DMS<float> src_depth : register(t0);
Texture2DMS<uint2> src_stencil : register(t1);
#else
Texture2D<float> src_depth : register(t0);
Texture2D<uint2> src_stencil : register(t1);
#endif
#else
#if MSAA
Texture2DMS<float4> src : register(t0);
#else
Texture2D<float4> src : register(t0);
#endif
#endif
RWByteAddressBuffer dst : register(u0);

uint SampleSelect() { return (flags >> 4) & 7; }
uint SampleCount() { return (flags >> 8) & 7; }
// Host sample of a guest sample s (0..3). SanitizeCopySampleSelect: 2x k2/k3 -> k0/k1, 1x -> 0.
// GetD3D10SampleIndexForGuest2xMSAA (native 2x): Xenos 2x sample 0 is the top one, which is
// sample 1 in Direct3D 10.1+; 4x samples map 1:1.
uint HostSample(uint s) {
  uint n = SampleCount();
  return n == 4u ? s : (n == 2u ? 1u - (s & 1u) : 0u);
}

uint U(float v, float scale) { return (uint)(saturate(v) * scale + 0.5); }
uint S8(float v) { return ((uint)(int)round(clamp(v, -1.0, 1.0) * 127.0)) & 0xFF; }

// Xenia xenos::Float32To7e3 (pre-clamped to [0, 31.875]).
uint F7e3(float f) {
  uint u = asuint(clamp(f, 0.0, 31.875));
  if (u < 0x3E800000u) {
    uint shift = min(125u - (u >> 23), 24u);
    u = ((u & 0x7FFFFFu) | 0x800000u) >> shift;
  } else {
    u += 0xC2000000u;
  }
  return ((u + 0x7FFFu + ((u >> 16) & 1u)) >> 16) & 0x3FFu;
}
// Xenia xenos::Float32To20e4 truncating, as ReXGlue's EDRAM store of host D32 depth
// (PreClampedDepthTo20e4 with depth_float24_round off, the plugin's default).
uint F20e4(float f) {
  uint u = asuint(clamp(f, 0.0, 1.99999988));
  if (u < 0x38800000u) {
    uint shift = 113u - (u >> 23);
    u = shift < 24u ? (((u & 0x7FFFFFu) | 0x800000u) >> shift) : 0u;
  } else {
    u += 0xC8000000u;
  }
  return (u >> 3) & 0xFFFFFFu;
}
float F20e4ToF32(uint v) {
  uint m = v & 0xFFFFFu, e = v >> 20;
  if (e == 0u) {
    if (m == 0u) return 0.0;
    uint lz = firstbithigh(m);  // 0..19
    return asfloat(((lz + 93u) << 23) | (((m << (20u - lz)) & 0xFFFFFu) << 3));
  }
  return asfloat(((e + 112u) << 23) | (m << 3));
}
// Xenia xenos::Float7e3To32.
float F7e3ToF32(uint v) {
  uint m = v & 0x7Fu, e = (v >> 7) & 7u;
  return e == 0u ? (float)m * (1.0 / 512.0) : asfloat(((e + 124u) << 23) | (m << 16));
}

#if !DEPTH
// One sample as the Xenos EDRAM holds it. The host target of a 7e3 surface is
// R16G16B16A16_FLOAT; ReXGlue packs every sample into 7e3 (alpha 2-bit unorm) when it stores
// the target to EDRAM before a resolve, so values below 7e3's smallest step (2^-9) become 0.
float4 EdramSample(float4 c) {
  if (src_format == 3u || src_format == 12u)
    return float4(F7e3ToF32(F7e3(c.r)), F7e3ToF32(F7e3(c.g)), F7e3ToF32(F7e3(c.b)),
                  (float)U(c.a, 3.0) * (1.0 / 3.0));
  return c;
}

float4 LoadColor(int2 p) {
#if MSAA
  uint n = SampleCount(), s = SampleSelect();
  if (s <= 3u) return EdramSample(src.Load(p, HostSample(s)));
  if (n == 4u) {
    if (s == 6u)
      return (EdramSample(src.Load(p, 0)) + EdramSample(src.Load(p, 1)) + EdramSample(src.Load(p, 2)) +
              EdramSample(src.Load(p, 3))) * 0.25;
    if (s == 5u) return (EdramSample(src.Load(p, 2)) + EdramSample(src.Load(p, 3))) * 0.5;
  }
  return (EdramSample(src.Load(p, 0)) + EdramSample(src.Load(p, 1))) * 0.5;
#else
  return EdramSample(src.Load(int3(p, 0)));
#endif
}

// The EDRAM bits of a color render target format (raw copies).
uint4 PackRt(float4 c) {
  switch (src_format) {
    case 0u: case 1u:
      return uint4(U(c.r, 255.0) | (U(c.g, 255.0) << 8) | (U(c.b, 255.0) << 16) | (U(c.a, 255.0) << 24), 0, 0, 0);
    case 2u: case 10u:
      return uint4(U(c.r, 1023.0) | (U(c.g, 1023.0) << 10) | (U(c.b, 1023.0) << 20) | (U(c.a, 3.0) << 30), 0, 0, 0);
    case 3u: case 12u:
      return uint4(F7e3(c.r) | (F7e3(c.g) << 10) | (F7e3(c.b) << 20) | (U(c.a, 3.0) << 30), 0, 0, 0);
    case 6u: return uint4(f32tof16(c.r) | (f32tof16(c.g) << 16), 0, 0, 0);
    case 7u: return uint4(f32tof16(c.r) | (f32tof16(c.g) << 16), f32tof16(c.b) | (f32tof16(c.a) << 16), 0, 0);
    case 14u: return uint4(asuint(c.r), 0, 0, 0);
    case 15u: return uint4(asuint(c.r), asuint(c.g), 0, 0);
    default: return uint4(0, 0, 0, 0);
  }
}

// Texels of the destination format (convert copies).
uint4 PackDest(float4 c) {
  c *= exp2((float)exp_bias);
  if (flags & 2u) c = c.bgra;
  uint number = (flags >> 12) & 7u;
  switch (dest_format) {
    case 2u: return uint4(U(c.r, 255.0), 0, 0, 0);                                  // k_8
    case 10u: return uint4(U(c.r, 255.0) | (U(c.g, 255.0) << 8), 0, 0, 0);          // k_8_8
    // 16-bit formats in the texture cache's host layout (its load conversion applied):
    // k_5_6_5 as B5G6R5 (guest R in bits 0-4 moves to 11-15), k_6_5_5 as B5G6R5 holding
    // R | B << 5 | G << 11 (sampled with swizzle RBGG).
    case 4u: return uint4(U(c.b, 31.0) | (U(c.g, 63.0) << 5) | (U(c.r, 31.0) << 11), 0, 0, 0);
    case 5u: return uint4(U(c.r, 31.0) | (U(c.b, 63.0) << 5) | (U(c.g, 31.0) << 11), 0, 0, 0);
    case 6u:                                                                         // k_8_8_8_8
      if (number == 1u) return uint4(S8(c.r) | (S8(c.g) << 8) | (S8(c.b) << 16) | (S8(c.a) << 24), 0, 0, 0);
      return uint4(U(c.r, 255.0) | (U(c.g, 255.0) << 8) | (U(c.b, 255.0) << 16) | (U(c.a, 255.0) << 24), 0, 0, 0);
    case 7u:                                                                         // k_2_10_10_10
      return uint4(U(c.r, 1023.0) | (U(c.g, 1023.0) << 10) | (U(c.b, 1023.0) << 20) | (U(c.a, 3.0) << 30), 0, 0, 0);
    case 30u: return uint4(f32tof16(c.r), 0, 0, 0);
    case 31u: return uint4(f32tof16(c.r) | (f32tof16(c.g) << 16), 0, 0, 0);
    case 32u: return uint4(f32tof16(c.r) | (f32tof16(c.g) << 16), f32tof16(c.b) | (f32tof16(c.a) << 16), 0, 0);
    case 36u: return uint4(asuint(c.r), 0, 0, 0);
    case 37u: return uint4(asuint(c.r), asuint(c.g), 0, 0);
    case 38u: return asuint(c);
    default: return uint4(0, 0, 0, 0);
  }
}
#endif

uint4 PackPixel(int2 p, out float r32f) {
  r32f = 0.0;
#if DEPTH
#if MSAA
  // Depth is never averaged (ReXGlue SanitizeCopySampleSelect): 4x k23 -> sample 2, else 0.
  uint sel = SampleSelect();
  uint s = HostSample(sel <= 3u ? sel : (sel == 5u && SampleCount() == 4u ? 2u : 0u));
  float d = src_depth.Load(p, s);
  uint stencil = src_stencil.Load(p, s).g;
#else
  float d = src_depth.Load(int3(p, 0));
  uint stencil = src_stencil.Load(int3(p, 0)).g;
#endif
  // D24S8: round to the nearest even like ReXGlue's EDRAM store (OpRoundNE of d * 0xFFFFFF).
  uint d24 = src_format == 1u ? F20e4(d) : (uint)round(saturate(d) * 16777215.0);
  r32f = src_format == 1u ? F20e4ToF32(d24) : (float)d24 / 16777215.0;
  return uint4((d24 << 8) | (stencil & 0xFFu), 0, 0, 0);
#else
  float4 c = LoadColor(p);
  return (flags & 1u) ? PackDest(c) : PackRt(c);
#endif
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint bpp = 1u << bpp_log2;
  uint per_thread = bpp >= 4u ? 1u : (4u >> bpp_log2);
  uint x = id.x * per_thread;
  if (x >= size.x || id.y >= size.y) return;
  uint addr = id.y * row_pitch + x * bpp;
  float r32f;
  if (bpp >= 4u) {
    uint4 v = PackPixel(src_origin + int2(x, id.y), r32f);
    if (bpp == 4u) dst.Store(addr, v.x);
    else if (bpp == 8u) dst.Store2(addr, v.xy);
    else dst.Store4(addr, v);
#if DEPTH
    dst.Store(r32f_offset + addr, asuint(r32f));
#endif
  } else {
    uint word = 0, mask = (1u << (bpp * 8u)) - 1u;
    for (uint k = 0; k < per_thread && x + k < size.x; ++k)
      word |= (PackPixel(src_origin + int2(x + k, id.y), r32f).x & mask) << (k * bpp * 8u);
    dst.Store(addr, word);
  }
}
)";

struct ShaderConstants {
  int32_t src_origin[2];
  uint32_t size[2];
  uint32_t row_pitch, dest_format, src_format, flags;
  int32_t exp_bias;
  uint32_t r32f_offset, bpp_log2, pad;
};
static_assert(sizeof(ShaderConstants) == 48);

using PFN_D3DCompile = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
                                        LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

D3D12_RESOURCE_BARRIER Barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b = {};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = r;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = before;
  b.Transition.StateAfter = after;
  return b;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Decoding

bool DecodeResolve(const uint8_t* base, const HookArgs& args, ResolveParams* out) {
  static uint32_t log_tex = 0, log_cmd = 0, log_packed = 0, log_fmt = 0;
  ResolveParams p;
  const uint32_t device = args.device;
  const uint32_t flags = args.r[1];
  const uint32_t rect_ptr = args.r[2], tex = args.r[3], point_ptr = args.r[4];
  const uint32_t level = args.r[5], slice = args.r[6], color_ptr = args.r[7];
  p.regs = SurfaceRegsFromDevice(base, device);
  p.source = flags & 7;
  if (p.source > 4) {
    PGR4_LOG_ONCE(log_cmd, "PGR4 native resolve: source select {} unsupported", p.source);
    return false;
  }
  // Sample select: Flags bits 4-6 hold it plus one; 0 means "by the source's MSAA".
  uint32_t field = (flags >> 4) & 7;
  if (!field) {
    const uint32_t msaa = (p.regs.surface_info >> 16) & 3;
    field = msaa == 0 ? 1 : msaa == 1 ? 5 : 7;
  }
  p.sample_select = field - 1;
  if (flags & 0x2000) {
    PGR4_LOG_ONCE(log_cmd, "PGR4 native resolve: null copy (Flags {:08X}) unsupported", flags);
    return false;
  }
  p.copy_command = p.source == 4 ? 0 : 1;
  p.exp_bias = int32_t(flags << 0) >> 26;  // bits 26-31, signed

  // The destination texture's fetch constant: D3DBaseTexture + 28, 6 big-endian dwords.
  if (!tex) {
    PGR4_LOG_ONCE(log_tex, "PGR4 native resolve: no destination texture");
    return false;
  }
  uint32_t fetch[6];
  for (int i = 0; i < 6; ++i) fetch[i] = Load32(base, tex + 28 + 4 * i);
  TextureKey key;
  if (!TextureKeyFromFetch(fetch, &key)) {
    PGR4_LOG_ONCE(log_tex, "PGR4 native resolve: invalid destination fetch constant {:08X} {:08X}", fetch[0], fetch[1]);
    return false;
  }
  if (key.dimension == xenos::DataDimension::k3D) {
    PGR4_LOG_ONCE(log_tex, "PGR4 native resolve: 3D destination unsupported");
    return false;
  }
  const uint32_t fetch_format = fetch[1] & 0x3F;
  p.dest_format = D3DCopyDestFormat(fetch_format);
  p.dest_endian = (fetch[1] >> 6) & 3;
  p.dest_swap = ((fetch[3] >> 1) & 7) != 0;  // swizzle X is not R
  {
    const uint32_t sign_x = (fetch[0] >> 2) & 3;
    const bool integer = fetch[3] & 1;
    p.dest_number = IsFloatFormat(p.dest_format) ? 7 : integer ? (sign_x == 1 ? 3 : 2) : (sign_x == 1 ? 1 : 0);
  }
  if (DestBppLog2(p.dest_format) < 0) {
    PGR4_LOG_ONCE(log_fmt, "PGR4 native resolve: destination format {} (fetch format {}) unsupported, source {}, "
                  "surface {:08X} {:08X} {:08X}", p.dest_format, fetch_format, p.source, p.regs.surface_info,
                  p.regs.color_info[0], p.regs.depth_info);
    return false;
  }
  p.dest_width = std::max(1u, key.width >> level);
  p.dest_height = std::max(1u, key.height >> level);
  // Destination address of the level and slice. The texture object holds virtual addresses;
  // the D3D (sub_82696978) makes them physical the way the 0xE0000000 view maps: low 29 bits
  // plus 4 KiB for addresses at or above 0xE0000000. That is also the physical address the
  // GPU-side fetch constant (and so the texture cache) sees.
  auto d3d_phys = [](uint32_t a) { return (a & 0x1FFFFFFFu) + ((((a >> 20) + 0x200u) & 0x1000u)); };
  const texture_util::TextureGuestLayout layout = texture_util::GetGuestTextureLayout(
      key.dimension, key.pitch, key.width, key.height, key.depth_or_array_size, key.tiled,
      rex::graphics::GetBaseFormat(xenos::TextureFormat(fetch_format)), key.packed_mips,
      true, std::max(level, key.mip_max_level));
  const texture_util::TextureGuestLayout::Level* lv;
  if (level == 0) {
    p.dest_base = d3d_phys(fetch[1] & 0xFFFFF000u);
    lv = &layout.base;
  } else {
    if (layout.packed_level != UINT32_MAX && level >= layout.packed_level) {
      PGR4_LOG_ONCE(log_packed, "PGR4 native resolve: packed mip tail destination (level {}) unsupported", level);
      return false;
    }
    p.dest_base = d3d_phys(fetch[5] & 0xFFFFF000u) + layout.mip_offsets_bytes[level];
    lv = &layout.mips[level];
  }
  p.dest_base += lv->array_slice_stride_bytes * slice;
  p.dest_tiled = key.tiled;
  p.dest_pitch_blocks = lv->row_pitch_bytes >> DestBppLog2(p.dest_format);
  p.dest_extent_bytes = lv->array_slice_data_extent_bytes;

  // Rectangle: pSourceRect or the destination level's size; right/bottom rounded up to 8.
  if (rect_ptr) {
    p.x0 = int32_t(Load32(base, rect_ptr));
    p.y0 = int32_t(Load32(base, rect_ptr + 4));
    p.x1 = int32_t(Load32(base, rect_ptr + 8));
    p.y1 = int32_t(Load32(base, rect_ptr + 12));
  } else {
    p.x1 = int32_t(p.dest_width);
    p.y1 = int32_t(p.dest_height);
  }
  p.x1 = (p.x1 + 7) & ~7;
  p.y1 = (p.y1 + 7) & ~7;
  if (point_ptr) {
    p.dest_x = int32_t(Load32(base, point_ptr));
    p.dest_y = int32_t(Load32(base, point_ptr + 4));
  }
  // Clears: color from pClearColor (default: the D3D's constant at 0x82240844), depth from
  // ClearZ (f1), stencil from the stack argument (caller r1 + 92, read in sub_82696978).
  p.clear_flags = flags & 0x300;
  if (p.clear_flags) {
    if (color_ptr) {
      for (int i = 0; i < 4; ++i) p.clear_color[i] = LoadF(base, color_ptr + 4 * i);
    } else {
      const float c = LoadF(base, 0x82240844u);
      for (float& v : p.clear_color) v = c;
    }
    p.clear_depth = float(args.f[0]);
    p.clear_stencil = uint8_t(Load32(base, args.sp + 92));
  }
  *out = p;
  return true;
}

// ---------------------------------------------------------------------------------------------
// Resolver

struct Resolver::Image {
  ComPtr<ID3D12Resource> texture, depth_r32f;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  uint32_t width = 0, height = 0;
  D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
  ResolvedImage view;
};

namespace {
uint32_t g_full_readback_holds = 0;
std::string g_readback_before;
}  // namespace

void HoldFullReadback(bool hold) {
  // The plugin reads the cvar per resolve on its GPU thread; "fast", "some" and "full" are all
  // short strings, so the assignment never reallocates under it.
  if (hold) {
    if (g_full_readback_holds++ == 0) {
      g_readback_before = rex::cvar::GetFlagByName("readback_resolve");
      if (!rex::cvar::SetFlagByName("readback_resolve", "full"))
        REXLOG_WARN("PGR4 native: could not set readback_resolve=full; guest copies may be a frame old");
    }
  } else if (g_full_readback_holds && --g_full_readback_holds == 0) {
    rex::cvar::SetFlagByName("readback_resolve", g_readback_before.empty() ? "none" : g_readback_before);
  }
}

struct Resolver::Dump {
  std::filesystem::path dir;
  bool recording = false;
  uint64_t frame = 0, fence = 0;  // the dumped frame and the fence it signals
  struct Entry {
    ResolveParams p;
    uint32_t width, height, row_pitch, bpp;
    ComPtr<ID3D12Resource> readback;
    std::vector<uint8_t> guest;  // guest memory at dest_base after the frame (AfterSwap)
  };
  std::vector<Entry> entries;
  bool guest_read = false;
};

Resolver::Resolver() = default;
Resolver::~Resolver() = default;

bool Resolver::Initialize(ID3D12Device* device, RenderTargets* targets, ID3D12CommandQueue* queue) {
  device_ = device;
  targets_ = targets;
  D3D12_DESCRIPTOR_HEAP_DESC hd = {};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors = kHeapPerFrame * kHeapSlots;
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (Failed(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)), "resolve CreateDescriptorHeap")) return false;
  heap_stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  if (!CreatePipelines()) return false;
  const std::string dir = REXCVAR_GET(pgr4_native_resolve_dump);
  if (!dir.empty()) {
    dump_ = std::make_unique<Dump>();
    dump_->dir = dir;
    std::error_code ec;
    std::filesystem::create_directories(dump_->dir, ec);
    REXLOG_INFO("PGR4 native resolve: dumps to {} (create dump.request there)", dir);
  }
  if (REXCVAR_GET(pgr4_native_resolve_selftest) && queue)
    REXLOG_INFO("PGR4 native resolve selftest: {}", SelfTest(queue) ? "all passed" : "FAILED");
  return true;
}

bool Resolver::CreatePipelines() {
  // Root signature: b0 = 12 root constants, t0-t1 = table, u0 = root UAV (the scratch buffer).
  D3D12_DESCRIPTOR_RANGE range = {};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors = 2;
  D3D12_ROOT_PARAMETER params[3] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants.Num32BitValues = sizeof(ShaderConstants) / 4;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable.NumDescriptorRanges = 1;
  params[1].DescriptorTable.pDescriptorRanges = &range;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC rd = {};
  rd.NumParameters = 3;
  rd.pParameters = params;
  ComPtr<ID3DBlob> blob, error;
  if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error))) {
    REXLOG_ERROR("PGR4 native resolve: root signature: {}",
                 error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
    return false;
  }
  if (Failed(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                          IID_PPV_ARGS(&root_signature_)),
             "resolve CreateRootSignature"))
    return false;

  // d3dcompiler_47.dll ships with Windows 10/11 (and Proton); the shader is tiny.
  HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
  auto compile = dll ? reinterpret_cast<PFN_D3DCompile>(GetProcAddress(dll, "D3DCompile")) : nullptr;
  if (!compile) {
    REXLOG_ERROR("PGR4 native resolve: d3dcompiler_47.dll / D3DCompile not found");
    return false;
  }
  for (int depth = 0; depth < 2; ++depth) {
    for (int msaa = 0; msaa < 2; ++msaa) {
      const D3D_SHADER_MACRO defines[] = {{"DEPTH", depth ? "1" : "0"}, {"MSAA", msaa ? "1" : "0"}, {nullptr, nullptr}};
      ComPtr<ID3DBlob> code, errors;
      HRESULT hr = compile(kShader, sizeof(kShader) - 1, "pgr4_resolve.hlsl", defines, nullptr, "main", "cs_5_0",
                           D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
      if (FAILED(hr)) {
        REXLOG_ERROR("PGR4 native resolve: shader (depth {}, msaa {}): {}", depth, msaa,
                     errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return false;
      }
      D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
      pd.pRootSignature = root_signature_.Get();
      pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
      if (Failed(device_->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso_[depth * 2 + msaa])),
                 "resolve CreateComputePipelineState"))
        return false;
    }
  }
  return true;
}

bool Resolver::EnsureScratch(uint64_t bytes) {
  if (scratch_ && scratch_size_ >= bytes) return true;
  const uint64_t size = std::max<uint64_t>(bytes, 16ull << 20);
  D3D12_RESOURCE_DESC d = {};
  d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  d.Width = size;
  d.Height = 1;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.SampleDesc.Count = 1;
  d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  ComPtr<ID3D12Resource> res;
  if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON,
                                              nullptr, IID_PPV_ARGS(&res)),
             "resolve scratch buffer"))
    return false;
  if (scratch_) retired_.push_back({std::move(scratch_), frame_fence_});
  scratch_ = std::move(res);
  scratch_size_ = size;
  scratch_state_ = D3D12_RESOURCE_STATE_COMMON;
  return true;
}

Resolver::Image* Resolver::GetImage(const ResolveParams& p, uint32_t width, uint32_t height) {
  const HostFormat* hf = GetHostFormat(xenos::TextureFormat(p.dest_format));
  // The shader writes the texture cache's host layout, so its 16-bit field reorders are fine;
  // depth conversions (k_24_8 sampled as R32_FLOAT) come from depth_r32f instead.
  if (!hf || hf->block_compressed ||
      (hf->conversion != TextureConversion::kCopy && hf->conversion != TextureConversion::kR5G6B5ToB5G6R5 &&
       hf->conversion != TextureConversion::kR5G5B6ToB5G6R5WithRBGASwizzle))
    return nullptr;
  const bool depth = p.source == 4;
  std::unique_ptr<Image>& slot = images_[p.dest_base];
  if (changed_) changed_(p.dest_base);
  if (slot && slot->format == hf->resource && slot->width == width && slot->height == height &&
      bool(slot->depth_r32f) == depth)
    return slot.get();
  auto img = std::make_unique<Image>();
  img->format = hf->resource;
  img->width = width;
  img->height = height;
  D3D12_RESOURCE_DESC d = {};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = width;
  d.Height = height;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.Format = hf->resource;
  d.SampleDesc.Count = 1;
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, IID_PPV_ARGS(&img->texture)),
             "resolve destination texture"))
    return nullptr;
  if (depth) {
    d.Format = DXGI_FORMAT_R32_FLOAT;
    if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST,
                                                nullptr, IID_PPV_ARGS(&img->depth_r32f)),
               "resolve depth_r32f texture"))
      return nullptr;
  }
  img->state = D3D12_RESOURCE_STATE_COPY_DEST;
  img->view.guest_base = p.dest_base;
  img->view.format = xenos::TextureFormat(p.dest_format);
  img->view.texture = img->texture.Get();
  img->view.depth_r32f = img->depth_r32f.Get();
  if (slot) {
    retired_.push_back({std::move(slot->texture), frame_fence_});
    if (slot->depth_r32f) retired_.push_back({std::move(slot->depth_r32f), frame_fence_});
  }
  slot = std::move(img);
  return slot.get();
}

const ResolvedImage* Resolver::Find(uint32_t guest_phys_base) const {
  auto it = images_.find(guest_phys_base);
  return it == images_.end() || it->second->view.seq == 0 ? nullptr : &it->second->view;
}

void Resolver::BeginFrame(const uint8_t* base, uint64_t frame, uint64_t frame_fence, uint64_t completed_fence) {
  frame_ = frame;
  frame_fence_ = frame_fence;
  heap_next_ = 0;
  retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
                                [&](const Retired& r) { return r.fence <= completed_fence; }),
                 retired_.end());
  if (dump_) DumpBeginFrame(base, completed_fence);
}

void Resolver::OnResolve(ID3D12GraphicsCommandList* list, const uint8_t* base, const HookArgs& args) {
  ResolveParams p;
  if (!DecodeResolve(base, args, &p)) {
    ++failures_;
    return;
  }
  CheckAgainstShadow(base, args.device, p);
  if (!Resolve(list, p)) ++failures_;
}

void Resolver::CheckAgainstShadow(const uint8_t* base, uint32_t device, const ResolveParams& p) {
  // RB_COPY_DEST_BASE / RB_COPY_DEST_PITCH (0x2319, 0x231A) in the shadow block 0x2300 at
  // device+10680, as the guest Resolve that just ran left them. It zeroes RB_COPY_CONTROL and
  // RB_COPY_DEST_INFO there afterwards, so those are checked offline against the trace
  // (the dump's copy_control/dest_info vs the trace's RESOLVE lines, compare_resolves.py --trace).
  const uint32_t b = device + 10680 + 0x19 * 4;
  const uint32_t dest = Load32(base, b), pitch = Load32(base, b + 4);
  // The D3D moves the base by the tiled offset of the rectangle origin minus the dest point;
  // only the unshifted case is compared.
  const bool origin = p.x0 == p.dest_x && p.y0 == p.dest_y;
  // RB_COPY_DEST_PITCH's height is the level height minus the dest point's y (r18 in the D3D).
  if ((!origin || dest == p.dest_base) && ((pitch >> 16) & 0x3FFF) == p.dest_height - uint32_t(p.dest_y)) return;
  ++shadow_mismatches_;
  static uint32_t logged = 0;
  if (logged++ < 16)
    REXLOG_WARN("PGR4 native resolve: decode vs D3D shadow: dest {:08X}/{:08X} pitch {:08X} (decoded {}x{}, "
                "rect {},{}-{},{} point {},{})",
                dest, p.dest_base, pitch, p.dest_width, p.dest_height, p.x0, p.y0, p.x1, p.y1, p.dest_x, p.dest_y);
}

static uint32_t CopyControlOf(const ResolveParams& p) {
  return p.source | (p.sample_select << 4) | p.clear_flags | (p.copy_command << 20);
}
static uint32_t DestInfoOf(const ResolveParams& p) {
  return p.dest_endian | (p.dest_format << 7) | (p.dest_number << 13) | ((uint32_t(p.exp_bias) & 0x3F) << 16) |
         (uint32_t(p.dest_swap) << 24);
}

bool Resolver::Resolve(ID3D12GraphicsCommandList* list, const ResolveParams& p) {
  static uint32_t log_raw = 0, log_rt = 0, log_img = 0, log_heap = 0;
  const bool depth = p.source == 4;
  const int bpp_log2 = DestBppLog2(p.dest_format);
  if (bpp_log2 < 0) return false;
  RenderTarget* rt = depth ? targets_->GetDepth(p.regs, uint32_t(std::max(p.y1, 0)))
                           : targets_->GetColor(p.regs, p.source, uint32_t(std::max(p.y1, 0)));
  if (!rt) {
    PGR4_LOG_ONCE(log_rt, "PGR4 native resolve: no host target for source {} (surface {:08X})", p.source,
            p.regs.surface_info);
    return false;
  }
  if (p.copy_command == 0 && !depth && ColorRtBppLog2(rt->guest_format) != uint32_t(bpp_log2)) {
    PGR4_LOG_ONCE(log_raw, "PGR4 native resolve: raw copy from RT format {} into format {}", rt->guest_format,
            p.dest_format);
    return false;
  }
  // Clip the rectangle to the source target and the destination level.
  int32_t x0 = std::max(p.x0, 0), y0 = std::max(p.y0, 0);
  int32_t x1 = std::min<int32_t>(p.x1, int32_t(rt->width)), y1 = std::min<int32_t>(p.y1, int32_t(rt->height));
  int32_t dx = p.dest_x + (x0 - p.x0), dy = p.dest_y + (y0 - p.y0);
  x1 = std::min<int32_t>(x1, x0 + int32_t(p.dest_width) - dx);
  y1 = std::min<int32_t>(y1, y0 + int32_t(p.dest_height) - dy);
  if (x1 <= x0 || y1 <= y0 || dx < 0 || dy < 0) return true;  // nothing to copy
  const uint32_t w = uint32_t(x1 - x0), h = uint32_t(y1 - y0);

  Image* img = GetImage(p, p.dest_width, p.dest_height);
  if (!img) {
    PGR4_LOG_ONCE(log_img, "PGR4 native resolve: no host image for format {} {}x{}", p.dest_format, p.dest_width,
            p.dest_height);
    return false;
  }
  const uint32_t bpp = 1u << bpp_log2;
  const uint32_t row_pitch = (w * bpp + kPitchAlign - 1) & ~(kPitchAlign - 1);
  const uint64_t image_bytes = (uint64_t(row_pitch) * h + kPlacement - 1) & ~(kPlacement - 1);
  if (!EnsureScratch(depth ? image_bytes * 2 : image_bytes)) return false;

  // Descriptors: t0 = color / depth plane, t1 = stencil plane (null SRV for color).
  if (heap_next_ + 2 > kHeapPerFrame) {
    PGR4_LOG_ONCE(log_heap, "PGR4 native resolve: more than {} resolves in a frame; descriptors wrap", kHeapPerFrame / 2);
    heap_next_ = 0;
  }
  const uint32_t slot = uint32_t(frame_ % kHeapSlots) * kHeapPerFrame + heap_next_;
  heap_next_ += 2;
  D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap_->GetCPUDescriptorHandleForHeapStart();
  cpu.ptr += size_t(slot) * heap_stride_;
  D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap_->GetGPUDescriptorHandleForHeapStart();
  gpu.ptr += uint64_t(slot) * heap_stride_;
  D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
  sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sd.ViewDimension = rt->samples > 1 ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
  if (rt->samples == 1) sd.Texture2D.MipLevels = 1;
  sd.Format = rt->srv_format;
  device_->CreateShaderResourceView(rt->resource.Get(), &sd, cpu);
  D3D12_CPU_DESCRIPTOR_HANDLE cpu1 = cpu;
  cpu1.ptr += heap_stride_;
  if (depth) {
    sd.Format = rt->stencil_srv_format;
    if (rt->samples == 1) sd.Texture2D.PlaneSlice = 1;
    device_->CreateShaderResourceView(rt->resource.Get(), &sd, cpu1);
  } else {
    D3D12_SHADER_RESOURCE_VIEW_DESC nd = {};
    nd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nd.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(nullptr, &nd, cpu1);
  }

  // Source readable by the compute shader, scratch writable.
  RenderTargets::Transition(list, rt, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  if (scratch_state_ != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
    D3D12_RESOURCE_BARRIER b = Barrier(scratch_.Get(), scratch_state_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->ResourceBarrier(1, &b);
    scratch_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  }
  ShaderConstants c = {};
  c.src_origin[0] = x0;
  c.src_origin[1] = y0;
  c.size[0] = w;
  c.size[1] = h;
  c.row_pitch = row_pitch;
  c.dest_format = p.dest_format;
  c.src_format = rt->guest_format;
  c.flags = (p.copy_command == 1 ? 1u : 0u) | (p.dest_swap ? 2u : 0u) | (p.sample_select << 4) |
            (rt->samples << 8) | (p.dest_number << 12);
  c.exp_bias = p.exp_bias;
  c.r32f_offset = uint32_t(image_bytes);
  c.bpp_log2 = uint32_t(bpp_log2);
  ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(root_signature_.Get());
  list->SetPipelineState(pso_[(depth ? 2 : 0) + (rt->samples > 1 ? 1 : 0)].Get());
  list->SetComputeRoot32BitConstants(0, sizeof(c) / 4, &c, 0);
  list->SetComputeRootDescriptorTable(1, gpu);
  list->SetComputeRootUnorderedAccessView(2, scratch_->GetGPUVirtualAddress());
  const uint32_t per_thread = bpp >= 4 ? 1 : 4 >> bpp_log2;
  list->Dispatch(((w + per_thread - 1) / per_thread + 7) / 8, (h + 7) / 8, 1);

  // Scratch -> destination texture (and the depth float image).
  D3D12_RESOURCE_BARRIER bs[3];
  uint32_t nb = 0;
  bs[nb++] = Barrier(scratch_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  scratch_state_ = D3D12_RESOURCE_STATE_COPY_SOURCE;
  if (img->state != D3D12_RESOURCE_STATE_COPY_DEST) {
    bs[nb++] = Barrier(img->texture.Get(), img->state, D3D12_RESOURCE_STATE_COPY_DEST);
    if (img->depth_r32f) bs[nb++] = Barrier(img->depth_r32f.Get(), img->state, D3D12_RESOURCE_STATE_COPY_DEST);
  }
  list->ResourceBarrier(nb, bs);
  D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
  src.pResource = scratch_.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src.PlacedFootprint.Offset = 0;
  src.PlacedFootprint.Footprint = {img->format, w, h, 1, row_pitch};
  dst.pResource = img->texture.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst.SubresourceIndex = 0;
  list->CopyTextureRegion(&dst, UINT(dx), UINT(dy), 0, &src, nullptr);
  if (img->depth_r32f) {
    src.PlacedFootprint.Offset = image_bytes;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
    dst.pResource = img->depth_r32f.Get();
    list->CopyTextureRegion(&dst, UINT(dx), UINT(dy), 0, &src, nullptr);
  }
  nb = 0;
  bs[nb++] = Barrier(img->texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kImageReadState);
  if (img->depth_r32f) bs[nb++] = Barrier(img->depth_r32f.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kImageReadState);
  list->ResourceBarrier(nb, bs);
  img->state = kImageReadState;
  img->view.width = std::max(img->view.width, uint32_t(dx) + w);
  img->view.height = std::max(img->view.height, uint32_t(dy) + h);
  img->view.seq = ++seq_;
  if (changed_) changed_(p.dest_base);
  ++resolves_;

  if (dump_ && dump_->recording) DumpRecord(list, p, img, uint32_t(dx) + w, uint32_t(dy) + h);

  // Clears of the source surfaces over the resolved rectangle (after the copy).
  if (p.clear_flags) {
    ClearParams cp;
    cp.flags = ((p.clear_flags & 0x100) ? (1u << (depth ? 0 : p.source)) : 0) |
               ((p.clear_flags & 0x200) ? 0x30u : 0);
    std::memcpy(cp.color, p.clear_color, sizeof(cp.color));
    cp.depth = p.clear_depth;
    cp.stencil = p.clear_stencil;
    cp.rects.push_back({LONG(x0), LONG(y0), LONG(x1), LONG(y1)});
    targets_->Clear(list, p.regs, 0xF, true, uint32_t(std::max(p.y1, 0)), cp);
  }
  return true;
}

// ---------------------------------------------------------------------------------------------
// Oracle dumps

void Resolver::DumpRecord(ID3D12GraphicsCommandList* list, const ResolveParams& p, Image* img, uint32_t width,
                          uint32_t height) {
  Dump& d = *dump_;
  Dump::Entry e;
  e.p = p;
  e.width = width;
  e.height = height;
  e.bpp = 1u << DestBppLog2(p.dest_format);
  e.row_pitch = (width * e.bpp + kPitchAlign - 1) & ~(kPitchAlign - 1);
  D3D12_RESOURCE_DESC bd = {};
  bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bd.Width = uint64_t(e.row_pitch) * height;
  bd.Height = 1;
  bd.DepthOrArraySize = 1;
  bd.MipLevels = 1;
  bd.SampleDesc.Count = 1;
  bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_READBACK;
  if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, IID_PPV_ARGS(&e.readback)),
             "resolve dump readback"))
    return;
  D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
  src.pResource = img->texture.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst.pResource = e.readback.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint.Footprint = {img->format, width, height, 1, e.row_pitch};
  D3D12_BOX box = {0, 0, 0, width, height, 1};
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
  d.entries.push_back(std::move(e));
}

void Resolver::AfterSwap(const uint8_t* base) {
  if (!dump_ || !dump_->recording) return;
  Dump& d = *dump_;
  d.recording = false;
  // The guest thread is in the dumped frame's Swap: let the Xenos plugin's GPU thread execute
  // the frame (its resolves read back synchronously, HoldFullReadback) before reading memory.
  Sleep(DWORD(std::max(0, REXCVAR_GET(pgr4_native_capture_hold_ms))));
  for (Dump::Entry& e : d.entries) {
    // Physical memory through the 0xA0000000 view (64 KiB pages; maps all of it).
    const uint8_t* g = base + 0xA0000000u + e.p.dest_base;
    e.guest.assign(g, g + e.p.dest_extent_bytes);
  }
  d.guest_read = true;
  HoldFullReadback(false);
  REXLOG_INFO("PGR4 native resolve: read guest memory of {} resolves of frame {}", d.entries.size(), d.frame);
}

void Resolver::DumpBeginFrame(const uint8_t* base, uint64_t completed_fence) {
  (void)base;
  Dump& d = *dump_;
  const std::string prefix = "f" + std::to_string(d.frame);
  if (d.guest_read && completed_fence >= d.fence) {
    std::FILE* meta = std::fopen((d.dir / (prefix + "-resolves.jsonl")).string().c_str(), "wb");
    for (size_t i = 0; i < d.entries.size(); ++i) {
      Dump::Entry& e = d.entries[i];
      const ResolveParams& p = e.p;
      const std::string stem = prefix + "-r" + std::to_string(i);
      void* data = nullptr;
      D3D12_RANGE range = {0, size_t(e.row_pitch) * e.height};
      if (SUCCEEDED(e.readback->Map(0, &range, &data))) {
        std::FILE* f = std::fopen((d.dir / (stem + "-native.bin")).string().c_str(), "wb");
        if (f) {
          for (uint32_t y = 0; y < e.height; ++y)
            std::fwrite(static_cast<const uint8_t*>(data) + size_t(y) * e.row_pitch, 1, size_t(e.width) * e.bpp, f);
          std::fclose(f);
        }
        D3D12_RANGE none = {0, 0};
        e.readback->Unmap(0, &none);
      }
      if (std::FILE* f = std::fopen((d.dir / (stem + "-guest.bin")).string().c_str(), "wb")) {
        std::fwrite(e.guest.data(), 1, e.guest.size(), f);
        std::fclose(f);
      }
      if (meta)
        std::fprintf(meta,
                     "{\"index\":%zu,\"source\":%u,\"surface_info\":%u,\"source_info\":%u,\"sample_select\":%u,"
                     "\"copy_command\":%u,\"rect\":[%d,%d,%d,%d],\"dest_point\":[%d,%d],\"dest_base\":%u,"
                     "\"dest_format\":%u,\"dest_width\":%u,\"dest_height\":%u,\"endian\":%u,\"number\":%u,"
                     "\"swap\":%d,\"exp_bias\":%d,\"tiled\":%d,\"pitch_blocks\":%u,\"extent_bytes\":%u,"
                     "\"bpp\":%u,\"native_width\":%u,\"native_height\":%u,\"clear_flags\":%u,"
                     "\"copy_control\":%u,\"dest_info\":%u}\n",
                     i, p.source, p.regs.surface_info, p.source == 4 ? p.regs.depth_info : p.regs.color_info[p.source & 3],
                     p.sample_select, p.copy_command, p.x0, p.y0, p.x1, p.y1, p.dest_x, p.dest_y, p.dest_base,
                     p.dest_format, p.dest_width, p.dest_height, p.dest_endian, p.dest_number, p.dest_swap ? 1 : 0,
                     p.exp_bias, p.dest_tiled ? 1 : 0, p.dest_pitch_blocks, p.dest_extent_bytes, e.bpp, e.width,
                     e.height, p.clear_flags, CopyControlOf(p), DestInfoOf(p));
    }
    if (meta) std::fclose(meta);
    REXLOG_INFO("PGR4 native resolve: dumped {} resolves (native + guest) of frame {} to {}", d.entries.size(),
                d.frame, d.dir.string());
    d.entries.clear();
    d.guest_read = false;
  }
  if (!d.recording && !d.guest_read) {
    std::error_code ec;
    if (std::filesystem::remove(d.dir / "dump.request", ec)) {
      HoldFullReadback(true);  // before any of this frame's resolves reach the plugin
      d.recording = true;
      d.frame = frame_;
      d.fence = frame_fence_;  // signalled when the dumped frame is done on the GPU
      REXLOG_INFO("PGR4 native resolve: dumping the resolves of frame {}", frame_);
    }
  }
}


// ---------------------------------------------------------------------------------------------
// Self test

bool Resolver::SelfTest(ID3D12CommandQueue* queue) {
  ComPtr<ID3D12CommandAllocator> alloc;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  if (Failed(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)), "selftest allocator") ||
      Failed(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list)),
             "selftest list") ||
      Failed(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "selftest fence"))
    return false;
  list->Close();
  struct Case {
    const char* name;
    bool depth;
    uint32_t msaa;    // RB_SURFACE_INFO msaa_samples
    uint32_t format;  // color or depth render target format
    float color[4];
    float z;
    uint8_t stencil;
    uint32_t dest_format, copy_command, sample_select;
    bool swap;
    int32_t bias;
    uint32_t expect[2];  // first one or two dwords of every texel
    float expect_r32f;
  };
  // Expected values: the Xenos encodings, worked out by hand (unorm: v * max + 0.5 truncated;
  // 20e4 and 7e3 as xenos.h; halves exact). 7e3 sources go through the EDRAM's 7e3 first:
  // 1e-6 is below 7e3's smallest step and becomes 0, alpha 0.5 becomes 2/3 (2-bit unorm).
  const Case cases[] = {
      {"8888 1x -> k_8_8_8_8", false, 0, 0, {64 / 255.0f, 128 / 255.0f, 191 / 255.0f, 1.0f}, 0, 0, 6, 1, 0, false, 0, {0xFFBF8040u, 0}, 0},
      {"8888 1x -> k_8_8_8_8 swap", false, 0, 0, {64 / 255.0f, 128 / 255.0f, 191 / 255.0f, 1.0f}, 0, 0, 6, 1, 0, true, 0, {0xFF4080BFu, 0}, 0},
      {"8888 1x raw", false, 0, 0, {64 / 255.0f, 128 / 255.0f, 191 / 255.0f, 1.0f}, 0, 0, 6, 0, 0, false, 0, {0xFFBF8040u, 0}, 0},
      {"7e3 4x avg4 -> k_16_16_16_16_FLOAT", false, 2, 3, {1.5f, 0.25f, 1e-6f, 0.5f}, 0, 0, 32, 1, 6, false, 0,
       {0x34003E00u, 0x39550000u}, 0},
      {"7e3 1x -> k_8_8_8_8 (saturate)", false, 0, 3, {1.5f, 0.25f, 0.0f, 0.5f}, 0, 0, 6, 1, 0, false, 0, {0xAA0040FFu, 0}, 0},
      {"7e3_AS_16 2x avg2 -> k_2_10_10_10", false, 1, 12, {0.5f, 0.25f, 1.0f, 1.0f}, 0, 0, 7, 1, 4, false, 0,
       {0xFFF40200u, 0}, 0},
      {"8888 1x -> k_5_6_5", false, 0, 0, {1.0f, 0.0f, 0.0f, 1.0f}, 0, 0, 4, 1, 0, false, 0, {0xF800u, 0}, 0},
      {"8888 1x -> k_6_5_5", false, 0, 0, {0.0f, 1.0f, 0.0f, 1.0f}, 0, 0, 5, 1, 0, false, 0, {0xF800u, 0}, 0},
      {"32F 1x -> k_32_FLOAT", false, 0, 14, {3.0f, 0, 0, 0}, 0, 0, 36, 1, 0, false, 0, {0x40400000u, 0}, 0},
      {"16F 1x bias -1 -> k_16_16_16_16_FLOAT", false, 0, 7, {2.0f, 1.0f, 0.5f, 4.0f}, 0, 0, 32, 1, 0, false, -1,
       {0x38003C00u, 0x40003400u}, 0},
      {"D24FS8 2x raw -> k_8_8_8_8", true, 1, 1, {}, 0.5f, 0x5A, 6, 0, 0, false, 0, {0xE000005Au, 0}, 0.5f},
      // 0.5 + 7 ulp: ReXGlue truncates host D32 to 20e4 (round to nearest even would give E00001).
      {"D24FS8 1x truncate -> k_8_8_8_8", true, 0, 1, {}, std::bit_cast<float>(0x3F000007u), 0x5A, 6, 0, 0, false, 0,
       {0xE000005Au, 0}, 0.5f},
      {"D24S8 1x raw -> k_8_8_8_8", true, 0, 0, {}, 0.25f, 0x11, 6, 0, 0, false, 0, {0x40000011u, 0},
       float(0x400000) / 16777215.0f},
  };
  constexpr uint32_t kPitch = 64, kDestW = 64, kDestH = 32;
  const int32_t rx0 = 8, ry0 = 8, rx1 = 40, ry1 = 24, px = 16, py = 4;  // rect and dest point
  uint64_t fence_value = 0;
  bool all_ok = true;
  uint32_t index = 0;
  for (const Case& c : cases) {
    ResolveParams p;
    p.regs.surface_info = kPitch | (c.msaa << 16);
    const uint32_t base = 1024 + index * 32;  // EDRAM bases the game does not use at start
    if (c.depth) p.regs.depth_info = base | (c.format << 16);
    else p.regs.color_info[0] = base | (c.format << 16);
    p.source = c.depth ? 4 : 0;
    p.sample_select = c.sample_select;
    p.copy_command = c.copy_command;
    p.x0 = rx0; p.y0 = ry0; p.x1 = rx1; p.y1 = ry1;
    p.dest_x = px; p.dest_y = py;
    p.dest_base = 0xFFF00000u + index * 0x10000u;  // not a guest address: never found by textures
    p.dest_format = c.dest_format;
    p.dest_width = kDestW;
    p.dest_height = kDestH;
    p.dest_number = IsFloatFormat(c.dest_format) ? 7 : 0;
    p.dest_swap = c.swap;
    p.exp_bias = c.bias;
    ++index;

    alloc->Reset();
    list->Reset(alloc.Get(), nullptr);
    ClearParams cp;
    cp.flags = c.depth ? 0x30 : 0x1;
    std::memcpy(cp.color, c.color, sizeof(cp.color));
    cp.depth = c.z;
    cp.stencil = c.stencil;
    targets_->Clear(list.Get(), p.regs, 1, c.depth, kDestH * 2, cp);
    bool ok = Resolve(list.Get(), p);
    Image* img = ok ? images_[p.dest_base].get() : nullptr;
    const uint32_t bpp = 1u << DestBppLog2(c.dest_format);
    const uint32_t w = uint32_t(rx1 - rx0), h = uint32_t(ry1 - ry0);
    const uint32_t row_pitch = (kDestW * bpp + kPitchAlign - 1) & ~(kPitchAlign - 1);
    ComPtr<ID3D12Resource> readback[2];
    if (img) {
      D3D12_RESOURCE_DESC bd = {};
      bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      bd.Width = uint64_t(row_pitch) * kDestH;
      bd.Height = 1;
      bd.DepthOrArraySize = 1;
      bd.MipLevels = 1;
      bd.SampleDesc.Count = 1;
      bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      D3D12_HEAP_PROPERTIES hp = {};
      hp.Type = D3D12_HEAP_TYPE_READBACK;
      for (int k = 0; k < (img->depth_r32f ? 2 : 1); ++k) {
        if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                    nullptr, IID_PPV_ARGS(&readback[k])),
                   "selftest readback"))
          return false;
        D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
        src.pResource = k ? img->depth_r32f.Get() : img->texture.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = readback[k].Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint = {k ? DXGI_FORMAT_R32_FLOAT : img->format, kDestW, kDestH, 1, row_pitch};
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      }
    }
    list->Close();
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence.Get(), ++fence_value);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    fence->SetEventOnCompletion(fence_value, ev);
    WaitForSingleObject(ev, 5000);
    CloseHandle(ev);
    uint32_t bad = 0, first_bad[2] = {}, first_x = 0, first_y = 0;
    float r32f_seen = 0;
    if (img) {
      const uint8_t* data = nullptr;
      readback[0]->Map(0, nullptr, reinterpret_cast<void**>(const_cast<uint8_t**>(&data)));
      const uint8_t* fdata = nullptr;
      if (readback[1]) readback[1]->Map(0, nullptr, reinterpret_cast<void**>(const_cast<uint8_t**>(&fdata)));
      for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
          const uint8_t* t = data + size_t(py + y) * row_pitch + size_t(px + x) * bpp;
          uint32_t v[2] = {};
          std::memcpy(v, t, std::min(bpp, 8u));
          bool texel_ok = v[0] == c.expect[0] && (bpp < 8 || v[1] == c.expect[1]);
          if (fdata) {
            float f;
            std::memcpy(&f, fdata + size_t(py + y) * row_pitch + size_t(px + x) * 4, 4);
            r32f_seen = f;
            texel_ok = texel_ok && std::fabs(f - c.expect_r32f) <= 1e-6f;
          }
          if (!texel_ok && bad++ == 0) {
            first_bad[0] = v[0];
            first_bad[1] = v[1];
            first_x = x;
            first_y = y;
          }
        }
      }
      readback[0]->Unmap(0, nullptr);
      if (readback[1]) readback[1]->Unmap(0, nullptr);
      images_.erase(p.dest_base);
      if (changed_) changed_(p.dest_base);
    }
    const bool pass = img && bad == 0;
    all_ok = all_ok && pass;
    if (pass)
      REXLOG_INFO("PGR4 native resolve selftest: ok   {} ({:08X} {:08X})", c.name, c.expect[0], c.expect[1]);
    else
      REXLOG_ERROR("PGR4 native resolve selftest: FAIL {}: {} of {} texels wrong, first at ({}, {}) = {:08X} {:08X}, "
                   "expected {:08X} {:08X}, r32f {} (expected {}){}",
                   c.name, bad, w * h, first_x, first_y, first_bad[0], first_bad[1], c.expect[0], c.expect[1],
                   r32f_seen, c.expect_r32f, img ? "" : ", resolve failed");
  }
  return all_ok;
}

}  // namespace pgr4::native
