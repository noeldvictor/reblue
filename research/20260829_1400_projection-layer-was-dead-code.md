# Research: the projection layer was dead code, and that is why VR was flat

Date: 2026-08-29 14:00
Topic: two bugs between a renderer drawing two eyes and a compositor showing one.

The renderer has been drawing correct stereo since `bd_stereo` landed - verified by screenshot,
parallax and all. The headset was still showing a flat image. The reason was not in the renderer.

## Bug 1: the projection layer never submitted

`Session::EndFrame`:

```cpp
const bool hadProjection = g_projectionQueued;
g_projectionQueued = false;
(void)hadProjection;              // captured, then explicitly discarded
...
if (g_projectionQueued && swapchain_) {   // always false: cleared two lines up
```

The flag is latched into `hadProjection`, cleared, the latch is thrown away with a `(void)` cast,
and then the **cleared** flag is tested. The projection branch could never run. Every VR frame fell
through to the quad layer - a flat rectangle pinned in space - regardless of what the renderer had
put in it.

This is the "**projection layer built, never seen render**" line that has been in CLAUDE.md for
weeks. It was not a rendering problem, a NaN, or a matrix. It was a discarded boolean.

## Bug 2: both eyes got the whole image

With the branch live, both `XrCompositionLayerProjectionView`s used the full image rect and
`imageArrayIndex = 0` - the same picture handed to both eyes. `bd_stereo` draws the two eyes side by
side into one image, so each view has to take its half:

```
eye0 rect 1832x1920 + 0        eye1 rect 1832x1920 + 1832
```

## Why this took so long to find

Every check that was run said the right thing. The renderer drew two eyes - screenshotted. The draw
count doubled. The per-eye constants were correct - dumped and read. The frame rate moved when the
levers moved. None of it touched the four lines that decide what the compositor is actually handed.

**The lesson is about where to look, not how hard.** The pipeline was: guest -> renderer -> layer
submission -> compositor. Every measurement lived in the first two stages, and both bugs were in the
third. When output is wrong and the stage you are staring at is provably correct, the fault is
downstream of it, and no amount of further work upstream will find it.

`[xr] projection views: ...` now logs what is handed to each eye, once per run. That line is the
cheapest possible check that stereo is real and it costs nothing to keep.

## State

Both fixed and verified on device from the log. **Not yet confirmed through the lenses** - nobody
has worn the headset - but the compositor is now receiving two distinct images in a world-locked
projection layer, which is the thing that was missing.
