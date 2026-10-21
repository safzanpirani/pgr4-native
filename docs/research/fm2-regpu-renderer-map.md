# FM2-ReGPU renderer map (raw notes)

Raw notes behind `docs/native-renderer-design.md`, written by a read-only research agent on
2026-10-21 from `zer0ducksgiven/Forza-Motorsport-2-ReGPU` at `128ddea` ("0.1.1: hotfix
version"). Paths are relative to that repo. Nothing was built or run, so every behaviour claim
comes from reading the source; unconfirmed items are marked `[INFERENCE]`. Spot checks of
`renderer.cpp:64`, `:974-982`, `:1840-1841`, `:5203-5210`, `:5356-5371`, `:5826-5866`,
`:6418-6423` and `gpu.cpp` were repeated by hand and match.

**Headline findings for the design doc:**
- This is not EDRAM-free. It keeps one host colour surface and one host depth surface per EDRAM base. They are keyed by `RB_COLOR_INFO&0xFFF` and `RB_DEPTH_INFO&0xFFF` (`renderer.cpp:1046-1097`, `:1120-1140`), and resolves become host textures keyed by guest physical address (`:3507-3572`, `:4151-4281`). It does not emulate EDRAM memory.
- There is **no title-specific code in the renderer's core translation path**. The renderer has zero hard-coded `0x82…` addresses. All guest addresses live in `src/hooks/*`, `config/*.toml` and `static_shaders.cpp:267`.
- There is **one render target**. The pixel shader returns only `oC[0]` (`hlsl.cpp:900-917`), the PSO is `NumRenderTargets = 1` (`renderer.cpp:2468`) and the colour format is fixed at RGBA16F (`:173`). MRT is not implemented.
- The renderer assumes a 1280×720 main view in many places (`renderer.cpp:64`).

---

## 1. Module map

### Top-level orchestration

| File | Responsibility |
|---|---|
| `src/main.cpp:16-73` | `MemoryInit`, `LoadTitle`, `RuntimeInit`, `ThreadSystemInit`, then creates guest "main" thread (`:48`). |
| `src/kernel/runtime.cpp:67-110` | `RuntimeInit` creates the user dir, runs the launcher, then `PresenterInit()` (`:107`), `GpuInit()` (`:108`), `AudioInit()` (`:109`). |
| `src/memory.cpp:113-143` | `MemoryInit`: 4 GB placeholder, 512 MB physical section mapped at three views. |

### `src/gpu/`

| File | Responsibility |
|---|---|
| `gpu.cpp` (1428 lines) | PM4 command processor (CP), `Vd*` exports, vblank/interrupt dispatch, segment/draw-worker plumbing, lookahead. Map in §2. |
| `frame.cpp:182-285` | Glue between CP and renderer. `FrameOnShaderLoad` (`:182`) dedups shader uploads (two-generation cache `:52-82`, direct-mapped by address `:56-61`), hashes used dwords with FNV and calls `RendererSetShader`. `FrameOnDraw` (`:246`) calls `RendererOnDraw` unless `FM2_TEXVIEW`. `FrameOnSwap` (`:263`) calls `RendererOnSwap`. The rest is the texture-viewer compositor (`:110-177`). |
| `regs.h:10-52` | Register-file accessors; `GpuPhysToGuest` (`:36`); `FrameOn*` and `GpuLookahead` declarations. |
| `renderer.h:1-46` | The renderer's whole public surface (the porting seam). |
| `renderer.cpp` (6747) | See the section map below. |
| `hlsl.h` / `hlsl.cpp` (922) | Xenos ucode → HLSL SM5.1. Preamble `:11-261`, `Emitter` `:263-723`, `HlslPreamble` `:725`, `TranslateVertexFetchLayout` `:730`, `TranslateShader` `:752-922`. |
| `ucode.h` / `ucode.cpp` (327) | `Parse` `:123`, `UsedDwords` `:104`, `DecodeAlu` `:160`, `DecodeFetch` `:206`, `Disassemble` `:252`. |
| `texture.h` / `texture.cpp` (477) | Formats `:13-50`; Xenos 2D tiling `:52-66`; endian swap `:68`; DXT decode `:95-229`; `ReadTextureFetch` `:231`; `DecodeTexture` `:268`/`:296`; `TextureTiledOffset` `:362`; `TextureBCKind` `:386`; `CopyTextureBlocks` `:398`; `TextureMipLevel` `:429`; `TextureFaceStride` `:463`. |
| `recorded_list.h` (348) | `RecordedList`: a command-list recorder replayed on a worker thread (see §8). |
| `shader_scan.cpp` (128) | Runtime scan of the 512 MB physical view for embedded `IM_LOAD_IMMEDIATE` packets (`0xC0nn2B00`), `:44-76`. |
| `static_shaders.cpp` (273) | Static scan of game data and XEX image for shader microcode (§5). |
| `boot_screen.cpp` (216) | First-boot "compiling shaders" screen, composed from FM2's `.xds` art (`:23-24`, `:198-200`). |
| `dlss.cpp` (115), `fsr3.cpp` (84), `fsr1.hlsl` (126) | Upscalers. DLSS through NGX, FSR3 through the FidelityFX API, FSR1 as embedded fxc-compiled compute shaders. |
| `shader_scan.h`, `static_shaders.h`, `boot_screen.h`, `texture.h` | Headers. |

### `src/render/presenter.cpp` (429 lines)

| Lines | Content |
|---|---|
| `:62-87` | `Presenter` struct. |
| `:91-94` | `RenderSize()`. |
| `:99-107` | `WindowProc`; `WM_CLOSE` calls `std::_Exit(0)`. |
| `:120-135` | `EnsureDevice()`: `D3D12CreateDevice(nullptr, FL12_0)` plus one DIRECT queue. |
| `:137-241` | `CreateWindowAndDevice()`: window, DXGI flip-model swapchain, RTV heap, upload buffers. |
| `:243-309` | `DrawFrame()`: fallback path for CPU images; unused once the renderer is active. |
| `:311-336` | `RenderThread()`: message pump. |
| `:339-344` | `PresenterInit()` (thread start). |
| `:372-387` | `RenderDevice()` / `RenderQueue()` / `PresenterUseRenderer()`. |
| `:389-443` | `PresenterRecordCopy()`. |
| `:445-455` | `PresenterPresent()`. |
| `:457-464` | `DisplayRefreshHz()`. |

`PresenterSubmitFrame` (`:346`) has **no callers** in the repo. I grepped `PresenterSubmitFrame` and `PresenterInit`.

### `renderer.cpp` section map

| Lines | Section |
|---|---|
| 62-211 | Constants and scale math. Hard `kWidth=1280,kHeight=720` `:64`; `OutputWidth/Height` `:73-82`; upscaler state `:83-120`; tall/big pass `:122-142`; `ScaleX/Y`, `HostX/Y`, `ToHost` `:143-152`; Hor+ `ViewExtent` `:158-166`; `kFrames=3`, `kRingSize=64 MB`, `kDescriptorsPerFrame=8192`, `kBindless=65536` `:167-171`; `kColorFormat=RGBA16F` `:173`; `kMaxSurfaces=64` `:174`; register indices `:178-192`. |
| 213-446 | Core structs: `CompiledShader` `:213`, `CachedTexture` `:227`, `ResolvedTexture` `:240`, `FrameResources` `:265`, `PipelineKey` `:283`, `PipelineEntry` `:310`, `Renderer` `:316-378`, `DrawContext` `:391-406`. |
| 447-492 | Perf counters, `CountCreate`. |
| 493-587 | `TexturePool`: 128 MB placed-resource heaps, `CreateTexture`, `RetireTexture`, `ReclaimTextureBlocks`. |
| 589-671 | Per-frame binding memo (`SlotBinding`, `FrameMap`), `DrawState` cache `:657-671`. |
| 673-741 | Hash helpers, descriptor helpers, `CreateBuffer`. |
| 743-867 | `InitDepthBlit`: MSAA selection `:751-762`; blit shaders via `D3DCompile` `:803-856`; blit PSOs `:820-865`. |
| 869-1034 | **Device/queue/root signature/heap setup:** `Init()`. |
| 1036-1140 | **EDRAM → host surfaces:** `GetSurface` `:1046`, `PassMark` `:1103`, `SelectSurfaces` `:1120`. |
| 1142-1214 | Bindless descriptor allocator. |
| 1216-1360 | `BeginFrameIfNeeded` (fence wait, allocator reset, upscaler phase, per-frame SRVs). |
| 1362-1417 | Upload ring: `RingAlloc` `:1363`, `UploadGuest` `:1388`. |
| 1419-1578 | **Index buffers:** `IndexArena` `:1423`, `FlushIndexCopies` `:1449`, `GetIndexBuffer` `:1471-1577`. |
| 1580-1723 | `CompileStats`, `DiskWriter` `:1613`, `Manifest` `:1658-1723`. |
| 1725-1857 | **Shader compile:** `ShaderCompiler` thread pool; `D3DCompile` at `:1840`. |
| 1859-2055 | Canonical-key sharing of vertex-fetch-patched copies `:1859-1887`; `GetShader` `:1892-2055`; debug env hooks `:1987-2031`. |
| 2057-2185 | Motion-vector shader variants for temporal upscalers. |
| 2187-2233 | `Blend`/`AlphaBlend`/`BlendOp` Xenos→D3D12 maps. |
| 2235-2368 | **Pipeline library:** `PipelineLibrary` `:2237-2361`, `InitPipelineLibrary` `:2363`. |
| 2370-2525 | **PSO cache:** `PipelinePool` `:2370`, `EnsurePipeline` `:2407-2512`, `WaitForPipeline` `:2518`. |
| 2527-2599 | `MakePipelineKey` `:2527`, `GameSettled` `:2557`, `SkippableDraw` `:2563`, `GetPipeline` `:2572`. |
| 2601-2838 | Cube textures `:2604`, resolved-mip-chain `:2758`. |
| 2840-3138 | **Texture upload/untiling:** `ResolvedFor` `:2843`; `GetTexture` `:2858`; `GetTextureLocked` `:2884-3138` (BC native `:2982-3047`; CPU decode to RGBA8 `:3049-3137`). |
| 3140-3204 | F12 capture. |
| 3206-3371 | **Index-list expansion:** `ReadIndex` `:3206`, `BuildIndexList` `:3225-3327`, `CachedIndexList` `:3344`. |
| 3373-3421 | **Tiling:** `TileState` `:3374`, `WindowScissor` `:3384`. |
| 3423-3572 | `TransitionColor`, `EnsureResolved` `:3441`, `BlitResolve` `:3507`. |
| 3576-4146 | **Upscaling:** temporal `:3579-3820`, `InitUpscale` `:3822`, `UpscaleResolved` `:4024`, `UpscaleScene` `:4137`. |
| 3880-4022 | **Gamma:** `GradeFrontBuffer` `:3890`, `EnhanceFrontBuffer` `:3957`. |
| 4147-4281 | **Resolve:** `Resolve()`; rect from fetch const 0 `:4162-4181`; clear `:4254-4280`. |
| 4284-4396 | **Lookahead consumer:** `LookItem`, `AdvanceLookahead` `:4304`, `DrainScan` `:4373`. |
| 4397-4505 | Hold: `WaitHoldingImpl` `:4416`. |
| 4506-4549 | `WarmupStep`. |
| 4551-4760 | Overlay. |
| 4755-4893 | `HoldPresent`, `BootPresent`. |
| 4895-4960 | Public lookahead/predict API, `RendererSetShader`/`RendererOnShaderLoad`. |
| 4990-5330 | **Parallel draw pieces:** `AcquireList` `:4995`, `SplitMainList` `:5025`, `WaitForQueueIdle` `:5038`, `WriteResolveToGuest` `:5049`, `Workers` `:5116`, `TranslatePiece` `:5130`, `WorkerMain` `:5155`, `RendererDrawWorkers` `:5199`, `RendererParallelReady` `:5213`, `RendererIsResolveDraw` `:5218`, `RendererTrackTile`/`SetTile` `:5223`/`:5242`, `RendererRunPieces` `:5248-5314`. |
| 5316-5331 | `RendererOnDraw`. |
| 5333-6031 | **Draw translation:** `DrawImpl` (mode switch `:5358-5370`, shader get `:5398-5419`, index decision `:5430-5446`, constants `:5536-5604`, vertex fetch table `:5607-5630`, wide/HUD factors `:5631-5644`, viewport transform consts `:5649-5674`, indexed path `:5675-5699`, textures `:5720-5818`, viewport/scissor/tile `:5823-5876`, state/root binds `:5877-5929`, draw `:6003-6028`). |
| 6035-6129 | **Benchmark:** `BenchmarkRecord`/`WriteReport`. |
| 6131-6214 | `RendererOnSwap` wrapper (perf and bench accounting). |
| 6216-6395 | `PrecompileAll` (first-boot compile of stored shaders). |
| 6397-6725 | **Swap/present:** `RendererOnSwapImpl`. |
| 6727-6747 | `RendererTracing`, `GameAspect`, `RendererSetGammaEntry`. |

---

## 2. PM4 command processor (`src/gpu/gpu.cpp`)

**Ring source.** `VdInitializeRingBuffer` (`:1278-1285`) sets `g_gpu.ringBase = PhysToGuest(r3)`, `ringDwords = (1<<(r4+3))/4` and `rptr = 0`. The write pointer is **not an export**: the guest's D3D stores to the memory-mapped GPU register file at guest `0x7FC80000 + 0x1C5*4` (`CP_RB_WPTR`, `:29`). The CP polls it as `Reg(CP_RB_WPTR) % size` (`:1137`). Read pointer: `SetReg(CP_RB_RPTR)` after every packet (`:1173`) and, if `VdEnableRingBufferRPtrWriteBack` was called (`:1287-1292`), `StoreBE32(rptrWriteback, rptr)` (`:1174-1175`). When empty it does `seg::Flush(); Sleep(0)` in a spin loop (`:1138-1143`). `GpuInit` seeds `RBBM_STATUS`, `COHER_STATUS_HOST` and display regs `0x1951/0x194C/0x1961` (720p) at `:1221-1228`.

**Which thread.** `GpuStartInterruptThread` (`:1232-1238`), triggered by `VdSetGraphicsInterruptCallback` (`:1270-1276`), calls `CreateHostGuestThread("gpu command processor", CommandProcessorLoop)` (`:1237`) and one for "gpu vblank irq" (`:1236`). `CreateHostGuestThread` is at `src/kernel/thread.cpp:184-187` (a host thread with a PPCContext and PCR, 16 MB host stack `:32`/`:165`). Wrapped packets run from a contiguous scratch copy (`:1147-1168`). `t_cpContext` (`:445`) holds the CP's PPCContext.

**Packet types** (`ExecutePacket`, `:624-929`):
- **Type 0** `:633-644`: register writes via `WriteRegister` / `WriteRegisters`.
- **Type 1** `:645-648`: two register writes.
- **Type 2** `:649-679`: used by VdSwap as the "swap block". It matches the pending swap list and calls `FrameOnSwap`.
- **Type 3** `:684+`, opcodes by line:

| Opcode | Name | Lines | Behaviour |
|---|---|---|---|
| `0x3F`, `0x37` | INDIRECT_BUFFER (+_PFD) | 706-709 | `ExecuteBuffer(PhysToGuest(arg0&~3), arg1&0xFFFFF)` |
| `0x3D` | MEM_WRITE | 710-713 | `WriteBack` (fence) |
| `0x58` | EVENT_WRITE_SHD | 714-721 | value, or swap counter if bit 31 |
| `0x59` | EVENT_WRITE_CFL | 722-723 | no-op |
| `0x5A` | EVENT_WRITE_EXT | 724-733 | writes full-screen extents `{0,1024,0,1024,0,1}` |
| `0x5B` | EVENT_WRITE_ZPD | 734-756 | fake occlusion counts: 1000 samples at end, `RB_SAMPLE_COUNT_ADDR=0x2325` |
| `0x45` | COND_WRITE | 757-773 | |
| `0x3C` | WAIT_REG_MEM | 774-835 | spin, then yield, then `Sleep(1)`; 2 s timeout then continue `:797-801` |
| `0x21` | REG_RMW | 836-841 | |
| `0x54` | INTERRUPT | 842-856 | synchronous `DispatchInterrupt` on the CP thread per CPU bit |
| `0x60` / `0x61` | SET_BIN_MASK_LO/HI | 857-862 | |
| `0x62` / `0x63` | SET_BIN_SELECT_LO/HI | 863-868 | |
| `0x50` / `0x51` | SET_BIN_MASK/SELECT (64-bit) | 869-874 | |
| `0x2D`, `0x55` | SET_CONSTANT, SET_CONSTANT2 | 875-889 | bases `{AluConstants 0x4000, Fetch 0x4800, Bool 0x4900, Loop 0x4908, 0x2000}` (`regs.h:13-16`) |
| `0x2F` | LOAD_ALU_CONSTANT | 890-899 | |
| `0x27` | IM_LOAD | 900-906 | `SubmitShaderLoad` |
| `0x2B` | IM_LOAD_IMMEDIATE | 907-912 | `SubmitShaderLoad` |
| `0x22` | DRAW_INDX | 913-916 | `SubmitDraw(arg1, arg2, arg3)` |
| `0x36` | DRAW_INDX_2 | 917-920 | `SubmitDraw(arg0, 0, 0)` |
| `0x46`, `0x48`, `0x10`, default | EVENT_WRITE, ME_INIT, NOP, unknown | 921-926 | ignored |

Not handled (fall to default, so silently dropped): everything else, e.g. `SET_SHADER_CONSTANTS`, `VIZ_QUERY`, `CONTEXT_UPDATE`, `INVALIDATE_STATE`.

**Indirect buffers / prerecorded command buffers.** `ExecuteBuffer` (`:931-940`) recurses on `INDIRECT_BUFFER`, pushing `BufferCursor{next,end}` onto `thread_local t_cursors` (`:538-542`) so the lookahead can resume from the middle of the current buffer. Indirect-buffer packets that carry the predicate bit are skipped like any other (`:696-700`). Nothing in the buffer contents is cached or analysed.

**Predication / tiling.** State is `g_gpu.binMask`/`binSelect`, default all-ones (`:77-78`). Any type-3 packet with header bit 0 set and `!(binSelect & binMask)` is skipped (`:696-700`, counted `skippedPredicated`). The renderer then handles the residual tile logic itself. `WindowScissor` (`renderer.cpp:3384-3421`) maps the window offset and scissor to screen space. `DrawImpl` records `g_tile.originY`/`rect` for non-full scissors and shifts or clips full-scissor "rect" draws (depth resets) into the current tile (`:5829-5865`). `Resolve` clamps the resolve rect to the tile (`:4182-4188`) and extends full-width clears down to pass height so later tiles see a cleared surface (`:4251-4258`). **Every tile replay's draws still run**, clipped to that tile's rect in screen space on one screen-sized surface `[INFERENCE from :5829-5865; I did not find a "first-tile only" skip in code, though FM2-native-scope.md:75-80 describes it as a plan]`.

**Draw dispatch to the renderer:**
- Serial path (default): `SubmitDraw` (`:359-390`) → `FrameOnDraw` (`frame.cpp:246`) → `RendererOnDraw` (`renderer.cpp:5316`) → `DrawImpl`.
- Shaders: `SubmitShaderLoad` (`:330-357`) → `FrameOnShaderLoad` → `RendererSetShader` (`renderer.cpp:4948`). Max 0x10000 dwords (`:334`).
- Resolve is detected as a draw: `DrawImpl` reads `RB_MODECONTROL&7`; `6` is `Resolve()`, `4`/`5` are colour+depth or depth-only draws, anything else is skipped (`renderer.cpp:5358-5370`).

**Register storage.** The registers are the guest memory page at `0x7FC80000` (big-endian, `regs.h:12`, accessors `:21-32`). `WriteRegister` (`:568-603`) stores the register, intercepts `DC_LUT_RW_INDEX=0x1922` / `DC_LUT_30_COLOR=0x1925` and forwards entries to `RendererSetGammaEntry` (`:565-573`), and mirrors `SCRATCH_REG0..7` (0x578) to memory when `SCRATCH_UMSK` bit set (`:557-558`, `:587-602`). `WriteRegisters` (`:607-621`) bulk-copies. The renderer reads through `gpureg::Get`, which uses a per-thread `t_file` override (`regs.h:20-21`), so draw workers see a private register file.

**Lookahead (`GpuLookahead`).** Defined `:1060-1095`. Called from the renderer's hold-wait loop (`renderer.cpp:4447`) on the CP thread. It copies regs `0x2000..0x4FFF` (`:1065-1067`), then dry-runs the rest of every buffer on `t_cursors` (`:1072-1077`) and the ring from `g_ringNext` to wptr into a 4 MB scratch (`:1080-1091`). `LookaheadBuffer` (`:954-1057`) shadow-writes registers/constants (`ShadowWrite` `:948-952`), follows INDIRECT_BUFFER to depth 4 (`:991-995`), tracks bin mask/select (`:1047-1052`), and reports shaders and draws to `RendererPredictShader`/`RendererPredictDraw` (`:1026-1046`). Those build `LookItem`s that pre-compile pipelines (`renderer.cpp:4286-4396`, `:4900-4933`). Registers and bin state are restored after (`:1092-1094`). Off with `FM2_NO_LOOKAHEAD` or when `work::Active()` (`:1062-1064`).

**Pieces (`RendererRunPieces`) / segments.** **Off by default**: `RendererDrawWorkers()` returns 0 unless `FM2_DRAW_WORKERS` is set (`renderer.cpp:5206-5208`, comment "measured slower"). When enabled, `seg::*` (`gpu.cpp:183-328`) logs registers, shaders and draws between resolves, snapshotting full state every `kSnapEvery=128` draws (`:186`, `:206-220`). `seg::Flush` (`:266-321`) cuts at snapshot boundaries into `min(snapshots, workers, draws/96)` pieces (`:273`), each `Replay`ed (`:231-263`) on a worker via `RendererRunPieces` (`renderer.cpp:5248-5314`). Each piece records its own `ID3D12GraphicsCommandList` (`:4995`, `:5130-5149`) with `RecordedList` direct mode, texture uploads go to a shared "prep" list executed first (`:5273-5276`), and all lists are executed in order with `ExecuteCommandLists` (`:5313`). Flush points: resolves, fences/WriteBack (`gpu.cpp:500-502`), swaps (`:663`), waits (`:759`, `:778`), interrupts (`:843`), scratch-register writes (`:587`). A second opt-in mode, `FM2_DRAW_THREAD`, runs the draws on one worker via a 64 MB ordered log (`work::`, `:105-172`, `WorkerLoop` `:392`, started `:1102-1110`).

**Interrupts / fences / EVENT_WRITE.**
- Fences are plain CP memory writes (`WriteBack` `:500`) with the endian swap in the address low bits (`GpuSwap` `:483-491`). The **guest is never gated on host-GPU completion**.
- Vblank: `VblankLoop` (`:1198-1216`) runs at `2×frame cap` (120-720 Hz, or `FM2_VBLANK_HZ`) and pushes `RaiseInterrupt(0, 2)` (source 0, CPU 2). A separate guest-context thread `InterruptLoop` (`:466-479`) drains it.
- `DispatchInterrupt` (`:447-463`) sets the PCR `CurrentCpu` byte, takes `InterruptLockAcquire(cpu)`, clears MSR EE (`0x8000`), sets `r3=source`, `r4=interruptData` and calls `GuestCall(ctx, callback)`. Callback/data come from `VdSetGraphicsInterruptCallback`.
- Command-stream `INTERRUPT` (source 1) runs synchronously on the CP thread (`:842-856`).

**VdSwap** (`:1395-1428`):
1. Overwrites the guest's 64-dword swap block with type-2 NOPs (`:1401-1402`).
2. Computes `frontBuffer = LoadBE32(r4+4) & 0x1FFFF000`: the physical address from the front-buffer texture fetch constant (`:1407`).
3. Registers the block in `swapBlocks[16]` and the `pendingSwaps` deque (`:1408-1411`).
4. Frame limiter: sleeps until `next` using `FrameCap()` (`:1415-1426`, `FrameCap` `:1185-1196`).

When the CP later executes a type-2 NOP at that physical block (`:649-679`), it calls `FrameOnSwap(frame, frontBuffer)`.

---

## 3. FM2-specific items

### 3a. Hard-coded guest addresses

**Only in `src/hooks/`, `config/`, `main.cpp` and `static_shaders.cpp`. None in `src/gpu/renderer.cpp`, `hlsl.cpp`, `texture.cpp`, `presenter.cpp` or `src/kernel/`.**

| Address | What | Where |
|---|---|---|
| `sub_8257E0D0` | Camera aspect setter (`stfs f1,268(r3)`), patched to widen aspect on ultrawide (load-bearing) | `hooks/camera_aspect.cpp:1-5`, `:15-16`, `:29` |
| `0x820009CC` | Global D3D device pointer (diagnostic) | `hooks/d3d_debug.cpp:46`, `:61` |
| `sub_82371D60` | D3D wait-until-GPU-retires | `d3d_debug.cpp:13`, `:21`, `:27` |
| `sub_823724C8` | D3D submit indirect buffers (`{address,size}` pairs) | `d3d_debug.cpp:64`, `:73`, `:85` |
| `sub_8236CB28` | D3D swap (calls VdSwap) | `d3d_debug.cpp:123`, `:129`, `:136` |
| `sub_825ADE70` | Game present | `d3d_debug.cpp:124`, `:139`, `:145` |
| `sub_8237D158`, `sub_8236D830` | "The only two functions that build draw packets" | `d3d_debug.cpp:149-163` |
| `sub_82369340`, `sub_82373E38` | GPU wait loop and timeout path | `hooks/perf_probe.cpp:2`, `:17-19`, `:35-38` |
| `sub_82953E38` | Timer callback list walk; callback range `0x82000000-0x83000000` | `perf_probe.cpp:47-52`, `:62` |
| `sub_82957260` | Video plane writer (Bink); width 376 | `perf_probe.cpp:91-94`, `:229` |
| `sub_827C3BF0`, `sub_827C3CB8` | Title `malloc`/`free` | `perf_probe.cpp:254-255`, `:258`, `:268` |
| `sub_82754CD0/_D60/_DB0/_E90/_EF0`, `sub_82756580` | XAPI fiber functions replaced with host fibers (load-bearing for boot) | `hooks/fibers.cpp:93`, `:113`, `:128`, `:145`, `:162`, `:182`; fiber obj `0xA50` `:18`; KTHREAD+0x164 `:5-6` |
| `sub_824B91F0` | `luaD_throw` logging | `hooks/lua.cpp:11-27` |
| `0x82000000..0x83000000` | Loaded XEX image range | `main.cpp:24-28`; `static_shaders.cpp:267` |
| D3D device `+0x2A10`, `+0x2A3D/E`, `+16172/16192/16196/16200/16324/16328` | FM2's D3D device struct layout (diagnostic only) | `gpu.cpp:1254-1257`; `d3d_debug.cpp:37-41`, `:60` |

`config/fm2.toml:21-28` (save/restore helper addresses `0x824131C0` etc.), `:32-33` (setjmp `0x82418560`, longjmp `0x82418210`), `:37-58` (extra functions), invalid-instruction markers `0x827575F0`, `0x8294ECB8`, `0x8275DE80`. Plus 7124-line switch-table files and `config/fm2_tu4.toml`.

Title-update build: `CMakeLists.txt:13-75` runs `tools/xexremap.py` to translate guest addresses in any `src/*.cpp` that contains them. `CMakeLists.txt:18-31` hard-codes `out/ppc` / `out/ppc_tu4`.

### 3b. FM2 magic constants / heuristics in the renderer and CP

- **1280×720 baked in**
  - `renderer.cpp:64` (`kWidth/kHeight`; 29 uses of `kWidth`/`kHeight` in the file).
  - `presenter.cpp:60`, `frame.cpp:119`, `boot_screen.cpp:22`, `renderer.cpp:4812`.
  - Display regs and video-mode exports: `gpu.cpp:1225-1228`, `:1307-1322`, `:1337`, `:1356-1363`.
  - `settings.cpp:86-87` render-scale (`1280*s × 720*s`).
- **"Wide pass" = surface pitch exactly 1280**: `WidePass()` `renderer.cpp:145` and `:5860-5865`. It decides Hor+ widening, motion vectors (`:5495`) and scissor remap.
- **Swap only presents if the front buffer was a 1280×720 RGBA8 resolve**: `renderer.cpp:6420-6422`. Otherwise nothing is shown (`:6436-6437`).
- **Paint editor / livery hacks**
  - Big pass (2048² composite drawn in 256-row tiles; `pitch>1280` → `min(2048, pitch*64/65)`): `:127-135`, `:4205`.
  - Tall pass (1024² livery target, scissor `y1>720`): `:122-126`, `:3399-3417`, `:4206-4210` (`kMaxResolveHeight=2048`). Related disables: `FM2_NO_BIG_PASS`, `FM2_NO_TALL_PASS`.
- **Garage-thumbnail readback**: default `FM2_RESOLVE_WRITEBACK="256x128"`; `renderer.cpp:4230-4249`, `WriteResolveToGuest` `:5049-5114` (GPU stall plus CPU tiling to guest memory).
- **MSAA shadow-map clear heuristic**: prim 8 rect draws, `RB_COLOR_MASK&0xF==0`, pitch `<1000` get `vscale.w/voffset.w` scaling: `:5655-5669`.
- **Depth-only alpha test** kept because "the showroom's car shadow map has cut-out geometry": `:5711-5715`.
- **Vertex fetch constant ≥96 skips the draw** (black shapes on Mugello): `:5420-5429`.
- **Gamma**: FM2's display LUT "about x^1.15 darkens"; presenter applies the table: `:3880-3884`, `:3890-3953`; table fed by `gpu.cpp:565-573`.
- **HUD/ultrawide**
  - `vwide` factors `renderer.cpp:5631-5644`; shader logic `hlsl.cpp:864-895`.
  - Flat geometry (`|w-1|<1e-4`) is treated as HUD, and `vwide.w==0` for draws sampling a resolved ≥1280-wide render target (`renderer.cpp:2895-2896`, `:5821`).
  - Game camera hook sets `g_cameraAspectWidened` (`camera_aspect.cpp:26`; read `renderer.cpp:5638`).
- **Frame pacing assumption**: "title presents on vblanks: every one in races, every second one in menus": `gpu.cpp:1204-1214`; limiter in VdSwap `:1415-1426`.
- **`cullable = prim != 8 && prim != 1`** (rect list / point list): `renderer.cpp:5494`, `:4923-4924`. These are Xenos prim types, not FM2-specific.
- **Depth is `D24FS8` reverse-Z float** (the 20e4 float clear value): `:1068`, `:4271-4277`. Xenos-generic but FM2 sets it.
- **Resolve format mapping**: dest format `6` (8888) → RGBA8, else RGBA16F; depth → R32F: `:4215-4217`.
- **Texture formats supported**: only `k_8=2, k_1_5_5_5=3, k_5_6_5=4, k_8_8_8_8=6, k_8_8=10, k_4_4_4_4=15, DXT1=18, DXT2/3=19, DXT4/5=20, DXT5A=59` (`texture.cpp:13-50`). No float, 16-bit or 10:10:10:2 formats. Only 2D and cube textures (`renderer.cpp:2903`).
- **Boot/hold UI assets** from FM2 files: `boot_screen.cpp:23-24`, `:198-200`. Overlay text "FM2 preparing shaders" `renderer.cpp:4573`. Window title "Forza Motorsport 2" `presenter.cpp:160`, `:183`. Benchmark text "Sunset race, 8 cars" `renderer.cpp:6097`.
- **Shader dedupe**: "the title patches vertex-fetch layouts into its copies at load" → canonical-key sharing `renderer.cpp:1859-1887`, `:1939-1963`; `static_shaders.cpp:1-10`. This is a D3D9/XDK-wide behaviour, but the canonical masking constants at `:1879-1881` are specific to Xenos fetch encodings.
- **No hard-coded shader hashes** except debug env hooks (`FM2_DEBUG_VS`, `FM2_TINT_PS`, `FM2_DEBUG_PS`, `FM2_SKIP_VS`).
- **Data formats**: `.rmb`/`.rmb.bin` mesh files (zip members), 16-byte-aligned shader blobs: `static_shaders.cpp:3-4`, `:159`, `:257`.
- 128 distinct `FM2_*` env vars across `src/` (`gpu/renderer.cpp` has 124 lines mentioning one).

---

## 4. Own runtime vs generic Xenos

### Guest memory and address translation
- `g_base` (`memory.h:81`): one 4 GB `VirtualAlloc2` placeholder; guest address `A` is `g_base + A`. `GuestPtr<T>(A)` is `memory.h:83-87`.
- Low 0xA0000000 is committed; `0xA0000000/C0000000/E0000000` are three views of one 512 MB section from `CreateFileMapping2`/`MapViewOfFile3` (`memory.cpp:123-136`).
- GPU registers at `0x7FC80000` are plain committed memory, not MMIO: no trap, no handler; the CP polls.
- **GPU physical → guest pointer:** `GpuPhysToGuest(p) = 0xA0000000 + (p & 0x1FFFFFFF)` (`regs.h:36`) and an identical `PhysToGuest` (`gpu.cpp:38`). `MmGetPhysicalAddress(a) = a & 0x1FFFFFFF` (`imports_mm.cpp:92-95`).
- `PhysicalSection()` (`memory.cpp:211`) hands the file-mapping handle to the renderer.
- **The renderer maps guest physical memory directly into D3D12**: `ID3D12Device13::OpenExistingHeapFromFileMapping(PhysicalSection())` (`renderer.cpp:974-977`), fallback `OpenExistingHeapFromAddress1(GuestPtr(0xA0000000), 0x20000000)` (`:981`), then a placed 512 MB raw buffer `ALLOW_CROSS_ADAPTER` (`:987-1000`). Vertex and index fetches read it in place (`gphys` `t1`, `hlsl.cpp:41`). A Vulkan port would need `VK_EXT_external_memory_host` or a copy path.

### Threads

| Thread | Created at |
|---|---|
| CP, interrupt | `gpu.cpp:1236-1237` |
| Vblank | `:1229` |
| Presenter/window | `presenter.cpp:343` |
| Optional draw worker | `gpu.cpp:1105` |
| Optional piece workers (16 MB stacks) | `renderer.cpp:5284-5286` |
| Recorded-list replay | `recorded_list.h:28` |
| Shader compile workers | `renderer.cpp:1749-1758` |
| Pipeline build pool | `:2376-2381` |
| Disk writer | `:1624-1642` |
| Shader scanner | `shader_scan.cpp:103-110` |

### Guest signalling
See §2: memory fences by CP writes, interrupts via `GuestCall` into the callback registered by `VdSetGraphicsInterruptCallback`.

### Vd* exports (all in `gpu.cpp`)

| Export | Lines |
|---|---|
| `VdInitializeEngines` | 1263-1266 |
| `VdShutdownEngines` | 1268 |
| `VdSetGraphicsInterruptCallback` | 1270-1276 |
| `VdInitializeRingBuffer` | 1278-1285 |
| `VdEnableRingBufferRPtrWriteBack` | 1287-1292 |
| `VdSetSystemCommandBufferGpuIdentifierAddress` | 1294 |
| `VdGetSystemCommandBuffer` | 1296-1302 (fixed 64 KB physical buffer, id `0xBEEF0000`) |
| `VdQueryVideoMode` / `XGetVideoMode` | 1325-1333 (1280×720 hard-coded `:1307-1322`) |
| `VdQueryVideoFlags` | 1335-1338 (returns `0x7`) |
| `VdGetCurrentDisplayGamma` | 1340-1348 |
| `VdGetCurrentDisplayInformation` | 1350-1364 |
| `VdSetDisplayMode`, `VdPersistDisplay`, `VdRetrainEDRAM`, `VdRetrainEDRAMWorker`, `VdIsHSIOTrainingSucceeded`, `VdEnableDisableClockGating`, `VdCallGraphicsNotificationRoutines` | 1366-1381 |
| `VdInitializeScalerCommandBuffer` | 1383-1393 (fills `0x1CC` bytes with type-2 NOPs) |
| `VdSwap` | 1395-1428 |

`kernel/stubs.cpp:911-970` has weak fallbacks for the same exports.

### Front buffer → presenter
`VdSwap` (`gpu.cpp:1407-1409`) → CP NOP (`:649-679`) → `FrameOnSwap` (`frame.cpp:263`) → `RendererOnSwap` (`renderer.cpp:6132`) → `RendererOnSwapImpl` (`:6397`): looks up `g_r.resolved[frontBuffer]`, gamma-grades, `PresenterRecordCopy` (`:6445`), `ExecuteCommandLists` (`:6596-6597`), `PresenterPresent` (`:6599`).

### Window / swapchain ownership
- The presenter thread owns the HWND, the DXGI flip-model swapchain (`FLIP_DISCARD`, `ALLOW_TEARING`, 3 buffers, `presenter.cpp:193-206`) and the D3D12 device and queue.
- The renderer fetches them via `RenderDevice()`/`RenderQueue()` (`renderer.cpp:873-874`), so both share one DIRECT queue.
- `PresenterUseRenderer()` (`:1031`) disables the presenter's fallback drawing.
- Copy into the backbuffer handles letterboxing (`presenter.cpp:401-428`). There is no scaling shader; scaling is the swapchain's `DXGI_SCALING_STRETCH` (`:196`).

### Settings
`src/settings.h` / `settings.cpp`: `Documents\fm2\settings.ini` (`settings.cpp:18`). Fields: `width`, `height`, `vsync`, `maxFps`, `msaa`, `upscaler`, `quality`, `sharpness`, `enhance`, `fullscreen`. Env overrides.

### What ports unchanged to another runtime
Needs only a memory-base adapter, `UserDir()`, `Log` and `GetSettings()`:
- **Whole translation stack:** `hlsl.cpp`, `ucode.cpp`, `texture.cpp`.
- **Public seam** (`renderer.h`) and the `DrawImpl` / `Resolve` / `RendererOnSwapImpl` logic. It still uses D3D12 types throughout.
- **CP logic** in `gpu.cpp:546-929` (packet parser, predication, constants, lookahead) is generic Xenos. It only needs `LoadBE32` / `GuestPtr` plus the ring base/wptr source.
- `shader_scan.cpp` and the scan side of `static_shaders.cpp` (need the physical view base `0xA0000000`, `GameDir`, `ForEachZipMember`).

Tied to this runtime (rewrite or adapt):
- `Vd*` exports and the interrupt path (`gpu.cpp:87-94`, `:447-479`, `:1219-1428`), which depend on `CreateHostGuestThread`/`GuestCall`/`InterruptLockAcquire`.
- The ring-buffer wptr polling of the `0x7FC80000` page.
- `PhysicalSection()` heap mapping (`renderer.cpp:974-1002`).
- `presenter.cpp` (Win32/DXGI).
- `hooks/*`, `camera_aspect.cpp`, `MemScanOnSwap`, `LogAllHostThreadStacks` (`renderer.cpp:60`, `:61`), `GuestThreadCpuUsage` (`:6183`).

---

## 5. Shader pipeline

- **Decode:** `ucode::Parse` (`ucode.cpp:123`) → structured `Shader`. **Emit:** `TranslateShader` (`hlsl.cpp:752-922`) generates HLSL SM 5.1 text with a VS wrapper doing vertex fetch, index modes, point/rect expansion and the viewport transform (`:779-896`), and a PS wrapper with `oC[0]`, alpha test and generated parameter (`:900-917`).
- **Compile: FXC through `D3DCompile`.** Targets `vs_5_1`/`ps_5_1`, flags `D3DCOMPILE_SKIP_OPTIMIZATION | ENABLE_UNBOUNDED_DESCRIPTOR_TABLES` (`renderer.cpp:1840-1841`). Blit shaders compile as `vs_5_0`/`ps_5_0` (`:803-856`). There is **no DXC**, no DXIL, no SPIR-V. FSR1 is compiled at build time with `fxc` `cs_5_1` through `tools/embed_hlsl.py:49` (`CMakeLists.txt:82-89`).
- **Cache format / location** (`UserDir()` = `%USERPROFILE%\Documents\fm2`, `runtime.cpp:71-72`):
  - `shadercache/<hlslhash>.dxbc`: raw DXBC blob keyed by FNV of the HLSL text (`renderer.cpp:1728-1733`, `:1853-1854`, `:2040-2052`).
  - `shadercache/{vs|ps}_<hash>.ucode`: original microcode, written only once a pipeline uses it (`:1667-1672`, `:1702-1712`).
  - `pipelines.manifest`: `P vs ps blend depth stencilMaskMode flags` lines (`:1666`, `:1681`, `:1698`).
  - `pipelines.bin`: `ID3D12PipelineLibrary` serialisation (`:2249`).
  - `static_shaders.bin`: magic `FMSS`, version 5, with a signature over file names/sizes (`static_shaders.cpp:143-219`).
- **Pipeline library:** `ID3D12PipelineLibrary` via `ID3D12Device1::CreatePipelineLibrary` (`renderer.cpp:2251-2269`), `LoadGraphicsPipeline`/`StorePipeline` (`:2286`, `:2297`), serialised off-thread at most every 10 s (`:2311-2360`). Pipeline name is `vs_ps_blend_depth_stencilMask_flags` (`:2271-2276`).
- **Ahead-of-time scan (runtime, not XEX tools):**
  - Data files: `.zip` (deflated `.rmb*` members) and `.bin` under `GameDir`, `DlcDir`, `TitleUpdateDir` (`static_shaders.cpp:148-164`, `:257`).
  - Plus the loaded XEX image `0x82000000..+0x2000000` at dword alignment (`:267`).
  - Candidate = control flow decodes cleanly to an end flag, first exec addr = CF length, exec ranges tile with no gaps (`:38-97`).
  - Consumed by `PrecompileAll` (`renderer.cpp:6334-6381`) behind the boot screen.
- **Runtime scan:** `ShaderScanStart` re-scans the 512 MB physical view every 1.5 s for `0xC0nn2B00` packets (`shader_scan.cpp:44-76`, `:98-111`); the render path also forces a scan on an unseen shader (`renderer.cpp:5377-5396`).
- **Python tools** (offline analysis, not used by the runtime): `tools/findshaders.py:1-14` (structure finder, Python/numpy), `tools/scanshaders.py:1-8` (`IM_LOAD_IMMEDIATE` finder), `tools/staticshaders.py:1-9` (checks known `.ucode` blobs against game data).
- **Counts claimed:** README:46-51: "about 4,800 shaders in about 4 seconds ... about 3.5 minutes on 16 threads, once". README:6: 51,034 recompiled functions. `static_shaders.cpp:270` logs the real count at runtime. I did not run it.
- **Worker model:** compile on N-1 background threads (`:1753`); `DrawImpl` waits via `WaitHoldingImpl` (which also presents the overlay) rather than dropping opaque draws (`:2516-2525`, `:5410-5413`). Opt-in `FM2_ASYNC_SHADERS`/`FM2_ASYNC_PIPELINES`. Blended draws are skippable while a build runs (`:2563-2570`).
- **Root signature layout** (`:877-936`): `b0` VS root CBV, `b1` PS root CBV, table[2] = `t0` upload-ring raw SRV + `t1` guest-physical raw SRV, table[3] = two 65536-descriptor unbounded SRV ranges (2D in `space1`, cube in `space2`, same descriptors), 4 static samplers `s0..s3`. A second signature adds `b2` for the motion variant (`:937-947`).

---

## 6. Third-party dependencies and Windows-only APIs

**`CMakeLists.txt`:**
- `:2` project `fm2 C CXX RC`; `:7-9` requires clang-cl.
- `:11` `thirdparty/XenonRecomp` (git submodule, `.gitmodules`).
- `:77-105` **AMD FidelityFX SDK v2.3.0** (README:133) from `thirdparty/fidelityfx`: headers, `amd_fidelityfx_loader_dx12.lib` linked, loader and upscaler DLLs copied post-build (`:99-102`).
- `:90-93`, `:99`, `:103-104` **NVIDIA DLSS v310.9.16** (README:134): `nvsdk_ngx_s.lib`, `nvngx_dlss.dll`.
- `:113-120` **FFmpeg** LGPL shared build (`avcodec`, `avutil`, `swresample` DLLs) for XMA.
- `:105`, `:109` system libs: `d3d12 dxgi d3dcompiler synchronization winmm onecore xaudio2 dbghelp bcrypt winhttp`.
- `:123-125` `xenos_dis` tool links `d3dcompiler`.
- XenonRecomp's own `thirdparty/` (simde, fmt, tiny-AES-c, xxHash, tomlplusplus, libmspack, TinySHA1, disasm).

**Not present** (grepped for `agility|D3D12MA|dxcompiler|dxil|spir|vulkan|plume|sdl`; only FM2-native-scope.md's plan text matches): Agility SDK, DXC/DXIL, D3D12 Memory Allocator (the renderer rolls its own `TexturePool` `renderer.cpp:497-561`), Vulkan, SDL.

**Windows-only APIs in `src/gpu` and `src/render` beyond D3D12:**
- DXGI: `dxgi1_4.h` (`renderer.cpp:32`), `dxgi1_6.h`, `CreateDXGIFactory2`, `CreateSwapChainForHwnd`, `IDXGISwapChain3`, tearing flag (`presenter.cpp:187-206`, `:306`, `:451-453`).
- Win32 windowing: `RegisterClassW`, `CreateWindowExW`, `PeekMessageW`, `AdjustWindowRect`, `SystemParametersInfoW`, `GetSystemMetrics`, `EnumDisplaySettingsW`, `GetForegroundWindow` (`presenter.cpp:99-186`, `:319-323`, `:357`, `:459`).
- `D3DCompile` / `d3dcompiler.h` (`renderer.cpp:33`; sites `:803-856`, `:1840`).
- `DirectXPackedVectors.h` (`:42`); `wrl/client.h` `ComPtr`.
- `GetAsyncKeyState(VK_F12)` plus `GetWindowThreadProcessId` for capture (`renderer.cpp:6457-6460`).
- Threading and sync: `SetThreadDescription`, `SetThreadPriority`, `DuplicateHandle`, `GetThreadTimes`, `Sleep`, `SwitchToThread`, `CreateThread` + `STACK_SIZE_PARAM_IS_A_RESERVATION` (`gpu.cpp:1105-1127`, `renderer.cpp:5285`), `std::atomic::wait/notify`.
- `GetActiveProcessorCount` (`renderer.cpp:6098`), `_mm_pause`/`<intrin.h>`/SSE intrinsics (`gpu.cpp:13`, `renderer.cpp:13`, `:5569`).
- Memory: `VirtualAlloc2`, `MapViewOfFile3`, `CreateFileMapping2` (`memory.cpp:118-134`).

---

## 7. Licenses

- **The repo has no LICENSE/COPYING/NOTICE file.** `git ls-files | grep -iE 'licen|copying|notice'` returns nothing. There is no license statement in `README.md`, `TESTING.md` or any `src/` file header.
- Attribution mentions only:
  - `README.md:3-6` links XenonRecomp.
  - `README.md:126-127` says FFmpeg "stays under its own LGPL license (`thirdparty/ffmpeg/LICENSE.txt`)".
  - `CMakeLists.txt:77` calls FidelityFX "(MIT)"; `:111` says "(LGPL, linked dynamically)".
  - `ucode.h:3-4` and `texture.cpp:51` cite "as documented in Xenia" (bit layouts, tiling). No license text.
  - `FM2-native-scope.md:121`: "Legal. Ship no game data or copyrighted code... Generated C++ derived from the XEX is a grey area".
  - `installer/fm2.iss:22`: `AppPublisher=zer0ducksgiven`. README:20 links `github.com/zer0ducksgiven/Forza-Motorsport-2-ReGPU`.
  - `dlss.cpp:25-26`: made-up NGX project id.
- **XenonRecomp submodule license:** `thirdparty/XenonRecomp/LICENSE.md`, "MIT License, Copyright (c) 2025 hedge-dev and contributors". Our patch is `patches/xenonrecomp-fm2.patch` (215 lines), applied with `git apply` per README:139.
- Net: the repo's own code has **no declared license**, so default copyright applies; reuse needs the author's permission `[INFERENCE: standard default for an unlicensed repo]`. Its shader translator and texture code are cited as derived from Xenia docs, but the clone contains no Xenia source or license `[verified by grep; not a license determination]`.

---

## 8. D3D12 API footprint (sizing a Plume port)

**Counts** (`grep -c` over lines in the files):

| Pattern | `renderer.cpp` | `presenter.cpp` |
|---|---|---|
| lines with `ID3D12` | 122 | 18 |
| lines with `D3D12_` | 493 | 48 |
| `->Create…` | 71 | 8 |
| `ComPtr` | 74 | 16 |
| `D3D12_RESOURCE_STATE_` | 104 | n/a |
| `DXGI_` | 82 | 10 |
| `C().list->…` recorded-list call sites | 116 | n/a |
| `ResourceBarrier` | 36 | 4 |
| `CopyTextureRegion` / `CopyBufferRegion` / `CopyResource` | 10 / 1 / 1 | 3 / 0 / 1 |
| `CreateGraphicsPipelineState` | 9 | 0 |
| `CreateComputePipelineState` | 5 | 0 |

`recorded_list.h` already abstracts the command-list surface into **28 ops** (`:123-128`): barrier, CopyTexture, CopyBuffer, root-signature/table/CBV/32-bit-constants for graphics and compute, Dispatch, OMSetRenderTargets (max 2 RTs), SetPipelineState, viewports, scissors, stencil ref, blend factor, topology, SetDescriptorHeaps, Draw, DrawIndexed, SetIndexBuffer, EndQuery, ResolveQuery, ClearRTV/DSV, CreateRTV/DSV (in-order descriptor writes). That list is the natural RHI seam.

**D3D12 features actually used:**
- **Device:** FL 12_0 (`presenter.cpp:131`); `ID3D12Device1` pipeline library (`renderer.cpp:2253`); `ID3D12Device13` `OpenExistingHeapFromFileMapping`/`OpenExistingHeapFromAddress1` (`:974-981`); optional debug layer `FM2_D3D12_DEBUG` (`presenter.cpp:125-130`); `ID3D12InfoQueue` messages (`renderer.cpp:433-445`).
- **Root signature v1.0** (`D3D12SerializeRootSignature(..., D3D_ROOT_SIGNATURE_VERSION_1)`, `:934`, `:944`, `:778`, `:3852`): root CBVs, descriptor tables, 4 static samplers (`:905-927`, anisotropic 16× on wrap-linear `:917-922`). No root-signature flags.
- **Descriptor heaps:** one shader-visible CBV/SRV/UAV heap of `3*8192 + 65536` descriptors (`:949-954`), a CPU staging heap (`:955-957`), RTV heap of 64 (`:1005-1007`), DSV heap of 64 (`:1008-1009`), blit RTV heap (`:1010-1012`). Unbounded bindless 2D and cube arrays over the same descriptors (spaces 1 and 2, `:881-889`, HLSL `hlsl.cpp:43-44`). `CopyDescriptorsSimple` once (`:968`).
- **Memory:** committed resources (19 sites), placed resources in 128 MB `D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES` heaps (`:533-547`, textures only, no RT aliasing), 64 MB upload ring × 3 frames (`:1020-1021`), 128 MB `IndexArena` (`:1425`), readback buffers.
- **Formats:** colour `R16G16B16A16_FLOAT`, depth `R32G8X24_TYPELESS`/`D32_FLOAT_S8X24_UINT` with SRV `R32_FLOAT_X8X24_TYPELESS`, resolve `R8G8B8A8_UNORM`/`R32_FLOAT`/`R16G16_FLOAT` (motion), `BC1..BC4`, `R32_TYPELESS` raw buffer SRVs (`:1336-1340`).
- **MSAA** via `CheckFeatureSupport(MULTISAMPLE_QUALITY_LEVELS)` (`:754-756`), `TEXTURE2DMS` SRV/DSV, `Texture2DMS` blit shaders (`:828-865`), alpha-to-coverage (`:2429`).
- **Compute:** `CreateComputePipelineState` ×5 (FSR1 EASU/RCAS/depth copy/gamma/enhance, `:3861-3873`), `Dispatch` at `:3653`, `:3944`, `:4011`, `:4123`, `:4129`, UAV on `RGBA16F`/`RGBA8`/`R32` textures (UAV creates `:3635`, `:3934`, `:3985`, `:4084`; flags `:3491`, `:3609`, `:3730`, `:3919`, `:3970`, `:4060`). Typed UAVs (not raw), no ROVs.
- **Queries:** timestamp query heaps for perf (`:1233-1245`, `:6585-6592`). No occlusion/pipeline-stat queries (occlusion is faked on the CPU, `gpu.cpp:734-756`).
- **Sync:** one fence + event (`CreateFence` `renderer.cpp:1029`; waits `:1224`, `:5041-5046`). `ExecuteCommandLists` at 7 sites. Multiple command lists per frame (pool, `:4995-5013`). One DIRECT queue shared with the swapchain.
- **Legacy `ResourceBarrier` with transitions only.** No enhanced barriers, no UAV barriers found, no split barriers.
- **Not used:** `ExecuteIndirect`/command signatures (0 hits), mesh shaders, root constants in the main signature (only the blit root uses `SetGraphicsRoot32BitConstants`, `:3555`), `ResolveSubresource`, `GetCopyableFootprints` (footprints hand-built), DXC/SM6, bundles, tiled resources.
- **Pipeline state:** `D3D12_GRAPHICS_PIPELINE_STATE_DESC` only; keyed by `{vs, ps, blend, depth, stencil/mask/cull mode, flags}` (`:283-288`, `:2527-2547`). One render target (two for motion variants: `NumRenderTargets=2`, RT1 `R16G16_FLOAT`, `:2472-2483`). Triangle list and strip only; points, rects and quads are expanded in the VS or index list (`hlsl.cpp:783-861`).
- **Upscaler dependencies on D3D12 types:** `DlssFrame`/`DlssSetup` in `dlss.h` pass `ID3D12Resource*`; NGX `NVSDK_NGX_D3D12_*` (`dlss.cpp:37-43`); FFX `ffxCreateBackendDX12Desc` (`fsr3.cpp:33-35`). These are D3D12-only and would be dropped or re-backed in a Plume port.

---

## Items to flag for the design doc

1. **Single colour target, fixed formats.** If PGR4 uses MRT, the PSO/PS/resolve model must grow (`hlsl.cpp:900-917`, `renderer.cpp:2468`).
2. **Texture format coverage is narrow** (10 formats, no volume textures; `texture.cpp:13-50`, `renderer.cpp:2903`). PGR4 may need more.
3. **Resolve-to-guest readback is a stall** (`WriteResolveToGuest`, `:5049-5114`). Resolves otherwise stay GPU-only, so any PGR4 CPU read of a render target is only covered for the sizes in `FM2_RESOLVE_WRITEBACK`.
4. **Zero-copy guest memory needs `ID3D12Device13`** (`:974-981`). Plume/Vulkan needs an equivalent import path.
5. **Shader backend is FXC DXBC.** A Vulkan/Plume port needs HLSL→DXIL or SPIR-V (DXC), since the generated HLSL uses SM5.1 unbounded arrays (`hlsl.cpp:43-44`).
6. **The CP spins** with `Sleep(0)` when the ring is empty (`gpu.cpp:1138-1143`) and polls the wptr page (`:1137`) rather than being notified.
7. **The renderer reads frame size from the resolve** at swap (`renderer.cpp:6420-6422`), so a non-1280×720 PGR4 front buffer would present nothing unless that check is generalised.