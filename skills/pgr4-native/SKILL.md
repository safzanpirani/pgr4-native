---
name: pgr4-native
description: Use when building, running, profiling or image-checking the PGR4 native D3D12 renderer, driving its Windows build host with the `lab pgr4` CLI, running several coding agents on it at once, or optimizing a native renderer for an Xbox 360 static recompilation.
---

# pgr4-native

Read the repository's `AGENTS.md` first, then `docs/perf-workflow.md`. Builds and game runs
happen on a Windows build host under `C:\recomp`. `lab` (`bun tools/lab/cli.ts`) runs anywhere
and drives the host through `fleet` (`RECOMP_HOST=<fleet host>`).

```sh
lab pgr4 scripts                                # install lab/ and run/ scripts on the host
lab pgr4 status | trees                         # queue/lock/games/compilers; trees with a firewall rule
lab pgr4 build <tree> [--sdk] --wait            # sync this checkout + build (--sdk: first build of a tree)
lab pgr4 profile <tree> <tag> [--ref <base>] --wait   # sync + build + profiled race run
lab pgr4 compare <tree> --modes primary --wait  # unprofiled fps from the game log
lab pgr4 oracle <tree> <tag> --wait             # image PSNR against Xenos, queued
lab pgr4 check <tree> <prefix> --wait           # integrator check of a merged base
lab pgr4 ab <tree> --a-extra F --b-extra F --wait  # is B faster? paired, with a 95 % interval
lab pgr4 ab <treeA> <treeB> --a-ref <base> --b-ref HEAD --wait   # two builds
lab pgr4 diff <oracle tag>                      # heatmaps + worst tiles: where native != Xenos
lab pgr4 demo <tree> --xenos --wait             # recorded race clip, side by side, plus stills
lab pgr4 clean <tree>                           # after a dead wrapper
```

Run `--wait` commands in the background; the result arrives when the remote job ends. Do not
poll with `sleep` and `fleet jobs tail`.

## Rules

- Prior art before experiments. Many Xbox 360 ports are open source. For a problem they have
  probably solved (renderer performance, API traps, frame pacing), read their code first,
  adopt the technique, then confirm it with one measured run. Queued profiling and A/B series
  are for problems with no public solution.
- No game data, title updates, captures of game imagery or code generated from the game's
  executable in git.
- Evidence is the game running (frame times, `ab` verdict, oracle dB, a screenshot), written
  into your notes with run tags. GUI launches need an interactive non-elevated scheduled task:
  `docs/running-on-windows.md`.
- Measure with release flags (tap and traces off). Before trusting a profiled run, check its
  "compiler processes" line; nonzero means another build ran, so rerun.
- Acceptance for a change: `lab pgr4 ab` says B is faster with the 95 % interval excluding zero
  (`--rounds 2` for changes under about 3 %), and the oracle stays at 30 dB or more on each of
  its 3 captures and within about 1 dB of your own baseline.
- Never kill a wrapper job that may have launched the game; use `lab pgr4 clean <tree>`.
- How the renderer works: `docs/pgr4-native-architecture.md`. What to work on next:
  `docs/pgr4-perf-plan.md`.
- Native renderer performance (any project): read `references/native-renderer-performance.md`
  first. It holds 17 rules taken from UnleashedRecomp, MarathonRecomp, Xerenge and other Xbox
  360 ports, each with its source and PGR4's status. The ones that matter most: measure with
  release flags and report CPU and GPU ms; replace the guest D3D API instead of wrapping it;
  turn on the recompiler's register-as-local codegen options; translate shaders offline and
  compile pipelines before play; use bindless resources, dirty-flag flushes and deferred
  resolve copies; after `SetPipelineState`, set dynamic depth bias again.
- Proton and the Steam Deck: `docs/proton.md` and `proton/run.sh`.
- Several agents at once: a worktree, a firewall-ruled build tree and a notes section each;
  one game at a time through the queue. Briefs and the integrator's checklist are in
  `prompts/`.
