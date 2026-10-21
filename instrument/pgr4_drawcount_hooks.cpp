// pgr4-native DrawCounter: counts calls to PGR4's guest D3D packet builders.
//
// Built only into the instrumented MTR-PGR4 copy (build.ps1 adds it to the target). Each hook
// overrides the weak sub_X symbol from the generated code, bumps a counter and forwards to the
// original __imp__sub_X. The SDK side (rexglue-drawcount.patch, --gpu_stream_stats) finds
// rex_drawcount_api_counters in this executable and prints per-second deltas next to the PM4
// counts. Names starting with "draw" are summed into api_total.
//
// Addresses are for the base-disc default.xex (sha256 f9a411d8...f423e) as recompiled by MTR-PGR4
// 2db3edb. They were found by scanning the generated C++ for `lis rX,0xC00n` followed by
// `ori` with a PM4 type-3 low half (opcode << 8 | predicate bit); see the development notes,
// "Draw-path measurement", for the table and evidence.

#include <rex/hook.h>

#include <atomic>
#include <cstdint>

namespace {

enum Hook : uint32_t {
  kDrawIndxAuto_8269A4F8,     // 0xC0012201 x2: DRAW_INDX, 2 dwords (auto index), predicated
  kDrawIndxAuto_8269A9F0,     // 0xC0012201: DRAW_INDX, 2 dwords, predicated
  kDrawIndxDma_8269ADD8,      // 0xC0032201: DRAW_INDX, 4 dwords (index buffer), predicated
  kDrawIndx2_8269B230,        // 0xC0003601 | n<<16: DRAW_INDX_2, predicated
  kDrawIndx2_826A7318,        // 0xC0003601, initiator count<<16 | 0x81, split at 65534: predicated
  kDrawIndx2Np_826942B0,      // 0xC0003600 / 0x00010081 in a 24x loop: not predicated (aux)
  kDrawIndx2Np_82696978,      // 0xC0003600 | n<<16, initiator 0x00030088 (3-vertex rect): aux,
                              // likely the D3D clear/resolve rectangle
  kIndirectBuffer_82698618,   // 0xC0013F00: INDIRECT_BUFFER
  kBinAll_82690AF8,           // SET_BIN_MASK_LO/HI + SET_BIN_SELECT_LO/HI
  kBinSelectLo_826AAD48,      // SET_BIN_SELECT_LO
  kBinSelHiMask_826A10A8,     // SET_BIN_SELECT_HI + SET_BIN_MASK_LO/HI
  kBinMask_826A0D60,          // SET_BIN_MASK_LO/HI
  kCount
};

const char* const kNames[kCount] = {
    "draw_8269A4F8",  "draw_8269A9F0",  "draw_8269ADD8", "draw_8269B230",
    "draw_826A7318",  "aux_826942B0",   "aux_82696978", "ib_82698618",
    "bin_82690AF8",   "binsel_826AAD48", "binsel_826A10A8", "binmask_826A0D60",
};

std::atomic<uint64_t> g_counts[kCount];
uint64_t g_snapshot[kCount];

}  // namespace

extern "C" __declspec(dllexport) uint32_t rex_drawcount_api_counters(
    const uint64_t** values, const char* const** names) {
  for (uint32_t i = 0; i < kCount; ++i) g_snapshot[i] = g_counts[i].load(std::memory_order_relaxed);
  *values = g_snapshot;
  *names = kNames;
  return kCount;
}

#define DRAWCOUNT_HOOK(addr, index)                               \
  REX_EXTERN(__imp__sub_##addr);                                  \
  REX_HOOK_RAW(sub_##addr) {                                      \
    g_counts[index].fetch_add(1, std::memory_order_relaxed);      \
    __imp__sub_##addr(ctx, base);                                 \
  }

DRAWCOUNT_HOOK(8269A4F8, kDrawIndxAuto_8269A4F8)
DRAWCOUNT_HOOK(8269A9F0, kDrawIndxAuto_8269A9F0)
DRAWCOUNT_HOOK(8269ADD8, kDrawIndxDma_8269ADD8)
DRAWCOUNT_HOOK(8269B230, kDrawIndx2_8269B230)
DRAWCOUNT_HOOK(826A7318, kDrawIndx2_826A7318)
DRAWCOUNT_HOOK(826942B0, kDrawIndx2Np_826942B0)
DRAWCOUNT_HOOK(82696978, kDrawIndx2Np_82696978)
DRAWCOUNT_HOOK(82698618, kIndirectBuffer_82698618)
DRAWCOUNT_HOOK(82690AF8, kBinAll_82690AF8)
DRAWCOUNT_HOOK(826AAD48, kBinSelectLo_826AAD48)
DRAWCOUNT_HOOK(826A10A8, kBinSelHiMask_826A10A8)
DRAWCOUNT_HOOK(826A0D60, kBinMask_826A0D60)
