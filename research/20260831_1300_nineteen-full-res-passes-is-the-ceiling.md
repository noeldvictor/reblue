# 72Hz is arithmetically impossible at 19 full-resolution passes

2026-08-31. Desktop RenderDoc capture, Quest 2 arithmetic.

## The count

`bd_renderdoc` + `tools/rdc_outline.py` on one multiview field frame:

```
50 render passes in the frame
  full-resolution (>=1280 wide): 19, carrying 481 draws
  smaller post targets:          31, carrying 132 draws
```

**Nineteen full-resolution render passes.** The scene needs two or three of
them. The rest are post-processing steps that each take a fresh full-size
surface, and the per-target GPU census agrees: **14 distinct 1920x1080
colour-only targets and 6 at 1280x720, carrying about 18 draws between them.**
Twenty full-size surfaces for eighteen draws.

That is the X360 shape exactly. On a Xenon, taking a new EDRAM tile and
resolving it out was nearly free, so a pass per effect cost nothing. On a tiler
it is the most expensive thing in the frame.

## Why it caps the frame rate, before any shading happens

A tiled renderer loads the render target into tile memory at the start of a
pass and stores it back at the end. That traffic is proportional to the target,
not to the draws.

```
one full-res target, both layers (1376x720x2, RGBA8):  7.9 MB
a pass that loads AND stores it:                      15.9 MB
19 such passes per frame:                            301 MB/frame

   at 15 fps ->  4.5 GB/s of tile traffic alone
   at 30 fps ->  9.0 GB/s
   at 72 fps -> 21.7 GB/s
```

**A Quest 2 is LPDDR4X: roughly 25-30 GB/s for the entire system** - textures,
vertices, the CPU, the compositor, everything. So at 72Hz this pass structure
alone would need most of the machine's total memory bandwidth *before a single
texel is sampled or a single triangle shaded*.

72Hz is not a shading problem, a draw-call problem, or a CPU problem. **At
nineteen full-resolution passes it is arithmetically out of reach**, and no
amount of making the passes cheaper internally changes that - the cost is
proportional to how many times the frame is loaded and stored, which is what
"remove the EDRAM semantics" means concretely.

## What this reframes

- **The CPU work is not the ceiling.** The recompiled guest, the per-node draw
  submission, the constant marshalling - all of it is downstream of a frame that
  cannot fit in bandwidth anyway. That is consistent with the two negative
  results this session: host code replacing guest code measured *slower*, and
  draw batching measured zero.
- **Foveation and render scale attack the wrong axis.** Both reduce shading
  within a pass. Neither reduces the number of loads and stores, which is what
  this arithmetic is about.
- **`bd_render_scale` does help here** - it shrinks the targets, so tile traffic
  falls with the square - which is why quarter-scale looked so dramatic and why
  it also destroyed the image. The lever is right, the axis is wrong: cut passes,
  not pixels.

## What to do, in order

1. **Count the passes on a Quest** and confirm the number is 19 there too. The
   capture above is the desktop; the guest pass structure should be identical,
   but this whole note rests on that count.
2. **Kill the loads.** A pass that fully overwrites its target does not need to
   load it - `LOAD_OP_DONT_CARE` halves the traffic for every such pass. This
   file previously recorded that forcing DONT_CARE "changed nothing", but that
   was measured when the frame was CPU-bound at 100ms+, before the constant
   rewrite made it GPU-bound. **Re-test it.**
3. **Merge post passes.** Eighteen draws across twenty full-size surfaces is the
   clearest possible statement that these are separable-by-habit rather than by
   need. Each pair merged removes 15.9 MB/frame.
4. Only then worry about what happens inside a pass.
