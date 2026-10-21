# ReXGlue SDK: GPU integration points (raw notes)

Read-only notes for `docs/native-renderer-design.md`. Source: `rexglue/rexglue-sdk` `main` at
`c94f5eb` (2026-08-21), fetched as a tarball to `/tmp/recomp/rexglue-sdk-main`. `src/` and
`include/` are byte-identical to the `v0.10.0` tag (`f5337cd`) checked out at
`/tmp/recomp/rexglue-v0.10.0` (`diff -rq` reports only extra top-level dirs such as `tests/`,
`thirdparty/`). Paths below are relative to the SDK root.

## How a title gets a graphics system

- `RuntimeConfig` holds `std::unique_ptr<system::IGraphicsSystem> graphics` and
  `std::string gpu_plugin` (`include/rex/runtime.h:66-75`). `REX_GRAPHICS_BACKEND(Type)` is a
  `make_unique` helper (`runtime.h:77-82`).
- `ReXApp::SetupPresentation` (`src/ui/rex_app.cpp:308-334`) sets `config_.gpu_plugin` from the
  `--gpu_plugin` cvar (`rex_app.cpp:49-52`, default empty = no GPU emulation), calls the virtual
  `OnPreSetup(config_)` (`rex_app.cpp:314`), then, only if the app left `config_.graphics`
  empty, loads the plugin with `LoadGpuPlugin(name)` (`rex_app.cpp:316-326`; failure is fatal by
  design). It then calls `graphics->SetupPresentation(&app_context())` (`:328-334`).
- `ReXApp` virtuals an app overrides: `OnPreSetup` (`include/rex/rex_app.h:99`),
  `OnPostSetup` (`:105`), `OnPreLaunchModule` (`:159`), `OnPostLaunchModule` (`:163`),
  `OnCreateImmediateDrawer` (`:190`, "detached overlay mode / bring your own renderer",
  documented at `:169-189`).
- Shader storage: `LaunchModule` calls `graphics_system->InitializeShaderStorage(cache_root,
  title_id, true)` before resuming the main thread (`rex_app.cpp:459-465`).

## Plugin ABI

- `include/rex/system/gpu_plugin.h`: ABI version `kGpuPluginAbiVersion = 1` (`:32`), exports
  `rex_gpu_abi_version` and `rex_gpu_create(abi, const GpuCreateInfo*)` (`:34-47`),
  `GpuCreateInfo{struct_size, backend}` with backend `"d3d12" | "vulkan" | "any"` (`:37-40`).
- Loader: `src/system/gpu_plugin_loader.cpp`. File name `rexgpu-<name>.dll` /
  `librexgpu-<name>.so` / `.dylib` next to the executable (`:40-44`); `LoadGpuPlugin`
  (`:57-102`). `rex_app.cpp:317` calls it with the default backend `"any"`.
- The only plugin, `rexgpu-xenos` (`src/graphics/plugin_main.cpp:24-53`): `"any"` picks D3D12
  first when compiled in, else Vulkan (`:41-50`).
- Build: `src/graphics/CMakeLists.txt:1-31` lists the base GPU sources (`graphics_system.cpp`,
  `command_processor.cpp`, `register_file.cpp`, `packet_disassembler.cpp`,
  `shared_memory.cpp`, texture/render-target caches, `format/ucode.cpp`, shader translator).
  They compile **into the plugin DLL** with hidden visibility (`:99-105`), linked PRIVATE
  against `rexruntime` (`:133-139`). Only the stateless Xenos format layer
  (`xenos.cpp`, texture info/conversion) is in `rexcore` (`src/core/CMakeLists.txt:79-87`).
  So an external app cannot link against `rex::graphics::CommandProcessor`; it must compile
  those sources into its own target.
- `rexglue_configure_target(... GPU_PLUGINS <names>)` stages `rexgpu-<name>` next to the exe;
  it accepts an **in-tree target** named `rexgpu-<name>` as well as an installed
  `rex::gpu-<name>` (`cmake/rexglue_helpers.cmake:145-166`). A title repo can therefore build
  its own `rexgpu-native` plugin and ship it beside `rexgpu-xenos`.
- Backend options: D3D12 ON / Vulkan OFF on Windows, Vulkan ON elsewhere
  (`CMakeLists.txt:34-43`).

## IGraphicsSystem (the app-facing interface)

`include/rex/system/interfaces/graphics.h:34-95`: `SetupPresentation(WindowedAppContext*)`,
`SetupGuestGpu(FunctionDispatcher*, KernelState*)`, `has_presentation()`, `Shutdown()` are pure;
optional `provider()`, `presenter()`, `SetInterruptCallback(callback, user_data)`,
`InitializeRingBuffer(ptr, size_log2)`, `EnableReadPointerWriteBack(ptr, block_size_log2)`,
`InitializeShaderStorage(cache_root, title_id, blocking)`.

## Kernel Vd* exports (fixed, in the SDK)

`src/kernel/xboxkrnl/xboxkrnl_video.cpp`:
- `VdSetGraphicsInterruptCallback` → `graphics_system->SetInterruptCallback` (`:315-326`).
- `VdInitializeRingBuffer` → `InitializeRingBuffer` (`:328-339`).
- `VdEnableRingBufferRPtrWriteBack` → `EnableReadPointerWriteBack` (`:341-350`).
- `VdGetSystemCommandBuffer` writes `0xBEEF0000/1` (`:352-356`).
- `VdSwap` (`:440-521`) does not call the graphics system. It translates the front buffer's
  fetch constant to a physical address (`:458-480`) and writes into the guest's 64-dword swap
  block a type-0 write of fetch constant 0 plus a Xenia-only `PM4_XE_SWAP` packet
  (`kSwapSignature`, front buffer physical address, width, height), padded with type-2 NOPs
  (`:495-520`). `PM4_XE_SWAP = 0x64` is declared at `include/rex/graphics/xenos.h:1608`.
  There is no frame limiter here (FM2-ReGPU limits in its VdSwap).
- Without a graphics system the exports log a warning and return (`:320-323`, `:333-336`).

## rex::graphics::GraphicsSystem (base class inside the plugin)

`include/rex/graphics/graphics_system.h:49-133`, `src/graphics/graphics_system.cpp`:
- `SetupGuestGpu` (`graphics_system.cpp:118-176`): creates the command processor via the pure
  virtual `CreateCommandProcessor()` (`graphics_system.h:100`), registers GPU MMIO at
  `0x7FC80000`, 64 KB, through `memory_->AddVirtualMappedRange` with
  `ReadRegisterThunk`/`WriteRegisterThunk` (`graphics_system.cpp:140-147`), and starts the
  "GPU VSync" `XHostThread` that calls `MarkVblank()` at the guest video mode's refresh rate,
  or 1 kHz with `--vsync=false` (`:150-176`).
- `DispatchInterruptCallback(source, cpu)` (`:297-317`) runs the guest callback through
  `function_dispatcher_->ExecuteInterrupt` on the current XThread. `MarkVblank` bumps the CP
  counter and dispatches source 0 on CPU 2 (`:319-332`).
- Pure virtual `CreateProvider(bool)` (`graphics_system.h:98`) and `name()` (`:53`).

## rex::graphics::CommandProcessor (PM4 front end inside the plugin)

`include/rex/graphics/command_processor.h:76-284`, `src/graphics/command_processor.cpp`:
- Worker thread `WorkerThreadMain` (`command_processor.cpp:200-256`): spins on the write
  pointer (`write_ptr_index_`, set via MMIO), calls `ExecutePrimaryBuffer` (`:615`), writes the
  read pointer back to `read_ptr_writeback_ptr_` (`:247-250`).
- Packet dispatch: `ExecutePacketType3` (`:732-892`, switch from `:755`). Predication: packets with header bit 0
  are skipped when `(bin_select_ & bin_mask_) == 0`, and predicated `XE_SWAP`s are always
  skipped (`:744-753`). `SET_BIN_MASK/SELECT` (`:831-863`). Defaults `0xFFFFFFFF`
  (`command_processor.h:263-264`).
- `INDIRECT_BUFFER(_PFD)` → `ExecuteIndirectBuffer` (`:769-771`, `:636`, `:967`).
- `EVENT_WRITE_EXT` returns full-screen extents (`:1221-1247`), the same trick as FM2-ReGPU.
- `EVENT_WRITE_ZPD` fakes occlusion counts with `--query_occlusion_fake_sample_count`
  (`:72`, `:1249-1283`), virtual so a backend can override (`command_processor.h:196`).
- Draws: `ExecutePacketType3Draw` (`:1285-1404`) writes `VGT_DRAW_INITIATOR`,
  `VGT_DMA_BASE`, `VGT_DMA_SIZE` into the register file, builds `IndexBufferInfo`, rejects
  immediate indices (`:1351-1359`), drops draws with `viz_query_ena && kill_pix_post_hi_z`
  (`:1376`), then calls the pure virtual `IssueDraw(prim_type, index_count, index_buffer_info,
  major_mode_explicit)` (`:1381`; declared `command_processor.h:220-221`).
- Resolves: the base class never calls `IssueCopy`; the D3D12 backend's `IssueDraw` checks
  `RB_MODECONTROL.edram_mode == kCopy` and calls `IssueCopy()`
  (`src/graphics/d3d12/command_processor.cpp:2281-2284`).
- Shaders: pure virtual `LoadShader(type, guest_address, host_address, dword_count)`
  (`command_processor.h:217-218`), called from `IM_LOAD` / `IM_LOAD_IMMEDIATE`.
- Swap: `ExecutePacketType3_XE_SWAP` (`:929-965`) records frame time/FPS perf counters when
  `REXGLUE_ENABLE_PERF_COUNTERS` (`:933-945`) and calls the pure virtual
  `IssueSwap(frontbuffer_ptr, width, height)` (`:961`; declared `command_processor.h:109-110`).
- Register file: host-endian `uint32_t values[0x5003]` (`include/rex/graphics/register_file.h:40-44`).
  Virtual hooks: `WriteRegister` (`command_processor.h:145`), `WriteRegistersFromMem`
  (`:147`), `WriteRegisterRangeFromRing` (`:148`), gamma LUT callbacks (`:165-166`),
  `MakeCoherent`/`PrepareForWait`/`ReturnFromWait` (`:168-170`), `OnPrimaryBufferEnd` (`:173`).
- Protected state a subclass can read: `primary_buffer_ptr_`, `primary_buffer_size_`,
  `read_ptr_index_`, `write_ptr_index_` (`command_processor.h:248-256`), `register_file_`
  (`:236`), `bin_select_`/`bin_mask_` (`:263-264`).

## Guest memory

- Map comment `src/system/xmemory.cpp:50-79`: physical memory has three views at
  `0xA0000000` (64 KB pages), `0xC0000000` (16 MB), `0xE0000000` (4 KB).
- `Memory::TranslatePhysical(addr)` = `physical_membase_ + (addr & 0x1FFFFFFF)`
  (`include/rex/system/xmemory.h:404-405`). The file-mapping handle `mapping_` is private with
  no accessor (`xmemory.h:590`), so zero-copy import of guest physical memory into D3D12 needs
  `OpenExistingHeapFromAddress1(physical_membase(), 512 MB)` or an SDK patch.
- Write watching for caches: `RegisterPhysicalMemoryInvalidationCallback` and
  `EnablePhysicalMemoryAccessCallbacks` (`xmemory.h:492-500`).

## Hooking recompiled functions (for D3D call counting)

- Every recompiled function is emitted as `DEFINE_REX_FUNC(sub_XXXXXXXX)`
  (`src/codegen/function_graph.cpp:481-482`). On Windows/Linux the macro makes `sub_X` a weak
  alias of `__imp__sub_X`; on Apple a weak tail-call thunk
  (`resources/templates/codegen/pch_h.inja:65-87`).
- Direct calls in generated code call the weak name (`src/codegen/builders/context.cpp:229-230`),
  so a strong `sub_X` defined by the app (`REX_HOOK`, `REX_HOOK_RAW` in
  `include/rex/hook.h:42-49`) intercepts every direct call and can forward to `__imp__sub_X`.
- Mid-function hooks: `[[midasm_hook]]` entries in the codegen TOML (`src/codegen/config.cpp:310-366`).

## Measurement facilities already in the SDK

- Perf counters (`include/rex/perf/counter.h`, `src/core/perf/counter.cpp`): `frame_time_us`,
  `fps`, `draw_calls`, `pipeline_cache_*`, `texture_cache_*` (`counter.cpp:34-52`);
  `--perf_log_csv=<path>` writes one CSV row per frame (`counter.cpp:22-23`, `:128-166`).
  Compiled only when `REXGLUE_ENABLE_PERF_COUNTERS` (option ON, `CMakeLists.txt:25`) **and**
  the config is not Release (`src/core/CMakeLists.txt:133-137`). `draw_calls` counts host
  draws in the Xenos D3D12 backend (`src/graphics/d3d12/command_processor.cpp:2629`, `:2688`),
  not guest D3D API calls.
- Tracy (`REXGLUE_ENABLE_TRACY`, `CMakeLists.txt:24`), `--dump_shaders=<dir>`
  (`src/graphics/flags.cpp:27`), `--log_level`, `--log_file` (`src/core/logging.cpp:36-40`).
- No cvar counts indirect-buffer depth, predicated packets, or tiles. Those need a patch (see
  the measurement plan in the design doc).

## Linux

- POSIX sources (`fiber_posix.cpp`, `mapped_memory_posix.cpp`, ... in `src/core`) and the
  Vulkan default on non-Windows (`CMakeLists.txt:40-43`) mean a native Linux build of a ReXGlue
  title is possible in principle; not verified here.
