# Native directional bloom

2026-09-05, Windows Vulkan desktop, EDT. Base `434f13f`. In progress.

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
