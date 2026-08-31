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

## Side-by-side stereo works end to end. VR is not blocked.

Same build, same scene, `bd_stereo=true` with multiview off:

```
non-black 95.8%   mean RGB 60/55/44
far +4, near -7  ->  near - far = -11 px, crossed and correct
```

Looked at, not just measured: a proper stereo pair, both eyes carrying the full
scene. **So the VR path has a working stereo route today** - it is multiview
specifically that presents black, and multiview is off by default. Anyone
picking this up should use `bd_stereo` and treat multiview as the optimisation
it is, not as the thing blocking VR.

## RenderDoc confirms the passes execute

`bd_renderdoc` + `tools/rdc_outline.py` on a multiview frame, 50 passes:

```
[ 8] fb=15158 1920x1080 mask=3 STEREO pipes=8 draws=157   <- scene, stereo
[12] fb=1605  1920x1080 mask=0 mono   draws=2 vp=960x1080@960   <- its resolve
...
[48] fb=1953  1920x1080 mask=0 mono   draws=2 vp=960x1080@960   <- resolve at present
[49] fb=1568  1920x1080 mask=0 mono   draws=1 vp=1920x1080@0    <- the present blit
```

The scene renders stereo (`mask=3`, 157 draws over 8 pipelines), every resolve
runs mono with the two half-width viewports the eye loop asks for, and present
blits full-screen. **Structurally the whole chain is correct and executing** -
which rules out "the pass never runs" for good.

Note `pipes=0` on the resolve passes is not evidence of a missing pipeline:
plume dedups `vkCmdBindPipeline`, so a pipeline reused across passes is only
counted where it changes.

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

## The whole multiview frame is black, not just present

The decisive run: `bd_mv_redirect_srv=false`, which points every sampled read at
the array's own layer-0 2D view instead of at the resolved companion.

**Still 0.0% non-black.** So this is not a resolve bug and never was - *nothing*
can sample the two-layer scene target, which is why the post chain, the resolve
and present are black together. One cause, three symptoms.

That reframes every earlier entry in this note: they were all measuring the same
failure from different ends.

### `bd_mv_debug_known_srv` is not a valid probe, and it misled this hunt

It samples `tex->descriptorIndex` "instead of the per-eye views" - but with
`bd_mv_redirect_srv` on (the default) `descriptorIndex` *is the companion*. So
inside the resolve it samples the companion in order to write the companion.
That is circular and returns black by construction, whatever the per-eye views
are doing. It was read here as evidence exonerating them; it is not evidence of
anything. Either fix it to sample the array explicitly or delete it.

### One real Vulkan bug found and fixed, which was not the cause

`RenderTextureViewDesc::arraySize` defaults to `UINT32_MAX`, which plume turns
into the image's full layer count. surface_pool never set it, so the sampling
view a comment describes as "a plain 2D view of layer 0 even when the image has
two layers" was in fact a **2-layer view with `VK_IMAGE_VIEW_TYPE_2D`** - which
Vulkan forbids, that view type requiring `layerCount == 1`. Now sets
`arraySize = 1, arrayIndex = 0`.

Correct on its own terms and verified not to regress the flat path (95.5%
non-black), but the multiview frame stays black, so something else is still
wrong.

### Also eliminated

- **The deferred draw queue.** `bd_draw_defer=false` under multiview is black
  too. It has now been exonerated twice, on two different symptoms.

## Where to start next, and why it needs the Quest

Eleven desktop runs went into this. What is now known, all measured:

- the scene **renders correctly** into the layered target - the array grab still
  reads `far -4, near -26`, crossed and correct, on the current build
- **copies off that image work** (the array grab is a `copyTextureRegion`)
- **every sample of it returns black**, through the companion, through the
  array's own view, and through the per-eye slice views
- it is not the resolve, not the draw queue, not the descriptor redirect, not
  `multiviewDirty`, and not the view's `arraySize`
- the image is created with the same flags as every single-layer target, which
  sample fine

An image that copies but does not sample, with a legal view and correct
contents, is a layout or a usage-flag problem - and both are exactly what the
validation layers report in one line. **Khronos publishes no Windows validation
binaries**, so the next step is `bash tools/validate_quest.sh` on a Quest, not
another desktop hypothesis. That is where this should resume.

`bd_stereo` works and is the stereo path to use meanwhile.

## Note on the instrument

The resolve's own log prints on the 1st and 501st call of a **single shared
counter**, not per size - so "resolved 960x540" and "resolved 480x270" being the
only two lines does **not** mean the scene was never resolved. Reading it that
way sent this investigation down a dead end. The unconditional per-size entry
census added here is what settled it.
