# Research: the bottleneck is not VR, and not the things that were optimised

Date: 2026-08-28 23:30
Topic: what a field scene actually costs on a Quest 2, measured with `bd_perf_csv` on device.

Gameplay runs at 5.5 fps. This note records what that is *not*, because five separate hypotheses
were tested and eliminated in one session, and the answer is uncomfortable enough to be worth
writing down precisely.

---

## The measurement

`bd_perf_csv` needs no rebuild - set it in `args.txt`, relaunch, pull the CSV. A field scene, VR on:

```
dt_ms 182.6 | fence_ms 110.0 | other_ms 72.2
draws 2957  | pso_switches 1121 | fb_binds 23 | barrier_calls 82
```

Two halves: **~110ms waiting on the GPU fence, ~62-72ms of CPU** (guest simulation plus draw
recording). The GPU half is the larger one.

## What it is not

Each of these was measured on device, one variable at a time, with `args.txt` and no rebuild:

| Change | fence | Verdict |
| --- | --- | --- |
| baseline, 720p | ~110ms | — |
| MSAA 4x off | ~110ms | no effect |
| shadow map 1024 -> 512, shadow distance 1.0 | ~108-112ms | **no effect** |
| reflection resolution quartered | ~108-112ms | **no effect** |
| render height 720 -> **360** | ~108-110ms | **no effect** |
| **VR disabled entirely, flat renderer** | **~109ms** | **no effect** |

The last two are the important ones.

**Resolution does not matter.** Halving the render height changes nothing, so the GPU is not
fill-bound, not blend-bound, and not bandwidth-bound on the framebuffer. That eliminates every
setting in the quality menu and, notably, foveated rendering - which only reduces shading rate, and
shading is not what is costing the time.

**VR does not matter.** With `bd_vr_enabled false`, the flat renderer, no OpenXR session, no
projection layer and no camera override, the same scene costs **2925 draws, 108.9ms on the fence,
183ms a frame**. Identical within noise.

So: **Blue Dragon runs at 5.5 fps on a Quest 2 with VR switched off.** The VR work - the session,
the layer, the camera composition, the pad - costs essentially nothing. The port is slow, and VR was
never the reason.

## What is left

A GPU cost that is insensitive to resolution, with ~2925 draws per frame, on a tile-based deferred
renderer. That shape points at the **binning pass**: a tiler runs all geometry through vertex
processing and bins it into tiles *before* any shading, and that work scales with draw calls and
vertex count, not with pixels. It is exactly the cost that would not move when the resolution is
halved.

2925 draws a frame is a lot for an Adreno 650. It was fine on a Xenos, which had a dedicated
hardware command processor and no binning pass to speak of.

Supporting numbers from the same frame: **1121 PSO switches** for 2957 draws, so the pipeline dirty
gate fires on 38% of draws; 23 framebuffer binds; 82 barrier calls.

**This is not yet proven.** `gpu_total_ms` in the CSV reads 3.5ms, which contradicts a 110ms fence -
but that column is unreliable here, because `MarkDraw` writes a timestamp per draw into a 512-entry
pool and 2957 draws saturates it long before the frame ends. The next step is a GPU profiler that
can attribute properly (`ovrgpuprofiler` on Quest, or Snapdragon Profiler), not another guess.

## Corrections this session forced

Worth recording together, because the pattern is the same each time - a plausible mechanism,
measured, and wrong:

- **`non_argument_as_local`** cut context accesses 36% and **miscompiles the guest**. The game dies
  during startup. Reverted. A static metric said nothing about correctness.
- **The constant byte-swap** was predicted to be the top CPU cost (8 KiB per draw into
  write-combine, ~24 MB a frame). Hand-vectorised with NEON, verified bit-identical on the Quest's
  own CPU - **no measured gain**, almost certainly because clang at `-O3` had already
  auto-vectorised the loop.
- **MSAA** was dismissed as free on the strength of a title-screen measurement. It is free in a
  field scene too, but for a different reason than assumed, and the title screen should never have
  been used as evidence.
- **LSE atomics, fp16, dotprod, FPSCR flush-mode, indirect dispatch** - all eliminated by counting
  instructions rather than by testing.

## Also found

**`src/xr/xr_cull.cpp` is dead code.** Nothing outside the file references `CullVolume` or
`bdCameraViewFrustumTest`. The combined two-eye cull volume was written, unit-tested, and never
connected to anything. It is not causing the draw count - the flat renderer submits the same 2925
draws - but if draw-call reduction is the answer, that file is where the VR half of it already
lives.

## Proven: the frame is draw-bound

`bd_debug_max_draws` caps how many draws are submitted per frame. The frame renders wrong while it
is set - it is a measurement, not a setting - and it answers the question directly:

| draws submitted | frame | fence | CPU (`elsewhere`) |
| --- | --- | --- | --- |
| ~2925 (all) | 183.0ms | **112.8ms** | 60.7ms |
| 500 | **30.1ms** | **0.1ms** | 29.5ms |

Capping to 500 draws takes the GPU wait from 112.8ms to essentially nothing, and the frame from
5.5 fps to 33 fps. **The binning theory holds: the frame is draw-bound.**

Read `fence` as *waiting*, not as GPU work: at 500 draws the GPU finishes inside the CPU's 29.5ms so
there is nothing to wait for. Working the cost out per draw from both points gives roughly **60
microseconds of GPU time per draw**, near enough linear rather than a cliff.

**60µs a draw is the anomaly.** A simple draw on a mobile GPU should be single-digit microseconds.
2925 of them at that price is 175ms, which is the frame. This is not a content problem - it is a
per-draw constant that is roughly two orders of magnitude too large, and it is resolution
independent, which is why every quality setting failed to move it.

That per-draw constant is now the whole problem. Everything else measured in this note is a
consequence of it.

## The frame, fully attributed

Vertex counting and per-phase timers in `DispatchDraw`, both permanent, reported every ~5s:

```
2848 draws/frame, 398959 verts/frame, 140 verts/draw
per frame: mutex 0.5ms, bindFB 1.8ms, flushState 11.8ms
frame 159.2ms = fence 89.8 + elsewhere 60.7
```

Which resolves the frame into three parts:

| | cost | note |
| --- | --- | --- |
| GPU (`fence`) | **~90ms** | 2848 draws of 140 vertices each |
| Guest simulation | **~46ms** | `elsewhere` minus the measured draw phases - recompiled PowerPC |
| Draw recording | ~14ms | mutex 0.5 + bindFB 1.8 + flushState 11.8 |

**Only 400K vertices a frame, at 140 vertices per draw.** That is nothing for an Adreno 650 - it is
not a geometry problem. The GPU is spending **~32 microseconds per draw** on draws that move 140
vertices, which is one to two orders of magnitude more than such a draw should cost.

### Three renderer hypotheses killed by measurement

All three came from a careful reading of the draw path and all three were wrong:

- **Mutex contention: 0.5ms a frame.** The renderer mutex is taken per draw and per state-setting
  hook, plausibly 10-30 times a draw, and it costs essentially nothing. Not worth converting to
  atomics.
- **`BindDrawFramebufferLocked`: 1.8ms a frame**, including the 16-slot `TransitionResolveSources`
  scan that runs before the cache-hit early-out. Also worth noting that moving that scan after the
  early-out - which looked like free wins - would be a **correctness bug**: it reads `s.textures`,
  which changes per draw independently of the framebuffer.
- **The 8 KiB constant byte-swap** had already been eliminated separately by vectorising it for no
  gain.

`FlushRenderStateLocked` at ~12ms is the only renderer CPU cost worth anything, and eliminating it
entirely would buy 12ms of a 159ms frame.

**So the renderer's CPU path is not the problem, and neither is geometry.** The problem is ~32us of
GPU time per trivial draw, plus ~46ms of guest simulation.

## What to do next

1. **Find what costs 60µs in a single draw.** This is the question now, and it is narrow. Candidates
   worth testing in order: a pipeline switch on 38% of draws (1121 PSO switches for 2957 draws); a
   render-pass or tile flush being provoked per draw; per-vertex work in the recompiled vertex
   shaders (the `swapFloats`/`sintTexcoord` fixups read their masks with `vk::RawBufferLoad` per
   invocation, which is a buffer load per vertex rather than a uniform); or enormous vertex counts
   per draw, which is not currently recorded anywhere.
   `ovrgpuprofiler -e` then `-t` gives a render-stage trace on the Quest, but it needs the app
   started *after* detailed mode is enabled and it needs to be tracing while a heavy scene is
   actually on screen - both awkward with autoplay, and it produced nothing usable here.
2. **Count the geometry, not just the draws.** If binning is the cost, vertex count per frame is the
   number that matters and it is not currently recorded.
3. **Draw-call reduction is the likely lever** - more aggressive frustum culling through
   `bdCameraViewFrustumTest` (`0x82135030`), distance culling, or LOD. All of it is guest-side
   behaviour, which is why the renderer settings could not touch it.
4. **Stop testing hypotheses one rebuild at a time.** Every eliminated cause above cost a deploy;
   the profiler answers the question directly.
