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

## Measured: the prepass is null on the Quest

Built, verified on the desktop (91.5% non-black, mean RGB 67/61/47, stereo
verdict OK, `depth prepass: 134 of 134 draws` on the scene flush), then a
within-run A/B on the Quest 2 with the side-by-side path now on the queue:

| | prepass off | prepass on |
| --- | --- | --- |
| `gpu_draw_ms` | 32.50 | 32.61 |
| `gpu_total_ms` | 37.73 | 38.94 |
| `us/draw` (CPU) | 95.6 | 96.7 |

Nothing. Laying the nearest depth down first and shading only what passes
LEQUAL buys zero milliseconds, which means **the scene pass is not paying
for opaque overdraw**. Either there is little of it behind the nearest
depth-writer (the ~2x came from the desktop's depth-ALWAYS test and was never
run on the headset), or the cost is in the fragments that survive: the
blended stacks that shade whatever the depth says, the pixel shaders
themselves, or the 16-bit-float target's tile traffic. `bd_depth_prepass`
stays off; it is correct and free of image change, and it is not a lever.

A side result worth keeping: the side-by-side path now defers, so the queue,
its sort and anything else built on it apply to the shipping stereo route.
Every flush reports `0 opaque` and `depth 0..0` - the blended classification
says every scene draw blends, and the view-distance key is never set on this
path - so the sort has nothing to work with there either.

## Depth ALWAYS on the Quest: nothing. There is no overdraw to reject.

`bd_ab_flag=bd_debug_depth_always`, within one run: `gpu_draw_ms 35.76 ->
35.70`. Forcing every fragment to pass the depth test costs nothing, so the
scene pass was not rejecting anything to begin with - not because rejection is
broken, but because there is nothing hidden to reject. The desktop's "depth
ALWAYS doubles GPU time" was measured with reflections and shadows on and a
different cull; on the headset those are off and the visible set is the whole
cost. The prepass result above is the same fact from the other side.

So the scene pass is ~7 ms per eye of *visible* fragment work at ~1 Mpix, and
the levers are the pixel shaders, the 16-bit-float target's tile traffic, or
the vertex/binning side - which is what the on-device GPU profiler splits.

## The on-device GPU profiler: a texture-bound scene sampling mip 0

`tools/gpu_metrics_quest.sh` samples `ovrgpuprofiler -r` during the field
scene. Steady values, 20 fps, `bd_stereo`:

| metric | value |
| --- | --- |
| % time shading fragments / vertices | **99.0 / 1.0** |
| fragments shaded / s | 929 M (= ~46 M a frame, ~6.6 per scene pixel per eye) |
| % shader ALU capacity utilised | 22 |
| % shaders stalled | 21 |
| **% texture pipes busy** | **66** |
| % texture L1 miss | 25.6 |
| textures / fragment | 2.3 |
| ALU / fragment | 53 |
| **% non-base-level texture fetches** | **0.96** |
| % nearest filtered | 51 |
| read / write bandwidth | 2.8 / 1.9 GB/s (not the limit) |
| preemptions / s, avg delay | 100, ~1.7 ms |

Vertices are 1%. The ALUs are a fifth used. The texture units are the busiest
block in the GPU and a quarter of their fetches miss L1 - because **99% of
fetches come from the base mip level**. A 3D scene sampled without mipmaps is
the textbook texture-cache thrash, and it matches the fragment cost being
"proportional to fragment count" without any of it being depth-rejectable:
each of the ~6.6 fragments per pixel pays full-rate base-level fetches.

Two other things to keep: ~100 compositor preemptions a second at ~1.7 ms
each is up to a sixth of the GPU's time, and the shaders are stalled 21% of
the time, consistent with waiting on texture.

`native_texture_mirror.cpp` builds a mip chain only when the fetch constant
asks for one (`mip_max_level >= 1`) and the guest supplies mip data; a
histogram of that decision now prints as `[mips]`. If the guest's textures
have no chains, the modern fix is to generate them on upload - plume has no
blit, so that is a small downsample pass per level.

### The mip histogram, desktop

```
[mips] 512 2D mirrors: 316 with a mip chain, 196 with mip_max_level=0,
       0 mip_filter=baseMap, 512 mip_filter=point
```

Most textures carry a chain and every fetch constant asks for point mip
selection, which still selects a level; all 987 fetches in the dumped shaders
are implicit-LOD `Sample`. So the 99% base-level figure is not a missing
chain or a clamped sampler. The remaining readings are that the scene's
textures are simply magnified at this resolution (terrain tiles at texel:pixel
of one or less legitimately sit at mip 0), or that the profiler's metric means
something narrower. Either way the fragment count is the lever that does not
depend on the answer: fixed foveated rendering is being A/B'd next.
