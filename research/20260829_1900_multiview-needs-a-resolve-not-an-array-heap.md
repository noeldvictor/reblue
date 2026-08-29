# Multiview's last blocker, and the cheap way around it

2026-08-29. Closing out the multiview chase. Three bugs were found and fixed today; the fourth thing
in the way is not a bug, it is the descriptor architecture, and there is a way around it that does
not touch the architecture at all.

## Where it got to

| | |
| --- | --- |
| Scene targets two-layer, `viewMask 3` | works |
| Multiview render pass, framebuffer, pipelines, device feature, `maxViewCount 6` | all correct, validation clean |
| Per-eye skew reaching the shader | **fixed today** - it was emitted after the guest's `return;` and DXC dropped it as dead |
| Skew applying only to scene geometry | **fixed today** - it ran in every vertex shader, so post quads at `w = 1` got a constant slide instead of parallax |
| Post chain writing both layers | works, since every render target became two-layer |
| **Post chain *reading* per view** | **blocked** |

The two views of a single captured frame are identical, and this is why.

## The blocker is the bindless heap's type

`surface_pool.cpp` builds each surface's sampling view as a plain 2D view of layer 0, and the
comment there already says why:

> the bindless heap it is registered in is declared `Texture2D`, so an array view here would be a
> type mismatch

So every post pass samples one eye and writes it to both layers. The scene is rendered in stereo and
then flattened by the first pass that reads it.

Making the post chain view-aware properly means a **second bindless heap declared
`Texture2DArray`**, sampled by `ViewIndex`, which reaches into XenosRecomp's texture declarations,
the descriptor set layout, and every `tfetch` the recompiler emits. On Adreno that also has to fit
inside `maxBoundDescriptorSets = 4`, which is already full - see
`20260828_1720_quest-bindless-blocker.md`. That is a large change with real risk and it should not
be started casually.

## The cheap way round, and why it still gets the win

**The expensive part of stereo is the scene, not the post chain.** A field frame is ~2000 scene
draws against a couple of dozen full-screen passes, and `bd_stereo` doubles *all* of them by
submitting every scene draw twice.

So: render the scene with multiview - one draw, two layers, which is where the saving is - and then
**resolve the two layers into a side-by-side image** on a single-layer target before the post chain
runs. The post chain then proceeds exactly as it does for `bd_stereo` today, mono, over an image
that already contains both eyes, and `xr_session` already splits that into per-eye `imageRect`s.

That needs one blit pass and no descriptor changes: the resolve reads the two array slices with two
explicitly-created single-slice views, which is what `surface_pool` already makes, and writes them
into the two halves of a normal target. Nothing else in the renderer has to know.

The open question is *where* to insert it - the guest owns its render flow, so the resolve has to
land after the last scene draw and before the first post pass. `NoteDrawTarget` already sees every
render-target change, so the transition from the two-layer scene surface to a single-layer post
surface is observable; that is the seam.

## What is worth knowing before starting

- **`bd_stereo` works and is unregressed**: `far +4, near -5, near - far = -9px` on the flat
  desktop, bit-identical to the measurement taken before any multiview work. It is the shipping
  path and multiview is an optimisation of it, not a replacement.
- **Measure both eyes from one frame.** `bd_capture_after_s` captures both array slices stacked when
  the present source is a multiview target. Comparing two runs compares two different scenes,
  because autoplay is not frame-identical across restarts - that mistake made a whole round of
  multiview measurements meaningless earlier today.
- **A uniform disparity at every depth means `w = 1`**, i.e. a full-screen quad is being skewed
  rather than geometry. `2 * separation` in NDC is the signature. That is how the missing
  `scene_pass` gate announced itself, and it will announce a similar mistake the same way.
