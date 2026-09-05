# Recurring draw verification and the remaining later-scene failure

2026-09-05, Windows Vulkan desktop. Follow-up to
`20260905_0025_native-skin-bindings.md`. This checkpoint improves diagnosis;
it does not replace a renderer producer or qualify the failing scene.

## Verifier defects and changes

The old detailed-example limits were lifetime totals. Startup exhausted them,
leaving later scenes with aggregate counts but no useful register/state examples.
The new allowance is two examples per failure kind and render view per 300-frame
window, with a shared bounded bucket for unknown views. Full mismatch counters
are independent of that allowance. Shader hashes and declared-register counts
are reported alongside the full constant-file comparison; undeclared/unknown
registers are not silently excluded. Declaration metadata is not a dynamic
shader-branch trace or proof that a mismatch caused visible corruption.

A node with fewer compared draws than expected could previously be counted as
correct if its existing draws matched. The node count now records that failure,
with a separate draw-count counter. Capture refusal can also reduce the compared
count, so the log does not equate it with a missing GPU draw.

Review also found that byte comparisons included padding in
`RenderVertexBufferView` on this 64-bit build, which can flag semantically
identical views. The final comparison uses named fields, including all 16 slots,
and still compares index format, ranges, counts, topology and alpha reference.
Pipeline state remains byte-compared: its definition explicitly uses pack(1).
Named pipeline fields and per-stream details make actual differences traceable.
The short run still reports geometry differences after this change: padding
cannot explain all of them. The final log also names primitive/start-vertex/
alpha-reference differences, which the earlier detail message omitted.

`draw_verify.h` and the standalone test exercise per-view/per-kind budgets,
later windows, invalid-view bucketing, register-mask boundaries, missing/extra
compared draws, deliberately different view padding and genuine binding changes.
The initial helper checkpoint is `939daf4`, pushed to origin/main. All 12
texture/lifetime/upload/state/verification tests pass (0.54 seconds).

## Sampled baseline before the diagnostic changes

`reblue_689.log`, PID 15796, 00:41:13-00:47:27 EDT, same renderer code as
`be20698`. All six settings audited: autoplay/perf CSV enabled, capture delay
270 seconds, minimum 30 draws, 120 frames and `bd_host_draw_verify_every=31`.
Normal replay remains enabled outside the sampled candidates. This is a
correctness investigation, not a performance comparison.

The isolated `out/verification/native_draw_verify_late_baseline` contains 120
1920x1080 final captures, frames 14749-14868, 00:46:40-00:46:44. Analysis finds **109/119 jumps over
6%**, zero cyan patches (median/max 0%). Inspected pairs 1/2 and 43/44: the
character remains intact, but the latter pair still loses large background
rock surfaces and has damaged text. A resized preview of the actual diagnostic
PNG was used for the latter pair, not synthesized pixels. Motion contributes
to the change metric; the image inspection establishes the scenery failure.

Final verifier: 136195 nodes / 183120 compared draws, 69841 draws flagged;
VS 3288, world 0, PS 2974, fetch 0, texture 61227, state 145, geometry 917,
booleans 6605. These are the old noisy counters, not a count of visibly broken
draws. The old example budgets hid the later pipeline differences.
Last skin report: 831294 checks, zero mismatches, 47502 unsupported attempts;
459658 composed/replayed palettes and 4132942 joints. Sampling composes palettes
without submitting them, so this run's palette count is not all dispatched work.
Final consumer: 3499117 entries, 2686816 replayed, 799556 direct draws;
zero fallback/refusals. No error/critical, overflow/exhaustion, retirement-race,
VK_ERROR or Vulkan-failure log matches. Only the owned test process was stopped
after all 120 captures were present; captures were isolated by its start time.

## Source evidence from recurring reports

The guest-source skill kept the investigation in exact translated source and
the hook map. `config/hooks/render_list.toml` precedes the loop trace in
`generated/reblue_recomp.84.cpp`: 0x8227F9FC-0x8227FA80 sets r30=entry+388,
then publishes VS c0-c4 and PS c0-c13 from that entry's material snapshot when
its stamp changes. The host deferred consumer retains that same explicit adapter
in `BindEntry`, while replay still substitutes retained pass/visual values
for much of the block. This is a remaining ownership boundary, not a new native
scene/pass asset contract.

Recurring reports identify PS c1 camera mismatches, for example frame 1514 of
the diagnostic run: interpreted (19.881,149.184,35.502,1) versus replay
(16.266,151.440,34.664,1), view 3, list entry. VS c1 is undeclared in shader
`B5C88BB6295138CC` (`bd_mirror_vs_norm`), while PS c1 is declared in
`FB83DD3F5E67CEB7` (`bd_normal_ps`) as `g_vCameraPos` and used in its body.
The prior emitted HLSL in `out/verification/native_alpha_shaders` and the current
build-tree shader cache supply these names and masks. Early tree reports also
show declared VS c2/c3 differences in `BB6AC0229237A4CB`
(`bd_normal_cs_vs_norm`): `g_vUV01Offset` and `g_vUV2BOffset`.
These are concrete input defects to investigate, not proof that camera/UV
composition explains the disappearing rocks or the independently damaged text.

The devloop skill kept builds in the existing Vulkan/OpenXR desktop tree,
target `reblue`, and generated source unmodified. Codegen reports zero
writes/deletes and one module up to date; no guest objects were rebuilt.
No Quest, Thor or headset runs are authorized by this partial checkpoint.

## Recurring diagnostics through the later scene

`reblue_690.log`, PID 9876, 00:49:19-00:56:01 EDT, `939daf4` plus the integrated
recurring reports and named pipeline fields. This run preceded the semantic
buffer-comparison change. All six settings audited, identical to the late
sampled baseline. The isolated `native_draw_verify_late_diagnostics` sequence
has 120 1920x1080 final captures, frames 14647-14766, 00:54:44-00:54:48.
Again **109/119 jumps over 6%**, zero cyan patches (median/max 0%). Inspected
pairs 43/44 and 118/119: the character remains intact, but background surfaces
appear/disappear and text/dialogue presentation is still defective. These
diagnostic changes do not improve the failed later-scene qualification.

At frame 14760, view 3 direct node `2864392000000300`, visual `284D1120`,
sub-draw 0, shaders `B5C88BB6295138CC` / `FB83DD3F5E67CEB7`:

- PS c9 (`g_vShadowEpsilon`) is (0,0.5,16,18) interpreted versus
  (0,0.5,480,270) replayed; the shader declares and uses it.
- Explicit texture slot 5 is native asset `C7FA987FEB91D6BE` interpreted versus
  `30E2CB5D0B9BE5DA` replayed.
- The colour-write mask is 15 interpreted versus 7 replayed.

At frame 15561 another direct node (`285FAA4400000300`, visual `284CE6E0`)
reports the same texture/write-mask mismatch, with c9=(0,0.5,80,90) interpreted.
This is a specific stale material/pass recipe, not merely an unused inherited
texture slot. It is a stronger next investigation target than aggregate jump
counts, but has not yet been causally isolated from the disappearing scenery.

Final verifier: 148239 nodes / 201652 draws, 70933 draws flagged; declared VS/PS
differences on 1160/4296 draws, state 135, geometry 519, draw-count nodes zero.
The geometry count still includes the old representation comparison in this run.
Last skin report: 906728 checks, zero mismatches, 62949 unsupported attempts;
542951 composed/replayed palettes / 4553533 joints. Final consumer: 3659827
entries, 2751117 replayed, 894812 direct, zero fallback/refusals. No
error/critical, overflow/exhaustion, retirement-race, VK_ERROR or Vulkan-failure
log matches. Only this owned process was stopped after the complete capture.

## Short check with semantic buffer comparisons

`reblue_691.log`, PID 23668, 00:56:36-00:58:30 EDT. The same binary code plus
semantic buffer comparisons; all six settings audited, with capture delay 60
seconds/minimum 600 draws/120 captures and sampling still every 31 candidates.
The isolated `native_draw_verify_short` sequence has 120 1920x1080 captures,
frames 2841-2960 at 00:57:39.413-00:57:42.798: **0/119 jumps over 6%**, zero
cyan patches (median 0.011%, max 0.02%). Inspected the actual first and last
field images: character/terrain remain intact with no disappearing large
surface in that pair. The separate two-file preview compares nonconsecutive
endpoints for inspection only; its output is not the sequence stability metric.

Final verifier: 49946 nodes / 67444 draws, 24485 flagged; declared VS/PS
differences on 64/478 draws, state zero, geometry 119, draw-count nodes zero.
Thus geometry differences remain even after removing possible padding noise;
they are not evidence of newly missing meshes. Last skin: 218409 checks, zero
mismatches, 4715 unsupported attempts. Final consumer: 1272190 entries,
1013523 replayed, 254288 direct, zero fallback/refusals. No error/critical,
overflow/exhaustion, retirement-race, VK_ERROR or Vulkan-failure matches.

The final message-only expansion (primitive/start-vertex/alpha reference and
input-slot fields) was compiled and linked after this run; those extra message
fields were not exercised in a new runtime capture. The material/skin suite
also passes independently (one CTest test, 0.02 seconds). All renderer processes
were stopped by their owned PID after capture; the original five-setting flat
profile is restored. No new normal-without-sampling or VR run was made for this
diagnostic-only checkpoint. Full desktop/both-eye acceptance remains open.
