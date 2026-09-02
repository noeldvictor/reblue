# The scene pass is 28 of 39 GPU ms, and a depth prepass is the modern answer

2026-09-02. Quest 2, `bd_stereo`, from yesterday evening's runs.

## Where the GPU goes, by target

The per-target census of the field frame (`out/device/quest_setmove.log`):

| ms/frame | target | draws | binds |
| --- | --- | --- | --- |
| 7.18 | 1376x720 depth (scene) | 42.6 | 0.50 |
| 7.15 | 1376x720 depth (scene) | 43.7 | 0.50 |
| 7.06 | 1376x720 depth (scene) | 43.7 | 0.50 |
| 7.04 | 1376x720 depth (scene) | 43.2 | 0.50 |
| 0.58 x4 | 1376x720 colour-only | 0.5 | 0.50 |
| 0.38 x2 | 128x72 depth (shadow) | 65.7 | 0.50 |
| < 0.35 | everything else, 28 targets | | |

Four pooled scene surfaces at half a bind each: **the scene pass runs twice a
frame - once per eye - at ~7.1 ms each, 28.4 of 34.3 attributed ms** out of
39 total. The whole post chain is under 6 ms. Everything the last week said
about post-pass bandwidth is true and small; the scene pass is the frame.

Seven milliseconds for one megapixel at ~45 attributed draws is pathological
for an Adreno 650. The known reason is overdraw shaded in full: the scene
carries ~2x (forcing depth ALWAYS doubled desktop GPU time), 64% of its draws
blend and write depth, and that disables the tiler's low-resolution Z for the
rest of the pass - which is why front-to-back sorting measured zero.

## The EDRAM seed copies: +1.35 ms, measured within one run

`bd_ab_flag=bd_seed_targets`: arm off 37.79 ms GPU, arm on 39.14 -
`gpu_resolve_ms` 0.85 -> 2.45. Real, small, and a Xenon habit (seeding a
freshly acquired surface from its predecessor to imitate EDRAM persistence).
Not taken yet: a pass that relies on inherited content renders wrongly without
it, and the seed has to become per-surface rather than blanket.

## A depth prepass on the deferred queue

The modern renderer's answer to overdraw that early-Z cannot reject is to lay
depth down first. The deferred draw queue (`bd_draw_defer`, on by default)
already holds a whole pass's draws with their pipeline state, so:

- at record time, a draw that writes depth with a LESS/LEQUAL test and no
  stencil gets two extra pipelines from the same cache: colour writes off
  (the prepass) and depth writes off + LEQUAL (the colour pass);
- at flush, the prepass draws go first, near to far, then every draw in
  submission order with its colour pipeline.

Blended draws that write depth are included in both passes: in the prepass
they lay depth like anything else, in the colour pass they blend only where
they are the nearest depth-writer - which is what they did before whenever a
nearer opaque draw preceded them, and is the same image for opaque-looking
splat layers at equal depth. `bd_depth_prepass`, off until measured.

Cost: the scene's vertex work twice (~320k vertices, cheap) and one more
pipeline bind per prepassed draw on a render thread that now has 20 ms of
headroom. Expected gain: the overdraw half of the scene pass - up to ~7 ms of
the 28 - which is the 33.3 ms boundary.
