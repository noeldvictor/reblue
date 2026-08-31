# The second eye costs 20.5ms, and the post chain is not the lever

2026-08-31. Quest 2, multiview, field scene.

## The measurement

`bd_mv_force_mono_targets` gives every render target a single layer. The image is wrong by
construction - one eye's worth of everything - and the number is the ceiling on what removing the
second layer could ever save.

| | `gpu_total_ms` | `dt_ms` | fps |
| --- | --- | --- | --- |
| baseline (two-layer, correct stereo) | 56.18 | 66.88 (4 slots) | 14.9 |
| every target mono (wrong image) | **35.68** | 49.74 (3 slots) | **27.7** |

**20.5ms**, and it crosses two pacing tiers.

## What it does NOT mean

It is tempting to read that as 20ms of waste in the post chain. It is not. The probe makes the
*scene* single-layer too, and the scene pass is ~45ms of the 56ms - so most of the 20.5ms is simply
not rendering the second eye.

That is the stereo the port exists to provide. It is not available.

What the number actually quantifies is **the cost of the second eye under multiview: 20.5ms, or 36%
of GPU time.** Useful to know, and about what a second view should cost when multiview shares
vertex work but not fragments.

## What the post chain is worth on its own

From the per-target GPU census: the whole post chain is under 8ms, and the five full-resolution
single-draw post passes are ~3ms of it. Making only those mono - the careful version, resolving the
pair before the post chain reads it - is worth **about 3ms**, against the 6.2ms needed to reach the
next pacing tier.

So the careful version does not, on its own, buy a tier. `bd_mv_small_targets_mono` measuring as
neutral earlier is consistent with this: it was capped at the small targets, which are ~2ms
together.

## Where that leaves the budget

At 60Hz the port needs `gpu_total` under 50.0ms for 20fps and under 33.3ms for 30fps. It is at
56.18ms.

- Removing the second eye entirely: 35.68ms. **Not available** - it is the product.
- Mono post chain, done correctly: ~3ms. Short of the 6.2ms tier boundary by itself.
- Foveation by fragment density map: **negative 17ms** on this hardware, measured.
- Draw batching and sorting: zero, measured - the guest is already pipeline-coherent.

Nothing measured so far gets a tier on its own, and two of the four candidates are worse than
nothing. The 3ms from the post chain plus anything else worth 3ms would do it, which makes the
combination worth having even though neither half is.

## Note on the probe

`bd_mv_force_mono_targets` renders incorrectly on purpose and exists only to bound a decision before
paying for it. It should never be enabled outside a measurement. Keeping it is cheap and it answered
in one run a question that would otherwise have been answered by building the careful version first
and discovering it bought 3ms.
