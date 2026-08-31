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

`bd_stereo` remains the working stereo path and is unaffected by any of this.
