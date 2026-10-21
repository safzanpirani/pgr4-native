# Working in pgr4-native

This repository holds a native D3D12 renderer for the PGR4 static recompilation, the scripts
that build, run and measure it on a Windows PC, and the notes and prompts used to run several
coding agents on it at once. Game data and builds live on the Windows build host under
`C:\recomp`. The `lab` CLI (`bun tools/lab/cli.ts`, see `README.md`) drives that host through
`fleet` from any machine, including the Windows host itself.

## Start of every session

1. Read `README.md` (status, numbers, bugs) and `docs/pgr4-native-architecture.md` (how the
   renderer works).
2. Read `docs/perf-workflow.md` before building or running anything.
3. `lab pgr4 status` and `lab pgr4 trees` show what the host is doing and which build trees can
   run.

## Rules

- **No game data in git.** Discs, executables, title updates, extracted assets, saves, captures
  of game imagery, and C++ generated from the game's executable stay on the build host.
  `.gitignore` blocks the common extensions; check `git status` before every commit anyway.
- **Never use leaked or copyrighted source code.** Work from your own disc and public tools.
- **Run it to prove it.** A build that compiles is not evidence. Evidence is the game running:
  a screenshot, a frame-time log, an `ab` verdict, an oracle PSNR. Put the numbers, tags and
  paths in your notes.
- **Prior art before experiments.** For a problem other open-source ports have probably solved
  (renderer performance, API traps, frame pacing), read their code first, adopt the technique,
  then confirm it with one measured run. A game run takes 4-5 minutes plus the queue, so
  profiling series are for problems with no public solution. Renderer rules with sources:
  `skills/pgr4-native/references/native-renderer-performance.md`.
- **Licenses.** Code from projects without a license (FM2-ReGPU, for one) is reference only.
  `docs/research/` describes such code with file and line numbers and copies none of it. Ported
  Xenia/ReXGlue code keeps its BSD-3-Clause notice (`THIRD_PARTY_NOTICES.md`).
- **The host may be someone's desktop.** Check `lab pgr4 status` and the screen before starting
  a game or a heavy build. Do not kill processes you did not start. GUI programs started over
  SSH need an interactive scheduled task; see `docs/running-on-windows.md`.
- **Upstream contributions:** check each upstream's policy on AI-generated code before opening
  a PR.

## Running several agents

- **One git worktree per agent** (`git worktree add ../pgr4-native-<name> -b <name>`), merged by
  one integrator. Agents sharing one checkout hit rebase conflicts on the notes, and one agent's
  `git add` can drop another's file.
- **One build tree per agent** (`C:\recomp\builds\pgr4\native-<Name>`), chosen from the trees
  that already have a firewall rule (`lab pgr4 trees`). A new exe path pops a prompt that stalls
  every run at the title screen.
- **One game at a time** through the FIFO queue and `C:\recomp\locks\game.lock`
  (`docs/perf-workflow.md`).
- **Use `lab pgr4`** (`lab pgr4 help`) for sync, build, profile, compare, oracle, the integrator
  check, A/B bench, oracle diff, demo clips, status and clean. Do not hand-roll tar/copy/sleep
  chains. Shared host scripts live in `lab/` and `run/`; edit them there and run
  `lab pgr4 scripts`.
- Run every `--wait` command as a background command; the result arrives when the remote job
  ends.
- Before drawing conclusions from a profiled run, check its "compiler processes" line (nonzero:
  rerun) and compare stages in ns/draw against your own `--ref <base>` baseline in the same
  tree. Settle "is it faster" with `lab pgr4 ab`, and "what broke the image" with
  `lab pgr4 diff`.
- After a run whose wrapper died, run `lab pgr4 clean <tree>`. Never kill a wrapper that may
  have launched the game.
- Each agent owns named files; changes to someone else's file go through a message to that
  agent first.
- Briefs, the integrator's checklist and real examples: `prompts/`.
- Notes per agent: a `#### <Name>` section under the current milestone in `notes/STATE.md`
  (results, stopping point, exact next commands) and a `## <date> <Name>` section in
  `notes/worklog.md`. Create both on your first session; the notes behind this repository's
  own history are not published. `.gitattributes` merges the worklog by union;
  `notes/STATE.md` tail conflicts go through `tools/resolve-append-conflicts.py`.

## Layout

| Path | What |
|---|---|
| `native/` | The renderer: hooks, modules, `build.ps1`, `run.ps1`, Python tools in `native/tools/` |
| `instrument/` | The draw-count instrumentation and its SDK patch (`build.ps1` needs it) |
| `build/build-mtr.ps1` | Builds MTR-PGR4 and its patched ReXGlue SDK |
| `lab/` | Host scripts behind `lab pgr4`: run queue, build, profile, oracle, A/B, demo |
| `run/` | Input player, the St Petersburg route, the Xenos baseline bench |
| `proton/` | One race run under Proton on Linux (`run.sh`, `route_player.py`) |
| `tools/lab/` | The `lab` CLI (Bun) and its host-side helpers (`tools/lab/host/`) |
| `docs/` | Architecture, perf workflow, plans, research notes, Windows and Proton setup |
| `skills/pgr4-native/` | Agent skill and the renderer performance rules |
| `prompts/` | Wave brief template, integrator checklist, examples |
