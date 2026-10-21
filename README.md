# pgr4-native

A native Direct3D 12 renderer for the Project Gotham Racing 4 static recompilation
([MissingThePixel/MTR-PGR4](https://github.com/MissingThePixel/MTR-PGR4), built on the
[ReXGlue](https://github.com/rexglue/rexglue-sdk) SDK). The recompilation normally draws through
ReXGlue's Xenos plugin, which emulates the Xbox 360 GPU by replaying its command stream. This
renderer hooks the game's own Xbox 360 D3D calls in the recompiled code instead (197 functions:
draws, clears, resolves, Swap and the state calls around them) and turns each one into D3D12
work.

The target is Proton on the Steam Deck at 1280x800 and 60 fps.

## Status (2026-10-21)

- With `--pgr4_native_primary` the renderer is the game's output: it presents into the game
  window, and the Xenos plugin only runs fences and interrupts (`--gpu_skip_draws`, added by
  `native/rexglue-native-primary.patch`). Title screen, menus and the St Petersburg race render.
- On race frames the front buffer matches the Xenos plugin's at 31-33 dB PSNR. The car's rear
  spoiler is missing; that is a known bug.
- Measured on an i9-13900K with an RTX 3090, uncapped, St Petersburg race, same build for all
  three: 10.5 ms per frame (96 fps) with the native renderer, 8.0 ms (126 fps) with the Xenos
  plugin, and 7.8 ms (129 fps) with the native renderer and `--pgr4_native_render_thread`. The
  game's own code takes about 8 ms per frame on the game thread. The render thread moves the
  native work (about 2.7 ms per frame) off it and uses about 43 % of another core. The Xenos
  plugin keeps two cores busy.
- The render thread is off by default for now. With it on, one of three oracle captures drops to
  29.5 dB, which is under the 30 dB pass mark.
- A whole run from the title screen to the race has three frames of 50 ms or more. Two of them
  are start-up and a loading screen.
- Under Proton on Linux (GE-Proton 11-11, so vkd3d-proton), same hardware and build: 8.7-8.9 ms
  per frame with the render thread, 8.8-8.9 ms with the Xenos plugin and 10.9-11.0 ms without
  the render thread. Race captures match the Xenos plugin's at 31.5-31.7 dB, as on Windows.
  Pinned to eight of the CPU's efficiency cores, the native renderer with the render thread runs
  at 80 fps and the Xenos plugin at 72 fps.
- Not yet run on a Steam Deck.

## How it works

- `native/src/pgr4_d3d_trace.cpp` hooks the guest D3D functions listed in
  `native/src/d3d_surface.inc`. Each hook calls the original guest function first, so the guest
  D3D state and the Xenos plugin stay correct, then passes the call to the renderer.
- `d3d_state` tracks the Xenos register state the guest D3D device writes for each draw.
- Shaders: Xenos microcode goes through ReXGlue's DXBC shader translator
  (`shader_translate`, `shader_cache`).
- Textures (`textures`, `texture_layout`): guest textures are untiled on CPU worker threads into
  upload memory and copied into placed resources. Guest writes are caught with page watches.
- Render targets and resolves (`render_targets`, `resolve`): the 360's EDRAM targets become host
  render targets; resolves copy them into host textures that later draws sample.
- Pipelines (`draw`, `pipeline_store`): pipeline states compile on a worker thread and persist
  across runs in an `ID3D12PipelineLibrary` under the runtime's cache folder.
- `present` presents the guest's front buffer into the game window or a debug window.

Several files port Xenia/ReXGlue code (BSD-3-Clause), among them `draw`, `render_targets`,
`resolve`, `geometry_shader` and the texture code. Their headers name the ported functions.

## Building

You need Windows 10 or 11, Visual Studio 2022 Build Tools (x64), LLVM (clang-cl), CMake, Ninja,
PowerShell 7 and Git. You also need the files from your own PGR4 disc, extracted to a folder
(`default.xex`, `gw.xex`, `Game\`, `UI\` and so on). This repository contains no game data, and
the recompiled C++ that the build generates from `default.xex` must not be published.

1. Build MTR-PGR4 and its patched ReXGlue SDK:
   `pwsh -File build\build-mtr.ps1 -Root C:\recomp\builds\pgr4 -Game <disc folder> -Stage prep,sdk`
2. Build the renderer into its own tree. This copies the MTR-PGR4 checkout, applies the two SDK
   patches, adds the hooks and `native\src\native\` to the game, then builds:
   `pwsh -File native\build.ps1 -Root C:\recomp\builds\pgr4 -Game <disc folder> -Dest <tree>`
3. Run it: `pwsh -File native\run.ps1 -Action start -Dest <tree> -Game <disc folder> -Native -Extra "--pgr4_native_primary"`.
   `run.ps1` starts the game in the interactive desktop session through a scheduled task, so it
   also works over SSH. `-Action stop` closes it. The game's executable is
   `<tree>\run\runtime\pgr4_recompiled.exe`.

`build.ps1` expects `native\` and `instrument\` side by side, as in this repository.

## Flags

| Flag | Default | Effect |
|---|---|---|
| `--pgr4_native` | false | Run the native renderer beside the Xenos plugin, in a debug window |
| `--pgr4_native_primary` | false | The native renderer is the game's output; implies `--pgr4_native` |
| `--pgr4_native_async_pipelines` | true | Compile new pipelines on a worker thread; draws wait for theirs |
| `--pgr4_native_render_thread` | false | Record and submit on a render thread; the game thread only captures each call |
| `--pgr4_native_pipeline_cache` | true | Keep pipelines across runs |
| `--pgr4_native_dynamic_depth_bias` | true | Set polygon offset per draw instead of per pipeline |
| `--pgr4_native_csv=<file>` | off | Per-frame counters and CPU times |
| `--pgr4_native_profile` | false | Per-stage CPU time of the draw path, added to the CSV |

## Tools

`native/tools/` has the scripts used to measure and check the renderer: `perf_stats.py` (stage
costs per draw from the CSV), `perf_hitch.py` (slow frames and their causes), `perf_fps.py`
(frame time from the game log), `ab_compare.py` (two builds or flag sets compared in matched
time bins of the same route, with a 95 % interval), `oracle_diff.py` (side-by-side, heatmap and
per-tile PSNR of native and Xenos captures of the same frame), `compare_resolves.py` (resolve
outputs against the Xenos plugin's), a texture untiling harness and an offline shader
translation test.

## About this repository

The code is developed in a private repository and published here automatically. History starts
at the first public commit. Code comments sometimes point to development notes that are not
published.

Third-party notices: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
