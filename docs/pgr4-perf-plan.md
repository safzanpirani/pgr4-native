# PGR4 native renderer: performance plan (written 2026-10-21; slices 0 and 1 done, slice 4 measured)

Where the frame time goes now, what is left to win, and how to run it as a parallel agent
session. Measurements and tools: `docs/perf-workflow.md`, `docs/proton.md`. How
other Xbox 360 ports get their performance, as rules with PGR4's status:
`skills/pgr4-native/references/native-renderer-performance.md`.

## Where things stand

- Release flags (push-buffer tap off), Windows, i9-13900K + RTX 3090, uncapped race, one A/B
  round (`ab-notap`, 21:40-21:48): native with the render thread 6.99 ms (143 fps), Xenos
  7.94 ms (126 fps); native takes 12.6 % less time per frame (95 % CI 3.5 to 21.8 %). The gap
  is 2-2.5 ms in busy bins (0-4 s, 20-36 s after the mark) and about zero in light bins, where
  the game's own code (~8 ms of game thread in heavy scenes) sets the frame time.
- Every other number before 2026-10-21 22:30 (Windows tables, the whole Proton series) had
  `--pgr4_d3d_tap` on, because `native/run.ps1` and `proton/run.sh` always passed it. Since then
  the scripts leave it off unless `-Tap`/`--tap` is given. The tap costs the native path about
  0.8 ms per frame and the Xenos path about nothing (7.95 vs 7.94 ms). With Xenos the plugin's
  command thread is likely the limit, so game-thread cost hides; unverified.
- Depth bias fix (2026-10-21, `126180d`): D3D12 `SetPipelineState` reloads the pipeline's depth
  bias even with dynamic depth bias, and `draw.cpp` kept a stale cached value. Oracle `db1`
  31.74/31.63/31.93 dB.
- The render thread (+26-31 % for native) is on by default since 2026-10-21 23:20 (owner's
  call), although 1 of 3 oracle captures still shows wrong geometry with it.
- Native draws each D3D call once (~3,300 per race frame); Xenos executes ~5,500 GPU draws
  (two predicated tiles plus an untiled pass). GPU time per frame has never been measured for
  either path.
- Slice 0 (register-as-local codegen, master 9339b11) took the native race from 7.02 to 5.74
  ms/frame (142.5 → 174.2 fps), -17.9 % (95 % CI -21.6 to -14.2 %, `ab-cl`, 2 rounds). The
  numbers above predate it. Wave started 23:45 on top of it: RTFix (slice 2, `native-DrawPerf`),
  TrackCost (slice 3, `native-Integrator`), GuestD3D (slice 4, `native-StateReader`),
  CodegenMore (the other four codegen options, `native-RTResolve`); `native-TextureMod` holds
  the 9339b11 reference exe.

## Slices, in order of payoff

| # | Slice | Hypothesis / work | Measure | Expected gain | Risk |
|---|---|---|---|---|---|
| 0 | Recompiler register-as-local options (**done 2026-10-21 23:40**, master 9339b11) | Rule 17 in the rules file. `native/build.ps1` adds `cr_as_local`, `ctr_as_local`, `xer_as_local`, `non_volatile_as_local` under `[entrypoint]` and rewrites the `ctx.rNN`/`ctx.crN`/`ctx.ctr` patterns in the tree copy of MTR's `tools\apply_game_patches.ps1` (the generated code names them as locals). No `share_registers` was needed: setjmp calls go through ReXGlue's host `ppc_setjmp`, which saves and restores `ctx`. Follow-up (CodegenMore): `skip_lr`, `skip_msr`, `reserved_as_local`, `non_argument_as_local` | `ab-cl` 2 rounds: 7.02 → 5.74 ms/frame, -17.9 % (CI -21.6 to -14.2); oracle `cl1` 32.24/31.41/31.93 dB | measured: 1.28 ms per frame (guess was 0.5-1.5) | done |
| 1 | Release-flag baseline (integrator, first) | Done 2026-10-21 for the scripts: `-Tap` switch in `native/run.ps1` and `lab/co_run.ps1` (off by default; trees with an older `run.ps1` get `--pgr4_d3d_tap=false`), `--tap` in `proton/run.sh`. Re-measure Windows and Proton with 2 rounds each | `lab pgr4 ab`, `proton/run.sh` series | Correct numbers everywhere (README, STATE) | none |
| 2 | Render-thread geometry (RTFix) | Default on since 2026-10-21 23:20; the glitch remains. The oracle dips are extra or misplaced geometry (`lab pgr4 diff rt-oracle-on4/on5`): check the order of vertex copies, register snapshots and index binding between the game thread and the render thread; replay a captured frame both ways to bisect | oracle ≥ 30 dB on 3/3 captures in 3 runs | correct image with the default path | medium |
| 3 | Game-thread capture cost with the render thread | `track` was 251 ns/draw in RenderThread's profile (~0.8 ms/frame at 3,300 draws). Use the guest dirty masks to copy only changed register blocks; skip `AfterCall` for calls that flush nothing; cheaper per-draw capture records | `--pgr4_native_profile` stages; `ab` | 0.5-1 ms/frame in busy scenes, where native is game-thread bound | low |
| 4 | Guest D3D layer (GuestD3D) | In primary mode the guest still runs every D3D function body and builds PM4 packets that nothing draws. UnleashedRecomp and MarathonRecomp replace the D3D functions outright (rule 4 in the rules file). Measured (`gd1`, render thread off): `guest_d3d` 1.47 ms of a 9.27 ms frame, 625 ns/draw, the largest stage; it includes nested tracking. Next: replace the draw-path functions while keeping the state tracker fed and the originals for ring kicks, fences and Swap | `guest_d3d` and `track` stages of `lab pgr4 profile`; `ab` | 1.5-2 ms per busy frame (most of `guest_d3d` + `track` 0.60 ms) | high: guest code may read back D3D state, Xenos still needs fences and Swap |
| 5 | GPU time (Deck GPU) | Add D3D12 timestamp queries per frame to the native renderer; PresentMon GPUBusy for both paths. Look for GPU waste: 62 resolved copies per frame (defer them as UnleashedRecomp does: bind the surface while it is unchanged, copy only before it is written again; rule 12), ~170 inline-vertex barriers per frame, MSAA/overdraw | GPU ms per frame, native vs Xenos | Decides the Deck: its GPU is ~1/20 of a 3090, so the Deck is likely GPU-bound at 800p | low |
| 6 | Render-thread CPU (Deck CPU) | Render thread is ~43 % of a 13900K core, likely about a full Zen 2 core. Texture binding (218 ns/draw: memo misses after texture changes) and CBV binds (175 ns/draw, ~2.4 root CBV binds per draw) | profiled stages; Proton on 4 E-cores | Headroom on 4-core CPUs | low |
| 7 | Race-start hitch (76-84 ms) | Untile work that `FinishUploads` waits for (36-48 ms), first vertex copies (14-26 ms), cold shader translation | `perf_hitch.py` | Race start under ~50 ms | low |
| 8 | Deck-class proxies | Proton on 4 E-cores (`--cpus 16-19`) and a down-clocked GPU (`nvidia-smi -lgc` on Linux, e.g. 600-800 MHz) | `proton/run.sh` | A rough read on Deck CPU and GPU limits until a Deck is available | none |

## How to run it

- Integrator does slice 1 first, alone (about an hour), so every agent starts from release-flag
  baselines.
- Then three default `task` agents in parallel, each with a worktree, a firewall-ruled tree and
  a STATE/worklog section (`AGENTS.md`):
  - RTFix (slices 2 + 3, they touch the same files: render thread, capture, `d3d_state`) in
    `native-DrawPerf`.
  - GuestD3D (slice 4) in `native-StateReader`; the PDB build is slow, so it starts first.
  - GPUTime (slices 5 + 6) in `native-TextureMod`.
- Slices 7 and 8 are small; the integrator takes them between merges.
- Expect queue waits: one game at a time, 4-5 min per run. Every agent compares against its own
  `--ref master` baseline with `lab pgr4 ab`, tap off.
- After each merge: `lab pgr4 check CutOver <prefix>`, then update the public README numbers.
