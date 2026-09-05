# Experimental native sun camera: missing caster remains unresolved

2026-09-05, Windows Vulkan desktop, EDT. Follows the tested math checkpoint
`c8924ea`. The integration is **disabled by default**, not a completed camera
conversion. `bd_native_sun_camera=true` currently loses Shu's cast silhouette.
The ordinary default still executes the explicitly counted engine snapshot
and light fitter. This is a correctness gate, not a legacy/native FPS choice.

## Implemented boundary

The native sun producer uses the current host view/projection, authored sun
pitch/yaw and shadow dimension. It owns a texel-snapped orthographic camera,
receiver reach and upstream/downstream caster padding. It also derives the
scene-camera snapshot using native inverse/orientation math. No guest
trigonometric helper, projection fitter or object-ray query executes there.
Inactive/unsupported paths and remaining engine calls are explicit.

The camera record is current-frame scoped. Primary receiver projection reads
its matrix directly. The older draw-time register fit and target-size role
classifier are bypassed while this record is current. Six clip planes override
the view-cache volume, including cached-volume selection; the old 13-value
perspective shape remains a getter adapter, not an orthographic representation.
An explicit native shadow scope also replaces `sub_82287788` culling with a
sphere/plane test. Outside that scope the original culling adapter remains.
`bd_shadow_fit_diag` optionally compares original cull decisions and logs fit
and lighting values; these comparisons deliberately execute the original.

The producer refuses when native views/frustum support is disabled. It leaves
the authored light-position getter (+28) untouched; native eye position is in
the owned record/view matrix. Focus (+40), view/projection (+68/+132), projected
target (+336), the scene snapshot and existing downstream parameter publications
are still temporary engine ABI outputs. The new model does not reproduce the
old adaptive coverage/query algorithm or every derived descriptor field.

## Investigation and rejected explanations

The guest-source skill directed reading the exact translated sun fitter,
snapshot and helpers, the render-tweak hook configuration, the general object
culler and both its callers, and the special view branches of `sub_82287788`.
Generated code and hook TOML were not edited. The devloop skill kept builds
host-only and runs on desktop. No dependency, game asset or cooked cache changed.

The first integration failure is recorded in
`20260905_0838_native-sun-camera-math.md` (logs 736/737). A native light-eye write
had changed the authored getter from (0,0,0) to roughly (363,773,567). Removing
that write did not restore the silhouette. A trial hook on
`bdVisualObjectFrustumCull` was not exercised during the sun scope; it was removed.

The actual host walk excludes view 1 from its general native-plane branch.
Its remaining `sub_82287788` path at 0x82287B3C uses view-depth cutoffs and radial
tests against primary +324/+328/+332/+336, not the published six planes.
Replacing that path within the owned sun scope admits many small casters that
it rejected under the new fit, **but still does not restore Shu's shadow**.
This is not proof that culling was the sole cause of the visible regression.

## Desktop runs

All diagnostic runs used the original autoplay/perf/60-second/600-draw/120-frame
profile plus `bd_shadow_fit_diag=true`; log 741 additionally enabled one
RenderDoc frame at 65 seconds. Configuration audits succeeded. Runs were
stopped by verified owned PID before capture analysis/replay.

| Log / PID | Binary timestamp / bytes | Process interval | Result |
| --- | --- | --- | --- |
| 738 / 12932 | 08:40:02 / 47385600 | 08:40:03-08:41:30.100 | Authored getter preserved; silhouette still absent |
| 739 / 24596 | 08:44:50 / 47389696 | 08:44:51-08:46:43.662 | General object-cull experiment recorded zero sun-scope calls; removed |
| 740 / 4936 | 08:47:27 / 47389696 | 08:47:28-08:49:06.425 | Actual view-1 native culling exercised; silhouette still absent |
| 741 / 8784 | same 08:47:27 binary | 08:52:29-08:54:38.990 | One saved GPU frame, examined offline |

Log 738's 120 frames are isolated in `out/verification/native_sun_getter_flat`,
`frame_1788612065_0.raw` through `frame_1788612069_119.raw`. Log 740's are in
`out/verification/native_sun_cull_diag`, `frame_1788612510_0.raw` through
`frame_1788612514_119.raw`. Both have 0/119 jumps over 6%, no cyan patches,
median cyan 0.012%, maximum 0.02%. Full-resolution final images were inspected:
Shu is present but his cast silhouette is absent. Neither is a visual pass.

Final sampled log-740 totals: 4801 native fits/snapshots, no original snapshot
or light-fit calls, 4801 shadow begins / 4800 ends, 34475 ownership checks with
no mismatch/fallback. Sun culling compared 2304367 original decisions, with
485928 changed and 2290369 natively visible. These changed decisions are not
an equivalence claim. Counters are sampled mid-scope, not a balanced shutdown.

## GPU evidence and limits

The installed RenderDoc API 1.7.0 captured
`out/build/win-amd64-release/logs/renderdoc/reblue_frame3027.rdc`, 546383450 bytes.
It finished writing at 08:53:58 and the renderer advanced afterward. The
ordinary sequence completed before the capture. Replay occurred with no game
running. Scratch scripts and reports are ignored local diagnostics in `out/`.

The depth image `ResourceId::1125` is D32S8, 4096 square, one layer/sample, with
743 draw actions. It was exported and actually viewed in
`out/verification/native_sun_rdc3/shadow.png`. It contains village/windmill
geometry; this does not identify Shu's individual caster draw.

At scene pixel (1100,815), terrain event 2245 writes the last observed colour
change in the selected 1920x1080 MSAA4 scene target (`ResourceId::37420`). Its
receiver VS c36 matrix equals sampled caster VS c32. PS c9 is
(0.0002500000118743628, 0.5, 480, 270), matching the logged bias. Reports are in
`out/verification/native_sun_rdc_probe3/probe.txt`. This rules out a matrix
mismatch for those sampled draws, not every caster/material or sampled image.

Early export attempts hit an installed-API component-mapping mismatch and a
Windows-invalid colon in a filename; neither succeeded. The corrected export
did. `GetConstantBuffer` is absent in this installed replay API; the successful
read used `GetConstantBlocks(..., True)` and its descriptor resource/offset/size.
Post-VS whole-buffer bounds included unreferenced/sparse vertices and must not
be treated as proof of malformed geometry without decoding the index stream.
All owned replay processes were stopped after analysis.

## Default-off control

The final host-only build completed at 09:12:30: `reblue_vk.exe`, 47389696
bytes, source `c8924ea` plus this integration, with `bd_native_sun_camera=false`
compiled as the default. The original five-setting profile was restored exactly;
log 742 confirms all five took effect. No diagnostic or RenderDoc override
remains. PID 21764 ran 09:12:31-09:22:58.920 and was stopped after checking its
executable path and start time. The extra post-capture runtime is not additional
visual coverage or a performance measurement.

The 120 1920x1080 captures at 09:13:34-09:13:37 are isolated in
`out/verification/native_sun_default_off`, `frame_1788614014_0.raw` through
`frame_1788614017_119.raw`. Analysis after stopping the process found 0/119
jumps over 6%, no cyan patches, median cyan 0.013%, maximum 0.02%.
Full-resolution first and last images were inspected. Shu's cast silhouette
is clearly present in the first image; moving windmill shadows overlap it
in the last. This restores the short normal flat control, not qualification
of the opt-in native camera or the complete renderer.

The final sampled log totals are 35638 shadow begins / 35637 ends, 35637
explicit outputs, 4738 empty clears and 218561 ownership checks, with no
mismatch or lifecycle fallback. Engine snapshots and light fits are 35638
each; experimental native sphere-cull calls/comparisons remain zero.
Native views report 80173 updates, no compatibility/refusal/import/bootstrap.
Neither retained log segment (`reblue_742.log` / `reblue_742.1.log`) contains
an error, critical, assertion, fatal or device-lost marker from the checked
patterns. These sampled counters do not prove every mode or a balanced shutdown.

## Checks and next gate

All 23 CTests and twelve source guards pass. Sphere/volume tests include interior,
outside, overlapping and invalid spheres. Source guards check explicit sun
scope ownership and the absence of the unused object-cull hook. These do not
independently prove the integrated shadow image, ABI or threading behavior.
Host-only builds succeeded without rebuilding guest translation units.

Next: identify Shu's actual caster draw and its sampled depth contribution in
the saved GPU frame; trace any pre-pass selection and remaining derived-light
inputs. Then obtain normal, comparison-off flat and full-size final-eye
sequences before enabling this experiment. Later scenery/text, shadow modes,
mid-scope setting changes, native camera CPU cost and complete desktop coverage
remain unqualified. No Quest/Thor run or FPS improvement is claimed.
