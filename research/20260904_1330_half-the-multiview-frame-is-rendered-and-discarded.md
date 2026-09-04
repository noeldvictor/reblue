# Half the multiview frame is rendered and then discarded at present

2026-09-04, 13:30. Desktop under `tools/xrsim`, measured with a new one-shot
`[present] source ... -> back ..., rect ...` line rather than derived.

## The number

```
[present] source 960x1080 layers 2 -> back 960x1080, rect 960x536+0,272
          (aspect 1.778); 2.01 source pixels a destination pixel
```

The scene is shaded at 960x1080 per eye and lands in a 960x536 rect of the eye's layer.
**Two rendered pixels for every delivered one.** On a pass that is bound by fragments times
texture fetches (2026-09-03), that is the largest single lever found so far, and nothing in
the frame's own counters showed it - the pass, barrier and resolve counts are all healthy.

## Where it comes from

`Output::LatchedFit` fits the window to the game's 16:9 and then, under
`bd_stereo_multiview` + `bd_mv_half_width`, halves **only the width**:

```
fit 1920x1080 -> latched 960x1080, g_latched_full_w stays 1920
RenderAspect = g_latched_full_w / latched_h = 1920 / 1080 = 1.778
```

So the guest draws content for 1.778 into a 0.889-shaped target: an anamorphic squeeze,
exactly the one side-by-side's half-width viewports carry. The present's `ComputeFit` undoes
it by fitting 1.778 into the 960x1080 layer, which lands on 960x536 - and the vertical
factor of two is thrown away by the sampler.

The squeeze earns its keep on the side-by-side path, where two half-width eyes pack into one
full-width panel and the compositor un-squeezes each half by mapping it to a whole eye. On
the layered path there is no panel: each array layer *is* an eye, so the squeeze buys nothing
and costs half the shading.

The same arithmetic on a Quest: 1376x720 fitted, halved to 688x720, presented into 688x360.
The structural factor of two is identical.

## What it is not

It is not a bug in the present pass, and removing the fit there does not fix it - that was
tried this morning and stretched every frame 2:1, because the fit is what un-squeezes the
source. The waste is upstream, in the size the guest is told to render at.

Side-by-side measures 1.00 source pixels a destination pixel at the present pass, so this is
specific to multiview. (The desktop's side-by-side route then downscales again in the copy
to a `bd_xr_present_scale` swapchain, 1920x1080 into 768x432, but that is a desktop-only
artefact of that cvar and not what the Quest does.)

## The options, for the owner

Each keeps the delivered image at least as good as today's; the difference is what happens to
the fragment budget.

1. **Render the content size** (960x540 instead of 960x1080, no squeeze). Half the scene
   fragments, delivered image unchanged in size. The cost is losing the accidental 2x
   vertical supersample the downsample currently performs - edges get slightly harder. This
   is the direct fragment win.
2. **Keep the fragment budget and spend it on the image** (content 1358x764, no squeeze, the
   layer sized so the content maps 1:1 into its letterboxed rect). Same shading cost as
   today, roughly twice the delivered pixels. Strictly better image at no GPU cost, and the
   only one of the three that improves sharpness.
3. **Fill the eye** (content at the eye's own aspect, no bars). Needs the guest's 2D layout
   carried with it, since the HUD is authored for 16:9, and needs the projection view's FOV
   to match whatever is rendered. The most work and the real VR answer.

Option 2 is the recommendation if the frame budget is the constraint, since it costs nothing
and improves the image. Option 1 is the one to take if the Quest frame needs the milliseconds
more than it needs the edges. Both are a change to what the player sees, so neither is being
shipped without a decision.

## Option 1, built and measured (14:10)

`bd_mv_no_squeeze`, default off: under `bd_stereo_multiview` + `bd_mv_half_width` the
height is halved with the width and `g_latched_full_w` follows the halved width, so the
target's aspect equals `RenderAspect` and the present maps 1:1.

| | squeeze (today) | no squeeze |
| --- | --- | --- |
| present | source 960x1080 -> rect 960x536, **2.01** source pixels a destination pixel | source 960x536 -> rect 960x536, **1.00** |
| scene view fragments a frame | 5.65 M | 2.99 M |
| scene view pixels (both layers) | 2.074 M | 1.029 M |
| overdraw | 2.72 | 2.90 |

The two runs are different scenes - autoplay does not land twice in the same place, and the
second had 848 draws against 698 - so the fragment totals are not a clean A/B. What survives
that: the fragments scale with the target's area while the overdraw stays in the same place,
which is what "the discarded half is never shaded" predicts. The frame totals (12.8 M and
12.4 M) do **not** halve, because the shadow and reflection views keep their own sizes and
the denser second scene raised them.

The delivered content is 960x536 either way and the proportions match - the two captures put
side by side show the same derrick with the same shape, no squeeze or stretch. What is lost
is the accidental 2x vertical supersample the old downsample performed; thin rigging lines
are marginally harder. That is the whole of the trade.

## Option 3's sizing, built and measured (14:40)

`bd_xr_eye_sized` with `bd_xr_render_scale`, default off. The frame's shape comes from the
runtime instead of the desktop window: the render target is the game's aspect fitted into
the runtime's per-eye rect times the scale, the layered swapchain's layer *is* that rect, and
the present drops the content into its letterboxed place 1:1. Verified against a
Quest-shaped simulated eye (`XRSIM_WIDTH=1440 XRSIM_HEIGHT_PX=1584`):

```
[output] eye-sized frame: runtime 1440x1584 an eye x0.50 -> content 720x400 at aspect 1.800
[xr] layered swapchain at the runtime's per-eye rect: 720x792 x2
[present] source 720x400 layers 2 -> back 720x792, rect 720x400+0,196; 1.00 source pixels
```

Both layers hold the 16:9 content letterboxed in an eye-shaped layer, differing in 6.52% of
pixels with disparity on the near geometry - the same figure the working path reads. It is
still letterboxed, because the content keeps the game's aspect for the HUD's sake; what has
gone is the wasted half and the window's say in the matter. The scale is now the frame's
budget dial and it means something: a fraction of the headset's own rect.

Two bugs it exposed, both real and both caught before defaulting it on:

- **Refusing to answer until the session exists kills the app.** `LatchedFit` is needed
  before the device, the window and the guest's first surfaces exist, all of which precede
  the XR session. Returning false until the runtime reported left no render target and the
  process died before writing a log. It re-latches once instead, when the per-eye rect first
  arrives.
- **`ConfiguredAspect()` returns 0 in Auto mode**, meaning "follow the target". On a 16:9
  desktop window that is the game's aspect by luck; against a square simulated eye it made
  512x512 content, which would put the HUD through a mangle. The eye-sized path pins the
  aspect to `kDesignCanvasAspect` when Auto is set.
- **The scene-pass test measures against the design canvas.** `stereoEligible` asks whether
  the render target is at least the design canvas times `bd_render_scale`, doubling the
  width to undo the half-width squeeze. A 720x400 eye frame fails the height test, so the
  per-eye skew switched off and the two layers came out byte-identical: stereo silently
  gone, with nothing in the log to say so. Under `bd_xr_eye_sized` the yardstick is the
  latched frame instead. The same heuristic is presumably fragile under a small
  `bd_render_scale` on the other paths; not changed there, not investigated.

## The instrument

`[present] source WxH layers L -> back WxH, rect WxH+X,Y (aspect A); N source pixels a
destination pixel`, one shot on the first present and once more on the first layered one.
Two numbers that were not otherwise visible: what the guest rendered, and what survived.

Sources: `src/gpu/present.cpp` (the log, `RecordPresentPass`), `src/gpu/output.cpp`
(`LatchedFit`, the half-width branch and `g_latched_full_w`).
