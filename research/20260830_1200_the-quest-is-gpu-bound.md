# The Quest is GPU-bound, and two of my own measurements were stale

2026-08-30, later. Correcting two errors of mine from earlier today and recording the first
trustworthy on-device numbers this port has had.

## Error 1: I reported a previous session's files as today's result

`tools/verify_quest.sh` pulled `capture.raw` and `perf.csv` without clearing them first. Today's run
**crashed 33 seconds in** with an `ACCESS_VIOLATION` and wrote neither, so the script pulled files
dated the previous day and I analysed them as fresh. Everything I reported from them - the stereo
verdict `far +57 / near -80`, `100.18ms / 99.62ms CPU / 1.69ms GPU / 420 draws`, the 147us/draw fit,
the 3.13 logic ticks - **describes an older build, not this one.**

Fixed: the script now deletes the remote capture/perf/profile directories *before* launching, clears
the local ones before pulling, prints a loud warning when a fatal line appears in the log or the
process is dead at the end, and says explicitly when no capture was written.

## Error 2: I "eliminated" pipeline compilation on bad evidence

I checked the `pso: render-thread compile` warning count, found 19, noted the warning is deduplicated
per pipeline, and concluded inline Adreno compiles were a startup-only cost. That log was from the
run that crashed at 33 seconds. On a real run, **17.6% of all CPU samples are in `libllvm-qgl.so`** -
the Adreno shader compiler. It is not eliminated.

## The profiler was never broken; the crash was hiding it

`bd_sample_profiler` dumps every 600 ticks and not at shutdown, so a run that dies at 33s produces
nothing at all - no file, and no log line, because the dump returns early on zero samples. A clean
90-second run dumps normally.

What *was* broken is attribution. Samples were stored as offsets from `libreblue.so` and anything
below its base was discarded. On Android the SDK runtime is a **separate** `librexruntime.so`, and
with ASLR an offset from one library cannot name a PC in another - so 90.8% of samples resolved to
nothing. The profiler now records **raw PCs** plus `/proc/self/maps`, and
`tools/symbolize_profile.py` attributes per module first.

## The first trustworthy device profile

```
samples by module
  libc.so             78.5%     <- blocked, waiting on the GPU
  libllvm-qgl.so      17.6%     <- the Adreno shader compiler
  librexruntime.so     2.3%
  libreblue.so         0.1%     <- the recompiled guest
```

## The frame, measured, 513 field frames

```
dt_ms 142.77 | other_ms 25.59 | fence_ms 118.60
gpu_total_ms 142.76 | gpu_draw_ms 139.64 | gpu_resolve_ms 1.16
draws 601 | pso_switches 133 | barrier_calls 83 | fb_binds 23
```

**7 fps, and the GPU is doing all of it.** `gpu_draw_ms` is 97.8% of GPU time; the multiview resolve
chain is 1.16ms and is not the problem. The CPU spends 118ms of a 143ms frame *waiting*, and its own
work is 25.6ms.

**So in this configuration the recompiled guest is not the bottleneck - it is 0.1% of samples.**
That is the opposite of what every earlier note in this repo says, and the difference is
configuration: those measurements were taken at `bd_render_scale=25` with the side-by-side stereo
path, where the GPU was starved and the CPU dominated. This run is `render_scale=100` with
multiview, which renders two full layers.

## The prime suspect for 139ms of draw

**83 barrier calls per frame against 23 framebuffer binds.** On a tiler every barrier that ends the
active render pass is a full tile store and reload of a two-layer target. `TransitionResolveSources`
(`src/gpu/draw_framebuffer.cpp:301`) scans all 16 texture slots and can emit such a barrier, and it
runs *before* the framebuffer cache early-out at `:305` - so it pays on every draw, not only on the
~23 that actually rebind. That is the first thing to test, and it is a small change.

Second: `libllvm-qgl.so` at 17.6% says pipelines are still being compiled during the run. The
shipped PSO residual holds 929 entries and **none of them has `multiview` set**, while
`ResidualKey` (`src/gpu/pipeline/pso_recorder.cpp:68-75`) masks only `sampleCount` and
`enableAlphaToCoverage` - so under `bd_stereo_multiview` every scene pipeline is a genuine miss.

## Where the 139ms of draw goes: every target is two-layer

The per-target census from the same run:

```
target 1280x720x2L:  52 draws/frame,  47 Mpix/frame
target 1280x720x2L:  52 draws/frame,  47 Mpix/frame
target 1280x720x2L:  50 draws/frame,  46 Mpix/frame
target 1376x720x2L:   7 draws/frame,   6 Mpix/frame
target  344x193x2L:  37 draws/frame,   2 Mpix/frame  (x2)
```

**Three separate 1280x720 targets, and every one of them is `2L`** - roughly 140 Mpix/frame of
potential fill. `surface_pool.cpp:467-468` gives **two layers to every render target** when
`bd_stereo_multiview` is on, not just to the scene, so the entire post chain rasterises twice.

That was a deliberate change and it was correct at the time: a single-layer post target takes a
single-view pipeline, writes layer 0 and collapses the stereo pair, which cost a session and a half
to find. But the design note it superseded
(`research/20260829_1900_multiview-needs-a-resolve-not-an-array-heap.md`) called for the opposite
shape - render the *scene* with multiview, resolve once into a side-by-side single-layer image, and
run the post chain mono over it, exactly as `bd_stereo` already does. On a desktop the difference
measured 4.9% of GPU. On a tiler at 140 Mpix it is the frame.

**48 of the 83 barrier calls come from `TransitionResolveSources`** (`bar_drawfb 48`, `bar_resolve
32`, `bar_occlusion 3`), and each one ends the active render pass - a full tile store and reload of a
two-layer target. That is a second, smaller cost riding on the same decision.

## Two things this run could not settle

- **A cross-run render-scale comparison does not work here.** At `bd_render_scale=50` the game runs
  at 39fps and **never reaches a field scene** in 200 seconds - autoplay presses buttons on a fixed
  wall-clock schedule, so a faster frame rate desynchronises it and the run sits in a menu at ~115
  draws. Two attempts, 100s and 200s, both stuck. Any A/B that changes frame rate has this problem;
  the within-run `bd_ab_flag` mechanism exists precisely because of it, and `bd_render_scale` is not
  a bool so it cannot use it.
- **Whether the recompiled guest matters** at a configuration where the GPU is not the wall. At
  `render_scale=25` earlier notes measured it as ~77% of device CPU. Here it is 0.1% of samples,
  because the CPU is asleep waiting on the fence. Both can be true; the recomp case has to be
  re-measured once the GPU stops being the ceiling.

## What this does not change

The stereo work stands: multiview renders correct crossed depth, verified from a capture, and the
2D-overlay eye-seam item is closed. What changes is the *performance* plan: before this, the target
was the recompiled guest's per-draw ABI cost. On this evidence the first work is GPU-side and
renderer-side, and the recomp case has to be re-made on a configuration where the GPU is not the
wall.


## First recomp change attempted, and it is a measured null

`out/rexglue-src/include/rex/ppc/intrinsics.h` declared `VectorMaskL`, `VectorMaskR`,
`VectorShiftTableL` and `VectorShiftTableR` as **mutable** `inline uint8_t[]`. They are read-only
lookup tables for `lvlx`/`lvrx`/`stvlx`/`stvrx`/`lvsl`/`lvsr` - nothing anywhere writes one - and the
theory was that as mutable globals the compiler cannot prove a guest store through `base` (a plain
`uint8_t*`) does not alias them, so every guest store would invalidate a cached mask and force a
reload at each of ~5,800 sites. On ARM64 that reload is `adrp`+`add`+`ldr q` where x86 gets a
RIP-relative `movdqa` out of L1, so it looked like a large ARM64-only win for a one-word change.

**Measured, and it is worth nothing.** Disassembling `libreblue.so` before and after, with all 54
recompiled TUs force-rebuilt:

```
ldr 958025   adrp 138913   tbl 11775   ext 1675   rev32 184   rev64 60
```

Byte-identical in both. LLVM already treats these `inline` tables as immutable. The change is kept
because const-correctness on read-only data is right, not because it does anything.

**And a dev-loop trap found on the way: the 54 guest TUs do not rebuild when an SDK header changes.**
After editing `intrinsics.h` the build reported 14 targets and finished; the guest objects still
carried the *previous day's* timestamp and the change was completely inert. Only deleting
`CMakeFiles/reblue_recomp.dir/**/*.o` by hand forced the 111-target rebuild that actually applied it.
This is the same shape as the XenosRecomp two-step already in CLAUDE.md, in a new place: **after any
`out/rexglue-src` header change, delete the guest objects or you are measuring the old build.**
