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


## Stereo on the headset is NOT verified for the current build (RESOLVED - see below)

Stated plainly because I reported the opposite earlier from a stale file.

- The composited panel image (3664x1920, what the compositor actually shows) from a fresh run of the
  current build reads **`far -80, near -80`, spread 0 - FLAT**. A uniform inter-eye offset with no
  variation by depth.
- The stale capture from a previous day's build read `far +57, near -80` on the same tool and the
  same format, so the format is not inherently unmeasurable and the tool can see depth in it.
- **`bd_mv_capture_array` does not work on device.** Asked for the layered scene target it wrote a
  single-layer `1376x720` image that is **entirely black** - it picked a post/intermediate surface,
  not the scene. So the direct "do the two layers differ" test is unavailable on the Quest until that
  instrument is fixed. That is the third instrument in this family to be wrong on device.

**What is actually established:** multiview stereo has correct crossed depth *on the desktop*
(`far -4, near -26`, array capture, verified repeatedly today). On the Quest it is unverified, and
the one fresh measurement available says flat.

**What it is not safe to conclude:** that stereo is broken on device. The panel image is
post-composition and may be post-distortion, the eye rects inside it are the runtime's and not
necessarily exact halves, and the one instrument that would settle it is itself broken here.

**Next, and it is a correctness question rather than a performance one:** fix
`bd_mv_capture_array`'s surface selection on device (it should follow `last_scene_rt` and that
surface should be the 2-layer one the per-target census shows as `1280x720x2L`), then re-measure.
Do not tune `bd_stereo_separation` or touch the eye sign until an instrument that works on the
headset says which way it is wrong.


## RESOLVED: multiview stereo IS verified on the Quest 2

The instrument was wrong, not the renderer.

`bd_mv_capture_array` photographs `last_scene_rt`, and `draw_framebuffer.cpp:424` set that to the
**last** colour+depth target of the frame - which on device is a small late depth pass, not the
scene. It now keeps the **largest** colour+depth target instead, and the capture path logs what it
picked so a wrong choice is visible rather than silent:

```
[mv] capture_array picking 1376x720 layers=2 (scene rt 0x1bbcb0310)
```

With that, the device capture is `RGBA16F 1376 1440` - two stacked 1376x720 layers - and:

```
band (y%)   disparity(px)
     32%           -2      <- distant
     44%           -9
     52%          -11
     62%          -16
     72%          -18
     82%          -22
     90%          -24
     95%          -25      <- near

far -2, near -25  ->  near - far = -23 px
OK: crossed disparity, near separating more than far.
```

**Monotone across all eight bands.** Layer 0 is a real field scene (extrema 1-255) and 81.4% of
pixels differ between the eyes with a mean difference of 11.59. **Multiview stereo works on the
Quest 2 and has correct depth.**

**The earlier "flat" reading was a bad input, not a regression.** It came from the composited
3664x1920 panel image, which is post-composition and post-distortion, with eye rects the runtime
chooses rather than exact halves. `stereo_check --raw` without `--stacked` is not valid on that
image, and the number it produced (`far -80, near -80`) should be disregarded - as should the
`far +57 / near -80` from the stale capture earlier, which was a different build entirely.

**Use `--stacked` on a `bd_mv_capture_array` grab for a stereo verdict on device. Nothing else.**

## What this unblocks

The GPU work. The two-layer post chain can now be changed with a working way to check that stereo
survives it, which is what made it too risky before.


## The three big targets are all scene passes, not post

The census now records whether a depth-stencil was bound, which is what separates a 3D pass from a
post target of the same size. On device:

```
1280x720x2L depth:        54 draws/frame,  49 Mpix
1280x720x2L depth:        54 draws/frame,  49 Mpix
1280x720x2L depth:        53 draws/frame,  48 Mpix
1376x720x2L colour-only:   5-8 draws
 344x193x2L depth:       17-30 draws  (x2)
```

**All three carry depth.** So the fill is real scene geometry, not post-processing, and the
"make the post chain mono" idea would save almost nothing - the post targets are the 1376x720
colour-only one (5-8 draws) and the small ones. That idea is retired before it was built.

**The open question is now whether those three are one logical surface or three.** `surface_pool` is
a pool and there are 3 frame slots, so the same logical scene target can appear as three distinct
pointers - and the census keys on the pointer deliberately, because keying on dimensions once hid a
pass that re-rendered the scene into a second surface of the same size. If they are frame-slot
alternates the real fill is ~49 Mpix/frame and not 146, and the three rows are triple-counting.

**How to tell**, and it is cheap: the per-row `draws/frame` is `t.draws / ticks` over the whole
150-tick window, so alternating slots would each show roughly a third of the true per-frame count.
Summing the rows against the frame's own `draws` column settles it - the sum here is ~229 against a
median of ~523 draws, which does not match either hypothesis cleanly and means the 24-row table is
also dropping surfaces. Fix the accounting before drawing a conclusion about the fill.

**Do not act on the 146 Mpix figure until that is resolved.** It is the difference between "the
scene is rendered three times" and "the census counted one surface three times", and those have
completely different fixes.
