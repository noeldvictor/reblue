# The array bindless heap fixed the black multiview frame

2026-08-31, desktop, no headset.

## What changed

The bindless 2D texture heap is now `Texture2DArray` and every 2D read carries
an array layer of `SV_ViewID`. This is the change described here for weeks as
"the right way" and deferred as too invasive.

| | |
| --- | --- |
| XenosRecomp | array heap, `BD_UV`/`BD_LOAD` coordinate macros, `SV_ViewID` on **pixel** shaders, `ps_6_1` |
| plume | `RenderTextureViewDimension::TEXTURE_2D_ARRAY` |
| host shaders | gamma, copy_color, copy_depth, cel, bd_2d_blit |
| present | flattens the pair itself - left half layer 0, right half layer 1 |

**Multiview present: 0.0% -> 95.7% non-black.** The diagnosis held exactly:
nothing could sample a two-layer target through a `Texture2D` view, which is why
the post chain, the resolve and present were black together, and why the resolve
could never work - it was itself such a sample.

**Ordinary textures needed no special handling.** Vulkan clamps the array layer
to `[0, layers-1]`, so a one-layer texture read at layer 1 returns layer 0. That
one spec detail is what made a single heap viable and the whole change
tractable.

The five full-resolution resolve passes - **79.5 MB/frame of Quest tile
traffic** - are replaced by one branch in a blit that was already happening.

Flat path verified unregressed and pixel-correct throughout: 95.8% non-black,
mean RGB 61/56/44.

## What is still wrong, located by measurement rather than by guessing

Multiview renders a side-by-side pair whose **two halves are identical**, while
the scene array still measures `far -4, near -26`, crossed and correct.

Two plausible fixes were tried and both missed:

- **the sampling view exposed one layer.** `arraySize` was pinned to 1 by the
  earlier fix for the illegal one-layer `VK_IMAGE_VIEW_TYPE_2D` view - right
  about the view type, wrong about the count. Now `arraySize = layers`.
- **`surface->layers` was assigned after the views were built**, so
  `BindTextureSRV` read 1 and built a one-layer view regardless. Hoisted.

Both are real bugs and both are fixed. Neither was the cause.

So a probe was built instead of a third guess. `bd_mv_debug_layer_diff` makes
present output `|layer1 - layer0|` amplified:

```
|layer1 - layer0| at present: 0.0% of pixels differ
```

**Present's surface has identical layers.** The pair is flattened somewhere in
the post chain, upstream of present, and the present-side flatten is innocent.
The scene array carries stereo; whatever the post chain hands present does not.

That is a narrow, well-defined bug with a reusable instrument pointed at it,
where there used to be a black screen.

Worth noting for whoever picks it up: post pipelines *are* multiview (the census
reads `4001 of 4000 draws on two-layer targets had a multiview pipeline`) and
post pixel shaders *do* carry `SV_ViewID` (checked in the dump - `bd_blur_ps`
declares it). So the mechanism is present and something narrower is defeating
it - most likely one pass whose source descriptor is a single-layer view, which
would make both eyes read the same input and produce identical output legitimately.

## Found: the guest's EDRAM resolve is where the pair is flattened

`resolve.cpp` does not contain the words `layers`, `arraySize` or `slice`. The
guest's EDRAM resolve is a full-screen draw through `copy_color_ps`, which read
layer 0 - so it copied the left eye and discarded the right before the post
chain ever ran.

Fixed properly: `copy_color_ps` takes `SV_ViewID` (and moves to `ps_6_1`), the
resolve framebuffer takes `viewMask = 3` when its destination is layered, and
`GetOrCreateResolvePipeline` gained a matching mask keyed into its cache - a
pipeline and framebuffer that disagree on the mask is a render-pass
incompatibility, which Vulkan leaves undefined rather than reporting.

**That was necessary and not sufficient, and the reason is architectural.** The
guest resolves its two-layer scene target into an *ordinary guest texture*, and
those are single-layer. Present now shows a mono full-screen image rather than a
side-by-side pair, because the surface it samples has one layer:

```
scene RT (2 layers, correct stereo)  ->  RESOLVE  ->  guest texture (1 layer)
                                                          -> post chain -> present
```

So the last mile is: **a resolve whose source is layered must have a layered
destination.** Guest textures that receive such a resolve need two layers
allocated under multiview, the same way `surface_pool` already gives two layers
to render targets. That is a contained change in guest-texture creation, and it
is the only thing between here and multiview stereo end to end.

## A note on the instrument, again

After the resolve fix `tools/stereo_check.py --raw` reported
`far -90, near +90, spread 180px, INVERTED`. That is nonsense, and looking at
the capture said why immediately: the frame is a **mono full-screen image**, so
the tool was matching two halves of unrelated scene content. A stereo verdict on
an image that is not a stereo pair is not a weak signal, it is noise. **Look at
the capture before believing the number** - the same lesson this file recorded
about `--raw` on a composited Quest panel.

`bd_stereo` remains the working stereo path and is unaffected by any of this.
Flat path re-verified after every step above: 95.6% non-black, mean RGB
60/54/44.


## Layered resolve destinations: built, and the right eye is now black

Guest textures that can be render targets are allocated with **two layers** when
`bd_stereo_multiview` is on - they are what the guest resolves its two-layer
scene into, and a single-layer destination collapsed the pair at that copy. The
view exposes both layers and `texture->layers` is set.

With that in, and a correct config (`39` two-layer targets, present rt
`layers=2`), present emits a side-by-side pair in which **the left half renders
the scene and the right half is black**. So layer 1 of the final surface is
never written.

That is a step past "both halves identical" - the halves are now genuinely
different surfaces - but it is a **regression against the previous state**,
where the scene array at least held two populated layers. Multiview is off by
default, so nothing shipped is affected, and the flat path is unregressed
(95.7% non-black, mean RGB 64/58/46).

The likely cause, for whoever continues: giving these textures two layers makes
framebuffers built from them layered, so `draw_framebuffer.cpp` gives them
`viewMask = 3` - but the draws that write them must then have multiview
*pipelines*. A framebuffer with a view mask and a pipeline without one writes
layer 0 only, which is exactly the symptom. Check `s.pipelineState.multiview`
for the passes that target these textures, and note the census line
`of 4000 draws on two-layer targets, N had a multiview pipeline` only covers
draws the census sees.

### Gated off, not reverted

`bd_mv_layered_textures` defaults **false**. With it off, multiview present is
back to a complete image duplicated into both halves - measured byte-identical,
`mean abs diff 0.00`, `0.0%` of pixels differing - which is not stereo but is
better than a black right eye. With it on, the left half renders and the right
is empty.

The change is kept rather than reverted because the destination genuinely has to
be layered: a resolve whose source has two layers cannot preserve them into a
one-layer texture. It is the symptom that is unexplained, not the requirement.

**Where the two states leave multiview, precisely:**

| | present | halves |
| --- | --- | --- |
| `bd_mv_layered_textures=false` (default) | 95.8% non-black | **identical** (0.00 diff) |
| `bd_mv_layered_textures=true` | 47.9% non-black | left renders, **right black** |

Both are wrong; the first is wrong in a less destructive way. Note the pipeline
flag is not the obvious culprit - `state.cpp` sets
`pipelineState.multiview = surface->layers > 1`, so a two-layer guest texture
bound as a render target does get multiview pipelines. That eliminates the first
guess.

### Three well-founded fixes, none of which was the cause

For the record, so the next attempt does not re-tread them. Each was a real bug,
each is committed, and none made the right eye render:

1. **`arraySize` pinned to 1 on the sampling view** - the earlier illegal-2D-view
   fix was right about the view type and wrong about the count.
2. **`surface->layers` assigned after the views were built**, so `BindTextureSRV`
   read 1 and built a one-layer view regardless. Hoisted.
3. **No stale-view guard on the primary sampling view.** `multiview_resolve`
   guards its per-eye views with `layerViewOf` because surface_pool is a *pool*
   and can hand a GuestTexture a different image later - the main path had no
   equivalent, so a descriptor could keep sampling an image the surface no
   longer owns. Added `textureViewOf`, which rebuilds the view and re-points the
   existing descriptor rather than leaking a new slot.
4. **`plume::copyTexture` hardcoded `layerCount = 1`** - it loops over mip levels
   but copied only array layer 0, so the guest's 1:1 EDRAM resolve kept the left
   eye and dropped the right. Now copies every layer, and that is a correctness
   fix independent of multiview.

**CORRECTION.** That reasoning was wrong, and the instrument that settled it is
in `present.cpp` now:

```
[mv] present sample site: image=02249D40F410 viewOf=02249D40F410
     rt.layers=2 view.layers=2 desc=125 resolvedDesc=INVALID
```

The sample site is **correct in every respect** - the view is built against the
live image, it exposes both layers, the descriptor is valid, and the companion
path is not being taken. So sampling is not the fault.

The earlier conclusion ("the image is right, the sampling is wrong") came from
comparing the **scene** array capture against **present's rt**. Those are
different surfaces: `bd_mv_capture_array` picks the largest colour+depth target,
which is the scene, while present's rt is the end of the post chain. The scene
having two good layers says nothing about what present is handed.

So the fault is upstream after all, and inspection names the shape of it:
**only one host pipeline in the renderer sets a view mask** - the resolve
pipeline fixed above. `GetOrCreateCopyDepthPipeline`,
`GetOrCreateResolveMSAAPipeline` and `copy_color_pipeline` do not, so any of
them drawing into a two-layer target writes layer 0 and leaves layer 1 as it
found it. That is exactly the symptom, and it is a mechanical fix: give those
three the same `view_mask` parameter and cache key the resolve pipeline now has,
then find which one is actually in the chain.

Also eliminated by reading rather than by running: `state.cpp` sets
`pipelineState.multiview = surface->layers > 1`, so a two-layer guest texture
bound as a render target does get multiview pipelines.

What remains unexplained is narrow: with `bd_mv_layered_textures=true`,
present's rt is `layers=2`, its layer 0 holds the scene, and its layer 1 is
empty. Something that writes these textures is not producing layer 1. The next
instrument should be a per-pass layer-1 check - the `bd_mv_debug_layer_diff`
probe already added to present, applied earlier in the chain - not another
hypothesis.

## And a process failure worth recording

Two "multiview" measurements in this session were actually **flat-path runs**.
`tools/`-style helper scripts that write `profiles/default/reblue.toml` - the
`dtest` helper used throughout this session for regression checks - overwrite
whatever configuration was there. Running a flat regression check between two
multiview experiments silently reconfigured the next one.

The symptom was a confident `far -90, near +90, INVERTED` from
`stereo_check --raw` on a frame that was simply mono. **Print the `[config]`
audit line and the two-layer target count in every multiview run**, both of
which are already in the log:

```
[config] all 8 settings in reblue.toml took effect
[mv] present rt=... layers=2
2-layer targets: 39
```

`layers=1` or `0` two-layer targets means the run is not testing multiview,
whatever the intent was.


## The black right eye is fixed: host pipelines had no view masks

The correction above named it and it was right. Only the resolve pipeline
carried a `viewMask`; `GetOrCreateCopyDepthPipeline` and
`GetOrCreateResolveMSAAPipeline` did not, and the depth-only resolve framebuffer
did not either - so any of those drawing into a two-layer target wrote array
layer 0 and left layer 1 as it found it.

All three now take a `view_mask`, keyed into their pipeline caches the same way
the resolve pipeline is, and the callers pass `dst->layers > 1 ? 3 : 0`.

| | left half | right half | halves differ |
| --- | --- | --- | --- |
| before | 95.8% | **0.0%** | n/a |
| after | 95.8% | **95.8%** | **no** (0.00) |

**Layer 1 is now written.** What it is written with is layer 0's content, so the
frame is back to two identical halves - the same place the non-layered path
sits, but reached honestly: every layer is now populated by a pass that knows
about layers.

So the remaining question is the narrow one it always should have been: **the
scene renders two different layers (measured `far -4, near -26`) and something
between there and present is copying one of them into both.** With every host
pass now view-masked, the candidates are down to the shaders those passes run -
and `copy_depth_ps` is a known one: it was given `BD_L0(uv)` with a hardcoded
layer 0, not the per-eye `g_ViewIndex` that `copy_color_ps` got. Start there.

Flat path unregressed throughout: 95.5% non-black, mean RGB 61/55/44.
