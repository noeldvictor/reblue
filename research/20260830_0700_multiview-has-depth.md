# Multiview stereo has depth, and three instruments were lying about it

2026-08-30. The multiview path went from "renders two identical layers" to correct crossed
disparity in one session. Nothing about the technique was wrong. Three separate measuring
instruments were, and the bug they were hiding was two lines.

## What was actually wrong

**1. `bd_stereo` and `bd_stereo_multiview` were both on, and nothing stopped that.**

They are two implementations of one thing - the cvar help has said so all along ("Cheaper than
bd_stereo, which submits every draw twice") - but `gpu/hooks/draw.cpp` gated the side-by-side eye
loop on `bd_stereo` alone. With both set the frame is:

- every scene draw submitted twice, into half-width viewports, and
- each of those replicated into both array layers by multiview, and
- the resolve then squeezing a layer that *already holds a complete side-by-side pair* into one
  half of the companion.

So both layers carry the same two eyes and differ only by the shader skew. That is exactly the
"multiview renders identical layers" symptom, and it also rasterises every triangle four times -
which is why multiview once measured **slower** than the path it replaces. That number is void.

The desktop profile had both set, so **every multiview measurement in this project went through
this**.

**2. The per-eye sign was inverted.** `eyeSign = (iViewID == 0) ? -1.0f : 1.0f` gives view 0 - the
left eye - the negative constant. It must take the positive one. Backwards renders the scene
pseudoscopic, and that is invisible in any symmetric test.

## The instruments that were lying

**`bd_mv_capture_array` decoded the wrong format.** The readback buffer was sized at four bytes a
texel while `copyTextureRegion` was handed the surface's real format. A scene target is
`R16G16B16A16_FLOAT` - eight - so the capture overran its buffer, stacked the second slice half a
slice early, tagged the file `RGBA` and was decoded as RGBA8.

What comes out is plausible-looking noise with a doubled horizontal period. It reads as a rendering
bug, not a decode error. The standing figure **"the array holds two genuinely different views,
per-pixel difference mean 3.694, 23.2% of pixels"** was measuring misaligned halves of
misinterpreted bytes.

**A sampled log that always sampled the same slot.** `[mv] SetRenderTarget #N` printed every 4000th
call and reported `surface=null layers=0 -> mv=false` every time. `SetRenderTarget` is called per
slot and 4000 is a multiple of the slot count, so the sample never moved off one index. A later
sample that happened to land elsewhere read `surface=yes 1920x1080 layers=2 -> mv=true`.

**A bounded log read as a count.** `[mv] resolved ... ({} times)` prints at the 1st and 501st
resolve. Two lines therefore means *at least 501 resolves*, not two. It was briefly read as "the
resolve runs twice at startup and never again".

That is the fourth, fifth and sixth wrong multiview conclusion drawn from an instrument rather than
from the thing. The rule already in CLAUDE.md - *a bounded log answers "what happened first", never
"what happens"* - now has a companion: **a periodic sample whose period shares a factor with the
thing being sampled is not a sample.**

## What found it

RenderDoc, which was already installed and registered as a Vulkan layer. Khronos publishes no
Windows validation binaries, so on the desktop this is the instrument that exists.

`bd_renderdoc` loads it before the `VkInstance` (it hooks at load time) and `bd_renderdoc_after_s`
triggers a capture once autoplay is in a field scene. Triggering from inside the app is the point:
`renderdoccmd capture` waits on a keypress, which a headless run cannot supply.

`tools/rdc_outline.py` then turns `renderdoccmd convert -c zip.xml` into one line per render pass.
The frame read:

```
46 framebuffers created; 90 render passes, 49 with a view mask
  [  5] fb=15038  1920x1080 mask=3 STEREO pipes=8 draws=314 vp=960x1080@0,960x1080@960
  [ 10] fb=1582   1920x1080 mask=0 mono   pipes=0 draws=  2 vp=960x1080@0,960x1080@960
```

Pass 5 is the scene: a multiview pass being fed side-by-side viewports. Pass 10 is the resolve,
doing exactly what it should. One line each, no inference.

**Read the view mask, not the layer count.** `VkFramebufferCreateInfo::layers` must be 1 when the
render pass has a view mask, so a layered target is otherwise indistinguishable from a mono one -
"0 framebuffers are layered" looks like a smoking gun and means nothing. The mask lives in a
`VkRenderPassMultiviewCreateInfo` chained on `pNext`.

## Where it ended up

Decoded properly, a `bd_mv_capture_array` grab is a recognisable field scene in two layers whose
difference image lights up exactly the near silhouettes - posts, plants, a barrel - and leaves the
distant rock face and sky black. That is what parallax looks like.

`tools/stereo_check.py --stacked` gives the multiview path the same per-band verdict the
side-by-side path already had:

| `bd_stereo_separation` | far | near | verdict |
| --- | --- | --- | --- |
| 0.03 (default) | +0 | +0 | FLAT - sub-pixel |
| 0.2, old sign | +2 | +8 | INVERTED, monotone with depth |
| 0.2, corrected | **-2** | **-8** | **OK: crossed, near separating more than far** |

## The scale factor, measured

The "7x" above was a first estimate against a figure from another scene on another day. Measured
properly - both paths, one field scene, one build, which is the only comparison that means anything
given the +/-30% cross-run spread:

| path | separation | far | near | spread |
| --- | --- | --- | --- | --- |
| side-by-side | 0.03 | +4 | -7 | 11px of a 960-wide eye |
| multiview | 0.2 | -2 | -8 | 6px of a 1920-wide layer |
| multiview | 0.7 | -4 | -26 | 22px of a 1920-wide layer |

Linear in between - 3.5x the input gives 3.67x the output - and 22px over 1920 is the same angle as
11px over 960. **So multiview 0.7 matches side-by-side 0.03: a ratio of 23.3.**

`bd_stereo_separation` therefore meant two different things depending on a second cvar, which is
exactly the trap `bd_stereo`/`bd_stereo_multiview` had just sprung. It is now converted at the
multiview seam, so one knob means one thing and **the stock default produces correct stereo on the
multiview path**: far -4, near -26, crossed.

**Why it is 23 and not 1 is still not understood**, and is the one thing left open here. Both paths
add a constant to `clip.x` - the host at `m[0] += eye_skew`, the shader at
`oPos.x += eyeSign * g_StereoSeparation` - and a multiview layer is twice the width of a
side-by-side eye, so multiview should need *half*, not twenty-three times. The likeliest suspect is
the host's constant landing on a coefficient of a position component that is not `w`, which would
scale it by a typical guest coordinate - and Blue Dragon units are centimetres, which is the right
order of magnitude. Not chased, because it would change the path that already works.

**None of this is measured on ARM64.** Every number here is desktop. The Quest 2 has not been
attached this session.
