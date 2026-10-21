# Native renderer performance: rules from other Xbox 360 ports

Read this before performance work on a native renderer (PGR4 or a new project). The rules
come from open-source Xbox 360 recompilations with their own renderers, read in October
2026, plus what PGR4 measured. Each rule names its source so it can be re-checked.

Sources (GitHub):

- `hedge-dev/UnleashedRecomp`, `sonicnext-dev/MarathonRecomp`: XenonRecomp + XenosRecomp,
  own D3D12/Vulkan renderer (`UnleashedRecomp/gpu/video.cpp`). The most mature reference.
- `shipa-2/Xerenge` (Burnout Revenge, ReXGlue + plume, PM4-level renderer): `plume_draw.h`,
  `plume_d3d_hooks.cpp`.
- `nefariousjosiah` WWE SvR 2008-2010 (Vulkan, XenosRecomp shaders), `LittleBitUA/DPRecomp`
  (D3D12), `wivi514` Dead Rising 2 Case Zero, `SpeedBreakerProject/speedbreaker` (NFS Most
  Wanted, native Linux/Deck), `mchughalex/skate3recomp` (scene-level re-render),
  `fungalspores/prototype-switch`.

## Measuring

1. **Measure with release flags.** Debug decoders, taps, traces and draw logs off. PGR4's
   push-buffer tap cost the native path 0.8 ms/frame, and every number before 2026-10-21
   was pessimistic because of it. Write the flags next to every number.
2. **Report CPU and GPU time per frame.** DPRecomp publishes both (native renderer CPU
   5.1-6.1 ms, GPU 1.4-1.6 ms, RTX 5070 at 2x); Speedbreaker has an overlay with them. A
   Deck-class GPU is ~1/20 of a 3090, so GPU time decides the Deck.
3. **Time the guest D3D library before optimizing your own side.** In PGR4 the profile stage
   `guest_d3d` is the time inside the original D3D functions at the outermost hook.

## Structure

4. **Replace the D3D API; don't wrap it.** UnleashedRecomp replaces 42 guest D3D functions
   (`GUEST_FUNCTION_HOOK`) and stubs 15 more (MarathonRecomp: 81 and 16), so the guest
   never builds a PM4 packet. It fills the device's per-state `setRenderStateFunctions`
   table with host functions, so `SetRenderState` calls go straight to the renderer. For
   state the game writes inline into the device,
   they read the device's own dirty flags at draw time, enqueue only the dirty ranges
   (constants by `countl_zero`/`countr_zero` of the mask) and clear the flags. A wrapper that
   calls the original keeps paying for packet building that nothing draws. Keep the
   originals only for what the platform layer still needs: ring kicks, fences, Swap
   (Xerenge leaves the ring helpers on the original code).
5. **Render thread with a command queue.** The game thread enqueues small typed commands into
   a lock-free queue (UnleashedRecomp: `moodycamel::BlockingConcurrentQueue<RenderCommand>`);
   the render thread owns every graphics API call. If the queue is a fixed ring, the producer
   must not wait on a full ring while the consumer needs the producer (prototype-switch
   drains in place).
6. **No large per-draw copies on the game thread.** Xerenge measured a 9 KB state snapshot
   per draw at a quarter of the title thread on a phone. Send deltas or dirty blocks.

## Shaders and pipelines

7. **Translate every shader offline, at build time.** UnleashedRecomp, MarathonRecomp and WWE
   use XenosRecomp; Skate 3 compiles with DXC offline; Xerenge also covers shaders the game
   assembles at run time, with a recipe of hashes and patch masks, so "every shader is there
   from the first frame". No translator in the frame loop.
8. **Compile pipelines before they are needed.** UnleashedRecomp ships a recorded pipeline
   list (`gpu/cache/pipeline_state_cache.h`) and compiles it in the background; WWE compiles
   while each scene loads; DR2 pre-warms at start. A pipeline miss during play compiles
   asynchronously and the draw is skipped for a frame or two instead of stalling
   (UnleashedRecomp counts created-async and dropped; Speedbreaker's "Asynchronous Shaders").
9. **Hash the pipeline key only when pipeline state is dirty.** UnleashedRecomp keeps one
   dirty bool per state group (pipeline, render targets, viewport, scissor, depth bias,
   constants, vertex streams, indices) and rebuilds nothing for a clean group.

## Resources

10. **Bindless textures and samplers.** Indices go in a small constant block
    (UnleashedRecomp `SharedConstants`: 2D/3D/cube texture and sampler indices for 16
    slots). No per-draw descriptor tables.
11. **Constants as whole blocks from a per-frame linear upload allocator.** UnleashedRecomp
    uploads all vertex constants (4 KB) or pixel constants (3.5 KB) only when dirty and binds
    each as one root descriptor.
12. **Defer resolve copies.** UnleashedRecomp records a resolve (StretchRect) as "this
    texture's content is that surface", binds the surface itself when the texture is
    sampled, and copies only before the surface is written again. Xerenge hands later
    fetches from a resolve address the resolved image instead of untiling memory.
13. **Don't break render passes with uploads.** Xerenge: an upload inside a pass ends it and
    forces store/load of every attachment. Batch uploads before the pass (matters most on
    tilers and Vulkan).

## D3D12 traps

14. **`SetPipelineState` reloads the depth bias, even with
    `D3D12_PIPELINE_STATE_FLAG_DYNAMIC_DEPTH_BIAS`** (DirectX-Specs VulkanOn12: the PSO sets
    the initial dynamic depth bias state; UnleashedRecomp's comment says the same). Invalidate
    any cached bias after every PSO change. UnleashedRecomp also bakes the common bias value
    into the pipeline to save calls.

## Frame rate and the Deck

15. **Use the game's own 60 fps mode** where it has one (WWE hooks the mode it drops to 30
    for matches; DR2 surfaces a hidden mode). Otherwise feed the logic tick whole ticks from
    an error accumulator, as the console counts vblanks (DPRecomp). Check pacing over
    minutes, not seconds (DPRecomp 2.0.9 fixed a jerk every few seconds).
16. **Deck OLED: run at 60 Hz.** Speedbreaker stutters at 90 Hz and tells the user once.
    Speedbreaker ships a native Linux build; WWE ships Windows through Proton at 720p60.

## Recompiled game code

17. **Turn on the recompiler's register-as-local options.** They speed up all guest code,
    game logic included, and so lower the floor that no renderer change touches. With them,
    guest registers become C++ locals instead of `PPCContext` fields in memory, and the
    register save/restore helper calls disappear. XenonRecomp's README says they cut
    UnleashedRecomp's frame times "by several milliseconds" (its config `SWA.toml` sets all
    eight). ReXGlue v0.10.0 reads the same keys (`src/codegen/config.cpp`, all default
    false). On ReXGlue, `fungalspores/prototype-switch` (`prototype2/project/perf_locals.toml`)
    sets `cr_as_local`, `non_volatile_as_local`, `xer_as_local` and `ctr_as_local` and cites
    +9 % fps for Mass Effect NX. It leaves `skip_msr` and `reserved_as_local` off because
    that game needs its interrupt-region locks. Functions that break the ABI need
    `share_registers = true` under `[functions]`: guest setjmp (it saves r14-r31 and cr into
    the jmp_buf), fiber switches, fragments that read a parent's register, and helpers that
    return a value in a non-volatile register. Marking ordinary SEH-framed functions
    corrupted callers in Mass Effect NX. Arguments (r3-r10, f1-f13) and r1 stay in the
    context with these four options, so entry hooks that read them keep working.

## PGR4 status (2026-10-21)

| Rule | PGR4 |
|---|---|
| 1 release flags | Done: tap opt-in (`-Tap` in `native/run.ps1` and `co_run.ps1`, `--tap` in `proton/run.sh`) |
| 3 guest D3D time | Measured 2026-10-21 (`gd1`, DrawPerf, tap off, render thread off, race): `guest_d3d` 1.47 ms of a 9.27 ms median frame (625 ns/draw, ~2,350 draws), the largest stage. It includes nested tracking, so the D3D library alone is 0.9-1.5 ms; `track` adds 0.60 ms |
| 4 replace the API | Not done: hooks call the original, then read device state (slice 4, GuestD3D). Replacing the draw path would save most of `guest_d3d` + `track`: about 1.5-2 ms per busy frame |
| 5 render thread | On by default since 2026-10-21 23:20; wrong geometry remains in 1 of 3 oracle captures |
| 7 offline shaders | Not done: runtime translation, cached on disk |
| 8 pipelines early | Partly: prewarm from the previous run's keys, async compile with skipped draws |
| 10 bindless | Done (descriptor indices in b4) |
| 12 deferred resolves | Not done: 62 resolved copies per race frame |
| 14 depth bias | Fixed 2026-10-21 in `draw.cpp`: cached bias reset after `SetPipelineState` |
| 15 60 fps | Not checked against the game's own modes |
| 17 codegen locals | Done 2026-10-21 (master 9339b11): `cr`/`ctr`/`xer`/`non_volatile_as_local` in `native/build.ps1`, plus a rewrite of MTR's `apply_game_patches.ps1` patterns (`ctx.r31` → `r31`). Race 7.02 → 5.74 ms/frame, -17.9 % (CI -21.6 to -14.2), oracle 31.4-32.2 dB, exe 45.3 → 38.3 MB. Text patches against generated code break when these options rename registers; check them first. setjmp (0x828a9c50) needed no `share_registers`: ReXGlue emits host `ppc_setjmp` with a `ctx` copy |
