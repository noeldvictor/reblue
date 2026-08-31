# The multiview resolve chain is both the black frame and a quarter of the bandwidth

2026-08-31. One RenderDoc capture, classified.

## The count

Of the 19 full-resolution render passes in a multiview field frame:

```
  3  scene geometry      (221, 157, 84 draws)
  5  MULTIVIEW RESOLVES  (2 draws each: two half-width viewports)
 11  other post passes   (0-2 draws each)
```

**Five full-resolution passes per frame exist only to flatten a two-layer array
into a side-by-side companion.** On a Quest 2 a full-res two-layer target is
7.9 MB, so a pass that loads and stores it moves 15.9 MB: the resolves alone are
**79.5 MB/frame, 26% of the frame's 301 MB of tile traffic**.

They exist because the bindless texture heap is declared `Texture2D`, so nothing
downstream can read an array. Every layered surface therefore gets flattened
before the next pass can sample it - and under `bd_stereo_multiview` *every*
render target is layered, so the flatten happens repeatedly down the post chain.

## It is the same bug as the black frame

The multiview frame presents 0.0% non-black, and the decisive measurement in
`20260831_0700` was that **nothing can sample the two-layer scene target** -
not through the companion, not through the array's own view, not through the
per-eye slice views. Copies off that image work; samples return black.

That is not a coincidence sitting next to the resolve chain. It *is* the resolve
chain's premise: a `Texture2D` heap cannot sample a two-layer image, the resolve
exists to work around that, and the resolve is itself a sample of that image.
The workaround is blocked by the thing it works around.

So there are not two problems here. There is one:

> **The post chain cannot read a layered target, and everything built to hide
> that fact is both broken and expensive.**

## What follows, and it is what the port plan always said

The proper implementation is the one this repo has been describing as "the
right way" and deferring as too invasive: **a second bindless heap declared
`Texture2DArray`, sampled by `ViewIndex`**, so the post chain reads the array
directly and no flatten happens at all.

Doing that removes, in one change:

- the 5 full-resolution resolve passes - **79.5 MB/frame, 26% of tile traffic**
- the black frame, because nothing needs to sample a 2-layer image through a
  2D view any more
- `resolvedHolder`, `resolvedFramebuffer`, `layerView[2]`,
  `layerDescriptorIndex[2]`, `resolvedDescriptorIndex`, `multiviewDirty`,
  `multiviewResolvedFrame`, `bd_mv_redirect_srv`, `bd_mv_resolve`,
  `bd_mv_debug_clear`, `bd_mv_debug_known_srv` and the whole of
  `multiview_resolve.cpp` - every one of which exists to manage the flatten

It reaches into XenosRecomp's texture declarations, the descriptor set layout
and every `tfetch`, and on Adreno it has to fit inside a `maxBoundDescriptorSets`
of 4 that is already full - which is the reason it keeps being deferred. But the
cost of *not* doing it is now measured rather than assumed: a quarter of the
frame's bandwidth, plus a VR path that does not render.

**This also retires the "multiview measured slower than side-by-side" result**
without needing the forbidden comparison to explain it. Multiview as implemented
here adds five full-resolution passes; the technique does not. The number was
measuring the workaround, not the technique.

## Order

1. The `Texture2DArray` bindless heap. It is the fix for the black frame *and*
   the largest single bandwidth item after the scene itself.
2. Then merge what remains of the 11 other full-res post passes.
3. `LOAD_OP_DONT_CARE` is already in (plume, 2026-08-31) and removes the load
   half of every pass that fully overwrites its target.
