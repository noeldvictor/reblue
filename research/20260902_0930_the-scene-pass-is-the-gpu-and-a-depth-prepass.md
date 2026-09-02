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

## Fragment density map foveation: negative again, in a fragment-bound frame

`bd_ab_flag=bd_foveation, bd_foveation_strength=0.3`, within one run:
`gpu_draw_ms 33.78 -> 34.80 (+3.0%)`, `gpu_total 39.74 -> 41.64`. The device
reports `fragment density map yes` and the arms flipped on schedule. In a
frame that is 99% fragment shading, a density map that shaded the periphery
at 30% would have to show; it shows a cost and no reduction, so the
attachment is not reducing shading in this pass on this driver - the same
verdict as 2026-08-30, now in the configuration where it could not hide.
`XR_FB_foveation` through the runtime's own swapchain remains the route.

## The render-stage trace: NOTHING is tiled. Every pass runs in direct mode.

`tools/gpu_drawtrace_quest.sh` (`ovrgpuprofiler -e com.reblue`, then `-t`
during the field scene) lists every surface the GPU executed in a 0.1 s
window, with its rendering mode and bin count:

```
Surface 7  | 1376x720 | color 64bit, depth 32bit | Mode: 0 (Direct) | 1 1376x720 bins | 24.49 ms | Render 22.879ms ... Preempt
Surface 4  | 1376x720 | color 64bit             | Mode: 0 (Direct) | 1 1376x720 bins |  0.09 ms
Surface 28 | 3664x1920| color 32bit             | Mode: 0 (Direct) | 1 3664x1920 bins|  3.19 ms
... 26 more, all Mode: 0 (Direct), one bin each
Surface 5  | 128x72   |                          | Mode: 2 (SwBinning)
Surface 8  | 1376x720 |                          | Mode: 3 (HwDirect) | 0.25 ms
```

**The Adreno is not tiling.** One bin the size of the surface is system-memory
rendering: every blended fragment of the scene pass does its read-modify-write
against a 64-bit colour buffer in DRAM instead of on-chip tile memory, there
is no low-resolution Z (it lives in the binning pass that never runs), and
the shading is exposed to memory latency - which is the 21% shader stall, the
66% busy texture pipes, and the ~6.6 fragments per pixel each costing full
price. It also explains the day's null results at once: depth ALWAYS, the
prepass, foveation - none of them touch a direct-mode pass's cost structure.

Adreno drops a render pass to direct mode when something in it forbids
binning, and one documented trigger is a timestamp query inside the pass.
`gpu_timing.cpp` writes `vkCmdWriteTimestamp` inside the active render pass
on every category or target change - the per-segment split
(`gpu_draw/resolve/inter`) and the per-target census that this whole
investigation has leaned on. The instrument was shaping the measurement.
`bd_gpu_timing_segments` now gates those marks and defaults off on Android;
the frame begin/end pair stays, so `gpu_total_ms` survives.

## With the in-pass timestamps off: the scene pass is STILL direct

`bd_gpu_timing_segments=false`, same trace: the 1376x720 scene pass reads
`Mode: 0 (Direct), 1 bin, 24.66 ms` exactly as before, while a second
1376x720 colour+depth pass in the same frame now reads `Mode: 1 (HwBinning),
16 384x224 bins, 3.03 ms` with `Binning / LoadColor / Render / StoreColor /
LoadDepthStencil / StoreDepthStencil` stages - the shape the scene pass
needs. So the timestamps were *a* trigger, not *the* trigger, and the driver
decides per pass: something about the scene pass itself forbids binning.

What that pass has that the binned one does not, under side-by-side: every
draw twice (~1000 draws, ~640k vertices), a viewport and scissor change on
every draw (the per-eye loop), and ~300 blended depth-writing draws. Two
traces meant to separate those did not land: the multiview trace captured no
surfaces at all twice (the profiler may not see layered passes), and the
cull-150 trace caught a transition rather than the field scene. A mono trace
(no per-eye loop, half the draws) is running.

## Mono on the Quest: 33.7 ms GPU, so stereo is only +5

`bd_stereo=false`, field-scene mean from the CSV: `gpu_total 33.68 | dt 49.79`
against side-by-side's 38.9. One eye's worth of draws in the pass and no
per-draw viewport changes buys 5 ms, not the 12 a straight halving would.
Whatever keeps the scene pass out of tiled rendering is present in mono too.
The 0.6 s trace windows of that run caught lighter frames (present blits at
3.4 ms each, 1376x720 colour+depth passes at 2.3 ms in `HwDirect` - a binning
stage run and then direct chosen anyway), so a 2.5 s window is being taken
before autoplay starts walking.

## Mono's scene pass is direct too: 20.5 ms

The 2.5 s window landed on field frames: `1376x720 c64 d32 Direct, 1 bin,
20.46 ms` and `20.74 ms` in two windows, with no Binning stage at all. So
the doubled draws and the per-draw viewport churn of side-by-side are not
what forces direct mode; the pass forces it in mono, with ~500 draws. The
trigger is in the pass's content. Two probes bisect it: `bd_debug_max_draws=64`
(does a pass of 64 draws bin?) and `bd_blend_off_when_opaque=true` (does a
pass with no blending on depth writers bin?).

Side result: the deferred queue can emit side-by-side eye-major (all left-eye
draws, then all right), verified image-identical on the desktop with stereo
intact. `bd_draw_eye_major` is on by default; it removes ~1000 viewport and
scissor changes from the scene pass, whatever the tiler makes of them.

## Bisecting the direct-mode trigger, one probe per run

Each row is the scene pass in the 2.5 s render-stage trace of a mono field
scene, plus the field-scene GPU mean where a CSV was taken:

| probe | scene pass | mode |
| --- | --- | --- |
| baseline mono | 20.5 ms | Direct, 1 bin |
| in-pass timestamps off | 20.7 ms | Direct (a second full-size pass bins) |
| `bd_blend_off_when_opaque=true` (no blend on depth writers) | 20.3-21.1 ms | Direct |
| `bd_seed_targets=false` (no pre-pass copy) | 20.3-21.6 ms | Direct |
| `bd_debug_max_draws=64` | removed the scene pass itself | - |
| side-by-side (draws x2, viewport per draw) | 24.7 ms | Direct |

RenderDoc on the desktop says the scene passes contain only draws - no
in-pass clears or copies - and the binned full-size pass in the same frame
has the same attachment formats. What the scene draws have that the binned
pass's draws may not is their shaders: every guest vertex shader declares
`SV_ViewID` and reads it (`g_ViewIndex = iViewID`) whether or not the pass is
multiview. A probe shader cache without it in vertex shaders
(`XENOS_RECOMP_NO_VS_VIEWID=1` at cache generation) is being traced.

| no `SV_ViewID` in vertex shaders (probe cache) | 20.5-21.6 ms | Direct |

The vertex-stage ViewIndex is not it either. In the same frame a *small*
1376x720 colour+depth pass bins into 16 tiles with full load/store stages at
1.9 ms, so the surface type is fine and the driver's decision is about what
the big pass contains or how much of it there is. Next split: the draw count
and primitive count of the pass (cull distance 120), then the guest draws'
state - bindless descriptor sets with update-after-bind, per-draw dynamic
uniform offsets, 16-stream vertex input - against the host pipelines the
small pass uses.

| `bd_cull_distance=120` (fewer draws) | both windows at the title screen | - |

Twice now a run with a tighter cull distance was not in a field scene at
134 s (154 surfaces a window, all 3664x1920 present blits at 60 fps). The
draw-count question is being asked a different way instead:
`bd_pass_split_draws=100` ends and reopens the render pass every 100 draws
inside the deferred flush, with the same cull and the same scene.

| `bd_pass_split_draws=100` | 20.5-20.6 ms, one surface record | Direct |

The queue already flushes the scene in 60-134 draw chunks, so the pass is
several render pass instances of ~100 draws each and the profiler reports them
as one surface execution; splitting finer changed nothing. The draw count of
a render pass instance is not the trigger. Image usage and tiling are the
same for the surface that bins (plume gives every render target
TRANSFER|SAMPLED|COLOR_ATTACHMENT, optimal tiling). The verbose trace
(`ovrgpuprofiler -t -v`) is the next instrument, for whatever per-surface
detail it adds before another blind probe.
