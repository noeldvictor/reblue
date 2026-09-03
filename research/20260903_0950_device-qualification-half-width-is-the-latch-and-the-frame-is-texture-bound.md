# Device qualification: half width is the latch, and the frame is texture-bound

2026-09-03, 09:08-09:50. Quest 2 back on adb after a night offline. Head build
`82d00c2` (`out/probe/reblue_head.apk`, 02:21), then `1 commit` on top for the
half-width fix. Every run is `tools/verify_quest.sh` through autoplay, which
lands in a different scene each time; only within-run A/Bs and same-build
traces are compared below.

## The runs

| # | build | config | GPU p50 (ms) | scene | note |
| --- | --- | --- | --- | --- | --- |
| 1 | head | side-by-side defaults | 20.8 (p10 20.0, p90 25.2), 398 draws, CPU 21 | rock close-up (fill-heavy) | host-built list live: 63 entries in 39 runs a frame, matrix check 5,397 ok / 0 off; 125 of 199 node draws host-issued, 3 volatile |
| 2 | head | side-by-side, render-stage trace | scene pass 12.3-22.8 in the trace window | rock close-up, crossed stereo verified | 26 surfaces a frame, all Direct |
| 3 | head | multiview half width | **hung at launch** (alive at 150 s, no guest threads, log ends at `OnInitialize: done`) | - | not reproduced on the rerun; no crash record in logcat |
| 3b | head | multiview half width, stacked capture | **39.8** (p10 27.1, p90 41.2), 393 draws, dt 50 | rock | both layers correct, crossed; `rs_eager` 6 against side-by-side's 2 |
| 5 | head | multiview half width, trace | scene 688x720x2: **14.6 render + 4.5 preempt**; 1376-wide chain passes ~7 | - | the 344x180 bright-mask pass read 2.96 ms |
| 6 | head | multiview half width, GPU counters | - | - | table below |
| 7 | half-width fix | multiview half width, stacked capture | **24.6** (p10 23.8, p90 44.7), 392 draws, dt 33.5 | rock | chain at 680x720 a layer; `rs_eager` 2; pair correct |
| 8 | half-width fix | multiview half width, A/B `bd_depth_prepass` | arm off **23.8**, arm on **29.2** (+22.6% GPU, +38.8% CPU/draw) | - | 225 of 225 draws prepassed |

## What the multiview number was

The first half-width run read 39.8 ms against side-by-side's 20.8 in the same
kind of scene, with six eager resolves a frame against two. The log named
them: `688x720 fmt 33 -> 1376x720` (depth), `688x720 fmt 10 -> 1376x720 scale
0.25` (the HDR scene, no longer an alias because the sizes differ), two
`688x720 -> 688x360` copies that side-by-side skips as "downscaled scene, no
reader", then the composite seed and front copy at 1376x720 on two layers. The
dof pyramid and the composite ran "from the 1376x720 scene": the guest sizes
its whole post chain from the back buffer, and `bdSceneResolutionScaleHook`
had halved only the scene surface, so each half-width layer was resolved back
up to full width and the chain ran at twice side-by-side's pixels.

The seam is `Output::LatchedFit`: every guest texture follows that rect (the
output-res hooks write it into the guest's device dims, the composite texture,
the view scale). Halving it there under `bd_stereo_multiview && bd_mv_half_width`,
with `RenderAspect` kept on the unhalved width so the projection and the 2D
fit stay at the composed aspect, puts the whole guest frame at 680x720 a
layer. Desktop: the chain runs at 960x1080 a layer and the pair is correct
(capture 09:34, dof, bloom and the sun shadow in both eyes, proportions right).
Quest: 39.8 -> 24.6 ms, resolves back to two.

## The scene pass is fragment-bound, and not by hidden fragments

Trace of run 5, one frame: the two-layer 688x720 scene pass renders in 14.6 ms
plus 4.5 ms of compositor preemption. Side-by-side's 1376x720 scene pass in run
2 rendered in 12-13 ms for the same pixels per eye and twice the draws. So at
this point multiview's one-submission saving is invisible: the pass costs what
its fragments cost.

GPU counters (run 6, multiview half width, `tools/gpu_metrics_quest.sh`):

| metric | value |
| --- | --- |
| % time shading fragments / vertices | 99.3 / 0.7 |
| fragments shaded / s | 1.26 G |
| % shader ALU capacity utilised / % time ALUs working | 21 / 33 |
| % shaders busy / stalled | 85 / 16 |
| **% texture pipes busy** | **72** |
| L1 texture cache miss per pixel | 0.36 |
| textures / fragment | 2.27 |
| ALU / fragment | 43 |
| wave context occupancy | 49% |
| **% non-base-level texture fetches** | **0.87** |
| % nearest filtered | 41 |
| average polygon area | 374 px |

The prepass A/B (run 8) rules out overdraw as the lever: emitting every
depth-writing draw twice costs 5.4 ms of GPU and 39% more CPU per draw, and
rejects nothing worth having. The closed door on the prepass stands, now in a
fill-bound frame as well as in the draw-bound one of 2026-09-02.

## The mip chains are not sampled on the Quest

0.87% of fetches from a non-base level, against 0.96% on 2026-09-02 *before*
the host mip chains landed. The chains exist on the device (`[mips] host chain
#N ... -> 8 levels` in the log, 316 guest chains plus the host's), the texture
carries the levels, the bindless view exposes them
(`bindless.cpp: view_desc.mipLevels = tex->mipLevels`), the samplers decode
`mip_filter` (point, on all 512 fetch constants) with plume's LOD range open,
and the recompiled pixel shaders sample with implicit LOD (`tfetch2D` ->
`texture.Sample`, 85 of 85 pixel shaders; no `SampleLevel`).

`bd_debug_mip_bias` (a probe cvar, mip LOD bias on every guest sampler): the
desktop at bias 6 blurs every surface to flat colour (capture 09:44), so the
host plumbing reaches the chains on a desktop driver. **The Quest at bias 6
blurs the same way** (run 9, `capture_run9_mipbias.png`: the rock a smooth
blob, the water flat cyan), so the chains are sampled on the device too, and
the counter does not mean what the 2026-09-02 note took it to mean. And the
frame did not move: **20.2 ms** with every fetch on a tiny level, against 20.8
in the same rock scene without the bias. Texture cache misses are not the
cost. The cost is the fetch count: 1.26 G fragments a second at 2.27 fetches
each is ~2.9 G fetches a second, which is the texture units' throughput on
an Adreno 650, and matches "% texture pipes busy 72" with ALUs a fifth used.

So the scene pass is bound by fragments x fetches, the fragments are not
hidden ones (run 8), and the levers left are the number of fragments the
visible layers produce (blended layers in front, foliage cards, fog and glow
quads - the render list's sorted materials are the suspects) and the fetches
per fragment. Foveation cuts the periphery's share of both; it is the stage 7
work. Any target above today's 688x720 a layer multiplies this cost by the
pixel ratio: 1440x1584 a layer is 4.7x.

## The scene pass was eight passes (desktop, 10:05-10:15)

Owner decision at 10:00: no more Quest runs until the host owns the frame; engine code may
be replaced. First cut, from a desktop `PLUME_FB_TRACE` of the multiview half-width frame:
26 render passes a frame, and the 960x1080 scene framebuffer began and ended **eight
times**, with `setFramebuffer old=X new=X active=1` between them - the host re-setting the
framebuffer it already had (every `draw_framebuffer_bound = false` site: the guest's
mid-pass resolves, the deferred queue's flushes), and plume ending the pass for a rebind
of the open framebuffer. On a tiler each boundary stores and reloads the two-layer
fp16+depth target; on the Quest each is a compositor preemption point, which is the
4.5 ms of "Preempt" inside the scene pass of run 5.

plume (fork `0bf3d63`): a rebind of the framebuffer whose pass is open is a no-op when no
clear is pending. Desktop: **21 passes, the scene in 3 sub-passes**, image identical. The
PC GPU frame did not move (6.0 vs 5.5 ms; a desktop GPU does not pay for pass boundaries),
which is why the PC never showed this. The three that remain are ended by barriers. The
sun-occlusion query issues one inside the scene pass (its counter zeroing and readback are
buffer copies bracketed by barriers); those moved to the command list's begin and submit:
scene in 2. The last one, once plume's trace named textures at creation and the host named
the guest object behind each (`PLUME_FB_TRACE` had also been truncating the host's lines:
plume opened it with "w" after the host had written), was the **reflection surface**
flipped to shader-read on the first water draw by `TransitionResolveSources` - which
flushed the queued scene draws first (plume opened the pass on them) and then issued the
barrier. The barrier goes ahead of the queued draws now; they do not touch the surfaces it
flips. **Desktop: 19 passes a frame, the scene pass one pass, image unchanged** (capture
10:44). What is left of the frame's structure on the desktop: shadow (a held-clear flush
plus the pass), reflection, scene, the MSAA depth and colour resolves (desktop only), the
dof pyramid (5), bloom (3), the tail (composite, UI, front copy, a second 32-bit pass, with
barriers between) and the present.

## The chain seeds are tile aliases now (desktop, 10:50-11:07)

The tail of the frame was: host composite into the guest's 16-bit surface; a fresh 16-bit
surface *seeded* with a copy of it for the guest's 2D pass, whose first draw is
`bd_simple2d_ps` on a four-vertex quad, source-alpha blend, sampling the composite (the
360's "blit the composite into the tile it will draw over"); a format-converting resolve
into the 8-bit front texture; a fresh 8-bit surface seeded from that for the guest's last
2D draws; the present. Seeding off is wrong (stale pool content and a faded title card
bleed through the partial-alpha quads), so the previous image is genuinely inherited.

The console's own model does not copy: the fresh surface is the same EDRAM tile under a
new handle. `AliasFreshTargetToChainHeadLocked` (`bd_chain_alias`, default on) makes a
fresh full-screen surface bound after the chain head *be* the head's texture: the head's
lazy-linked resolve textures are materialised first (the resolve the guest asked for; it is
what keeps sampling the previous image while drawing over it defined), the pool holds a
head while an alias lives, the alias ends on return or reuse. Steady state, multiview
half width: **seeds 2 -> 0 a frame, barrier calls 44 -> 35, GPU 6.0 -> 5.8 ms**, vsync
held, image identical to the seeded reference. Then the 16-to-8-bit front conversion (11:13): the resolve gate accepts a
colour-to-colour format change for the full-screen chain while the host owns the post
chain, so the front texture is a lazy link to the composite's fp16 surface and the
conversion pass never runs (every reader samples float4; a read the substitution cannot
reach still materialises through the converting shader resolve). The alias follows a
head's link to the surface that holds its image and takes its format, so the guest's last
8-bit pass draws into the same fp16 image too. **Eager copies 3 -> 2** (the desktop's MSAA
resolves are all that remain), framebuffer binds 10 -> 9, barrier calls 44 -> 34 over the
morning, image identical. On the Quest, which has no MSAA, the residue of the tail is now
zero copies; the reflection stub's 128x72 resolve and one materialise are what is left of
the EDRAM model in a frame.

## The tail after the aliases (11:15-11:40)

The last 2D pass samples the front texture, so its alias materialises a copy of the image
it draws over; with the front texture declared RGBA8 that copy was the converting shader
pass again. A full-screen chain texture of another colour format is now re-backed in the
source's format the first time it is materialised while the host owns the post chain
(four textures at startup, never per frame; `[resolve] ... re-backed`). Every copy in the
tail is a blit from then on. The alias materialises every link along the chain (the front
links to the last alias, not the root), `TransitionResolveSources` leaves a source that
shares the bound target's texture alone, and an alias uses its root's framebuffer cache.
The desktop tail is composite | blit | 2D | blit | 2D | present: 18 passes a frame, image
identical to the seeded reference. The two 2D passes cannot join the composite: each
samples the image it draws over, which needs two images, and the copy between them ends
the pass. The console did that read from the tile itself; the modern equivalent is a
same-pixel read through an input attachment with a self-dependency, which would put the
whole tail into one pass with no copies. Later.

Two traps met on the way: the first retype build deadlocked two seconds in (the Park
helpers take the mutex the copy holds; the objects are pushed into the graveyard by hand
now), and **frame time must not be read from a `PLUME_FB_TRACE` run**: the unbuffered
trace makes the frame CPU-bound (51 ms, 20 fps); the same build untraced is vsync-locked
at 4.9 ms of CPU, with the alias on or off.

## The fragment census (11:40)

The Quest's counters say the scene pass is bound by fragments x fetches; which shaders
produce the fragments is what the materials stage needs first. plume gained
pipeline-statistics query pools (fragment shader invocations; Vulkan, fork `6a6f679`+)
and the host brackets every queued draw with one (`bd_frag_census`, `gpu/frag_census.cpp`,
the draw's pixel shader hash on the queued draw), folding the counts per pixel shader
every 300 frames: `[frag] N M fragments a frame over D draws; the top ten`. Desktop only,
but the geometry, the overdraw and the shaders are the Quest's. Frame time is unchanged
with it on (4.9 ms CPU, vsync held).

First report, desktop, multiview half width at 960x1080 a layer (2.07 M pixels a frame),
shadows and reflections on (the desktop defaults), the village:

| pixel shader | fragments a frame | share |
| --- | --- | --- |
| `bd_normal_ps` (the lit material: colour, two detail colours, normal, cube, up to six shadow taps; 1,333 lines, 19 fetch sites) | 5.08 M | 48% |
| `bd_normal_ps_nolight` | 2.09 M | 20% |
| `bd_shadowmap_ps` (the 4096x4096 shadow pass) | 2.08 M | 20% |
| `bd_normal_ps_wind` (foliage) | 1.16 M | 11% |
| `bd_normal_ps_ref` | 0.11 M | 1% |
| `bd_toon_ps` | 0.05 M | 0.5% |
| **total** | **10.6 M over 756 draws** | 5.1 fragments a pixel |

Five shaders are 99% of the fragments, and one family (`bd_normal_ps` and its unlit and
wind variants) is 79%.

**The paths (11:52).** The uber-shaders are steered by the guest's pixel-shader boolean
constants (`BOOL_BIT(128+n)` is bit n of the first PS boolean word). A census of draws per
(shader, four words) found 38 paths in the village frame with only the first word varying,
and the lit material drawn under four values of it: `0x046000E9`, `E1`, `E0`, `C1`. Named:

| bit | name | E9 | E1 | E0 | C1 |
| --- | --- | --- | --- | --- | --- |
| 0 | `g_bTexture0` (colour texture) | on | on | off | on |
| 3 | `g_bNMap` (normal map) | on | off | off | off |
| 5 | `g_bShadowMap` (six taps) | on | on | on | off |
| 6 | `g_bFog` | on | on | on | on |
| 7 | `g_bDiffuse` | on | on | on | on |
| 21, 22, 26 | (149, 150, 154; unnamed) | on | on | on | on |
| 4 `g_bEnvMap`, 8 `g_bSpecular`, 1-2 `g_bTexture1/2`, fog modes, debug | | never set | | |

So the host lit material is: an optional colour texture times one directional diffuse
light plus ambient from the constants, an optional normal map, an optional shadow term
(the guest's six taps; the host's can be one to four), and fog - and nothing else. The
cube fetch, the specular term and the two detail textures are dead paths in the field.
Substituted for `bd_normal_ps` by hash (the `bd_normal_ps_wind` and `_nolight` variants
next), verified against a capture of the same frame with the guest shader. That is the materials stage's target list: a host material for the
normal family with a lighting-model slot (the guest look, cel), fewer fetches per fragment,
and the shadow taps only where a shadow can land. The shadow pass's 20% is the 4096 map at
the desktop's setting; the host shadow map (stage 5) sizes it to the view.

## Also seen

- The launch hang (run 3): the process stayed alive with no guest threads and
  the log's last line was the SDK's `OnInitialize: done`; the next line in a
  healthy boot is written by the same thread in the same millisecond. The
  deferred `LaunchModule` goes through an SDL user event the Android poll loop
  (SDK fork, 2026-09-02) does deliver, and the rerun booted. Unexplained; one
  occurrence in nine launches today.
- `tools/gpu_drawtrace_quest.sh` takes its config as its first argument, not
  from `EXTRA` in the environment; one trace was side-by-side by mistake.
- `verify_quest.sh` pulled no capture from run 1 although the log shows one
  written; the next run's pre-clear removed it before it could be checked.
- The stub shadow pass with shadows off reads 0.5-0.7 ms on a 64x64 map in
  every trace today; it becomes the host shadow map in stage 5.

## Sources

- `out/device/verify_run1.log`, `perf.csv` of each run as `perf_run*.csv`
- `out/device/gpu_drawtrace_run2_0903.txt` (side-by-side), `gpu_drawtrace_mv_0903.txt` (multiview)
- `out/device/gpu_metrics_mv_0903.txt`
- `out/device/capture_run2.png`, `capture_run3b_mv.png`, `capture_run7_mv_half.png`
- desktop captures `out/build/win-amd64-release/logs/capture/frame_1788442462.png` (half width), `frame_1788443010.png` (bias 6)
- `research/20260902_0930_the-scene-pass-is-the-gpu-and-a-depth-prepass.md` for the earlier counters
