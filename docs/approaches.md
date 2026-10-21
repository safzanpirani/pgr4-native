# Approaches

Four kinds of project fit in this lab. They need different tools, verification and effort,
so pick one per project and record it as `kind` in `project.toml`.

| Kind | Output | How you know it works | Typical tools |
|---|---|---|---|
| `matching-decomp` | C/C++ that the original compiler turns into the same bytes | Byte comparison per function | splat, m2c, decomp-permuter, objdiff, asm-differ, [Ketbome/decomp-skills](https://github.com/Ketbome/decomp-skills) (`dk`) |
| `static-recomp` | The original machine code translated to host C++, linked against a host runtime | The game boots and plays; behaviour matches the console | XenonRecomp, ReXGlue (Xbox 360), N64Recomp, rexdex-style tools |
| `native-renderer` | A host GPU backend replacing GPU emulation inside a static recomp | Frame captures and frame times against the emulated path | PM4/command-stream parsing, Xenos microcode to HLSL/SPIR-V, D3D12/Vulkan (Plume RHI) |
| `port` | Hand-maintained source (from a finished decomp) built for PC | The game runs; mods possible | the decomp's own build system |

## Matching decomp

- Best for MIPS-era consoles (PS1, PS2, N64, PSP) where the original compiler is known
  and available.
- Work is per function and easy to split across agents. Each function is verified by
  a byte comparison, so agent output is checkable without trust.
- [Ketbome/decomp-skills](https://github.com/Ketbome/decomp-skills) holds a ready kit: the `dk` per-function loop, worker and
  forensics agent roles, session recovery. Its relocation-aware comparison only supports
  MIPS today.
- Progress is slow and measured in percent of functions matched.

## Static recompilation (Xbox 360)

- Translates every PowerPC function in `default.xex` into C++ once; no JIT, no
  interpreter. Needs a list of function boundaries, jump tables and fiber/setjmp sites
  (the "manifest"), and a runtime that implements the 360 kernel, XAM, audio and GPU.
- **ReXGlue** (`rexglue/rexglue-sdk`) bundles the recompiler with a Xenia-derived runtime
  and GPU plugin. Fastest route to "it boots". PGR4-Recomp, MTR-PGR4 and hl360-Forza2
  use it.
- **XenonRecomp** (`hedge-dev/XenonRecomp`) is only the recompiler; the project writes
  its own runtime. Unleashed Recompiled and FM2-ReGPU use it.
- Generated C++ is derived from the game. Keep it out of git; regenerate from the dump.

## Native renderer

The step that removes Xenos GPU emulation. Two designs exist in the wild:

1. **Hook the game's D3D calls** (`D3DDevice_SetRenderState`, draw calls, `Resolve`) and
   map them to D3D12/Vulkan. Simple when the game issues draws through the API every
   frame. PGR4-Recomp v2 takes this path with Plume.
2. **Consume the PM4 command stream** the game builds, but render it natively: real
   render targets instead of EDRAM, resolves as copies, shaders translated ahead of
   time. FM2-ReGPU takes this path after measuring that about 94% of FM2's draws come
   from prerecorded command buffers, which API hooks never see. It still replays every
   predicated tile, clipped to that tile on one full-size surface; skipping the replays
   is planned in its scope doc, not implemented.

Measure first: count draws executed by the GPU against draws issued through the game's
D3D draw function. If most draws bypass the API, design 1 misses them. The full plan for
PGR4 is in [native-renderer-design.md](native-renderer-design.md).

## Agent workflow that holds up

- One owner per file or function range; integrate in a separate checkout.
- Commit each verified unit at once; keep state in `STATE.md`, not in an agent's context.
- Verify by the strongest available oracle: byte match (decomp), a frame capture or
  benchmark run (renderer), a boot and a scripted playthrough (recomp).
- Log every run with its numbers in the worklog, including failures.
