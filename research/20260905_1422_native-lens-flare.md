# Native lens-flare production and instanced submission

2026-09-05, Windows Vulkan desktop, EDT. Base `14fd0b3` plus this checkpoint.
This is another post-effect ownership conversion, not a completed host frame
or a Quest qualification. The full desktop/game-coverage gate remains.

## Exact boundary and implementation

The guest-source skill guided complete PPC-comment reads of the outer post
initializer `sub_8221AF58`, lens initializer `sub_82217F28`, full lens producer
`sub_82218140`, sprite submission `sub_8221E298`, visibility getter
`sub_82183DE8`, query initialization `sub_82179440`, and transform helpers
`bdVec3TransformByMatrix` / `sub_824911F8`. The vtable at 0x82070A74 has two
no-op preparation entries (`sub_820DFA50`) and the lens producer as its draw
entry. Root+8660 is the lens object, not an unspecified glare filter.

The 15 optical sprite recipes, four texture diameters, geometric constants
and shader path were checked against the owned XEX in memory. No decoded
image was written to disk. The existing dumped `bd_pe_ps_lenzflare` shader
confirms RGB = texture RGB * tint RGB * intensity, with texture alpha
preserved. The retained pipeline description and blend decoder confirm
screen blending: source times inverse destination, plus destination.

`lens_flare.h` now produces typed rectangles, colors and native recipe image
indices from native transforms and authored light/focus/visibility inputs.
It preserves the original projected-z lens convention and authored normalized
sprite sizing. The host post schedule no longer calls the lens wrapper,
per-sprite constant flushes, texture setters, UP vertex submission, target
allocator or emulated output resolve. Two small native shaders submit all
15 sprites with one `drawInstanced(6, 15, ...)` into the explicit persistent
post attachment. Flat and layered targets use the same native pipeline
description with an appropriate multiview mask. No new render target or
copy of an optical asset is needed.

The initial optical lookup wrongly assumed host-allocated handles. Read-only
inspection of the exact owned process confirmed four engine-owned headers
(0x20017620, 0x200176A0, 0x20017720, 0x200177A0). The corrected path resolves
the already eagerly imported/cooked native texture mirrors, outside the
renderer lock; it does not lazily decode arbitrary memory or execute a setter.
Required descriptors are bound explicitly before native GPU work.

When no other trailing adjustment is active, the supported post scope now
executes zero old tail filters and zero state-308 calls. Unsupported packed
effects, dual-mask mode, other trailing adjustments and unavailable inputs
remain counted compatibility boundaries. Authored light sources, buffered
properties, completed visibility-query records, optical-image handle lookup
and downstream output/UI getters remain adapters. Per-eye optical-source
derivation and fully native visibility production are still required.

## Build and parameter evidence

The devloop/vrsim skills keep all work on desktop. Existing build trees and
native texture caches were reused; no guest translation unit rebuilt. The
first shader build rejected the reserved HLSL identifier `point`; renaming it
fixed compilation. A silent sandbox unit-build attempt was explicitly stopped
through its live session before the successful elevated retry.

All 26 CTests and 21 source guards (10 scene, 3 reflection, 8 post) pass.
The expanded post unit test covers projection, authored sprite sizes/tints,
occluded intensity, edge clamping and all recipe image indices. SPIR-V
inspection confirms 48-byte sprite stride, member offsets 0/16/32, instance
indexing and matching shader interfaces. The pixel shader's non-uniform
image indexing is explicitly decorated; it does not import a register file.

The initial flat diagnostic, log 761 (PID 17888, 14:12:44-14:14:58.142),
had 5807 matching bloom checks but zero flare checks because the optical
lookup refused visible frames. It does not qualify the active flare path.
Provisional normal flat log 762 (PID 25336, 14:15:55-14:18:21.622) identified
241 such refusals. Its 120 frames in `lens_flare_provisional_flat`, frames
2836-2955, have 0/119 changes over 6% (max 3.28%) and no cyan patches; they
are not the final corrected-build evidence.

After correcting lookup, diagnostic log 763 (PID 20672,
14:18:53-14:20:48.785) recorded 3615 matching authored sprite comparisons and
5142 matching bloom comparisons, zero mismatches and zero input refusals.
The diagnostic compares original sprite rectangles and four color lanes,
checks exact sprite counts, and executes the original scope deliberately.
All six settings audited and the full 1673 archives / 119346 names mounted.
Capture delay 600 prevented unnecessary raw captures during diagnostics.

Final source/comment build: 14:21:02, 47441920-byte `reblue_vk.exe`, embedded
base `14fd0b3bb` with local modifications. Normal image verification follows.

## Corrected normal flat verification

Log 764, PID 24180, 14:21:35-14:23:38.662, final 14:21:02 binary. Original
five-setting profile audited: autoplay/perf on, capture delay 60, minimum
600, 120 frames. Full install mounted. Native post/DoF on; diagnostics,
preview, native sun and VR off.

120 exact hard links in `out/verification/native_lens_flare_flat`:
`frame_1788632557_0.raw` through `frame_1788632561_119.raw`, frames 2832-2951.
1920x1080, 8294420 bytes each; 0/119 changes over 6% (max 3.22%), no cyan
patches or whole-frame cyan, median 0.011%, max 0.02%. Full-resolution
first/last images inspected: Shu and cast silhouette, foliage, rocks, distant
DoF and moving windmill geometry/shadows remain readable. The capture window
has no visible lens source; earlier native sprite execution is counted, not
misrepresented as visible-flare image qualification.

Last counters: 5441 native schedules, 860 original (858 unsupported effect
calls, two image/preflight refusals), zero old tail-effect/state-308 calls;
241 authored visible flare frames / 3615 native sprites, 5200 inactive flare
frames, zero flare-input refusals. The exact two non-flare preflight failures
remain untraced. No checked error/critical/device-loss markers appeared.

## Visible preview failure and correction in progress

The first synthetic VR preview (log 766, PID 27184, 14:26:29 to stop requested
14:32:42 and confirmed stopped shortly afterward) captured only its bounded
32 frames: 8196-8227, 14:27:32.038-14:27:40.922. The 1440x3168 files are
`frame_1788632852_0.raw` through `frame_1788632860_31.raw`, isolated with hard
links in `out/verification/native_lens_flare_preview_vr`. Its 0/31 large jumps
(max 0.19%) and no cyan patches did NOT qualify the effect. Inspection of
first/last left/right PNGs found hard-edged rectangular tinted regions.
The original five-setting profile was restored when the preview stopped.

A bounded no-capture flat diagnostic (log 767, PID 11460,
14:38:07-14:39:02) confirmed native images, no source aliases:
0: `5616e2ea6cac8f0f` 512x512; 1: `71e9c7c72f73e714` 256x256;
2: `ce75badf286f01a5` 128x128; 3: `1917c9a72006422e` 512x512.
All four existing `.bdtex` bases decoded and inspected without recooking.
They hold QUARTER glows/rings. The earlier assumption that the ten-vertex
original fan had a globally linear UV mapping was wrong. Its center is (0,1),
horizontal edge midpoints (1,1), vertical edge midpoints (0,0), corners (1,0).
The exact `sub_8221E298` stores establish this folded mapping.

The native shader now folds each interpolated quad coordinate with
U = abs(2*x-1), V = 1-abs(2*y-1), retaining one instanced draw and the existing
assets. Shared C++/HLSL helpers test all ten original fan vertices and
interior mirrored pairs. This is a sampling correction, not a texture binding
or asset decoding failure. Corrected GPU verification follows below.

Storage preflight for this correction: 57,478,524,928 bytes available
(53.53 GiB). Reserve 1 GiB for incremental build/link scratch and 3.6 GiB
for corrected flat/VR/preview captures; projected remaining reserve >48 GiB.
Earlier short flat/VR becomes the before-fix baseline; the superseded
provisional flat is historical, not active qualification. The failed preview
remains regression evidence until corrected pixels are inspected. Only small
optical PNGs are exported; raw isolation uses hard links, not payload copies.

## Corrected folded preview

The corrected binary linked at 14:41:22, 47,444,992 bytes, embedded base
`202568c4b` plus local modifications. Only host sources/shaders rebuilt;
codegen reported up to date. All 26 CTests and now 22 source guards pass.
The emitted pixel SPIR-V contains both absolute-value folds, not just the
correct-looking source. The shared header is an explicit shader build dependency.

Log 768, PID 27592, exact process start 14:42:13.640, stopped 14:44:31.386.
The 17-setting audit, full install mount, absolute local simulator manifest,
1440x1584 native scene/final-eye output, XR scale 1.0, height zero and diorama
mode 2 were verified. Native sun/shadows and layered multiview were on;
side-by-side stereo, scene-array capture, XR mirror and comparisons were off.
Capture delay 60, minimum 450, count 32; synthetic flare preview on.

`out/verification/native_lens_flare_folded_preview_vr` contains 32 hard links:
`frame_1788633795_0.raw` through `frame_1788633797_31.raw`, frames 8380-8411,
ending 14:43:17.674. All are 1440x3168, 18,247,700 bytes each.
0/31 changes over 6% (max 0.03%), no cyan patches/whole-frame hits,
median cyan 0.007%, maximum 0.01%. First/last left/right images were inspected
at full resolution: smooth centered glows and closed mirrored optical rings
replace the hard-edged quarter rectangles. The background remains visible.
This proves visible native GPU sampling in both eyes, not authored light
visibility, per-eye optics or full-game qualification.

Last sampled counters: 6216 native schedules/visible flare frames, 93240
sprites, zero flare-input refusals or tail-effect/state-308 calls. The 5785
original scopes comprise 5783 packed-effect refusals and two non-flare input
refusals. Earlier failed preview log 766 ran longer while work was interrupted:
its final counters reached 28992 native schedules and 96 non-flare input
refusals; its short capture does not qualify that later uncaptured interval.

The pre-fold normal VR control (log 765, PID 25756, 14:23:44-14:25:34.297)
has 120 frames 8145-8264, `frame_1788632687_0.raw` through
`frame_1788632695_119.raw` in `native_lens_flare_vr`. It has 0/119 large jumps
(max 0.53%), no cyan, and first/last crossed stereo depth: far -1, near -9,
spread 8 pixels. All four eye images were inspected. Its flare was inactive
throughout; it never established visible flare correctness. Last native count
3604, original 5697 (5695 packed effects/two inputs), zero tail/state calls.
The final folded-build normal checks below supersede these controls.

## Final folded-build normal verification

Flat log 769, PID 17960, 14:44:37.021-14:46:41.454. Original five settings
audited, full install mounted, native flare preview/comparisons/VR/native sun
off. The same final 14:41:22 binary captured 120 1920x1080 frames, 2834-2953:
`frame_1788633939_0.raw` through `frame_1788633942_119.raw`, ending
14:45:42.768, isolated as hard links in `native_lens_flare_folded_flat`.
0/119 changes over 6% (max 3.03%), no cyan patches/whole-frame hits, median
cyan 0.011%, max 0.02%. First/last full-size PNGs inspected: Shu, his cast
silhouette, moving windmill/shadows, foliage, ground and distant scenery remain.
This field window has no active flare. The intro executed 241 authored visible
flare frames/3615 sprites; those are counts, not an active-flare image check.
Last counters: 5438 native schedules, 863 original (861 packed effects/two
input refusals), 5197 inactive flare frames, no flare-input refusal and no
tail-effect/state-308 calls. This result does not supersede late-scene failures.

Normal VR log 770, PID 21540, 14:46:47.492-14:48:40.572. Same final binary;
all 16 settings audited, full install mounted. The preview's VR configuration
was retained except preview off and count 120; comparisons off. Final scene
and eye layers are 1440x1584, capture 1440x3168. The 120 hard links in
`native_lens_flare_folded_vr` span frames 8438-8557:
`frame_1788634069_0.raw` through `frame_1788634078_119.raw`.
0/119 changes over 6% (max 0.53%), no cyan hits at all. First/last stereo
bands 44/52/62/72/82/90/95% give -1/-2/-3/-5/-6/-8/-9 pixels: correctly
crossed depth, far -1, near -9, spread 8. First/last left/right full-resolution
PNGs inspected: native full-height scene, readable foreground stairs/ground,
distant scenery and changing windmill geometry/shadows. Distant blur remains;
this framing does not qualify Shu's cast shadow. Flare inactive throughout.
Last counters: 4660 native schedules/inactive flare frames, 5841 original
scopes (5839 packed-effect refusals/two input refusals), zero flare refusals,
tail-effect calls or state-308 calls in this supported post scope.

The original five-setting profile is restored, no `reblue_vk` process remains,
and logs 767-770 contain none of the checked error/critical/device-loss/
exception/assertion markers. These log checks do not replace pixel inspection.
No Quest run, GPU timing claim or native visibility/comfort qualification.

## Retention and remaining ownership

After all corrected captures and PNGs, actual available volume space is
53,654,601,728 bytes (49.97 GiB), above the projected reserve. No file was
deleted and no game data, source asset, save or active build tree was removed.
The final flat/VR sequences and folded preview remain current evidence.
The pre-fold flat/VR sequences remain the before-change baseline until the
next qualified post checkpoint; the failed preview stays regression evidence.
These six raw sets total about 7.02 GiB, counted once per unique payload,
not once per hard link. Provisional flat log 762 is superseded historical
evidence eligible for lossless compression; earlier post checkpoints are
historical rather than additional active baselines. Existing unresolved
late-scene evidence must remain until that separate regression is fixed.

Next ownership work remains substantial: native light/visibility production,
native image/scene associations (instead of engine handle lookup), per-eye
optical-source derivation and aspect-aware sizing, packed/dual-mask/other
filter combinations, post/output/UI getters, and the broader scene/material/
animation/frame producers. Supported post conversion is not complete frame
ownership. Later scenery/text failures and representative field/battle/
cutscene/menu/transition/reload coverage remain open before Quest optimization.
