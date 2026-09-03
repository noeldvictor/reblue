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
