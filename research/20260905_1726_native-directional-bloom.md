# Native directional bloom

2026-09-05, Windows Vulkan desktop, EDT. Base `434f13f`; implementation
`e636761`. Effect checkpoint, not full renderer or authored-event qualification.

## Source and storage preflight

Guest-source/devloop/vrsim and current AGENTS read. Outer `sub_8221B1D8`
read completely: mode 1 bright-passes the completed DoF scene into two
quarter-size images, then repeatedly blurs the first horizontally and the
second vertically. The masks are independent, not a separable H->V chain.
Both contribute RGB weight 1, alpha 0, alongside scene weight 4. Heat follows
mask preparation and must not distort either mask's sampling coordinates.

`sub_82214F40` and `sub_82215050` map sigma/gain/axis and image dimensions
to the original kernel. Complete PPC comments/control flow of `sub_822150F0`
and complete Gaussian helper `sub_82214980` inspected. Axes 0/1 use a
normalized center plus six symmetric Gaussian pairs. Initializer
`sub_82214E78` identifies ms_weight/ms_bright shaders; owned XEX constants
decoded in memory confirm 1, 0, 2, 2*pi, negative zero and sample count 13.
No extracted image or decompiler. `sub_82216740` publishes input count as
float c26.x; ms_tex uses weights from c13 onward. Buffered sigma/gain/count
payloads are owner+12612/+12624/+12636, plus 4*bank; outer dispatch and the
property serialization reader independently establish the descriptors.

Native plan: quarter-resolution paired-mask atlases, ping-ponged between
at most two private images. Prepare brightness from native DoF before heat;
each iteration renders the two independent directions in one multiview
render pass. Composite samples their separate rectangles without bleeding.
No guest mask cache, texture setter, UP quad, EDRAM or emulated resolve.
Zero iterations shares the one bright region; zero sigma is a native impulse.
Existing single-mask folded bloom and native DoF approximations remain.

Free preflight: 53,106,892,800 bytes (49.46 GiB). Reuse configured desktop
and CPU-test trees. Incremental build/test/link budget 1 GiB, same cumulative
allowance across retries, stop unexpected guest rebuilding or reserve below
20 GiB. No asset copies/downloads/build backups. Capture budget is separate:
no new captures until eligible superseded raws are reviewed and reclaimed
to cover all incoming retained bytes under the current no-growth gate.

## Integration and pre-capture checks

Resumed at documentation base `3979ddc`. Native mode 1 now imports the
buffered sigma, gain and signed iteration count (nonpositive means no blur),
instead of refusing the whole scope. A native-only preview selects two
iterations or the shared unblurred image; it cannot run as an authored
parameter comparison. Existing weighted-composite diagnostics now sum both
mask weights for three inputs and ignore stale mask weights for one input.
Kernel/publication and authored mode-1 event qualification remain unproven.

Original DoF shader export is not saturated. Removed an unnecessary clamp
before native bright preparation; its own bright-pass clamp is retained.
The quarter-size evaluation remains a native approximation, not an identical
legacy full-size DoF resolve. Native filtering uses 13 texel loads per pixel,
explicit half-local clamps and the current view layer. It never chains the
horizontal result into the vertical mask. At most two private quarter-pair
atlases are reused, one render pass per iteration and a 32-byte kernel.

The initial restricted CPU build was live but idle, with zero compiler
children and zero Ninja CPU. Only its identified processes were stopped;
the permission-approved retry compiled and linked both tests successfully.
All 29 CTests and 33 source guards pass. New CPU coverage compares normalized
weights with an independent 13-sample prefactor Gaussian, positive/negative/
zero sigma and gain, and 45 atlas cases across tiny/odd dimensions, zero
through four iterations, poisoned unwritten regions and two distinct eyes.
These source/CPU checks do not prove GPU pixels.

Existing Vulkan/OpenXR/PCH host target linked at 17:40:41 EDT, 47,506,432-byte
executable, embedded base `3979ddc` with local changes. Both native shaders
compiled to SPIR-V; codegen wrote zero files, no guest translation unit
rebuilt. Reused test/build trees and kept within the 1 GiB cumulative budget.

## Bounded capture and cleanup plan

Fresh scoped inventory of both capture roots: 28,441 unique NTFS raw files,
264,784,630,700 logical and 235,024,411,272 allocated bytes. The historical
archive remains frozen under `20260905_1655_raw-retention-inventory.md`;
its mixed unresolved failure/control evidence is not cleared by this change.

Reviewed the complete grading, lens-flare and heat worklogs. Current heat
normal flat/VR supersedes grading normal flat/VR and the pre-fold normal
lens-flare flat control. The lens-flat capture never qualified active flare;
all failed/fixed flare previews remain protected. Delete only 120 raw frames
each from `native_grade_flat`, `native_grade_vr`, `native_lens_flare_flat`
and their validated source hard links: 360 payloads, 4,180,384,800 logical
bytes. Their first/last PNGs, reports and app logs remain. Exact historical
raw frames are not recoverable; reruns reproduce tests, not identical bytes.
Validate exact targets, reparse ancestry, two-link membership, other named
references and stopped producers; measure actual recovered volume bytes.

New qualification exception: `native_bloom_flat` / `native_bloom_vr`, at most
120 frames each; `native_bloom_preview_flat` / `native_bloom_preview_vr`,
at most 32 each. Additional zero-iteration branch probes
`native_bloom_shared_flat` / `native_bloom_shared_vr` are one frame each,
not sequence-stability qualifications. Total retained raw cap 4,060,944,360
bytes (306 frames), plus 100 MiB cumulative logs and representative exports.
This fits the reviewed reclamation; launch only after it is measured.
Retries share the cap, never add a new allowance. Stop runs at capture
completion or 110 seconds, whichever comes first; restore the owner profile.
Keep normal runs as current evidence only after inspected; retain previews
until authored directional events or replacement probes supersede them.
Review at the next checkpoint. Do not reduce full-game verification scope.

Cleanup completed: 360 frames / 720 paths removed; all ten representative
PNGs and reports/logs retained. Exact two-link membership and all same-name
references checked, no reparse traversal or running renderer. Free space
53,108,695,040 -> 57,290,547,200 bytes: 4,181,852,160 bytes (3.89 GiB)
recovered. The first ancestry check stalled before deletion because direct
DirectoryInfo parents lack PowerShell's PSIsContainer extension. Its exact
process was stopped; corrected type-based ancestry with a hard depth bound
validated successfully. No other files/directories removed. The capture cap
and 100 MiB exports leave over 49 GiB reserve; no archive growth is funded.

## First GPU evidence

All successful runs below use the 17:40:41 build. The complete directional
SPIR-V function was inspected: kernel offsets 0/16, ViewIndex in every image
fetch, 13 taps, independent direction vector and rectangle-local integer
clamps. Composite SPIR-V retains its 224-byte layout and separate paired/
shared selection. These checks supplement, not replace, image inspection.

Flat preview log 791, PID 21896, 17:47:52.653-17:48:57.797. All six settings
audited; full 1673 archives / 119346 names mounted. Original flat profile
except 32 frames and `bd_native_bloom_preview=1` (sigma 3, gain 1, two
iterations, threshold .04, intensity 8). Captures
`frame_1788644935_0.raw` through `frame_1788644936_31.raw`, 1920x1080.
Streaming analysis: 0/31 changes over 6%, max 5.0584%, no cyan hits,
max cyan .00550%. First/last full images inspected: intentionally strong
bright halos/clipping, recognizable Shu, vegetation and moving windmill/
cast shadows. Not authored intensity, activation or art-style qualification.
Last sample: 2,698 native directional scopes / 5,396 iterations, three
original/input refusals, zero authored parameter checks.

VR attempt log 792, PID 22756, started 17:49:20.090; config audit rejected
six misspelled setting names. Stopped before capture (zero raws), no GPU
qualification claimed. Names then verified directly in settings sources;
the bounded runner now stops automatically on rejected config entries.

Corrected VR preview log 793, PID 23380, 17:50:35.854-17:51:41.541.
All 17 settings audited and full install mounted: autoplay/perf true,
delay 60/minimum 450/count 32, preview 1, bd_native_sun_camera and
bd_native_shadow_passes true, bd_vr_enabled true, bd_stereo false,
bd_stereo_multiview and bd_mv_layered_textures true, bd_mv_capture_array
and bd_xr_mirror false, bd_vr_camera_mode 2, bd_vr_diorama_height 0,
bd_xr_render_scale 1. Existing absolute xrsim manifest; process-local
1440x1584 and eye height 0, no device/global runtime change.
Captures `frame_1788645098_0.raw` through `frame_1788645100_31.raw`,
1440x3168. Streaming analysis: 0/31 changes over 6%, max .78333%, no cyan
hits, maximum .04294%. Both full first/last eyes inspected: coherent strong
bloom with foreground and sky clipping; scenery and windmill remain.
The stereo checker finds only -1/-2/-2 at 44/52/62%: both results are
INCONCLUSIVE, spread 1, because too little usable near texture remains.
Do not label these normal-depth or comfort passes. Last sample: 7,498 native
directional scopes / 14,996 iterations, three original/input refusals.

Normal flat/VR regression and zero-iteration probes are still pending at
this implementation checkpoint. The original authored mode-1 kernel
publication/events, combined active heat+bloom, image/property/UI adapters,
scene/animation/material/frame ownership and full representative desktop
game gates remain. No Quest qualification; the full objective stays active.

## Normal regression verification

Implementation checkpoint `e636761` pushed during verification. No renderer
source changed or binary rebuilt after the 17:40 build. Normal runs use no
synthetic overrides, and neither activates authored directional bloom.

Normal VR log 794, PID 25856, 17:52:03.255-17:53:16.518. All 16 settings
audited (corrected preview VR settings minus preview, count 120); full
install mounted, same process-local simulator. Final-eye captures
`frame_1788645185_0.raw` through `frame_1788645194_119.raw`, 1440x3168.
Streaming analysis: 0/119 changes over 6%, maximum .40115%, zero cyan.
Both first/last stereo checks pass: bands 44/52/62/72/82/90/95% have
-1/-2/-3/-5/-6/-8/-9 pixels, near-far -8, spread 8, correctly crossed.
All four full-size eye images inspected: normal orange sky and ground,
stairs/village/rocks, changing windmill geometry and shadows. Existing
distant blur remains; Shu's shadow is not qualified in this framing.
Last sample: 8,098 native scopes, three original/input refusals, directional
and heat inactive. This does not qualify a full game session.

Normal flat log 795, PID 19296, 17:54:12.163-17:55:19.301. Original five
settings audited, full install mounted. Captures
`frame_1788645314_0.raw` through `frame_1788645317_119.raw`, 1920x1080.
0/119 changes over 6%, maximum 3.06525%, no cyan hits, max cyan .02083%.
Full first/last images inspected: normal colours and recognizable Shu,
cast silhouette, vegetation/ground, moving windmill and shadows; distant
DoF remains. Last sample: 2,998 native scopes, three original/input
refusals, 860 grade/scanline scopes, 241 visible flare frames / 3,615 sprites,
zero directional or heat activation. Normal captures retain the existing
appearance; previews above are intentionally stronger and separate.

## Zero-iteration branch probes and final handoff

Flat log 796, PID 26864, 17:55:41.267-17:56:45.122. All six settings
audited; original flat profile except one frame and preview 2. The one
1920x1080 frame `frame_1788645403_0.raw` (frame 2837) was inspected at full
size: stronger unblurred highlights over coherent Shu/scenery/shadows,
without the broader directional halos. Cyan .00121%, below the hit threshold.
Last sample: 2,698 directional scopes, zero iterations, three input refusals.

VR log 797, PID 25036, 17:57:31.070-17:58:35.091. All 17 settings audited;
same corrected VR configuration with preview 2 and one frame. Both eyes of
`frame_1788645513_0.raw` (1440x3168) inspected: coherent unblurred bright
image, strongly clipped foreground/sky. Cyan .06403%, below threshold.
The stereo check is again INCONCLUSIVE (-1/-2/-2, spread 1); no normal-depth
or sequence-stability claim for either single-frame probe. Last sample:
7,798 directional scopes, zero iterations, three original/input refusals.

All seven logs 791-797 mounted 1673 archives / 119346 names and contain no
checked error/critical/VK_ERROR/device-loss/exception/assertion/fatal markers.
The rejected-config attempt 792 remains explicitly unqualified and created
zero raw files. There was no raw retry or extra capture beyond the 306-frame
cap. Every renderer/analysis process is terminal; the original five-setting
profile was restored and read back. No Quest/Thor run or headset timing claim.

Executable remains 47,506,432 bytes, 17:40:41 EDT, Clang 22.1.8, embedded
`3979ddc78` with local modifications; SHA-256
`4c2b4328fccef0cc279a7daf9e4abcdbc27fdf4d992c18ff7b68ff8d914f8b0b`.
No rebuild or recapture for documentation stamps. Guest-source established
the independent mask schedule and parameters; devloop reused the configured
host/test trees; vrsim kept final-eye verification entirely on desktop.

New retained evidence: 306 unique raw frames, 4,060,944,360 bytes;
19,954,835 bytes of representative PNGs; 2,491,527 bytes of app logs;
7,906,064 bytes across 14 perf files. Streaming sequence/cyan analysis kept
only adjacent frames in memory and exported endpoints, not every frame.
These outputs fit the raw allowance and 100 MiB logs/exports budget.
No asset copies, dependency downloads or duplicate build trees were made.

Ending free space: 53,171,654,656 bytes (49.52 GiB). Net volume usage
decreased 64,761,856 bytes (61.76 MiB) from the original source preflight.
The 3.89 GiB deletion mostly funded new verification; it is not the net
saving. Final archive union: 28,387 unique raw files, 264,665,190,260 logical
and 234,904,970,832 allocated bytes, 119,440,440 fewer raw bytes than before.
The archive still substantially exceeds 10 GiB; the no-growth gate and
frozen historical review obligation remain. Current normal bloom flat/VR
supersedes the heat normal pair only as the short baseline. The heat pair
is eligible for review at the next replacement checkpoint, not an automatic
deletion. Retain synthetic bloom/shared probes until authored events or
replacement probes supersede them; preserve all unresolved/control evidence.

The authored mode-1 activation/kernel-publication gate, combined active
heat/bloom and diverse effect events remain unqualified. Other modes retain
the existing native folded-kernel approximation. Next source work should
trace the three exact input refusals and replace remaining image/property/
post-UI adapters; native animation/scene/material/frame ownership, removal
of all console resource paradigms, and fields/battles/cutscenes/menus/
transitions/reloads in both eyes remain necessary before Quest optimization.
