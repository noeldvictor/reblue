# Stereo has depth

2026-08-29. Fourth note today. The first three each blamed the wrong thing; this one has a working
result and the reason the earlier answers kept missing.

## The bug: `clip.x += separation * clip.z` is not parallax

Both stereo paths - the host matrix patch in `UploadVertexShaderConstants` and the shader skew
emitted by XenosRecomp - applied the eye offset as a term proportional to `clip.z`:

```
clip.x' = clip.x + sep * clip.z - conv * clip.w
```

For any normal projection `clip.z` and `clip.w` agree to within a fraction of a percent beyond a few
metres. So after the perspective divide that term is `sep * (z/w) ≈ sep`, **a constant sideways
slide of the entire image**. It looks like stereo in a still frame and carries no depth whatsoever.

That is precisely the flat measurement that survived three sessions:

```
sky +59px    near ground +57px      2px across a scene hundreds of metres deep
```

A lateral eye translation is a **constant** added to `clip.x`. Dividing by `w` then makes the
screen-space shift inversely proportional to depth - near geometry separates strongly, distant
geometry barely moves. That is parallax.

```
clip.x' = clip.x + sep - conv * clip.w
```

Host side, the constant lives in whichever component of register 32 multiplies the position's `w`,
and the shader's own swizzle names it:

```hlsl
r3.x = dot(r5.xyzw, g_mViewProj(0).wzyx);   // r5.w pairs with .x
```

so it is `m[0] += eye_skew`, a single float, not a whole-register add.

## Two more things had to be right at the same time

**The shader and host were both applying it.** `stereo_on` was `bd_stereo || bd_stereo_multiview`,
so the shared constants were populated for the side-by-side path too - where `SV_ViewID` is always 0
and `eyeSign` is therefore `-1` for *both* eyes. Each eye got an extra `-sep` on top of the host's
per-eye patch, leaving eye 0 at `-2*sep` and eye 1 at `0`: still a pair, but asymmetric about the
mono image and with convergence applied twice. The shared constants are now multiview-only; the
side-by-side path does its per-eye work entirely in `UploadVertexShaderConstants`.

**The sign was inverted.** The left eye's camera sits to the left, so the world appears shifted
*right* in its image - the left eye takes the positive constant. It had them the other way round,
which renders the scene pseudoscopic: near geometry reading as far, the world inside out. It fuses
badly and it is invisible in any symmetric test, which is the third time that particular trap has
been hit on this port.

The check that catches it, from a capture and nothing else: with the convergence plane at infinity
every point must have **crossed** disparity - further left in the right eye - by more the nearer it
is.

## Measured, on device, in a field scene

`bd_stereo=true`, separation 0.02, convergence 0, `render_scale=25`:

| band (y%) | 32 | 44 | 52 | 62 | 72 | 82 | 90 | 95 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| disparity (px) | +21 | +18 | +15 | +26 | +8 | -3 | +8 | +5 |

**far +21, near +5, near - far = -16px.** Crossed, correctly signed, monotone with depth apart from
the band at 62% which straddles the structure at mid distance. Before the fix the same measurement
was flat to 2px.

The constant offset - far sitting at +21 rather than 0 - is the two half-width viewports not being
pixel-aligned with each other, and is uniform, so it is a convergence offset rather than a depth
error. Worth trimming later; it does not affect the depth cue.

## Why the side-by-side path and not multiview

Multiview renders the scene into two layers correctly - validation is clean and both layers are
written - but **the post-process chain is mono** (`rtLayers=1 viewMask=0`), so it collapses the pair
before present. Fixing that is a bigger job. The side-by-side path submits every scene draw twice
into half-width viewports, and the post passes - full-screen quads with `vertexOrIndexCount <= 6` -
run once over the whole target, covering both halves at once. So the whole chain survives, and it is
already what `xr_session` expects: `bd_stereo` makes it split the swapchain image into per-eye
`imageRect`s.

Multiview remains the better architecture and is worth returning to when the post chain can be made
view-aware. It is strictly cheaper: one draw instead of two.

## Cost

Every scene draw is submitted twice, and the CPU was already the constraint. The per-eye path was
counted at 2001 draws before the first census tick, against ~540 draws/frame mono. Frame timings
under stereo need re-measuring against the 60/2 tier; if it does not fit, the levers are
`bd_cull_distance` and `bd_render_scale`, both of which are cheap and already characterised.

## Method

Four wrong conclusions in one day - "94% of pipelines are mono", "view 1 never rasterises", "the
post chain is the only blocker", and implicitly "the skew maths is fine because it is unit-tested
elsewhere". Each was corrected by one measurement, and the last one only fell to a *number read off
a captured frame*. The frame capture built this morning is what made every one of these checkable;
before it, all of this was unfalsifiable.
