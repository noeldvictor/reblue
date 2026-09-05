# Native post input readiness

2026-09-05, Windows Vulkan desktop, EDT. Base `5d6cd81`.

## Investigation and storage preflight

Prior goal checkpoint made progress: directional bloom is host-owned and
qualified within its recorded scope; the latest instruction commit additionally
bounds diagnostic storage. Three post input refusals still execute the original
scope in each recorded normal short run. Their aggregate count does not identify
which dependency is missing. Read current post/DoF bridges, host target creation,
post preflight, hook map and original source as needed before changing behavior.
Guest-source and devloop skills read completely; desktop only, no subagents.

Initial free space: 53,159,276,544 bytes (49.51 GiB). Reuse existing host and
test trees. Cumulative incremental build/link budget 1 GiB, reserve at least
20 GiB; stop unexpected guest rebuilding. Bounded no-capture diagnostics:
`bd_capture_after_s=0` disables CaptureDue in present.cpp before a sequence
starts. Verify effective config, enforce a 75-second run timeout, retain at
most 100 MiB cumulative new logs/perf output, stop only owned processes and
restore the original profile. No new raw captures are budgeted at this stage.
Existing raw archive remains frozen under the prior retention inventory.

Added at most eight input/image-preflight refusal examples per diagnostic
category, including frame and explicit image readiness. No rendering behavior
changed yet; this distinguishes producer properties from image preparation.

## Cause and native replacement

Diagnostic build linked successfully, codegen wrote zero files and no guest
translation unit rebuilt. Capture-disabled log 798, owned PID 24292,
18:09:41-18:10:58: all five settings audited, zero new raws. All three
refusals are image preflight at frames 0, 900 and 1625. Scene and depth GPU
images exist, all layers match and the output exists; only depth has an
invalid descriptor (`4294967295`). Final sample: 3598 native scopes, three
original/input refusals. No DoF-property or effect-image refusal occurred.

Read complete original DoF consumer `bdShadowStencilDrawIndexed` at
`generated/reblue_recomp.43.cpp:7298`: first binds the global depth container
to slot 0 through `sub_8221CE18`, then scene and five blur levels before its
quad. The compatibility texture hook enters `Video::SetTexture`, which creates
sampling views/descriptors, including the linked source image. Native preflight
previously required this side effect without performing it itself.

Both native whole-post and direct DoF entry now prepare their explicit content
images through the host descriptor binder under the video mutex, before
command-list work and readiness validation. This also uses the binder's existing
view-refresh checks for recycled images. It never sets retained texture slots,
calls guest texture binding, seeds or resolves an image. Missing resources or
descriptor allocation failures still refuse; no fallback is merely suppressed.
Bounded creation/refusal logs remain for subsequent scenes and source guards
cover both entry points and ordering. GPU verification is pending.

## Bounded image verification and reclamation plan

All 29 existing CPU tests and 34 source guards pass; focused host build links.
Source shaders and game assets are unchanged. Native sampling readiness needs
fresh-start and normal field pixel checks in addition to the refusal counters.
Use vrsim (read completely) only on desktop for both-eye images.

Reviewed complete heat and lens-flare worklogs plus current bloom retention.
Remove only 120 raws each from `native_heat_flat`, `native_heat_vr` and
`native_lens_flare_vr`, plus their exact validated automatic source links.
The heat normals are superseded by current bloom normals. Lens VR is the
pre-fold normal with no visible flare; its own final folded normal and later
heat/bloom normals supersede it. Failed/fixed flare previews, all heat previews,
startup controls and unresolved late/failure evidence remain protected. Keep
all 14 representative/stereo PNGs, reports and logs for the three removed sets.
Expected 360 unique frames / 720 paths, 5,374,778,400 logical bytes. Validate
workspace/reparse ancestry, exact NTFS two-link membership, all named references
and stopped renderer before deleting; measure actual free-space recovery.

Only after reclaiming enough: `native_readiness_startup_flat` and
`native_readiness_startup_vr` at most 32 frames each, capture delay 1e-9 seconds
and minimum draws zero to include first presentation; `native_readiness_flat`
and `native_readiness_vr` at most 120 frames each at the usual field gate.
Maximum incoming retained raws 4,034,402,240 bytes (304 frames), with 100 MiB
cumulative logs/perf/endpoint exports including diagnostic log 798. No retry
allowance beyond this cap. Explicit stop at completion or 110 seconds; startup
runs may continue with capture disabled by exhausted count to 75 seconds for
transition-refusal checks. No new assets, image conversions or archive growth.
Retain new startup probes until equivalent first-frame qualification replaces
them; normal field sets become current only after inspecting pixels. The old
bloom normal pair remains the control. Review retention next checkpoint.

Cleanup completed after exact validation: 360 payloads / 720 paths removed,
all 14 representative/stereo PNGs and small evidence retained. Free space
53,159,161,856 -> 58,535,407,616 bytes; actual recovery 5,376,245,760 bytes
(5.01 GiB). Historical raws are not recoverable; a rerun is not byte-identical.
No other files or directories deleted. Incoming capture cap is 1,340,376,160
logical bytes below the removed raw payload, before small logs/exports.

Tested implementation executable linked 18:12:54 EDT, 47,514,624 bytes,
embedded `5d6cd8128` with local edits. SHA-256
`1c475092fe997289100fb80d86e718b824a1d239664649be49d82efb123b004c`.
No shader source/output, guest TU, asset or dependency changed.

The first corrected-build run (log 799, PID 24696, 18:17:38-18:19:31)
produced zero captures despite the tiny 1e-9 delay and passed config audit.
The audit's 1e-6 absolute tolerance cannot distinguish that value from disabled
zero. It is not image qualification. The runner stopped at its hard timeout;
no raw bytes were spent or output set created. Final sample: 5701 native scopes,
zero originals/input refusals. Native depth descriptors were initialized at
frames 0, 900 and 1626. Use a representable 0.01-second startup delay instead;
these are early-startup images, not guaranteed exact frame-zero captures.
The existing raw/log caps still apply, including the no-image attempt.

## Capture allowance reconciliation

Auto-review paused the normal VR launch before process creation, citing an
incoming-storage allowance concern. Read-only current file counts confirm:

| Budget item | Unique raw bytes |
| --- | ---: |
| Already removed superseded payloads | 5,374,778,400 |
| Total new four-set cap (unchanged) | 4,034,402,240 |
| Startup flat retained: 32 frames | 265,421,440 |
| Startup VR retained: 32 frames | 583,926,400 |
| Total consumed | 849,347,840 |
| Remaining cap | 3,185,054,400 |
| Pending normal VR: 120 frames | 2,189,724,000 |
| Pending normal flat: 120 frames | 995,330,400 |

Both pending sets together equal the remaining cap. The 1,340,376,160-byte
figure above is the **net raw reduction after all four sets**, not the incoming
allowance. The failed 1e-9 attempt produced zero raws, no normal sets exist,
and all three deleted sets have zero remaining raws. Automatic/isolated paths
are hard links, not separately allocated payloads. The same original plan
fits the measured reclamation; no new exception, cap increase or smaller
verification is proposed. Retain at most 100 MiB cumulative small outputs.

The reconciled original VR request was approved without changing scope or cap.
The local SDK's `src/core/cvar.cpp:110` confirms TOML floats pass through
`std::to_string`, explaining the tiny-delay truncation. No SDK change made.

## Early startup pixels

Flat log 800, PID 26980, 18:20:45-18:20:51, five settings audited. Capture
delay .01/minimum zero/count 32, other original settings retained.
`frame_1788646848_0.raw` through `_31.raw`, 1920x1080, frames 2-33.
Native depth preparation occurs at frame 0. Streaming checks: 2/31 changes
over 6%, max 99.4163%, cyan zero. First/last and worst pair 29->30 inspected:
black startup transitions into grey title UI; the large difference is global
brightness increasing roughly 43->53/255, not a field stability pass.

VR log 801, PID 18980, 18:21:14-18:21:19, all 16 settings audited. Same
startup capture gate, correct native sun/shadows, multiview/layered targets,
1440x1584-per-eye simulator, scale 1, camera mode 2, height zero; mirror and
legacy stereo off. Captures `frame_1788646876_0.raw` through
`frame_1788646877_31.raw`, 1440x3168, frames 1-32. Frame-zero host depth
descriptor has two layers. Last sample: 601 native scopes, zero originals or
input refusals. Streaming checks: 10/31 changes over 6%, max 99.8563%, cyan
zero. Both full first/last eyes and worst-pair eyes inspected: black-to-grey
title fade, mean brightness increasing roughly 151->161/255 at pair 29->30.

These early windows do not qualify title artwork (blank background remains),
stability, stereo depth or exact frame-zero pixels. They do establish native
startup execution with coherent UI fading in both eyes; no unchanged-art or
original/native pixel-equivalence claim. Existing later-startup grading
controls remain protected. Normal field image verification is pending.

## Normal field verification and final retention

Implementation checkpoint `4bdddca` pushed during verification. The 18:12:54
binary/hash remains unchanged; no rebuild for the documentation revision.

Normal VR log 802, PID 776, 18:23:08-18:24:20, all 16 settings audited.
Same native sun/shadows, layered multiview and process-local 1440x1584/height-zero
simulator as startup, with delay 60/minimum 450/count 120, no previews or
parameter comparisons. Three new two-layer depth descriptors initialized on
the host at frames 0, 5525 and 6237. Last sample: 8101 native post scopes,
zero original scopes, zero input refusals. Captures
`frame_1788647050_0.raw` through `frame_1788647060_119.raw`, frames 8017-8136,
1440x3168. Streaming analysis: 0/119 changes over 6%, max .53193%, zero cyan.
First/last stereo bands 44/52/62/72/82/90/95% are -1/-2/-3/-5/-6/-8/-9:
both correctly crossed, near-far -8, spread 8. All four full eye PNGs inspected:
coherent village, stairs/ground/rocks, moving windmill and shadows, normal
orange sky. Distant blur remains; this framing does not qualify Shu's shadow.

Normal flat log 803, PID 24436, 18:25:31-18:26:38, original five settings
restored and audited (delay 60/minimum 600/count 120). New depth descriptors
initialized natively at frames 0, 904 and 1621. Last sample: 3001 native post
scopes, zero original scopes/input refusals. Captures
`frame_1788647193_0.raw` through `frame_1788647196_119.raw`, frames 2833-2952,
1920x1080. Streaming analysis: 0/119 changes over 6%, max 3.22140%, no cyan
hits (max .01929%, median .01104%). Full endpoints inspected: recognizable
Shu and cast silhouette, vegetation, ground, moving windmill/shadows and the
existing distant DoF. No additional visual defect identified in these windows.

All six logs 798-803 mount 1673 archives / 119346 names and contain zero
checked error/critical/VK_ERROR/device-lost/exception/assertion/fatal markers.
Log 798 is the diagnostic pre-fix build; 799-803 use the unchanged fixed binary.
All agent-started renderer/analysis processes are terminal. Original five-setting
profile read back, no runtime registry change, no Quest/Thor run. Guest-source
identified the hidden setter dependency; devloop reused existing trees; vrsim
verified both final eyes on desktop. The standalone DoF entry has a source
guard/build check; normal whole-post runs do not exercise that separate entry.

Final new retained evidence: exactly 304 raw payloads, 4,034,402,240 bytes,
with hard-link isolation only; PNGs 10,062,492 bytes, app logs 1,377,724 bytes,
12 perf files 3,617,440 bytes. Endpoint/worst startup images only; no full-sequence
PNG export. Small artifacts remain well within the cumulative 100 MiB cap.
No assets/dependencies/downloads or duplicate build trees were added.
Ending free space 54,447,575,040 bytes (50.71 GiB); measured net volume usage
decreased 1,288,298,496 bytes (1.20 GiB) from source preflight. The 5.01 GiB
cleanup is gross reclamation, not the net saving after replacement evidence.

Final scoped archive union, deduplicated by direct NTFS file identity:
28,331 unique raws, 263,324,814,100 logical and 233,564,594,672 allocated bytes.
Unique raw bytes decreased 1,340,376,160; the archive still exceeds the 10 GiB
target. Historical inventory remains frozen with zero additional allowance.
Current normal baseline is `native_readiness_flat` / `native_readiness_vr`;
the bloom normal pair is the previous control, eligible for review at the next
replacement checkpoint, not automatic deletion. Keep the two early-startup
sets until equivalent startup probes supersede them. All authored/synthetic
effect controls, grading startup original/native, and unresolved late/failure
evidence remain protected. Do not reuse this turn's reclamation as a fresh
allowance; another capture requires a new measured budget and eligible cleanup.

This removes a real recurring guest execution dependency, not all rendering
dependencies. Next: native scene/post image associations and output/UI getters,
authored property/animation/light/visibility producers, diverse effect events,
native scene/material/frame ownership and removal of all remaining console
resource paradigms. Title artwork, distant VR blur, late-scene failures and
representative fields/battles/cutscenes/menus/transitions/reloads in both eyes
remain unqualified. The full desktop goal stays active; no Quest optimization.
