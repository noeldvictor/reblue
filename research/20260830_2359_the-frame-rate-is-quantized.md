# The frame rate is quantised. 6ms is worth a whole tier.

2026-08-30. Quest 2, 60Hz refresh (`bd_xr_refresh_rate=60`).

## What the data says

`dt_ms` does not vary continuously with GPU time. It lands on whole multiples of the refresh
interval, because the compositor paces the app to a scanout:

| run | `gpu_total_ms` | `dt_ms` | slot distribution |
| --- | --- | --- | --- |
| baseline | 56.18 | 66.88 | **4 slots (66.7ms)** |
| foveation, strength 0.15 | 70.00 | 83.50 | **5 slots (83.4ms), 90% of frames** |

90% of field frames in the second run sit exactly on 5 x 16.67ms, the remaining 9% on 4. Frame time
is not a continuous function of the work; it is `ceil(work / 16.67) * 16.67`.

## Why this changes the targets

At 60Hz the tiers are:

| GPU under | slots | fps |
| --- | --- | --- |
| 66.7 ms | 4 | 15 |
| **50.0 ms** | **3** | **20** |
| 33.3 ms | 2 | 30 |
| 16.7 ms | 1 | 60 |

The port sits at `gpu_total 56.18ms`, which is 4 slots. **Getting 6.2ms off moves it to 3 slots and
20 fps** - a 33% frame-rate gain for an 11% GPU saving. Nothing about that is visible if you only
watch `dt_ms`, and it is invisible in a mean over a whole run.

It also explains why several changes today measured as "neutral": a 1-3ms saving inside a tier is
genuinely worth zero fps, and a 3ms saving that crosses a boundary is worth 5 fps. The same change
can be either, depending only on where the frame happens to sit.

## How to measure from here

- **Quote `gpu_total_ms`, not `dt_ms` or fps.** `dt_ms` is quantised and fps is its reciprocal, so
  both hide the work and both move in jumps.
- **State the distance to the next tier**, because that is what decides whether a saving is worth
  anything: from 56.18ms the next boundary is 50.0ms, so the budget is 6.2ms.
- A change that saves less than the distance to the boundary shows as **exactly zero fps** and is
  still real. Do not report it as "no effect"; report the millisecond change and the remaining gap.

## The 6.2ms that is nearest to hand

The per-target GPU census puts the scene pass at ~45ms and the whole post chain under 8ms, of which
the full-resolution passes are ~3ms. Five of the frame's ~22 passes are single-draw full-screen post
passes at 1376x720 **and two layers**, so each rasterises twice under multiview - and the post chain
does not need stereo, because the resolve flattens the pair before anything downstream reads it.

`bd_mv_small_targets_mono` already exists for exactly this and is limited to targets below half the
design canvas, which excludes precisely the full-resolution ones that would pay. Extending it is the
cheapest candidate for the 6.2ms, and unlike foveation it removes work rather than adding a
mechanism.

## Caveat on the cull experiment

An attempt to force GPU under 50ms with `bd_cull_distance=220` produced a run whose draw count never
exceeded 127 - it never reached a field scene at all, the same failure mode as the 4x MSAA run
earlier. It says nothing about culling. Autoplay does not reliably reach a field scene, which is why
`bd_capture_min_draws` exists for captures; the perf summary needs the same treatment.
