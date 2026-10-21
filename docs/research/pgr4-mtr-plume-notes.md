# PGR4-Recomp, MTR-PGR4 and Plume (raw notes)

Raw notes behind `docs/native-renderer-design.md`, written by a read-only research agent on
2026-10-21. Clones: `beatrixzy/PGR4-Recomp` `38d8d87` (2026-10-20), `MissingThePixel/MTR-PGR4`
`2db3edb` (2026-10-11), `renderbag/plume` `e48a55b` (2026-10-12, tarball, submodules empty).
Paths are relative to `/tmp/recomp/` on the Mac. Nothing was edited or built.

**Headline facts**
- `PGR4-Recomp` contains only 2 source files (`src/main.cpp`, `src/pgr4_recompiled_app.h`). Every `src/gpu/*`, `src/render/*`, `src/profiler.cpp`, `src/content/*`, `src/music_volume_hook.cpp`, `tools/` and `shaders/` file that `CMakeLists.txt` references is missing. `git log --all --name-only` shows none of them was ever committed. The D3D12/Plume renderer's logic is not in this clone.
- `MTR-PGR4` uses no custom renderer. It keeps the stock Xenia-derived Xenos plugin (`rexgpu-xenos`, D3D12) and patches it for performance. It has no resolution-scale code of its own, only `--resolution_scale=2`.
- Plume was cloned at `/tmp/recomp/plume` (renderbag/plume, MIT). The `contrib/*` submodule dirs are empty (volk, VMA, D3D12MA, Vulkan-Headers).

---

## 1. PGR4-Recomp

### 1.1 CMake options (`PGR4-Recomp/CMakeLists.txt`)
- **L11-15, option comments:**
  - `PGR4_D3D12_RENDERER=ON` is "Plume/D3D12 native renderer in src/gpu (hooks the guest D3D calls; no Xenos/PM4 emulation)" (L12-13).
  - `PGR4_NATIVE_RENDERER=ON` is the Vulkan native renderer, needing `PGR4_NATIVE_RENDERER_ROOT` (L14).
  - Both OFF selects the SDK Xenos plugin (L15).
- **L16:** `option(PGR4_D3D12_RENDERER ... OFF)`.
- **L17:** `option(PGR4_NATIVE_RENDERER ... ON)`. The default is the Vulkan native path, which cannot configure without `PGR4_NATIVE_RENDERER_ROOT`.
- **L18:** `PGR4_PLUME_DIR` defaults to a path under another developer's Windows user profile, `.../Documents/GH-II-Recomp/thirdparty/plume`. That points at another recomp project (GH-II), so a GH-II-specific Plume fork is plausible. This is `[INFERENCE]`.
- **L23-28, always-built sources:**
  - `src/main.cpp` exists.
  - `src/music_volume_hook.cpp`, `src/content/dlc.cpp` and `src/profiler.cpp` are **missing**.
- **L30-43, `PGR4_D3D12_RENDERER` sources, all MISSING:** `src/gpu/video.cpp`, `imgui_drawer.cpp`, `renderer.cpp`, `shader_cache.cpp`, `texture_convert.cpp`, `hooks/device.cpp`, `hooks/draw.cpp`, `hooks/shader.cpp`, `hooks/state.cpp`, `fault_trace.cpp`. The hook-file names (`device`, `draw`, `shader`, `state`) show it hooks the guest D3D API rather than the PM4 stream.
- **L54-55:** D3D12 is Windows-only (`FATAL_ERROR` otherwise).
- **L57-60:** requires `${PGR4_PLUME_DIR}/CMakeLists.txt`, then `add_subdirectory(... plume EXCLUDE_FROM_ALL)`.
- **L62-85, host shaders:**
  - ImGui and present shaders `imgui_vs`, `imgui_ps`, `present_vs`, `present_ps` are compiled with `fxc` to **SM 5.1 DXBC** headers (`--Vn g_<stem>_dxbc`).
  - The comment says DXBC is used because "DXIL needs SM 6.0 drivers" (L62-63).
  - The `.hlsl` files in `src/gpu/shaders/` are missing.
- **L87-102, guest shader cache:**
  - XenosRecomp-generated HLSL in `shaders/hlsl/*.hlsl` is built by `tools/shaders/build_shader_cache.py` with `shader_common_sm51.h` into `shader_cache_sm51.bin`.
  - All of `shaders/hlsl/`, `tools/shaders/*` and the XenosRecomp HLSL are missing.
  - This is an ahead-of-time Xenos-to-HLSL shader path, not runtime ucode translation.
- **L103-116:** the same HLSL is built to **DXIL SM 6.0** (`--dxil`) as `shader_cache_dxil.bin`. "the renderer picks it at startup (`ShaderCache::Load`)" (L103-104). Both caches are copied beside the exe (L118-121).
- **L122-124:** links `plume d3d12 dxgi dxguid` and defines `PGR4_D3D12_RENDERER=1 PGR4_NATIVE_RENDERER=0`.
- **L125-128:** the Vulkan path defines `PGR4_NATIVE_RENDERER=1` and includes `${PGR4_NATIVE_RENDERER_ROOT}/native_renderer/integration/pgr4_native_renderer.cmake`. That is an external tree (and `src/render/graphics_system.h` in the app header) that is not in the repo.
- **L129-132:** `rexglue_setup_target(pgr4_recompiled GPU_PLUGINS xenos)`.
- **L134-145:** `pgr4_launcher` built from `tools/launcher/launcher.cpp` and `launcher.rc` (missing).
- `CMakePresets.json:203-212` adds the preset `win-amd64-release-d3d12` (`PGR4_D3D12_RENDERER=ON`, `PGR4_NATIVE_RENDERER=OFF`). L194-202 adds the preset `win-amd64-release-xenos` (`PGR4_NATIVE_RENDERER=OFF`).
- `include(generated/rexglue.cmake)` at L20 is missing in this repo, so it is not self-contained.
- **What exists vs missing:** only `src/main.cpp`, `src/pgr4_recompiled_app.h`, `pgr4_recompiled_manifest.toml`, `CMakePresets.json`, the docs and `LICENSE`. Everything in the lists above is missing.

### 1.2 App class (`src/pgr4_recompiled_app.h`, 168 lines)
- `src/main.cpp:3-7`: includes `generated/default/pgr4_recompiled_init.h` and the app header, then `REX_DEFINE_APP(pgr4_recompiled, Pgr4RecompiledApp::Create)`.
- **Creation, L35-43:** `class Pgr4RecompiledApp : public rex::ReXApp`. `Create()` returns `new Pgr4RecompiledApp(ctx, "pgr4_recompiled", PPCImageConfig)`.
- **Three compile-time modes** (`#if` at L26-33, L72, L129, L167).
- **Overrides common to all modes:**
  - `OnConfigurePaths(rex::PathConfig&)`, L53-66. It calls `pgr4::content::DumpCvarsAndExitIfRequested()` and `InstallTitleUpdate(...)`. On failure it shows a message box saying Title Update #2 is required. It sets `paths.update_data_root = tu_dir`.
  - `OnLoadXexImage(std::string&)`, L68-70. It sets the image to `game:\TitleUpdate\default.xex`. `kTitleUpdateDir = "TitleUpdate"` is at L51, and the comment at L46-50 explains the TU's `.xexp` is applied at load.
- **D3D12/Plume mode (L72-128):**
  - `OnPreSetup`, L73-78. It runs `config.gpu_plugin.clear(); config.graphics = nullptr;`. The comment says null graphics "makes Runtime::Setup skip SDK GPU setup; the Plume device is brought up in OnPreLaunchModule (same split as re:Blue)".
  - `OnPreLaunchModule`, L80-85. It calls `pgr4::gpu::Video::CreateHostDevice(window()->GetNativeWindowHandle())` and `InstallOverlayDrawHook()`.
  - `OnPostLaunchModule(rex::system::XThread*)`, L88-92. It calls `pgr4::StartProfiler()`, `content::InstallDLC()` and `content::DumpGuestImage()`.
  - `OnCreateImmediateDrawer()`, L96-98. It returns `pgr4::gpu::ImGuiOverlayDrawer`. This is the SDK's "detached overlay mode", documented at `rexglue-v0.10.0/include/rex/rex_app.h:171-190`.
  - `InstallOverlayDrawHook()`, L103-115. It calls `Video::SetOverlayDrawHook(...)` with a `(plume::RenderCommandList*, plume::RenderFramebuffer*, w, h)` lambda. The lambda marshals to the UI thread with `app_context().CallInUIThreadSynchronous` and draws via `imgui_drawer()->Draw(OverlayDrawContext)`.
  - `OnWindowPixelSizeChanged`, L117-119, and `OnWindowRestored`, L121, both call `Video::RequestResize()`.
  - `OnShutdown`, L123, calls `Video::Shutdown()`.
  - `CreateAchievementNotificationDialog`, L125-128, returns nullptr.
- **Vulkan "Pgr4GraphicsSystem" mode (L129-166):**
  - `OnPreSetup`, L130-135. It does `auto graphics = std::make_unique<pgr4::render::Pgr4GraphicsSystem>(); native_graphics_ = graphics.get(); config.gpu_plugin.clear(); config.graphics = std::move(graphics);`. This is the only place a custom `rex::system::IGraphicsSystem` is supplied. `Pgr4GraphicsSystem` is declared in `render/graphics_system.h`, which is missing.
  - `OnPostSetup`, L137-144. It calls `AttachWindow(window()->GetNativeWindowHandle())` and `LoadShaderDirectory(current_path() / "pgr4-native-shaders")`.
  - `OnWindowPixelSizeChanged` and `OnWindowRestored`, L146-156, call `ResizeSwapChain()`.
  - `OnShutdown`, L158, clears `native_graphics_`.
- **Xenos mode, L167-174:** `OnPreSetup` sets `config.gpu_plugin = "xenos"` if it is empty.
- **SDK interfaces behind these overrides** (from the local `rexglue-v0.10.0`):
  - `RuntimeConfig` is at `include/rex/runtime.h:66-70`: `std::unique_ptr<system::IGraphicsSystem> graphics; std::string gpu_plugin;`.
  - The `ReXApp` virtuals are at `include/rex/rex_app.h`: `OnPreSetup` L99, `OnLoadXexImage` L102, `OnPostSetup` L105, `OnShutdown` L111, `OnConfigurePaths` L115, `OnPostLoadXexImage` L155, `OnPreLaunchModule` L159, `OnPostLaunchModule` L163, `OnGuestThreadExit` L167, `OnCreateImmediateDrawer` L190, `OnWindowPixelSizeChanged` L201, `OnWindowRestored` L219, `CreateAchievementNotificationDialog` L227.
  - `IGraphicsSystem`'s virtuals are in `include/rex/system/interfaces/graphics.h`: `SetupPresentation` (L43), `SetupGuestGpu` (L48), `has_presentation`, `InitializeRingBuffer`, `EnableReadPointerWriteBack`, `Shutdown`, and others.

### 1.3 Hooks into guest D3D functions
- **No D3D hook addresses or names are in this clone.**
  - The only pointers to them are `CMakeLists.txt:12-13` and `pgr4_recompiled_manifest.toml:13-15`, which includes `"config/d3d_functions.toml"` (missing). That file is the likely home of the named D3D function list.
  - A grep for `D3DDevice|d3d_functions|BeginTiling|EndTiling|RunCommandBuffer|CommandBuffer|PM4|predicat|resolve|edram` in PGR4-Recomp matched only those two spots.
  - `src/gpu/hooks/{device,draw,shader,state}.cpp` are the likely hook sites. `app.h:80-85` (`CreateHostDevice` at `OnPreLaunchModule`) and `rex_app.h:178-181` ("the guest D3D device-creation hook") imply a device-creation hook, but the function and address are unknown.

### 1.4 Config and flags
- `config.gpu_plugin` is cleared in Plume and Vulkan modes (`app.h:76,133`) and set to `"xenos"` in Xenos mode (`app.h:170-172`).
- `Setup.md:25` gives the SteamOS launch arguments: `--game_data_root "..." --execute_unclipped_draw_vs_on_cpu --gpu_plugin="xenos"`. It says all of them are required, and the game will not launch without them.
- `Setup.md:14-16` uses a `run.bat`.
- `--content_root` appears in the TU error text at `app.h:59-61`.
- `pgr4::content::DumpCvarsAndExitIfRequested` (`app.h:54`) is a cvar-dump facility. The other cvars are unknown because their sources are missing.

### 1.5 Docs and license
- `LICENSE` L1-21: **MIT**, "Copyright (c) 2026 Bea" (L3).
- `README.md:9`: "v2 is in the works! Release will be around November 10th!" `README.md:14` lists the known issue "Lotus Espirit Essex Turbo doesn't seem to load and freezes the game, solution will be implemented in v2, alongside native rendering."
- `Future-plans.md:4`: "[x] Native Renderer (Complex, Plume will be the base of this.), Included in v2."
- `Status.md:11`: "v2 - Quality of life patches, Native Rendering and Netplay". It lists v1.1 as "update ReXGlue SDK + fix AVX1 device support".
- Credits at `README.md:21-26`: ReXGlue SDK, @Creesic (Tera), xextool, Goopie, VGEsoterica, GenerationAmiga.

### 1.6 Recomp manifest (`pgr4_recompiled_manifest.toml`, 95 lines)
- L1: generated by ReXGlue v0.9.0.1-dev. L5: `sdk_version = "0.10.0"`.
- **Entrypoint (L9):** `../PGR4-Files/TitleUpdate/default.xex`, which is the Title Update #2 XEX.
- **`setjmp_address = 0x828de260` (L11), `longjmp_address = 0x828de7e0` (L12).**
- **Function counts:**
  - L18-41, hand-listed entry functions: **24**.
  - L44-95, "this-adjust thunks (addi r3,r3,-N; b) reached through vtables": **52**.
  - **Total: 76.**
- **Entry functions (L18-41):** 0x824DAB48, 0x8254B090, 0x829F75A0, 0x829191D0, 0x8291E2B0, 0x82919C48, 0x827348D0, 0x828033d8, 0x82931e78, 0x82803850, 0x828036d8, 0x82934650, 0x827e30a8, 0x828F8E88, 0x8278AECC, 0x827854E8, 0x82739968, 0x8277BA24, 0x822E5C00, 0x8274FD78, 0x8274FD88, 0x8278C8C0, 0x8291BD58, 0x8291BD60.
- **Thunks (L44-95):** 0x827348E0/F0, 0x82734918/20, 0x8273AEA8/B0/B8, 0x8274FC68/70/88/B8/C8/D0/E0/E8/F0, 0x8274FD00/28/30/40/60/68/70/90/98, 0x8276CC30/50/60/68/70, 0x8277D008/10, 0x82782CA8/B8/C0/E0/E8, 0x8278B5C0/20/28/38, 0x8278C8B8, 0x827966D0, 0x82919B18, 0x8291BCF8, 0x8291BD20/28/30/48/70, 0x829468B0/B8.
- **No named D3D function addresses are in this file.** They would be in the missing `config/d3d_functions.toml`.

---

## 2. MTR-PGR4

Origin: `README.md:9` and `THIRD_PARTY.md:3-7` say it forked from `beatrixzy/PGR4-Recomp` at commit `464cc5b6…`. The SDK is pinned to ReXGlue v0.10.0, commit `f5337cdc…` (`scripts/setup-sdk.ps1:5`).

### 2.1 `patches/rexglue-v0.10.0.patch` (4789 lines, 36 files)
`git apply`ed by `scripts/setup-sdk.ps1:22`. The extra new files in `sdk-overrides/` are copied over the SDK at L24-30.

**Graphics / presentation (the relevant part)**

| Patch lines | File | Change |
|---|---|---|
| 56-72 | `include/rex/graphics/command_processor.h` | Adds `write_ptr_update_ns_` and the `slow_frame_wait_*` stat members. |
| 73-185 | `include/rex/graphics/d3d12/texture_cache.h` | Adds a texture-resource reuse pool (`TextureResourceShape` tuple, `ReusableTextureResource`, `TakeReusableTextureResource` / `RecycleTextureResource`), `ResourceFrameStats`, and D3D12MA allocator members. |
| 186-279 | `include/rex/graphics/pipeline/texture/cache.h` | Adds `FindOrCreateTexture(key, bool* created_out)`, `recent_texture_lookups_[256]`, the eviction/churn diagnostic sets, and `texture_bindings_need_retry_`. |
| 280-301 | `include/rex/graphics/shared_memory.h` | Lock type change. |
| 1355-1374 | `src/graphics/CMakeLists.txt` | Adds `thirdparty/d3d12ma/D3D12MemAlloc.cpp` and its include path to `rexgpu-xenos`. |
| 1375-1729 | `src/graphics/command_processor.cpp` | See bullets below this table. |
| 1730-2344 | `src/graphics/d3d12/command_processor.cpp` | See bullets below this table. |
| 2345-2423 | `src/graphics/d3d12/shared_memory.cpp` | Adds timing and a "Slow shared-memory upload" log (L2409-2420), with no behaviour change. |
| 2424-2853 | `src/graphics/d3d12/texture_cache.cpp` | See bullets below this table. |
| 2854-2946 | `src/graphics/graphics_system.cpp` | Adds a vblank deadline timer and stats (see bullets). |
| 2947-3433 | `src/graphics/pipeline/texture/cache.cpp` | See bullets below this table. |
| 3434-3715 | `src/graphics/shared_memory.cpp` | `RequestRanges` fast path, with the same upload-scan cost reduction. |
| 4653-4735 | `src/ui/d3d12/d3d12_presenter.cpp` | New cvar `d3d12_present_vsync` (L4669). |
| 4736-4789 | `src/ui/presenter.cpp` | Adds the cvar `guest_frame_stats` (L4752). |

- **`command_processor.cpp` (generic, 1375-1729):**
  - New cvars: `gpu_command_stats` (L1398), `gpu_slow_frame_stats` (L1402), `gpu_ring_wake_fast` (L1406), `gpu_wait_reg_mem_yield` (L1410), `gpu_wait_reg_mem_high_res` (L1414).
  - `SignalRingWriteEvent` uses `NtSetEventBoostPriority` (L1425-1439), and a high-resolution waitable timer is added (L1441-1465).
  - `WorkerThreadMain` idle wait is 2 ms instead of 5 ms when `fast_ring_wake` (L1521).
  - Shader-constant register writes (`SHADER_CONSTANT_000_X..511_W`, `BOOL/LOOP`, `FETCH_*`) skip the register-info logger (L1598-1612).
  - **PM4 type-0 multi-register packets** use `WriteRegisterRangeFromRing(reader, base_index, count)` (L1622-1631). The comment reads: "PGR4 sends thousands of multi-register constant packets per frame" (L1623-1625).
  - `WAIT_REG_MEM` wait uses sleep, yield or high-res polling (L1660-1700), with a slow-frame stat (L1716-1726).
- **`d3d12/command_processor.cpp` (1730-2344):**
  - New cvars: `d3d12_fence_stats` (L1746), `d3d12_draw_timing_stats` (L1750), `pgr4_sky_draw_trace` (L1756).
  - `SetupContext` now logs "Draw resolution scale active: {}x{}" (L1814-1815). **This is the only resolution-scale touch.**
  - `WriteRegister` skips unchanged constants (L1824-1860).
  - `WriteRegistersFromMem` skips identical bulk constant writes via `range_unchanged()` (L1871-1879, 1888, 1898, 1908).
  - `IssueDraw` restricts vertex-buffer residency checks to out-of-sync fetch constants (L2155-2156). Draw phases are timed at L1985-2010, 2089-2147, 2151-2195.
  - `IssueSwap` adds the "Slow GPU frame" report (L1927-1978).
  - **`pgr4_sky_draw_trace` (L2018-2080):**
    - It triggers on a `kTriangleStrip` draw with `host_draw_vertex_count == 131`.
    - It logs vertex-fetch and texture-fetch constants.
    - It special-cases VS ucode hash `0x72EE8062BF5E764A` with vertex-fetch slot 95 `size == 574`.
    - It reads CPU bone halves at `vfetch(94).address + (bone_first + bone) * 24` for bones 9, 12, 15.
    - It uses shader constant `4*55`.
  - `CheckSubmissionFence` adds fence-wait stats (L2202-2258).
  - `EndSubmission` adds per-phase timing around `command_allocator->Reset()`, `command_list_->Reset`, `deferred_command_list_.Execute(command_list_, command_list_1_)`, `Close`, and `ExecuteCommandLists` (L2259-2318). **This is a `DeferredCommandList` recorded host-side and replayed per submission.**
- **`d3d12/texture_cache.cpp` (2424-2853):**
  - New cvars: `d3d12_texture_shared_heaps` (L2460), `texture_reuse_pool_replace_oldest` (L2463), `texture_resource_allocation_stats` (L2466), `texture_resource_reuse` (L2469), `texture_reuse_pool_limit_mib` (default 64, L2472).
  - D3D12MA allocator creation (L2491-2516), the reuse pool (L2688-2748), and `CreateTexture` placed-resource allocation with a `PushAliasingBarrier` (L2785-2801).
  - 3D-as-2D wrapper creation is hoisted before draw setup (L2598-2599).
- **`graphics_system.cpp` (2854-2946):** new cvars `gpu_vblank_deadline_timer` (L2874) and `gpu_vblank_stats` (L2877). The vblank worker uses `DeadlineTimerWin` (L2887-2889, 2930-2943, defined in `sdk-overrides/include/rex/graphics/util/deadline_timer_win.h`, 35 lines) instead of 1 ms sleeps.
- **`pipeline/texture/cache.cpp` (2947-3433):** cvars `texture_cache_request_stats` (L2963), `texture_keep_valid_bindings` (L2967), `texture_recent_lookup_cache` (L2971). It adds a fast exit when all textures are in sync (L3112-3122), a retry flag on failed loads, and recent-lookup memoization (L3362-3377).
- **Presenter:**
  - `d3d12_present_vsync` switches `Present(0, ALLOW_TEARING)` to `Present(1, ...)` (L4698-4706), and `is_vsync_implicit_out` follows it (L4681, 4690).
  - `guest_frame_stats` / host presents / gaps >25 ms logging is at L4707-4732 and `presenter.cpp:4761-4786`.

**Non-graphics hunks (not renderer-related)**
- **Audio:**
  - `audio_system.h/.cpp` (L1-20, 526-730), `sdl_audio_driver/system.cpp` (731-858), `xma_context.h/.cpp` (21-55, 859-1204), `xma_decoder.cpp` (1205-1255), `xboxkrnl_audio.cpp` (4440-4511), `xmp_app.h/.cpp` (302-370, 3716-4361).
  - Cvars: `audio_*`, `xma_*`, `pgr4_trace_swizel` (L881), `xmp_music_gain` (default 0.35, L3751), `xmp_xenia_playlist_notify` (L3759).
  - `xmp_app.cpp:3870` notes PGR4 registers tracks with placeholder "Title"/"Artist". `xam_input.cpp:4340` notes PGR4 reads `track_number` at byte 976 to detect a new song.
- **Mutex/locking:** `thread/mutex.h` (437-513) and `core/mutex.cpp` (1306-1354) add `rex::thread::global_unique_lock_type`, which replaces `std::unique_lock<std::recursive_mutex>` in the headers at 202-301 and 371-436.
- **Codegen:** `codegen/builders/vector.cpp` (L1256-1291) fixes VMX128 NORMPACKED64 unpack (this matches the generated-code patch in the ps1 at L74-101). `codegen/manifest.cpp` is at L1292.
- **Other:** `xam_input.cpp` (4362-4425, menu buttons mask `0xF30F`), `xam_notify.cpp` (4426), `xboxkrnl_ob.cpp` (4512), `system/CMakeLists.txt` (4525), `system/xmemory.cpp` (4539-4652). The last includes `pgr4_bone_writer_trace` (L4558, 4613-4640).
- **No hunk for:** motion blur, 60 FPS, EDRAM, resolve path changes, presenter scaling or MSAA. A grep for `edram|tiling|predicat` returns nothing relevant. The hunk header "IssueCopy_ReadbackResolvePath" at L2199 is just git context for the fence-stats hunk.

**Frame rate, resolution and blur are not in the patch. They come from:**
- `launcher/Launcher.cs:272-277`:
  - 60 FPS passes `--pgr4_60fps --pgr4_garage_walk_fix --d3d12_texture_shared_heaps --gpu_vblank_deadline_timer --gpu_wait_reg_mem_high_res --no-clear_memory_page_state --gpu_ring_wake_fast --texture_resource_reuse --texture_reuse_pool_limit_mib=256 --texture_cache_memory_limit_soft_lifetime=30`.
  - 1440p passes `--resolution_scale=2`.
  - Fullscreen passes `--fullscreen`.
  - Blur off passes `--pgr4_disable_motion_blur`.
  - Defaults are `ResolutionScale = 1`, `FrameRate = 60`, `Fullscreen = true`, `MotionBlur = true` (L18-19).
- `--resolution_scale` is therefore the stock Xenia/D3D12 draw-resolution scale. The related paths at `texture_cache.cpp:2524` and `pipeline/texture/cache.cpp:3355` are `IsDrawResolutionScaled()`, existing SDK code.
- `src/pgr4_patches.cpp` defines the cvars `pgr4_60fps` (L10), `pgr4_disable_hdr` (L13), `pgr4_disable_motion_blur` (L17), `pgr4_frame_wait_yield` (L21), `pgr4_trace_garage_walk` (L54), `pgr4_garage_walk_fix` (L57).

### 2.2 Guest addresses hooked in MTR-PGR4
These are applied by regex edits to the generated C++ in `tools/apply_game_patches.ps1`. The base game's `default.xex` (not the TU) is used.

- **60 FPS:** `sub_82695738`, `loc_826957EC`. `lwz r10,13580(r31)` becomes `Pgr4Enable60Fps() ? 1 : load` (ps1 L52-53, L62). It follows the Xenia Canary PGR4 patch (THIRD_PARTY L44-45).
- **HDR off:** `sub_829A8960`, `0x829A89EC`, `li r11,1` becomes `Pgr4DisableHdr() ? 0 : 1` (ps1 L54-55, L63).
- **Motion blur off:** `sub_829A9A10`, comment address `0x829A9A9F`, `li r11,1` becomes `Pgr4DisableMotionBlur() ? 0 : 1` (ps1 L64-66). The address is quoted as written; it is odd-aligned, which looks like a typo for a PPC instruction.
- **Frame-wait yield:** `sub_82697970`, `loc_8269798C`. It inserts `Pgr4FrameWaitYield()` before the `db16cyc` comment (ps1 L68-72).
- **Bone decode:** `sub_823BD9D8` (VMX128 NORMPACKED64 fix, registers v10 and v6, ps1 L74-101).
- **Lotus Esprit preview freeze:** `sub_822A53E0`. It adds `pgr4_chain_steps` and `pgr4_cell_steps` limits (4096 and 1024) at `loc_822A5520`, `loc_822A54DC` and `loc_822A5570`, with a goto `loc_822A5588` (ps1 L103-117).
- **Garage walk trace:** `sub_827A6B90` call with `lr = 0x827A7510`, in partition `.85.cpp` (ps1 L119-129).
- **Garage collision step:** `sub_82499578`, in partition `.58.cpp` (ps1 L130-144). `pgr4_patches.cpp:64` checks caller `0x827A7510`.
- **Generated-file partition names** used by the script: `.22`, `.110`, `.16`, `.69`, `.123`, `.85`, `.58` (ps1 L57-61, L129, L143).
- **MTR-PGR4 manifest** (`pgr4_recompiled_manifest.toml`):
  - Plain game XEX: `game-data/default.xex` (L8).
  - `setjmp_address = 0x828a9c50` (L10), `longjmp_address = 0x828aa1d0` (L11).
  - 17 hand-listed functions (L62-78): 0x824B8670, 0x82526280, 0x828DDA98, 0x829C23A0, 0x828E5488, 0x827cd4b8, 0x828fd7c0, 0x827cd930, 0x827cd7b8, 0x828fff98, 0x826ffdb8, 0x827ad090, 0x826c7a08, 0x82756214, 0x82704E30, 0x82746C84, 0x822C5970.
  - The base-game and TU address spaces differ: the TU's `setjmp_address` is 0x828de260. `includes = []` here, so there is no `d3d_functions.toml`.
- **App hooks** (`src/pgr4_recompiled_app.h`, 40 lines): `OnPostLoadXexImage` (L31, `title_handoff::Restore(runtime(), 1)`), `OnGuestThreadExit` (L32, `title_handoff::Complete(runtime(), 1)`), `CreateAchievementNotificationDialog` (L38-44, returns nullptr, with a comment about the presenter's paced vs fast path). The Geometry Wars sibling is `geometry-wars/src/gw_recompiled_app.h:24-37` (`OnLoadXexImage` sets `game:\gw.xex`).

### 2.3 AGENTS.md and README notes (23 lines)
- **L9-10:** "Keep the 30 FPS path intact. Garage walking needs a collision step for short 60 FPS moves; do not bypass collision checks or change global physics timing."
- **L11-13:** PGR4 and Geometry Wars have overlapping guest addresses, separate function tables, and a process handoff.
- **L16-18:** launcher choices for resolution, FPS, fullscreen and blur are independent.
- **L19-20:** verification advice for gameplay changes.
- `README.md:49-51` says it relies on Xenia's renderer through ReXGlue with modifications. `README.md:53-57` lists the known issues: vibration and noise drop at high RPM, small hitches, shadow problems in car interiors, and livery differences.
- `README.md:4-7` advertises 720p/1440p, 30/60 FPS and optional motion blur.

### 2.4 License
- `LICENSE` L1-27: **MIT**, "Copyright (c) 2026 MissingThePixel and MTR-PGR4 contributors".
- L24-27 scope note: it covers new MTR-PGR4 contributions only. ReXGlue, third-party code and game assets keep their own licenses (see `THIRD_PARTY.md`).
- `LICENSES/ReXGlue-BSD-3-Clause.txt` covers the SDK, and `LICENSES/PGR4-Recomp-MIT.txt` covers the upstream code.

---

## 3. Evidence on prerecorded command buffers, predicated tiling, resolves, and D3D tiling

- **No explicit evidence in either repo** for PGR4's use of `BeginTiling`, `EndTiling`, predicated tiling, `D3DCommandBuffer`, `RunCommandBuffer`, or prerecorded command buffers.
  - A grep across `PGR4-Recomp/`, `MTR-PGR4/src`, `tools`, `launcher`, `geometry-wars` and the docs found no match for `BeginTiling|EndTiling|CommandBuffer|RunCommandBuffer|predicat`.
  - The only hits for these terms are `PGR4-Recomp/CMakeLists.txt:13` (`PM4`), `PGR4-Recomp/pgr4_recompiled_manifest.toml:14` (`d3d_functions`), and `Launcher.cs:273` (`gpu_wait_reg_mem_high_res`).
  - `d3d_functions.toml` and `src/gpu/hooks/*` are the missing source of any D3D-level evidence.
- **Indirect PM4/ring-buffer evidence:**
  - PGR4 submits through the ordinary PM4 ring: `CommandProcessor::WorkerThreadMain` / `ExecutePrimaryBuffer` is hit and tuned (patch L1471-1568). `ExecutePacketType0` shows **thousands of multi-register type-0 constant packets per frame** (patch L1614-1633).
  - `WAIT_REG_MEM` is exercised: the game polls a register or memory location with sleep-like waits (patch L1634-1727).
  - A guest frame busy-wait exists at `sub_82697970` / `loc_8269798C` (`apply_game_patches.ps1:68-72`).
- **Resolves:**
  - The only resolve mention in either repo's authored code is the Xenia texture-cache `IsRangeScaledResolved` / `ScaledResolveGlobalWatchCallback` (patch L3416-3433), existing SDK code touched only for the lock type.
  - There is no PGR4-specific EDRAM or resolve change.
- **CPU-written animation buffers:** `pgr4_bone_writer_trace` (patch L4558-4640) and `pgr4_sky_draw_trace` (L2018-2080) show the sky/character bone buffer is written by the CPU and fetched via vertex-fetch constants 94/95.
- **Swap path:** `IssueSwap` ends with `EndSubmission(true)` (patch L1927-1930). `d3d12_submit_on_primary_buffer_end` (patch L1742-1744, existing cvar) submits when the PM4 primary buffer ends.
- **Gap for the design doc:** whether PGR4 uses tiled rendering (360 predicated tiling for 720p+MSAA) cannot be settled from these repos. It would need the missing `d3d_functions.toml` or a guest trace.

---

## 4. Plume (`/tmp/recomp/plume`, renderbag/plume)

### 4.1 License
- `LICENSE` L1-21: **MIT**, "Copyright (c) 2024 renderbag and contributors".
- `contrib/metal-cpp/LICENSE.txt` is bundled. `contrib/{volk,D3D12MemoryAllocator,VulkanMemoryAllocator,Vulkan-Headers}` are **empty directories** (submodules not fetched).
- `README.md:1-3`: a low-level RHI, "bring your own compiler" for shaders. It is "not considered... production-ready or to have stable API until... barrier management and texture transitions are further refined". It was originally made for RT64.

### 4.2 Backend file list
| File | Lines |
|---|---|
| `plume_d3d12.h` / `plume_d3d12.cpp` | 495 / 4259 |
| `plume_vulkan.h` / `plume_vulkan.cpp` | 469 / 4657 |
| `plume_metal.h` / `plume_metal.cpp` | 784 / 4245 |
| `plume_apple.h` / `plume_apple.mm` | 43 / 178 |
| `plume_render_interface.h` | 268 |
| `plume_render_interface_types.h` | 1822 |
| `plume_render_interface_builders.h` | 281 |
| `CMakeLists.txt` | 130 |

- **Build:** `CMakeLists.txt:64-83` always builds Vulkan, adds Metal and Apple files on APPLE, and adds D3D12 plus `contrib/D3D12MemoryAllocator/src/D3D12MemAlloc.cpp` on WIN32. C++17.
- **Options:** `PLUME_D3D12_AGILITY_SDK_ENABLED` (L22, needs `directx-headers` and `directx12-agility`), `PLUME_SDL2/3_VULKAN_ENABLED`, `PLUME_BUILD_EXAMPLES`.
- **Examples:** `examples/triangle` and `examples/cube` (`.hlsl` shaders); `examples/cmake/PlumeShaders.cmake` compiles through DXC and SPIRV-Cross.

### 4.3 Core interface (`plume_render_interface.h`)
- **`RenderInterface`, L254-259:** `createDevice(preferredDeviceName)`, `getDeviceNames()`, `getCapabilities()` returning `RenderInterfaceCapabilities`. That struct is at `types.h:1819-1821` and holds only `RenderShaderFormat shaderFormat` (L1820).
- **`RenderDevice`, L226-252:**
  - `createDescriptorSet` (L228).
  - `createShader(const void*, uint64_t, const char* entryPointName, RenderShaderFormat)` (L229).
  - `createSampler` (L230).
  - `createComputePipeline` (L231), `createGraphicsPipeline` (L232), `createRaytracingPipeline` (L233).
  - `createCommandQueue(RenderCommandListType)` (L234).
  - `createBuffer` (L235), `createTexture` (L236), `createPool` (L238), `createPipelineLayout` (L239).
  - `createCommandFence` (L240), `createCommandSemaphore` (L241), `createFramebuffer` (L242), `createQueryPool(queryCount)` (L243).
  - `getCapabilities` (L247), `getDescription` (L248), `getSampleCountsSupported(format)` (L249), `beginCapture` / `endCapture` (L250-251).
- **`RenderCommandQueue`, L200-211:** `createCommandList()`, `createSwapChain`, `executeCommandLists(lists, waits, signals, fence)`, `waitForCommandFence`.
- **`RenderCommandList`, L114-198:**
  - `begin` / `end`, `barriers(stages, bufferBarriers, textureBarriers)` (L118).
  - `dispatch` (L119), `traceRays`.
  - `drawInstanced` (L121), `drawIndexedInstanced` (L122).
  - `setPipeline` (L123).
  - Per-bind-point `set*PipelineLayout`, `set*PushConstants(rangeIndex, data, offset, size)`, `set*DescriptorSet(set, setIndex)` (L124-133). `setGraphicsRootDescriptor` (L130) is D3D12-only.
  - `setIndexBuffer` (L134), `setVertexBuffers` (L135).
  - `setViewports` (L136), `setScissors` (L137), `setFramebuffer` (L138), `setDepthBias` (L139).
  - `clearColor` (L140), `clearDepthStencil` (L141).
  - `copyBufferRegion` (L142), `copyTextureRegion` (L143), `copyBuffer` (L144), `copyTexture` (L145).
  - `resolveTexture` (L146), `resolveTextureRegion` (L147), `discardTexture` (L150, D3D12 only).
  - `resetQueryPool` (L151), `writeTimestamp` (L152).
- **Not present in `RenderCommandList`:**
  - No predication, no indirect draw/dispatch, no bundles or secondary command lists. Greps for `SetPredication|ExecuteIndirect|DrawIndirect|Bundle|SECONDARY` found nothing.
  - No `OMSetBlendFactor`/blend-constant setter, no stencil-reference setter and no depth-bounds setter. The stencil reference is baked into the pipeline (`d3d12.cpp:2468,3145`, `vulkan.cpp:1602,1609`). `BLEND_FACTOR` exists in `RenderBlend` (`types.h:259-260`), but no setter was found.
- **Pipeline creation:** `RenderGraphicsPipelineDesc`, `types.h:1276-1314`.
  - `pipelineLayout`, `vertexShader`, `geometryShader`, `pixelShader`.
  - `depthFunction`, `depthClipEnabled`, `depthBias`, `depthBiasClamp`, `slopeScaledDepthBias`, `dynamicDepthBiasEnabled`.
  - `depthEnabled`, `depthWriteEnabled`.
  - `stencilEnabled`, `stencilReadMask`, `stencilWriteMask`, `stencilReference`, `stencilFrontFace`, `stencilBackFace`.
  - `multisampling`, `alphaToCoverageEnabled`, `primitiveTopology`, `cullMode`, `frontFace`.
  - `renderTargetFormat[8]`, `renderTargetBlend[8]`, `renderTargetCount`.
  - `logicOpEnabled`, `logicOp`, `depthTargetFormat`.
  - `inputSlots`, `inputElements`, `specConstants`.
  - Pipeline state is monolithic and immutable. There is no pipeline-state-stream and no incremental PSO. Fill mode is always solid (`d3d12.cpp:3100`, `vulkan.cpp:1514`).
- **Shader format:**
  - `RenderShaderFormat { UNKNOWN, DXIL, SPIRV, METAL }`, `types.h:377-382`. One format per backend; `RenderInterfaceCapabilities::shaderFormat` tells the caller which to supply.
  - D3D12 asserts `format == DXIL` (`plume_d3d12.cpp:2970`) and reports `shaderFormat = DXIL` (L4212). Vulkan asserts `SPIRV` (`plume_vulkan.cpp:1290`, reports at L4601). Metal asserts `METAL` (`plume_metal.cpp:1336`, L4209).
  - Plume does not take DXIL and SPIR-V in one call. The caller supplies blobs for the active backend.
  - The D3D12 backend supports SM 5.1 only behind an undefined macro: `plume_d3d12.cpp:3671` tests `#if SM_5_1_SUPPORTED`. It is defined nowhere in this tarball, and `D3D12Shader` still asserts DXIL. `PGR4-Recomp/CMakeLists.txt:62-66` says it builds SM 5.1 DXBC. An unmodified Plume would therefore reject DXBC, so the missing D3D12 shader path presumably needs a Plume modification or fork. `[INFERENCE]`

### 4.4 Feature support for a Xenos-style renderer

| Feature | Status | Where |
|---|---|---|
| Root / push constants | Yes | `RenderPushConstantRange`, `types.h:1450-1467`. `setGraphicsPushConstants` / `setComputePushConstants` at `render_interface.h:125,128`. D3D12 maps to `SetGraphicsRoot32BitConstants` (`d3d12.cpp:2098-2110`), Vulkan to `vkCmdPushConstants` (`vulkan.cpp:2980-2998`). D3D12 asserts `range.offset == 0` (L2077, L2103). |
| Descriptor sets | Yes | `RenderDescriptorSet` (`render_interface.h:76-86`), `RenderDescriptorSetDesc` / `RenderDescriptorRange` (`types.h:1409-1448`). |
| Bindless / unbounded arrays | Yes, via a boundless last range | `RenderDescriptorSetDesc.lastRangeIsBoundless` and `boundlessRangeSize` (`types.h:1437-1438`). Capability `descriptorIndexing` (`types.h:1786`) is hard-true on D3D12 (`d3d12.cpp:3859`). Vulkan uses `VK_EXT_descriptor_indexing` (`vulkan.cpp:76,1201-1208,1926,2061,3841`). |
| D3D12 root descriptors | Yes, D3D12-only | `RenderRootDescriptorDesc` (`types.h:1469-1481`), `setGraphicsRootDescriptor` (`render_interface.h:130`). |
| Buffer device address | Yes | `bufferDeviceAddress` (`types.h:1790`, D3D12 at `d3d12.cpp:3861`). `RenderBuffer::getDeviceAddress` (`render_interface.h:27`). |
| Pipeline library / caching | **No** | Grep for `PipelineCache`, `PipelineLibrary`, `CachedPSO` found nothing. Only `VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME` appears (`vulkan.cpp:83`), used for raytracing. |
| Typed UAV | Partial | `READ_WRITE_FORMATTED_BUFFER` and `READ_WRITE_TEXTURE` (`types.h:342-355`), `RenderBufferFlag::UNORDERED_ACCESS` and `FORMATTED` (`types.h:430,414`), `RenderTextureFlag::UNORDERED_ACCESS` (`types.h:443`). `createBufferFormattedView(format)` (`render_interface.h:25`). No typed-UAV-load capability query. |
| MSAA resolve | Yes | `resolveTexture` (`render_interface.h:146`) and `resolveTextureRegion(dst, dstX, dstY, src, srcRect, mode)` (L147). `resolveRegion` / `resolveModes` are in `types.h:1782-1783`. D3D12 uses `ResolveSubresource` (L2347) and `ResolveSubresourceRegion` (L2367); Vulkan uses `vkCmdResolveImage` (L3371). `RenderResolveMode { MIN, MAX, AVERAGE }` (`types.h:511-515`). |
| Sample locations | Yes | `RenderMultisampling.sampleLocations[16]` (`types.h:788-789`). D3D12 calls `SetSamplePositions` (`d3d12.cpp:2487-2508`). |
| Copy texture regions | Yes | `RenderTextureCopyLocation` (`types.h:1096`), `copyTextureRegion` (`render_interface.h:143`). Types are `SUBRESOURCE` and `PLACED_FOOTPRINT` (`types.h:1090-1094`). |
| Timestamp queries | Yes | `RenderQueryPool` (`render_interface.h:219-224`), `writeTimestamp` (L152), `resetQueryPool` (L151), capability `queryPools` (`types.h:1816`). D3D12 uses `D3D12_QUERY_HEAP_TYPE_TIMESTAMP` (`d3d12.cpp:1785`). |
| Occlusion queries | **No** | Only timestamp pools exist. |
| Dynamic rendering | **No** | Vulkan uses `vkCreateRenderPass` (`vulkan.cpp:1678-1733,2634`) and `RenderFramebuffer` objects (`render_interface.h:108`). |
| Primitive restart | Hardwired | Vulkan enables it for LINE_STRIP and TRIANGLE_STRIP (`vulkan.cpp:1495-1497`). D3D12 sets `IBStripCutValue = 0xFFFF` for strips (`d3d12.cpp:3096-3098`). There is no 32-bit cut value and no per-draw control. |
| Conservative raster | **No** | No such grep hits. |
| Dual-source blend | Enum only | `RenderBlend::SRC1_COLOR` etc. (`types.h:261-264`). D3D12 maps these at `d3d12.cpp:286-293`. |
| Depth bias | Yes | Static `depthBias` (`types.h:1285-1287`). Dynamic via `setDepthBias` (`render_interface.h:139`), capability `dynamicDepthBias` (`types.h:1807`). On D3D12 it needs the Agility SDK and `D3D12_PIPELINE_STATE_FLAG_DYNAMIC_DEPTH_BIAS` (`d3d12.cpp:3106-3110,2240-2246,3727`). |
| Stencil | Yes, with a baked reference | `stencilEnabled`, masks, `stencilReference`, per-face ops (`types.h:1291-1296`, `RenderStencilOp` at L229-239). |
| 16-bit index | Yes | `RenderIndexBufferView.format` (`types.h:1505-1511`). Vulkan accepts R8/R16/R32_UINT (`vulkan.cpp:673-682`). D3D12 uses `toDXGI(view->format)` (`d3d12.cpp:2132-2140`). |
| Extended dynamic state | **No** | Vulkan dynamic states are only VIEWPORT, SCISSOR and optionally DEPTH_BIAS (`vulkan.cpp:1611-1617`). |
| Triangle fan | Capability flag | `triangleFan` (`types.h:1806`). D3D12 reads `Options15.TriangleFanSupported` (`d3d12.cpp:3720,3764`). Vulkan sets it true except on Apple (`vulkan.cpp:4150-4154`). Quad lists and rect lists have no topology, so Xenos prims need emulation. |
| Logic op | Yes | `logicOpEnabled`, `logicOp` (`types.h:1305-1306`, enum L286). |
| Alpha to coverage | Yes | `types.h:1298`. |
| Depth clip | Yes | `depthClipEnabled` (`types.h:1284`). Vulkan maps it to `depthClampEnable = !depthClipEnabled` (`vulkan.cpp:1512`). |
| Geometry shader | Capability only | `geometryShader` (`types.h:1772`), `RenderGraphicsPipelineDesc.geometryShader`. |
| Max render targets | 8 | `MaxRenderTargets = 8` (`types.h:1277`). |
| Texture formats | Broad | Includes BC1+ (`types.h:148`). Texture view swizzles `RenderComponentMapping` (`types.h:1014-1035`). |
| Barriers | Explicit only | Buffer and texture barriers by stage (`render_interface.h:118`, `types.h:823-858`). The README admits barrier and layout management is not finished. |

**Fit against what a Xenos-style renderer needs:**
- **Present:** push constants, bindless descriptor ranges, MSAA resolve with sample positions, copy regions, timestamps, dynamic depth bias, stencil, 16-bit indices, strip restart, dual-source blend enums.
- **Missing:**
  - Pipeline caching, so every unique PSO compiles from scratch.
  - Indirect draws, predication and occlusion queries.
  - Conservative raster, and any control over blend constants or the stencil reference at command time.
  - Fill mode.
- **Not tested:** whether the hooks land at the D3D API level (device, draw, shader, state) rather than at PM4.