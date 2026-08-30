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

## What this does not change

The stereo work stands: multiview renders correct crossed depth, verified from a capture, and the
2D-overlay eye-seam item is closed. What changes is the *performance* plan: before this, the target
was the recompiled guest's per-draw ABI cost. On this evidence the first work is GPU-side and
renderer-side, and the recomp case has to be re-made on a configuration where the GPU is not the
wall.
