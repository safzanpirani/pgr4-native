# Native renderer for the PGR4 recompilation: design

Status: design, 2026-10-21; **decision recorded in §0** after the PGR4 draw-path measurement
(`the development notes`, "Draw-path measurement") and the FM2 benchmark (`the FM2-ReGPU notes (not published)`).
Sections 1-7 are the original study; where §0 disagrees with them, §0 holds. Every technical
claim cites source by `repo:path:line` or a URL. Inferences are marked `[INFERENCE]`. Raw notes
are in `docs/research/`.

## Sources and pins

| Prefix | Repository | Commit read | License |
|---|---|---|---|
| `FM2:` | [zer0ducksgiven/Forza-Motorsport-2-ReGPU](https://github.com/zer0ducksgiven/Forza-Motorsport-2-ReGPU) | `128ddea` (2026-10-20) | **none declared** (see §6) |
| `SDK:` | [rexglue/rexglue-sdk](https://github.com/rexglue/rexglue-sdk) | `main` `c94f5eb` (2026-08-21); `src/`+`include/` identical to tag `v0.10.0` `f5337cd` | BSD-3-Clause |
| `P4R:` | [beatrixzy/PGR4-Recomp](https://github.com/beatrixzy/PGR4-Recomp) | `38d8d87` (2026-10-20) | MIT |
| `MTR:` | [MissingThePixel/MTR-PGR4](https://github.com/MissingThePixel/MTR-PGR4) | `2db3edb` (2026-10-11) | MIT (own code) |
| `PLM:` | [renderbag/plume](https://github.com/renderbag/plume) | `e48a55b` (2026-10-12) | MIT |
| `VKD:` | [HansKristian-Work/vkd3d-proton](https://github.com/HansKristian-Work/vkd3d-proton) | `master` `ef9a02e` (2026-10-20) | LGPL-2.1 |

Raw notes: `docs/research/fm2-regpu-renderer-map.md`, `docs/research/rexglue-gpu-integration.md`,
`docs/research/pgr4-mtr-plume-notes.md`.

## Summary

- FM2-ReGPU is a XenonRecomp title with its own kernel, not a ReXGlue title (`FM2:README.md:3-10`).
  Its renderer sits behind a 13-function seam (`FM2:src/gpu/renderer.h:5-46`) fed by its own PM4
  command processor (`FM2:src/gpu/gpu.cpp`). The renderer core has no hard-coded guest code
  addresses, but it has many 1280x720 and FM2-content assumptions (§1.3).
- ReXGlue already has the PM4 front end FM2 wrote by hand: ring reading, MMIO registers,
  predication, indirect buffers, interrupts, vblank, and a swap packet
  (`SDK:src/graphics/command_processor.cpp:200-256`, `:732-892`, `:929-965`). It calls four
  virtuals a backend implements: `LoadShader`, `IssueDraw`, `IssueCopy`, `IssueSwap`
  (`SDK:include/rex/graphics/command_processor.h:109-110`, `:217-222`).
- **Decision (§0): hook PGR4's D3D API in the app** (design A in §0; option C in §2.2) and
  translate each unique draw once, with no tiling. Measured on PGR4: every game draw goes through
  the D3D draw functions, and the D3D device keeps a verbatim shadow of the GPU registers, so the
  hooks see complete Xenos state per draw without parsing PM4. The Xenos plugin stays in the same
  binary as the A/B oracle: the hooks forward to the original guest code and the forwarding is
  switchable at run time. The PM4 plugin (`rexgpu-native`, design B in §0, option A in §2.2) was
  the original recommendation and is now the fallback.
- **Graphics API:** D3D12 first, run on the Deck through Proton/vkd3d-proton. Plume is not needed
  for the first target and lacks features this renderer uses (§3).
- **Licensing blocker:** FM2-ReGPU has no license, so its code cannot be copied. Use it as a
  design reference and behaviour oracle; build on BSD-licensed ReXGlue/Xenia code and new code
  (§6). Asking the author for a license is the cheapest unblock.

---

## 0. Decision (2026-10-21): hook the D3D API in the app

Names used from here on: **design A** = hook PGR4's guest D3D functions in the app and translate
D3D calls to D3D12 directly, drawing each unique draw once (option C in §2.2, P4R's D3D12 shape).
**Design B** = a `rexgpu-native` PM4 plugin behind ReXGlue's command processor (option A in §2.2,
the original recommendation).

### 0.1 What was measured

| Fact | Number | Source |
|---|---|---|
| Unique game draws through PGR4's D3D draw functions | all of them: first-pass + later-passes/2 equals the API count to the draw (title 2,037, menu 2,644, race 3,282 per frame) | `the development notes`, "Draw-path measurement" |
| Draws not from the API | 24 one-point dummy draws per pass from one D3D-internal buffer (`sub_826942B0`, start-up only) | same |
| Predicated tiling | 2 tiles + 1 untiled pass; GPU executes 1.6-1.85x the unique draws under emulation (race 5,474 avg, 9,432 max per frame) | same |
| Resolves | 32 (menus) to 44 (race) per frame, all but one from `sub_82696978` | same |
| D3D device state | the device (22,400 bytes) keeps the GPU registers verbatim at fixed offsets and flushes dirty blocks as type-0 packets copied from them; fetch constants at +1152, VS/PS float constants at +1920/+6016, bool/loop at +10112, register blocks 0x2000-0x238D at +10368..+10888, push pointer at +48, dirty masks at +0..+40 | `native/tools/scan_d3d_surface.py` (`SHADOW`), `sub_8269ADD8`, `sub_826A8720` |
| Game-facing D3D surface | 193 functions (189 called from game code, plus 2 internal draw builders, MakeSpace and the D3D-internal entry list): draws, Resolve, Swap, Clear, shader/constant/texture/stream/surface setters, 33 render-state setters named by the register they change, queries (occlusion type 9), CreateDevice | `native/src/d3d_surface.inc` |
| FM2-ReGPU per-draw CPU cost | renderer draw 4.6-5.5 ms per frame at 2,000-3,300 draws on the i9-13900K (1.7-2.3 µs per draw), CPU-bound at 170-192 fps while Xenos emulation (hl360) ran the same race at 240-250 fps | `the FM2-ReGPU notes (not published)` |

### 0.2 A against B

| Criterion | A: D3D hooks in the app | B: `rexgpu-native` PM4 plugin |
|---|---|---|
| Coverage | 100% of unique game draws (measured); the only non-API draws are D3D-internal dummies | 100% by construction |
| Tiling | never sees it: each unique draw arrives once, at the API, before the D3D splits it into tiles | must find and drop tile replays itself (keep the untiled pass, run one tile pass at full size, skip the other), with tile-dependent window offsets, EDRAM bases and per-tile resolves; or pay 1.6-1.85x draws |
| Per-draw CPU | state comes from the device shadow plus its dirty masks (only changed blocks need translating); no PM4 parsing, no predication, no per-register write callbacks | parses every packet of every pass (~8,000-11,000 `SET_BIN_MASK` and ~2,000 `EVENT_WRITE_EXT` per frame alone) and rebuilds dirty state from register writes |
| Cost at FM2-ReGPU's per-draw rate (1.7-2.3 µs) | 3,282 draws: 5.6-7.5 ms per race frame on the dev PC (i9-13900K + RTX 3090) | 5,474 draws: 9.3-12.6 ms (max frame 9,432: 16-22 ms) unless tiles are dropped |
| Deck CPU (4 Zen 2 cores) | `[INFERENCE]` a Deck core is ~2-2.5x slower than a 13900K P-core, so FM2-ReGPU's rate would cost 11-19 ms per race frame on the Deck even in A; the per-draw budget for 60 fps is ~1 µs on the dev PC (≈3.3 ms per frame, ~8 ms on a Deck core) | same budget, applied to 1.6-1.85x the draws |
| A/B oracle | the Xenos plugin keeps running; hooks forward to the guest D3D and can be switched off per class at run time (implemented, §0.4). During bring-up both can run at once: native renders into its own window while Xenos renders the same frame | `--gpu_plugin=xenos` vs `=native` in two runs of the same binary; not in the same frame |
| Ties to the SDK | none beyond `REX_HOOK_RAW` and public headers; no hidden SDK classes, no plugin ABI | compiles hidden SDK sources into the plugin; tied to ABI v1 and protected members (§2.2) |
| Ties to the game binary | addresses of 193 D3D functions and the device layout (one `d3d_surface.inc`, generated); a title update or another XDK build needs a rescan | none |
| Effort | new: draw/resolve/clear/present translation from register state (the same Xenos semantics B needs), plus the hook layer (done in milestone 1). Shader, texture and render-target work is the same as B | plugin skeleton, tile-pass dropping, CP pacing; then the same shader/texture/RT work |
| Licensing | own code + BSD SDK headers; FM2-ReGPU stays reference-only | same |

**Decision: A.** Coverage is measured complete, tiling disappears instead of having to be
undone, and the per-draw path does less work, which is the number the FM2 benchmark says decides
the frame rate. The oracle that motivated B is available in A too, inside one run. B stays the
fallback if a later PGR4 build (P4R v2, title update) moves rendering into prerecorded buffers.

Risks that remain with A: state written to the push buffer outside the device shadow (the draw
functions write `VGT_INDX_OFFSET` directly; milestone 1 lists every such register), the 24
dummy draws and any other D3D-internal buffer, occlusion queries (the game issues them through
`QueryIssue`/`QueryGetData`; A must answer them), and the binary-specific address table.

### 0.3 Revised phases

These replace §5's phases 1-3. Phases 4-6 stay as written, with "IssueDraw" read as the draw hook.

| # | Phase | Deliverable | Oracle |
|---|---|---|---|
| 1 | **Hooks and trace (done, §0.4)** | 193 D3D functions hooked with run-time switchable forwarding; push-buffer tap decoding all state per call; one-frame traces of menu and race | trace draws per frame = the 5 draw-builder calls (DrawCounter's API count) and = Xenos's unique draws in the same run; decoded registers = device shadow at every draw; screenshot unchanged |
| 2 | First native frame, side by side | D3D12 device and debug window in the app; per-draw state from the device shadow; ucode → HLSL/DXIL; textures for the formats menus use; render targets keyed by surface; resolves as copies; present at Swap | menu frame from the native window vs the Xenos window in the same run, PSNR > 30 dB; per-draw CPU µs in a CSV |
| 3 | In-race rendering | MRT, all formats used, cube/volume, MSAA, occlusion query answers, then cut-over: `skip draw,clear,resolve` and present native | race captures vs Xenos; zero untranslated draws; Xenos sees no draws |

### 0.4 Milestone 1: hooks and frame trace

Code: `native/` (`src/pgr4_d3d_trace.cpp`, `src/d3d_surface.inc`,
`tools/scan_d3d_surface.py`, `build.ps1`, `run.ps1`), built on the build host in
`C:\recomp\builds\pgr4\native` on top of MTR-PGR4 `2db3edb` with DrawCounter's
`--gpu_stream_stats` SDK patch. Results: `the development notes`, "Renderer decision". Oracle
so far (intro FMV, >1,000 frames): 22 draw packets decoded per frame = 22 draw-builder calls =
DrawCounter's API count, 2 resolves, 0 decoder resyncs/torn packets, title screen rendered
normally with the hooks on. Menu and race frames: see STATE.md; all trace checks pass.

---

## 1. FM2-ReGPU's renderer

### 1.1 Module map

| Module | Role | Key references |
|---|---|---|
| `src/gpu/gpu.cpp` (1428 lines) | PM4 command processor, `Vd*` kernel exports, vblank and interrupts, lookahead, optional parallel segments | ring loop `:1099-1181`; packet switch `:624-929`; Vd exports `:1263-1428` |
| `src/gpu/regs.h` | register file accessors over the guest page at `0x7FC80000`, per-thread override for workers | `:10-33`; `GpuPhysToGuest` `:36` |
| `src/gpu/frame.cpp` | glue CP → renderer; dedups shader uploads, FNV hash of used dwords | `FrameOnShaderLoad` `:182`, `FrameOnDraw` `:246`, `FrameOnSwap` `:263` |
| `src/gpu/renderer.h` | the whole renderer API (the porting seam) | `:5-46` |
| `src/gpu/renderer.cpp` (6747) | the renderer | section map below |
| `src/gpu/ucode.cpp` (327) | Xenos microcode decode | `Parse` `:123`, `UsedDwords` `:104` |
| `src/gpu/hlsl.cpp` (922) | microcode → HLSL SM 5.1 | `TranslateShader` `:752-922`; PS writes only `oC[0]` `:900-917` |
| `src/gpu/texture.cpp` (477) | formats, 2D tiling, DXT decode | formats `:13-50`, `TextureTiledOffset` `:362` |
| `src/gpu/recorded_list.h` (348) | command-list recorder with 28 ops, replayed on a thread | op list `:123-128` |
| `src/gpu/static_shaders.cpp`, `shader_scan.cpp` | find shader microcode in game files, the XEX image and physical memory | `static_shaders.cpp:38-97`, `:267`; `shader_scan.cpp:44-76` |
| `src/gpu/dlss.cpp`, `fsr3.cpp`, `fsr1.hlsl` | upscalers (D3D12 types) | `dlss.cpp:37-43`, `fsr3.cpp:33-35` |
| `src/render/presenter.cpp` (429) | Win32 window, DXGI flip swapchain, D3D12 device and the one DIRECT queue | device `:120-135`, swapchain `:193-206`, present `:445-455` |

`renderer.cpp` sections (from `docs/research/fm2-regpu-renderer-map.md`):

| Lines | Section |
|---|---|
| 62-211 | constants: `kWidth=1280, kHeight=720` (`:64`), `kColorFormat=RGBA16F` (`:173`), 3 frames, 64 MB upload ring (`:167-171`) |
| 869-1034 | device, root signature, descriptor heaps, guest physical heap import (`:974-1000`) |
| 1036-1140 | EDRAM → host surfaces: one colour and one depth surface per EDRAM base (`GetSurface` `:1046`, `SelectSurfaces` `:1120`) |
| 1419-1578 | index buffers (`GetIndexBuffer` `:1471-1577`) |
| 1725-2055 | shader compile pool (`D3DCompile` `vs_5_1`/`ps_5_1`, `:1840-1841`) and `GetShader` |
| 2235-2525 | `ID3D12PipelineLibrary` and PSO cache (`:2237-2361`, `EnsurePipeline` `:2407-2512`) |
| 2840-3138 | texture fetch, untile, BC passthrough or CPU decode |
| 3373-3421 | tile state and `WindowScissor` |
| 3423-3572, 4147-4281 | resolves as GPU copies into textures keyed by guest physical address |
| 3576-4146 | upscalers and gamma |
| 4284-4396, 4895-4960 | lookahead consumer: precompile pipelines for draws not executed yet |
| 4990-5330 | parallel draw pieces (`RendererRunPieces` `:5248-5314`) |
| 5333-6031 | `DrawImpl`: per-draw translation |
| 6035-6129 | benchmark report |
| 6397-6725 | swap/present (`RendererOnSwapImpl`) |

### 1.2 How a frame flows

1. The title's D3D writes PM4 into the ring and stores the write pointer to the register page;
   FM2's CP polls `Reg(CP_RB_WPTR)` (`FM2:src/gpu/gpu.cpp:1137`) on a guest-context host thread
   (`:1237`) and publishes the read pointer after every packet (`:1170-1175`).
2. Type-3 packets with the predicate bit are skipped when `binSelect & binMask` is zero
   (`gpu.cpp:696-700`). Indirect buffers recurse (`:706-709`, `:931-940`).
3. `IM_LOAD`/`IM_LOAD_IMMEDIATE` → `SubmitShaderLoad` (`:900-912`); `DRAW_INDX(_2)` →
   `SubmitDraw` → `RendererOnDraw(initiator, dmaBase, dmaSize)` (`:913-920`).
4. `DrawImpl` reads `RB_MODECONTROL & 7`: 6 is a resolve, 4/5 draw, anything else is skipped
   (`renderer.cpp:5358-5370`).
5. Tiling: each tile pass still runs. The renderer works in screen space on one full-size
   surface and clips each tile pass to its scissor (`renderer.cpp:3381-3383`, `:5826-5866`).
   This removes EDRAM save/restore between tiles but not the repeated CPU translation of each
   tile's draws. The "render the first tile only" idea in `FM2:FM2-native-scope.md:87-89` is not
   implemented in this commit (searched `renderer.cpp` for tile skipping; none found).
6. `VdSwap` turns its 64-dword block into type-2 NOPs, queues the block address with the front
   buffer's physical address, and sleeps to the frame cap (`gpu.cpp:1395-1428`). When the CP
   reaches that NOP it calls `FrameOnSwap` (`:649-679`), which presents the resolved texture at
   that address (`renderer.cpp:6418-6423`).

Performance features: ahead-of-time shader discovery (`static_shaders.cpp:38-97`) plus
first-boot compile of about 4,800 shaders, "about 3.5 minutes on 16 threads, once"
(`FM2:README.md:49-51`); a pipeline library (`renderer.cpp:2237-2361`); lookahead pipeline
precompile (`gpu.cpp:1060-1095`). The parallel pieces path is off by default because it
"measured slower than the serial path so far" (`renderer.cpp:5203-5208`). Reported result:
59.9 fps average, 59.1 fps 1% low on an RTX 4070 machine (`FM2:README.md:61`, `:77`).

### 1.3 FM2-specific assumptions

Hard-coded guest code addresses live only outside the renderer core:

| Address | Purpose | Reference |
|---|---|---|
| `sub_8257E0D0` | camera aspect setter, widened for ultrawide | `FM2:src/hooks/camera_aspect.cpp:1-29` |
| `0x820009CC`, `sub_82371D60`, `sub_823724C8`, `sub_8236CB28`, `sub_825ADE70` | D3D device global, GPU wait, submit, swap, present (diagnostics) | `src/hooks/d3d_debug.cpp:13-145` |
| `sub_8237D158`, `sub_8236D830` | "the only two functions that build draw packets" (used for the 94% measurement) | `d3d_debug.cpp:149-163`; `FM2-native-scope.md:77-82` |
| `sub_82369340`, `sub_82373E38`, `sub_82953E38`, `sub_82957260`, `sub_827C3BF0`, `sub_827C3CB8` | perf probes, Bink plane writer, malloc/free | `src/hooks/perf_probe.cpp:2-268` |
| `sub_82754CD0`..`sub_82756580` | XAPI fibers → host fibers | `src/hooks/fibers.cpp:93-182` |
| `0x82000000..0x83000000` | XEX image range scanned for shaders | `src/main.cpp:24-28`, `static_shaders.cpp:267` |
| D3D device offsets `+0x2A10` etc. | diagnostics | `gpu.cpp:1254-1257` |

Assumptions inside the renderer and CP that PGR4 will break or must re-check:

- 1280x720 everywhere: `renderer.cpp:64` (29 uses), display registers and video mode
  (`gpu.cpp:1225-1228`, `:1307-1322`, `:1350-1364`). Present only happens when the front buffer
  is a 1280x720 RGBA8 resolve (`renderer.cpp:6420-6422`).
- One render target per draw, RGBA16F colour (`renderer.cpp:2468`, `:173`; `hlsl.cpp:900-917`).
  No MRT.
- Ten texture formats only (`texture.cpp:13-50`), 2D and cube only (`renderer.cpp:2903`).
- FM2 content heuristics: "wide pass" = pitch 1280 (`renderer.cpp:145`, `:5860-5865`); paint
  editor big/tall passes (`:122-135`, `:4205-4210`); garage thumbnail readback size
  (`:4230-4249`); shadow-map clear scaling (`:5655-5669`); vertex fetch constant ≥96 skips the
  draw (`:5420-5429`); HUD detection by `|w-1|` (`:2895-2896`, `:5821`); boot screen from FM2
  art (`boot_screen.cpp:23-24`).
- Frame pacing: "title presents on vblanks: every one in races, every second one in menus";
  vblank at twice the frame cap (`gpu.cpp:1204-1214`).
- Occlusion queries answered on the CPU with a fixed 1000 samples (`gpu.cpp:734-756`).
- 128 `FM2_*` environment variables control behaviour (research notes §3b).

### 1.4 Own runtime vs generic Xenos

| Depends on FM2's runtime | Reference | ReXGlue equivalent |
|---|---|---|
| guest memory = one 4 GB placeholder, `GuestPtr(a) = g_base + a`; physical = three views of one section | `FM2:src/memory.h:81-87`, `memory.cpp:113-143` | same layout (`SDK:src/system/xmemory.cpp:50-79`); `TranslatePhysical` (`xmemory.h:404-405`) |
| zero-copy guest physical memory in D3D12 via `OpenExistingHeapFromFileMapping(PhysicalSection())`, fallback `OpenExistingHeapFromAddress1` | `renderer.cpp:974-1000` | mapping handle is private (`SDK:include/rex/system/xmemory.h:590`); use `OpenExistingHeapFromAddress1(physical_membase(), 512 MB)` |
| GPU registers as a plain guest page the CP polls | `gpu.cpp:26-35`, `regs.h:21-27` | MMIO range with handlers (`SDK:src/graphics/graphics_system.cpp:140-147`), host-endian `RegisterFile` (`register_file.h:40-44`) |
| CP and interrupt threads via `CreateHostGuestThread`; interrupts by `GuestCall` with PCR and interrupt lock | `gpu.cpp:447-479`, `:1232-1238`; `src/kernel/thread.cpp:184-187` | `XHostThread` worker (`command_processor.cpp:200`), `DispatchInterruptCallback` → `ExecuteInterrupt` (`graphics_system.cpp:297-317`) |
| vblank thread at 2x frame cap; frame limiter inside `VdSwap` | `gpu.cpp:1185-1216`, `:1415-1426` | vsync worker at the video mode refresh (`graphics_system.cpp:150-176`); SDK `VdSwap` has no limiter (`xboxkrnl_video.cpp:440-521`) |
| swap = NOP block match | `gpu.cpp:649-679` | `PM4_XE_SWAP` packet → `IssueSwap` (`xboxkrnl_video.cpp:510-515`, `command_processor.cpp:929-965`) |
| window, swapchain, device, queue owned by its presenter | `presenter.cpp:120-241` | `SetupPresentation` / provider / presenter (`interfaces/graphics.h:43-58`) or detached overlay mode (`rex_app.h:169-190`) |
| settings in `Documents\fm2\settings.ini`, env vars | `src/settings.cpp:18` | cvars (`SDK:include/rex/cvar.h`) |

Generic and portable once a memory/register adapter exists: the packet parser and lookahead
(`gpu.cpp:546-1095`), `ucode.cpp`, `hlsl.cpp`, `texture.cpp`, and the `DrawImpl`/`Resolve`/swap
logic, which still uses D3D12 types throughout (footprint in research notes §8: 122 lines with
`ID3D12`, 493 with `D3D12_`, 71 `->Create…`, 116 recorded-list call sites in `renderer.cpp`).

---

## 2. ReXGlue integration

### 2.1 Integration points

| Point | Reference |
|---|---|
| App supplies a graphics system in `OnPreSetup` by setting `config.graphics`; else `--gpu_plugin` loads `rexgpu-<name>` | `SDK:src/ui/rex_app.cpp:308-334`, `include/rex/runtime.h:66-82` |
| Plugin ABI v1: `rex_gpu_abi_version`, `rex_gpu_create(abi, GpuCreateInfo{struct_size, backend})` | `SDK:include/rex/system/gpu_plugin.h:32-47`; loader `src/system/gpu_plugin_loader.cpp:40-102` |
| `IGraphicsSystem`: `SetupPresentation`, `SetupGuestGpu`, `SetInterruptCallback`, `InitializeRingBuffer`, `EnableReadPointerWriteBack`, `InitializeShaderStorage`, `Shutdown` | `SDK:include/rex/system/interfaces/graphics.h:34-95` |
| Kernel `Vd*` exports forward to it; `VdSwap` writes `PM4_XE_SWAP` into the ring | `SDK:src/kernel/xboxkrnl/xboxkrnl_video.cpp:315-350`, `:440-521` |
| Base `GraphicsSystem`: MMIO, vsync worker, interrupt dispatch | `SDK:src/graphics/graphics_system.cpp:118-176`, `:297-332` |
| Base `CommandProcessor`: PM4 parser; virtuals `LoadShader`, `IssueDraw`, `IssueCopy`, `IssueSwap`, `WriteRegister`, gamma callbacks, `OnPrimaryBufferEnd`, `EVENT_WRITE_ZPD` | `SDK:include/rex/graphics/command_processor.h:109-110`, `:145-173`, `:196`, `:217-222` |
| These base classes compile into the plugin DLL with hidden visibility, not into a linkable library | `SDK:src/graphics/CMakeLists.txt:1-31`, `:99-105`, `:133-139` |
| A title can build its own in-tree `rexgpu-<name>` and stage it with `GPU_PLUGINS` | `SDK:cmake/rexglue_helpers.cmake:145-166` |
| Weak-symbol hooks on any recompiled function, original at `__imp__sub_X` | `SDK:resources/templates/codegen/pch_h.inja:65-87`, `include/rex/hook.h:42-49`, `src/codegen/builders/context.cpp:229-230` |

How the PGR4 projects use them:

- P4R's planned D3D12 mode clears the plugin and graphics and brings up a Plume device in
  `OnPreLaunchModule` (`P4R:src/pgr4_recompiled_app.h:73-85`); its CMake describes it as hooking
  "the guest D3D calls; no Xenos/PM4 emulation" (`P4R:CMakeLists.txt:12-13`). Its Vulkan mode
  supplies `pgr4::render::Pgr4GraphicsSystem` as `config.graphics` (`app.h:130-135`). None of
  the renderer sources are in the repository (`P4R:CMakeLists.txt:23-43` lists files that git
  history never contained).
- MTR keeps `rexgpu-xenos` and patches it (4,789-line `MTR:patches/rexglue-v0.10.0.patch`,
  36 files): timing stats, constant-write dedup, texture reuse pools, vblank deadline timer,
  present vsync (`docs/research/pgr4-mtr-plume-notes.md` §2.1). It changes no EDRAM, resolve or
  tiling behaviour.

### 2.2 Two ways to host an FM2-style renderer

**A. `rexgpu-native` plugin (recommended).** A shared library target in the PGR4 repo that
compiles the SDK's base `graphics_system.cpp`, `command_processor.cpp`, `register_file.cpp`,
`registers.cpp`, `packet_disassembler.cpp` and `format/ucode.cpp` (`SDK:src/graphics/CMakeLists.txt:7-31`)
plus our renderer, exports `rex_gpu_create`, and subclasses `GraphicsSystem` and
`CommandProcessor`.
- Reuses ReXGlue's PM4 parser, predication, MMIO, interrupts and swap packet, which already run
  PGR4 in P4R v1 and MTR.
- `--gpu_plugin=xenos` and `--gpu_plugin=native` run the same executable, same generated code,
  same save, so frame captures and frame times compare directly.
- Cost: base sources compiled twice (once per plugin); tied to ABI v1
  (`gpu_plugin.h:32`) and to the SDK's protected members, so SDK upgrades need a check.

**B. App-supplied `IGraphicsSystem` (P4R's Vulkan shape).** Port FM2's own CP into the app and
implement `IGraphicsSystem` directly.
- No dependency on hidden SDK classes.
- Re-implements what ReXGlue already has; MMIO must still be registered with
  `AddVirtualMappedRange` and interrupts dispatched through `FunctionDispatcher::ExecuteInterrupt`
  (`graphics_system.cpp:140-147`, `:314-316`). No A/B against Xenos inside one binary.

**C. Hook the guest D3D API (P4R's D3D12 mode).** Only viable if §4 shows most PGR4 draws go
through the D3D draw functions. FM2's measurement is the warning: 9,600 GPU draws vs 600 API
draws in 5 s (`FM2:FM2-native-scope.md:77-82`).

### 2.3 Interface mapping (FM2 hook → ReXGlue plugin)

| FM2-ReGPU | ReXGlue `rexgpu-native` |
|---|---|
| `GpuInit` seeds display registers (`gpu.cpp:1219-1230`) | `CommandProcessor::SetupContext` override (`command_processor.h:142`); registers live in `RegisterFile` |
| `VdInitializeRingBuffer` / `VdEnableRingBufferRPtrWriteBack` (`gpu.cpp:1278-1292`) | handled by the base (`GraphicsSystem::InitializeRingBuffer`, `graphics_system.h:74-75`) |
| `VdSetGraphicsInterruptCallback` + `InterruptLoop` (`gpu.cpp:1270-1276`, `:466-479`) | base `SetInterruptCallback` + `DispatchInterruptCallback` (`graphics_system.h:77-78`) |
| `VblankLoop` at 2x cap (`gpu.cpp:1198-1216`) | base vsync worker (`graphics_system.cpp:150-176`); pacing tuned in `IssueSwap` |
| `CommandProcessorLoop` / `ExecutePacket` (`gpu.cpp:1099-1181`, `:624-929`) | base `WorkerThreadMain` / `ExecutePacketType*` (`command_processor.cpp:200-256`, `:665-892`) |
| `WriteRegister` gamma LUT intercept → `RendererSetGammaEntry` (`gpu.cpp:565-574`) | `OnGammaRamp256EntryTableValueWritten` / `OnGammaRampPWLValueWritten` (`command_processor.h:165-166`) |
| `RendererOnShaderLoad` / `RendererSetShader` (`renderer.h:5-7`) | `LoadShader(type, guest_address, host_address, dwords)` (`command_processor.h:217-218`) |
| `RendererOnDraw(initiator, dmaBase, dmaSize)` (`renderer.h:8`) | `IssueDraw(prim, count, IndexBufferInfo*, major_mode_explicit)` (`command_processor.h:220-221`); initiator, DMA base and size are already in the register file (`command_processor.cpp:1302`, `:1323-1335`) |
| resolve detected inside `DrawImpl` (`renderer.cpp:5358-5363`) | `IssueDraw` checks `RB_MODECONTROL.edram_mode == kCopy` and calls `IssueCopy()` (pattern at `SDK:src/graphics/d3d12/command_processor.cpp:2281-2284`) |
| `RendererOnSwap(frame, frontBuffer)` (`renderer.h:10`) | `IssueSwap(frontbuffer_ptr, width, height)` (`command_processor.h:109-110`) |
| frame limiter in `VdSwap` (`gpu.cpp:1415-1426`) | in `IssueSwap` (the guest thread is not blocked there, so the limiter must block the CP or pace the vblank; `[INFERENCE]` needs testing) |
| `EVENT_WRITE_ZPD` fake counts (`gpu.cpp:734-756`) | base already fakes it (`command_processor.cpp:1249-1283`); virtual if PGR4 needs real queries |
| `GpuLookahead` dry run (`gpu.cpp:1060-1095`) | reimplement in the subclass from `primary_buffer_ptr_`, `read_ptr_index_`, `write_ptr_index_` (`command_processor.h:248-256`) and `TranslatePhysical` |
| predication state `binMask/binSelect` | `bin_mask_`/`bin_select_` (`command_processor.h:263-264`) |
| `gpureg::Get` over the guest page (`regs.h:21-27`) | adapter over `RegisterFile::values` (host-endian, no byte swap) |
| `PhysicalSection()` heap import (`renderer.cpp:974-1000`) | `OpenExistingHeapFromAddress1(memory->physical_membase(), 0x20000000)`; vkd3d-proton implements it under Win32/Proton (`VKD:libs/vkd3d/device.c:7507-7541`) |
| `InitializeShaderStorage` (none; own `Documents\fm2\shadercache`) | `CommandProcessor::InitializeShaderStorage(cache_root, title_id, blocking)` (`command_processor.h:115-116`) |
| presenter (own window and swapchain, `presenter.cpp`) | `GraphicsSystem::CreateProvider` + SDK presenter, or own swapchain on the SDK window (`rex_app.cpp:336-345`) |

---

## 3. Graphics API: D3D12, Vulkan, Plume

### 3.1 Recommendation

Write the renderer against D3D12 first and run it on the Deck through Proton (vkd3d-proton).
Keep all command recording behind one small internal interface, shaped like FM2's
`RecordedList` op set (`FM2:src/gpu/recorded_list.h:123-128`), so a Vulkan backend can be added
later without touching draw translation. Generate shaders as HLSL and compile with DXC so the
same source yields DXIL and SPIR-V.

### 3.2 Why not Plume first

Plume's interface covers most of what is needed (push constants, bindless ranges, MSAA resolve,
copy regions, timestamps; `PLM:plume_render_interface.h:114-252`), but it is missing things this
renderer uses or will need:

- no pipeline cache or library (FM2 depends on `ID3D12PipelineLibrary`, `renderer.cpp:2237-2361`);
- no occlusion queries (research notes §4.4), no predication, no indirect draws;
- no command-time blend factor or stencil reference setters (stencil ref is baked into the PSO,
  `PLM:plume_d3d12.cpp:2468`, `:3145`); FM2 sets both per draw (`recorded_list.h:123-128`);
- Vulkan dynamic state limited to viewport, scissor, depth bias (`PLM:plume_vulkan.cpp:1611-1617`);
  no dynamic rendering (`:1678-1733`);
- strip-cut index fixed at `0xFFFF` on D3D12 (`plume_d3d12.cpp:3096-3098`);
- D3D12 backend asserts DXIL (`plume_d3d12.cpp:2970`); SM 5.1 DXBC is behind an undefined
  `SM_5_1_SUPPORTED` (`:3671`), while P4R builds SM 5.1 DXBC (`P4R:CMakeLists.txt:62-66`), so P4R
  presumably uses a patched Plume `[INFERENCE]`;
- the README says it is not production-ready until barriers and transitions are refined
  (`PLM:README.md:1-3`).

Porting FM2-ReGPU's D3D12 code to Plume would mean: replacing about 116 recorded-list sites and
71 resource-creation sites (research notes §8), adding pipeline caching and dynamic stencil
ref/blend factor to Plume or baking them into PSO keys, swapping FXC for DXC
(`renderer.cpp:1840-1841`), and dropping or re-backing DLSS/FSR3. That is the same work as
writing a Vulkan backend behind our own interface, with an extra dependency.

### 3.3 Is vkd3d-proton adequate for the Deck?

Yes as a first target, with checks:

- It implements `ID3D12PipelineLibrary` but by default ignores the app's blobs in favour of its
  own disk cache (`vkd3d-proton.cache`); `VKD3D_CONFIG=pipeline_library_app_cache` lets the app
  manage it ([vkd3d-proton README](https://github.com/HansKristian-Work/vkd3d-proton)). Either way
  first-run compile cost stays.
- It implements `OpenExistingHeapFromAddress1` as a custom write-back heap over the host pointer
  (`VKD:libs/vkd3d/device.c:7507-7541`). `OpenExistingHeapFromFileMapping` maps a temporary view
  and unmaps it after creating the heap (`device.c:7586-7607`), so prefer the address variant on
  the guest's permanent physical view `[INFERENCE: lifetime of the imported pages after
  UnmapViewOfFile is not verified]`.
- DXBC goes through dxil-spirv's DXBC front end
  ([dxil-spirv](https://github.com/HansKristian-Work/dxil-spirv)), so FXC output works; DXIL works
  too.
- Evidence that emulation is the Deck problem: a ReXGlue-based recomp (Lego Dimensions) on a
  Deck LCD under Proton ran busy scenes at about 13-14 real fps, GPU-bound, with per-frame
  memexport syncs and 10 MB/frame of CPU-write re-uploads
  ([NeverCookFirst/DimensionsRecomp#1](https://github.com/NeverCookFirst/DimensionsRecomp/issues/1)).
  The same report found the presenter submitting repeated presents, so Steam's overlay read
  55-78 fps; measure real guest frames, not presents.

### 3.4 Deck-specific risks

Deck: Zen 2 4 cores/8 threads at 2.4-3.5 GHz, 8 RDNA 2 CUs up to 1.6 GHz (1.6 TFLOPS), 16 GB
shared LPDDR5 at 88 GB/s on the LCD model, 102.4 GB/s on OLED
([Valve specs](https://www.steamdeck.com/en/tech),
[Valve tech specs PDF](https://cdn.cloudflare.steamstatic.com/steamdeck/images/press/techspecs/steamdeck_techspecs.pdf),
[Wikipedia](https://en.wikipedia.org/wiki/Steam_Deck)).

| Risk | Why | Mitigation |
|---|---|---|
| First-run shader and pipeline compile | FM2 needs about 3.5 min on 16 threads for ~4,800 shaders with FXC `SKIP_OPTIMIZATION` (`FM2:README.md:49-51`, `renderer.cpp:1840-1841`); on 8 slower threads expect several times longer `[INFERENCE]`, plus vkd3d-proton/RADV pipeline compile on top | install-time compile on the Deck itself behind a progress screen; compile on 6 of 8 threads; persist both our cache and `vkd3d-proton.cache` (`VKD3D_SHADER_CACHE_PATH`); lookahead precompile as in FM2 |
| CPU cost of recompiled game code | the 360 ran PGR4 on three 3.2 GHz cores; FM2's reference port "needs a fast CPU" for 120 Hz (`FM2:FM2-native-scope.md:17`); 60 fps PGR4 needs MTR's game patch and a collision step (`MTR:AGENTS.md:9-10`) | measure guest CPU time per frame on the dev PC first, then on a Deck; keep CP and render translation off the game threads; skip repeated tile translation (below) |
| CP and translation thread cost | FM2 translates every tile pass's draws (§1.2 step 5); with 2 tiles that doubles CPU translation | translate the first tile pass only, if §4 shows tile passes replay identical draws |
| GPU at 1280x800 | RGBA16F single colour target per draw (`renderer.cpp:173`) is bandwidth-heavy on 88 GB/s; 2x MSAA likely (§4) | use RGBA8/R11G11B10 where the guest format allows; MSAA off or 2x; FSR1 from 960x600 if needed |
| Aspect | Deck is 16:10; FM2's Hor+ code is FM2-specific (`camera_aspect.cpp`, `renderer.cpp:5631-5644`) | ship 16:9 letterboxed at 1280x720 first |
| Presentation | presenting without a new guest frame inflates fps counters (DimensionsRecomp#1) | present only on `IssueSwap` |

---

## 4. Measurement plan (first work once the disc is on the build host)

Goal: decide between design C (API hooks) and A (PM4-driven) with numbers, and size the tiling
problem. Build base: MTR-PGR4 from source, because P4R's repository cannot build (missing
sources, `docs/research/pgr4-mtr-plume-notes.md` §1.1) and MTR keeps the stock Xenos plugin.
Use the base game XEX and MTR's manifest (`MTR:pgr4_recompiled_manifest.toml:8-11`).

### 4.1 What exists already

- `--perf_log_csv=<file>` writes per-frame `frame_time_us`, `fps`, `draw_calls` and cache
  counters, but only in non-Release builds (`SDK:src/core/perf/counter.cpp:22-23`,
  `src/core/CMakeLists.txt:133-137`). `draw_calls` counts host draws in the Xenos D3D12 backend
  (`SDK:src/graphics/d3d12/command_processor.cpp:2629`, `:2688`).
- MTR adds `--gpu_command_stats`, `--gpu_slow_frame_stats`, `--d3d12_draw_timing_stats` (timing
  only) (`MTR:patches/rexglue-v0.10.0.patch:1398-1414`, `:1746-1750`).
- Tracy (`SDK:CMakeLists.txt:24`), `--dump_shaders` (`src/graphics/flags.cpp:27`).
- Nothing counts indirect-buffer depth, predicated packets or tiles.

### 4.2 Instrumentation to add (one SDK patch + one hook file)

In `CommandProcessor` (patch applied in the measurement checkout only), behind a cvar
`--gpu_stream_stats`:

1. Per frame (between `XE_SWAP`s, `command_processor.cpp:929`): draws by indirect-buffer depth
   (0 = primary ring, 1+ = inside `INDIRECT_BUFFER`, `:967`).
2. Per indirect buffer: address, size, and a 64-bit hash of its contents. A buffer whose address
   and hash recur across frames is prerecorded. Report draws inside recurring buffers vs fresh
   ones.
3. Predication: packets with the predicate bit executed and skipped (`:744-753`); distinct
   `bin_select_` values per frame (the tile count); `SET_BIN_*` packet counts (`:831-863`);
   `EVENT_WRITE_EXT` count (`:1221`).
4. Per tile pass: number of draws and a hash of (shader pair, draw initiator, DMA base) per
   draw, to check whether tile passes replay identical draw lists.
5. Resolves per frame (`RB_MODECONTROL` copy mode) with destination format and size; front
   buffer size and format at `XE_SWAP`; render target formats and MRT usage
   (`RB_COLOR1..3_INFO`).
6. `IM_LOAD`/`IM_LOAD_IMMEDIATE` count and distinct shader hashes per frame.

In the app, count guest D3D API draw calls with weak-symbol hooks:

7. Find PGR4's draw-packet builders. Statically: functions in the generated C++ that store a
   type-3 `DRAW_INDX` (`0xC0xx2200`) or `DRAW_INDX_2` (`0xC0xx3600`) header. Dynamically: log the
   guest return address of the thread writing the ring when a draw packet is written. FM2 found
   two such functions (`FM2:src/hooks/d3d_debug.cpp:149-163`).
8. Hook each with `REX_HOOK_RAW(sub_X)` that increments a counter and calls `__imp__sub_X`
   (`SDK:include/rex/hook.h:49`, `pch_h.inja:82-86`). Also hook the command-buffer
   record/replay functions if found (names in the XDK: `BeginCommandBuffer`/`RunCommandBuffer`
   style; addresses unknown).

### 4.3 Runs

On the dev PC (i9-13900K + RTX 3090) at 1280x720, `--gpu_plugin=xenos`, Release-with-counters build, 3 runs each:
attract mode/main menu idle (30 s), garage, a race start on one fixed track and car (60 s
throttle via scripted input). Record: guest frames, `IssueDraw` count, API draw count, ratio,
tiles per frame, prerecorded share, resolves, distinct shaders, `frame_time_us` CSV, CPU time
of the CP thread and of game threads (Tracy). Store numbers in `the development notes`.

### 4.4 Decision rules

| Result | Design |
|---|---|
| API draws ≥ 95% of GPU draws, no prerecorded buffers | C is viable; still prefer A for the A/B oracle unless P4R v2 ships a working, licensed C |
| prerecorded or non-API draws > 5% | A |
| tile passes replay identical draw lists | translate tile pass 1 only, at full size |
| MRT or formats outside FM2's ten | budget for them in phase 3 |

PGR4 is reported as 1280x720 with 2x MSAA on the 360 (community figures,
[Beyond3D thread](https://forum.beyond3d.com/threads/project-gotham-racing-4.36382/page-9)).
1280 x 720 x 2 samples x 8 bytes (32-bit colour + 32-bit depth) = 14.7 MB, more than the
10 MiB of EDRAM, so predicated tiling with at least two tiles is expected `[INFERENCE]`.

---

## 5. Phases

Effort is in agent-days of focused work plus review, with wide error bars. Reference point: one
author wrote FM2-ReGPU's 22,302 lines of `src/` in 111 commits over five days
(2026-10-16..20; `git log` of `FM2:`), with design already proven by Xenia `[INFERENCE: AI
assistance likely but not stated]`.

| # | Phase | Deliverable | Oracle | Effort |
|---|---|---|---|---|
| 0 | Measure (§4) | numbers in the worklog; design decision recorded in `the development notes` | counters cross-checked against `draw_calls` from `--perf_log_csv` | 2-4 |
| 1 | **Smallest milestone: plugin skeleton** | `rexgpu-native` builds beside `rexgpu-xenos`; `--gpu_plugin=native` boots PGR4 to the main menu with no rendering; `IssueDraw` counts draws, `IssueSwap` paces and shows the guest front buffer read from memory (black until resolves exist) | same frame count and draw count per frame as the Xenos plugin over 30 s of attract mode; no hang (CP read pointer advances, interrupts fire) | 3-5 |
| 2 | First native frame: menus | ucode → HLSL emitter (new code), DXC → DXIL; host render targets per EDRAM base; resolves as copies; textures for the formats menus use; present from the resolved front buffer at any size | screenshot of the same menu frame from both plugins; per-pixel diff after tonemap below a threshold (e.g. PSNR > 30 dB) except known differences | 10-15 |
| 3 | In-race rendering | MRT if used, full format coverage, cube and volume textures if used, MSAA, depth formats, tile-pass-1-only translation, occlusion handling | captures at fixed race frames vs Xenos; draw-count parity; no missing-shader skips logged | 15-25 |
| 4 | Speed | install-time shader discovery and compile (scan like `FM2:src/gpu/static_shaders.cpp`), pipeline library, lookahead precompile, CP/translation split | frame-time CSV: 1% low and max frame time over a 60 s race on the dev PC at 1280x800 vs Xenos; zero compile stalls on a second run | 8-12 |
| 5 | Deck | Proton run on a Deck at 1280x800; settings preset; letterbox/16:10 | frame-time CSV on the Deck; real guest frames, not presents | 4-8 (needs a Deck) |
| 6 | Vulkan (optional) | second backend behind the recording interface, SPIR-V from the same HLSL; native Linux build | same captures as phase 3 on Vulkan | 10-15 |

### 5.1 Splitting the work across agents

One integration owner owns the plugin target, `IssueDraw` dispatch and the merge branch.
Other agents own one module each, in separate checkouts under `C:\recomp\builds\pgr4\<agent>`,
and hand back commits:

| Owner | Files (new, in the PGR4 checkout) | Verification they run |
|---|---|---|
| Integrator | `native/plugin_main.cpp`, `native/graphics_system.*`, `native/command_processor.*`, CMake | phase 1 parity runs |
| Measurement | SDK stats patch, `src/d3d_draw_hooks.cpp`, CSV tooling | §4 runs; publishes numbers |
| Shaders | `native/ucode_hlsl.*`, shader cache, AOT scanner | compile every shader from `--dump_shaders` output of the Xenos run; zero compile errors; spot-check disassembly |
| Textures | `native/texture_*` (formats, untiling with the SDK's `GetTiledOffset2D/3D` in `rexcore`, `SDK:include/rex/graphics/pipeline/texture/util.h:281-296`) | unit tests: untile known textures dumped from guest memory and compare to the Xenos backend's uploads |
| Render targets and resolves | `native/render_targets.*`, `native/resolve.*` | per-resolve image dumps vs Xenos for the same frame |
| Present and pacing | `native/present.*`, frame limiter, capture hotkey | frame-time CSV; capture tool output |

Rules: every phase result goes in `the development notes` with the command, build hash and
numbers; captures stay on the build host (they contain game imagery) and only metrics go in git. A
module merges only when the integrator's A/B run on the build host does not regress draw-count parity or
frame time.

### 5.2 Capture oracle

Add a cvar to both plugins, e.g. `--capture_frames=600,1200`, that writes the presented image
(and optionally each resolve) as PNG at those guest frame numbers. Run the same scripted input
(an input script player still has to be written; the SDK has none that this study found, and FM2
uses `FM2_TOUR`, `FM2:benchmark.cmd:17`) under each plugin.
Guest timing differs between runs, so compare static scenes (menus, paused race, replay camera)
first; moving scenes need a tolerance or frame matching by game timer `[INFERENCE]`.

---

## 6. Licensing

| Component | License | Evidence | What it allows |
|---|---|---|---|
| FM2-ReGPU | none declared | no LICENSE/COPYING file; GitHub API reports `license: null`; no header or README license statement (`docs/research/fm2-regpu-renderer-map.md` §7) | reading and learning only; default copyright applies, so copying or adapting its code needs the author's permission `[INFERENCE: standard default for unlicensed code; not legal advice]` |
| ReXGlue SDK | BSD-3-Clause, with portions from Xenia | `SDK:LICENSE` (copyright Tom Clay, "Portions ... derived from the Xenia project") | use, modify, ship in source or binary with the copyright notice and license text; no endorsement |
| Xenia | BSD-3-Clause | [xenia LICENSE](https://github.com/xenia-project/xenia/blob/master/LICENSE) | same |
| XenonRecomp | MIT | FM2 submodule `thirdparty/XenonRecomp/LICENSE.md`; [repo](https://github.com/hedge-dev/XenonRecomp) | use with notice |
| Plume | MIT | `PLM:LICENSE` | use with notice |
| PGR4-Recomp | MIT | `P4R:LICENSE:1-3` | use with notice; v2 renderer license unknown until published |
| MTR-PGR4 | MIT for its own code; SDK and third-party code keep theirs | `MTR:LICENSE:1-27`, `MTR:LICENSES/` | use with notices |
| vkd3d-proton | LGPL-2.1 | [repo](https://github.com/HansKristian-Work/vkd3d-proton) | ships with Proton; we do not redistribute it |
| FidelityFX / DLSS / FFmpeg (FM2 deps) | MIT / NVIDIA SDK license / LGPL | `FM2:CMakeLists.txt:77`, `:111`; `FM2:README.md:126-134` | not needed for the first milestones |

Consequences:

- Build `rexgpu-native` from ReXGlue's BSD code and new code. FM2-ReGPU informs the design
  (seam, resolve-as-copy, AOT shader discovery, lookahead) but no code is copied until its author
  grants a license. Asking is the owner's decision.
- If a license arrives, `ucode.cpp`, `hlsl.cpp`, `texture.cpp` and `recorded_list.h` are the
  first files worth reusing (generic Xenos, §1.4).
- Game data, dumps, captures and C++ generated from the XEX stay off git (`AGENTS.md`).

---

## 7. Unknowns

1. PGR4's split of GPU draws vs D3D API draws, and whether it uses prerecorded command buffers.
   Needs §4.
2. Whether PGR4 uses predicated tiling, how many tiles, and whether tile passes replay identical
   draw lists. The EDRAM arithmetic in §4.4 suggests tiling; not measured.
3. PGR4's render target formats, MRT use, MSAA mode, memexport use, occlusion queries, and
   texture formats beyond FM2's ten. MTR shows CPU-written bone buffers fetched through vertex
   fetch constants 94/95 (`MTR:patches/rexglue-v0.10.0.patch:2018-2080`, `:4558-4640`).
4. PGR4's front buffer size and format at swap (FM2 presents only 1280x720 RGBA8 resolves,
   `renderer.cpp:6420-6422`).
5. How long PGR4's shader set takes to compile on a Deck, and how many shaders and pipelines it
   has.
6. Guest CPU time per frame for PGR4 on a Deck at 60 fps (with MTR's 60 fps patch).
7. Whether `OpenExistingHeapFromAddress1` over ReXGlue's physical view works under Proton on
   RADV, and its cost vs the Xenos backend's watched-upload `SharedMemory`.
8. Whether `IssueSwap` pacing alone gives stable 60 fps, given ReXGlue's vsync worker runs at
   the guest refresh rate (`graphics_system.cpp:150-176`) while FM2 needed 2x-rate vblanks
   (`gpu.cpp:1204-1214`).
9. What P4R v2 (due 2026-11-10, `P4R:README.md:9`) ships: whether its renderer is PM4- or
   API-driven in practice, whether sources are published, under what license.
10. Whether FM2-ReGPU's author will license the code.
11. ReXGlue SDK changes after `c94f5eb` that alter `GpuCreateInfo` or the `CommandProcessor`
    virtuals (ABI v1, `gpu_plugin.h:32`).
12. Whether a Steam Deck is available for phase 5 (not in the lab inventory as far as this
    study knows).
