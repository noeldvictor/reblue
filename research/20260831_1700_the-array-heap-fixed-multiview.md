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
