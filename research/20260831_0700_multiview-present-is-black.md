# Multiview present is black, and the resolve pass is where it dies

2026-08-31, desktop, no headset. `bd_stereo_multiview` is off by default, so
this is a VR-path bug rather than a shipped one - but multiview *is* the VR path.

## The state

- **The layered targets are correct.** `bd_mv_capture_array` +
  `tools/stereo_check.py --stacked` reads `far -4, near -26`, crossed and
  monotone, and both layers visibly contain the scene.
- **Present is 0.0% non-black** under `bd_stereo_multiview=true`, on the same
  build where the flat path renders 95.7% non-black at mean RGB 59/54/44.

So the geometry is right and the flatten-for-display step is what fails.

## What was eliminated, each by one run

| hypothesis | probe | result |
| --- | --- | --- |
| the scene target never reaches the resolve | unconditional entry census by size | **it does** - `1920x1080 layers=2 companion=true dirty=true` |
| the companion is never allocated | same census | `companion=true` for every size |
| present samples the array, not the companion | `bd_mv_debug_clear` paints the companion magenta | present stayed **black**, so it is not showing the companion's clear either |
| the per-eye slice views are misregistered | `bd_mv_debug_known_srv` samples the surface's own known-good descriptor | still **black** - so the fault is upstream of the per-eye views |
| the resolve does not bind descriptor sets | `frame_ring.cpp:81-84` binds all four once per frame | it does, and the gamma blit relies on the same binding and works |

## Two real bugs found on the way, both fixed

- **`descriptorIndex` cannot be trusted to point at the companion.**
  `ResetPooled` re-binds a recycled surface with `BindTextureSRV`, which points
  the slot back at the *array* image, and `BindResolvedSRV` then early-outs
  because the surface already has a descriptor. The redirect is lost the first
  time a layered surface comes back out of the pool - which is every frame.
  Fixed by giving the resolve its own `resolvedDescriptorIndex`, rebuilt
  whenever the pool re-points the companion, and having present prefer it.

- **`multiviewDirty` is the wrong guard at present.** The scene alternates
  between two pooled 1920x1080 surfaces - the per-target census shows both at
  0.50 binds/frame - so the surface being presented is routinely not the one
  resolved this frame. Measured directly: the resolve is entered for 1920x1080
  with `dirty=true` while present's own rt reports `dirty=false` in the same
  run. Present now resolves whatever it is about to display, which is by
  definition the last thing drawn.

Neither fixed the black frame, which is why they are recorded here as
corrections rather than as the answer.

## Where it actually is

Every sample taken *inside the resolve pass* comes back black, including one
through a descriptor the rest of the renderer uses successfully in the same
frame. That points at the resolve pipeline itself rather than at any descriptor:
most likely a render-pass/framebuffer incompatibility between
`ResolvePipelineFor(format)` and `resolvedFramebuffer`, which Vulkan leaves
**undefined rather than reporting**, and which has already produced exactly this
symptom once in this file's history.

**Next step is a validation run, not another hypothesis.** Khronos publishes no
Windows validation binaries, so this needs either the Android layer via
`tools/validate_quest.sh` on a Quest, or a RenderDoc capture read with
`tools/rdc_outline.py`, which prints per-pass formats and view masks and named a
multiview bug in one line once before.

## Note on the instrument

The resolve's own log prints on the 1st and 501st call of a **single shared
counter**, not per size - so "resolved 960x540" and "resolved 480x270" being the
only two lines does **not** mean the scene was never resolved. Reading it that
way sent this investigation down a dead end. The unconditional per-size entry
census added here is what settled it.
