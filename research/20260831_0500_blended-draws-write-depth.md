# 64% of the frame blends AND writes depth, which is how a tiler loses early-Z

2026-08-31, desktop measurement, Quest confirmation still owed.

## The count

Instrumented in `FlushRenderState`, printed on field frames only:

```
[lrz] 337 of 530 draws REALLY blend and write depth; 0 more are opaque but the
      sort treats them as blended
```

**337 of 530 draws - 64% of a field frame - have `alphaBlendEnable` true and
write depth.** Counted, not inferred, and stable across frames (337 at 530 draws
and again at 581).

The second half of that line matters as much as the first. D3D9 ignores the
blend factors entirely when `alphaBlendEnable` is false, so a draw carrying
leftover `SRC_ALPHA` state is opaque. The draw queue classifies blended draws by
the factors alone (`!(ONE && ZERO)`) and pins them in submission order. If that
heuristic disagreed with the real flag, two thirds of the frame would be
unsortable for a bookkeeping reason and the zero gain from sorting would have a
trivial explanation. **It does not disagree: 0 misclassified.** That hypothesis
is dead, and the sort is doing what it says.

## Why this is the LRZ suspect

Qualcomm's and Mesa's documentation both say writing depth with blending enabled
forces low-resolution Z to be invalidated, and that the invalidation persists
**for the remainder of the pass**. With 337 such draws scattered through the
scene pass, an early one costs every later draw its early rejection.

That is exactly the shape of the three measurements already on the board, which
otherwise sit oddly together:

- the frame is fragment-bound (a quarter of the fragments halves `gpu_total`)
- the scene carries ~2x overdraw (forcing depth ALWAYS doubles desktop GPU time)
- front-to-back sorting buys **exactly zero**

Overdraw that ordering cannot exploit is a tiler that never rejects.

And it is a pure EDRAM-era habit. On a Xenon there was no LRZ to lose, so
depth-writing on transparent geometry cost nothing. Conventional renderers test
depth on blended geometry and do not write it - a transparent surface does not
occlude what is drawn after it.

## The lever

`bd_blend_no_depth_write` (default **off**) clears `zWriteEnable` when
`alphaBlendEnable` is set, in `FlushRenderState` before the pipeline is hashed,
so it produces genuinely different pipelines.

Verified to apply, by counter rather than by assumption:

```
[lrz] 0 of 530 draws REALLY blend and write depth
[lrz] depth-write suppressed on 351 draws this frame
```

337 -> 0.

## What the desktop can and cannot say about it

**Desktop `gpu_total_ms` does not move: 4.718 -> 4.718, +0.0%, within-run A/B,
4775 frames against 4798.**

That is not a negative result, and reporting it as one would be wrong. An
RTX 3060 is an immediate-mode renderer whose hierarchical Z is not invalidated
by blend-with-depth-write the way a tiler's LRZ is, and its `gpu_draw_ms` for
this scene is 0.364ms - there is no rejection headroom to win back. The desktop
is structurally unable to measure this lever.

What the desktop did establish is that the change is free there: no CPU
regression beyond noise (+3.3% on `us/draw`, one arm), and no new validation or
pipeline errors.

## The naive lever breaks the image. Do not ship it.

Captured on the desktop the moment the desktop could render again (see below),
`bd_blend_no_depth_write=true` against the same field scene:

| | mean RGB | what it looks like |
| --- | --- | --- |
| off | 62 / 57 / 45 | correct - cliffs, foliage, shadows, crisp |
| on  | 98 / 103 / 94 | **broken** |

Broken specifically: the rock cliffs on the right are replaced by a flat grey
wall, a large pale plane cuts across the middle of the scene, and the whole
frame is heavily blurred.

That is the signature of **the post chain reading the depth buffer**. Blue
Dragon's depth-of-field and distance fog sample depth, so a transparent surface
that stops writing depth leaves the post pass reading whatever is *behind* it -
and it blurs and fogs the scene as though the near geometry were not there.

So the 337 draws are not simply careless. Some fraction of them are genuinely
load-bearing for a depth-consuming post pass, and a blanket suppression trades
a frame rate win for an unusable image - the one trade this project has ruled
out from the start.

**The finding stands; the lever does not.** What is needed is a discriminator
narrower than `alphaBlendEnable`, separating true translucency (which should not
write depth) from alpha-blended geometry the depth buffer is expected to
contain. `bd_blend_no_depth_write` stays off by default and is now a probe
rather than a candidate default.

## Both naive levers are closed, by capture

Every one of the 337 uses **`src=SRC_ALPHA dst=INV_SRC_ALPHA`** - classic alpha
blending, and **not one is additive**. So there is no blend-mode discriminator
to separate glows (which never belong in depth) from cutouts (which do); they
are all the same mode.

Two state changes were tried against them and both destroy the frame:

| probe | mean RGB | result |
| --- | --- | --- |
| correct reference | 62 / 57 / 45 | the field scene |
| `bd_blend_no_depth_write` - stop writing depth | 98 / 103 / 94 | cliffs become flat grey, a pale plane crosses the scene, everything blurs: **the post chain samples depth** |
| `bd_blend_off_when_opaque` - stop blending | 1 / 1 / 1 | **0.4% non-black. The frame is gone.** |

The second was the more promising theory and it is now dead: 64% of a frame is
far too much to be real transparency, so the suspicion was an X360 habit of
leaving blending enabled where alpha is always 1.0 and the blend is a no-op. It
is not a no-op. Those draws need the blend *and* the depth write.

**So the LRZ cost is real and is not removable by a state change.** What is left
is ordering - getting the opaque set to populate low-resolution Z before the
blended majority - which is what the shipped sort already attempts and which
measured zero. Whether that is because the guest already submits opaque-first is
the next thing to instrument, and it is cheap: record the sequence positions of
the 166 opaque draws.

## What is owed

One Quest run, and the flag is already built and committed:

```sh
bash tools/verify_quest.sh "bd_stereo=false,bd_stereo_multiview=true,bd_blend_no_depth_write=true"
```

Baseline is `gpu_total 56.18ms`. The tier boundary is 50.0ms, which is 6.2ms
away, and the estimate from the desktop's 2x depth-rejection result is ~20ms.

**And the image has to be checked, not just the number.** Suppressing depth
writes on transparent geometry is the conventional behaviour, but Blue Dragon is
2006 code and something may lean on it - a particle sorting against itself, a
water plane, the door blackout carve pass that `draw.cpp` already documents.
This is a correctness change before it is a performance one.

## Method notes

- **The black desktop was this rewrite's own bug, and it was found here.** Six
  host blit shaders still declared their texture heap at `register(t0, space0)`
  - set 0 binding 0 - which the constant rewrite had reassigned to the vertex
  constant UBO. The gamma blit sampled a uniform buffer as a texture and
  produced black. Decoding the compiled SPIR-V settled it in one step after two
  sessions of retracted screenshot claims: **read the binding out of the
  module, do not read the HLSL.**
- **`tools/shot_window.ps1` will photograph the wrong window.** Two shots came
  back 355x159 and 99.6% non-black at mean 237 - a dialog, not the game, and
  near-white rather than black, which reads like a *success* if the size is not
  checked. Check the returned dimensions against the expected client size.
- **The desktop run needs the window in the foreground.** A background launch
  sat at 500 frames and never reached a field scene, logging only `[sleep]`
  lines; the same config foregrounded reached 9573 field frames. A stalled
  autoplay looks exactly like a slow one.
