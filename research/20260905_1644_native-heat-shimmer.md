# Native heat-shimmer conversion

2026-09-05, Windows Vulkan desktop, EDT. Base `4d440ac`.
Native effect checkpoint; no full renderer, authored event or Quest qualification claim.

## Source and bounded storage plan

Guest-source and devloop instructions read completely. Exact translated
owner initializer `sub_8221AF58`, heat initializer `sub_82216940`, producer
`sub_82216AE8`, submission `sub_82216D08`, wrappers `sub_8221E758` /
`sub_8221E700`, texture copy `sub_82184790` and setter `sub_8221CE18`
inspected. Existing generated `bd_pe_ps_heatshimmer.hlsl` pixel body read.
`config/hooks/render_tweaks.toml` read first; no decompiler installation.

The owner+2712 effect is heat shimmer, confirmed by the initializer's shader
path at 0x8207079C in the owned XEX decoded in memory. No extracted image
written. Four buffered scalars at effect+684/+696/+708/+720 are horizontal
amplitude, vertical amplitude, noise scale and depth exponent. Defaults
are .03, .03, 1 and 5. Effect+616 holds the noise image. The producer
publishes indices 0..3 and four rolling phases, incremented by .03 per call.
The phase address is in uninitialized PE data; a first attempt to read it
as file-backed bytes failed and is not evidence of its runtime values.

Schedule: DoF -> prepare bloom masks -> heat-distorted scene -> weighted
scene/bloom composition -> flare -> optical -> scanline -> grading. Bloom
must remain at original UV, not follow heat displacement. The shader uses
four normal/noise samples and rejects displacement when the displaced depth
is nearer than the original depth. The planned native implementation folds
coordinate selection into the existing composite, with no new full-image
intermediate. Authored properties and the cooked-image boundary remain.

Free at source preflight: 53,043,720,192 bytes (49.40 GiB). Reuse existing
build and test trees. Incremental build/test/link allowance: 1 GiB in those
trees, stop on unexpected guest rebuilding or reserve below 20 GiB; retain
the current executable/objects, no backup tree. Source/research are small.
No capture is authorized by this build budget. Before any new run that
captures, inventory retained unique raw evidence and dispose of eligible
superseded normal sets. The outstanding historical archive is over budget,
not a blanket exception. Capture allowances and exact sets must be recorded
separately before launch. No asset copies or downloads planned.

## Capture preflight and retention exceptions

Scoped inventory confirms the prior 28,497 unique files / 264,930,613,260
logical bytes / 235,170,393,832 allocated bytes. The exact existing set list,
zero-growth freeze and next-checkpoint review trigger are recorded separately
in `20260905_1655_raw-retention-inventory.md`. Direct os.stat file identities
used; no junction traversal. Python here lacks os.path.isjunction; the first
attempt failed before scanning and the corrected reparse-attribute guard ran.

Remove only 120 raw files each from `native_scanline_flat`,
`native_scanline_vr`, `native_full_eye_flat` and their validated source hard
links. These short normal controls are superseded by the protected current
grading flat/VR. Research and first/last full PNGs remain; full-eye VR's
changing-frame evidence, all previews and unresolved failures stay protected.
Target logical payload: 4,180,384,800 bytes, 360 frames / 720 paths. Exact
historical raws are not recoverable; new test runs are not byte-identical.
No source, assets, profiles, directories, dependencies or build trees removed.

New required qualification exception: `native_heat_flat` and `native_heat_vr`
are each capped at 120 frames (995,330,400 and 2,189,724,000 bytes);
`native_heat_preview_flat` and `native_heat_preview_vr` at 32 frames each
(265,421,440 and 583,926,400 bytes). Combined raw cap 4,034,402,240 bytes,
plus 100 MiB total logs/analysis exports. Retries share these allowances;
remove an unqualified superseded attempt before retrying, retaining its log
and representative failure images. Stop each process at capture completion
and restore temporary profile overrides. Current normals replace only the
short baseline once inspected; previews remain until authored heat events
and a replacement synthetic control supersede them. Reassess at the next
checkpoint. This does not waive other renderer verification requirements.

Pre-cleanup free: 53,045,018,624 bytes (49.40 GiB). New capture plus exports
fits within the targeted removed logical payload; actual savings will be
measured, not assumed from hard-link directory sums. Reserve even without
crediting cleanup is over 45 GiB. No other capture or asset output allowed.

Cleanup completed: 720 exact paths / 360 frames removed after validating
all reference names, exact two-link NTFS membership, sizes, non-reparse
ancestry and absence of app producers. No directories removed. Free changed
53,044,998,144 -> 57,226,850,304 bytes: 4,181,852,160 bytes (3.89 GiB)
actually recovered. Protected baseline/previews/failures remain untouched.

## Implementation and CPU/build checks

Native heat coordinate selection is folded into the current DoF/bloom
composite; it shifts scene and DoF samples together, but not bloom. It uses
the existing noise image mirror and explicit linear-wrap sampler, an ordered
same-eye depth veto, and render-frame phase (.03/frame) shared between eyes.
Animation no longer advances a global guest array per effect invocation;
activation history and exact legacy animation sequence deliberately differ.
The four scalars remain authored-property adapters. Original producer calls
occur only for explicit parameter diagnostics or refused whole scopes.
No new full-size scratch, quad, texture setter or emulated resolve added.

The composite now has an explicit 224-byte layout, replacing its declared
224-float4 register array. Heat offsets 176/192/208 are statically checked
and confirmed in emitted SPIR-V. Its shader dependency includes the shared
math header. All 28 CTests and 30 source guards pass. Heat CPU coverage:
16,384 independent literal/swizzle UV cases; 2,052 depth-power values with
three XY displacement checks each; equal/nearer/farther/NaN veto boundaries;
five frame extremes. These are not GPU or authored event qualification.

Existing Vulkan/OpenXR/PCH target built successfully at 16:52:23 EDT,
47,490,048-byte reblue_vk.exe, embedded base 4d440ac with local edits.
Codegen wrote zero files; only host sources/shader and link rebuilt.

Full emitted SPIR-V inspected through OpFunctionEnd: exact native layout,
four layer-zero noise taps with explicit sampler, same-eye original/displaced
depth, ordered >= veto, selected UV for scene/DoF and original UV for bloom.
No full-image intermediate or new post stage. The fused DoF evaluation is
the existing native approximation, not a claim of sampling an identical
legacy resolved DoF image. Alpha follows the selected colour through the
existing weighted composite.

## Runtime evidence

All four runs used the same 16:52:23.176 executable (47,490,048 bytes),
Clang 22.1.8, embedded base 4d440ac94 with local changes. All mounted
1673 archives / 119346 names. The config audit accepted every setting
(6/17/5/16 respectively). All four logs have zero checked error/critical,
VK_ERROR, device-loss, exception or assertion markers. An initial broad
search matched the benign word "exceptional"; the corrected whole-word
search produced zero errors. No timings used as headset performance claims.

### Flat heat preview

Log 787, PID 25056, 16:56:37.355-16:57:41.482. Original five settings
(autoplay/perf true, delay 60, minimum draws 600), count 32 instead of 120,
plus `bd_native_heat_preview=true`. Preview affects native inputs only:
amplitudes .03/.03, noise scale 1, depth exponent 1 (not authored default 5).

32 hard links in `out/verification/native_heat_preview_flat`,
`frame_1788641859_0.raw` through `frame_1788641860_31.raw`, frames 2842-2873,
1920x1080 / 8,294,420 bytes each, 16:57:39.743-16:57:40.598.
31/31 changes exceed 6%, maximum 14.6896% (22->23). This is **not** a normal
stability pass. First, last and both worst-pair full images inspected: strong
animated heat waviness over coherent Shu, village, foliage and shadows;
existing distant blur remains. No cyan hits; median .01051%, max .01987%.
Only four inspection PNGs exported, not all 31 flagged pairs.
Last sample: 2,698 native heat/post scopes, three original/input refusals,
866 grading/scanline scopes, 241 flare frames / 3,615 sprites, zero heat
authored checks. The preview proves GPU execution, not authored activation.

### Desktop VR heat preview

Log 788, PID 9440, 16:59:18.623-17:00:24.485. Same preview with minimum
draws 450, native sun and shadow passes true, VR true, legacy stereo false,
multiview/layered textures true, scene-array capture/mirror false, camera
mode 2, diorama height 0, XR scale 1.0 (17 settings). Existing absolute
xrsim manifest/DLL; process-local width 1440, height 1584, eye height 0.

32 hard links in `native_heat_preview_vr`, `frame_1788642020_0.raw` through
`frame_1788642022_31.raw`, frames 7705-7736, 1440x3168 / 18,247,700 bytes,
17:00:20.950-17:00:22.742. 31/31 changes exceed 6%, maximum 7.01722%
(30->31); no cyan. Both full first/last eyes inspected: coherent animated
distortion across ground/rocks/windmill, orange sky, no letterboxing. This
is not a normal stability or comfort pass. Both stereo checks exit 0:
32/44/52/62/72/82/90/95% bands 0/0/0/0/-4/-6/-8/-9; far 0, near -9,
spread 9, correctly crossed. Strong distortion reduces distant matching.
Last sample: 7,798 native heat/post scopes, three original/input refusals,
5,129 grading/scanline scopes, no visible flare, zero heat authored checks.

### Normal flat regression

Log 789, PID 25972, 17:01:02.698-17:02:09.725, original five settings
restored (no previews; 120 frames, minimum draws 600). 120 hard links in
`native_heat_flat`, `frame_1788642124_0.raw` through
`frame_1788642128_119.raw`, frames 2841-2960, 1920x1080,
17:02:04.975-17:02:08.315. Standard sequence/cyan tools: 0/119 changes
over 6%, max 3.32% (0->1), no cyan, median .011%, max .02%.
Full first/last inspected: recognizable Shu, cast silhouette, foliage,
moving windmill shadows and existing background DoF. No heat activation.
Last sample: 2,998 native post scopes / three original input refusals,
861 grading/scanline scopes and 241 flare frames / 3,615 sprites.

### Normal desktop VR regression

Log 790, PID 756, 17:03:04.655-17:04:15.115. Same 16 VR settings as the
preview minus heat override, count 120. Same process-local xrsim settings.
120 hard links in `native_heat_vr`, `frame_1788642246_0.raw` through
`frame_1788642254_119.raw`, frames 7932-8051, 1440x3168,
17:04:06.965-17:04:14.646. Standard sequence/cyan tools: 0/119 changes
over 6%, max .38% (106->107), no cyan. Both stereo checks exit 0:
44/52/62/72/82/90/95% bands -1/-2/-3/-5/-6/-8/-9; far -1, near -9,
spread 8, correctly crossed. Both full first/last eyes inspected: full-height
village/ground/stairs/rocks, windmill and moving shadow, existing distant
blur. Character-shadow visibility in this framing remains unqualified.
Last sample: 7,798 native scopes / three original input refusals, 5,370
grading/scanline scopes, heat/optical/grain off, no visible flare.

## Retention and remaining scope

All four processes and analysis sessions are terminal. Original five-setting
profile restored and read back. Guest-source established exact effect and
ordering; devloop reused the configured target; vrsim supplied desktop-only
final-eye verification. No Quest/Thor run, asset copy/conversion, dependency
download, build-tree duplication or guest-object rebuild occurred.

New raw payload is exactly 4,034,402,240 bytes (304 frames), inspection PNGs
26,103,797 bytes, app logs 1,335,679 bytes; no capture retry. All fit the
recorded allowances. Free after verification: 53,135,228,928 bytes (49.49
GiB), a net volume usage reduction of 91,508,736 bytes (87.27 MiB) relative
to the source preflight. The earlier 3.89 GiB cleanup mostly funded current
verification; it is not the net saving for the whole checkpoint.

Final raw archive union: 28,441 unique files, 264,784,630,700 logical bytes,
235,024,411,272 allocated bytes. Still over the 10 GiB target. Frozen
historical sets have zero additional allowance; their next-checkpoint review
remains required. Current heat normal flat/VR is the latest short verified
control; grading normal flat/VR is the previous baseline, now eligible for
review before another replacement capture. Keep synthetic controls,
startup controls and unresolved late/failure pairs until their recorded
cleanup conditions are met. Do not use this short result to discard them.

Heat authored activation/parameter comparison, events, noise-asset diversity,
VR comfort and exact animation parity remain unqualified. The diagnostic
publication comparator is implemented, but no authored heat producer ran
in these four tests. Dual-mask bloom remains an effect-combination refusal.
Three startup/transition input refusals need exact tracing. Post image,
authored property and UI boundaries remain, as do native animation/scene/
material ownership, full frame scheduling, removal of remaining console
resource paradigms and representative fields/battles/cutscenes/menus/
transitions/reloads in both eyes. The full objective remains active.

## Final checkpoint review

Resumed after documentation-only `ccd335c`. Renderer sources, the existing
16:52 executable, logs 787-790 and restored profile were checked again;
no app/build producer was running. All 28 CTests and 30 source guards passed
again using existing binaries. Streaming read-only analysis of all 304 raw
frames reproduced 0/119 normal flat/VR jumps and 31/31 preview jumps, with
zero cyan hits. Normal last flat/both-eye images and strong preview images
were reinspected. No new captures, image exports, asset copies or rebuilds.
At 17:16:50 EDT, free space was 53,109,997,568 bytes (49.46 GiB).
The frozen historical archive remains cleanup debt; the new no-growth gate
in `AGENTS.md` applies before any further capture production.
