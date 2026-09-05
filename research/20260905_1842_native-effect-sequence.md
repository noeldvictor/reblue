# Native effect-sequence scheduling

2026-09-05, Windows Vulkan desktop, EDT. Base `60293d6`.

## Scope and source evidence

Previous checkpoint made progress: storage-accounting rules committed/pushed.
Guest-source and devloop skills read completely. No subagents or device work.

Scene/post tracing found two different container layouts, not an offset typo:
`bdResolveToTexture` (`generated/reblue_recomp.85.cpp:4033`) reads +4.
`bdRenderViewSubmit` (.16.cpp:7311) passes that texture to `sub_8221C9A0`;
its complete constructor (.57.cpp:7679) stores it at +0 in a temporary post
container and builds surface-level wrappers. The scheduler then passes colour
and depth containers to `bdEffectSlotArrayApply` (.23.cpp:4126).

Read that complete wrapper: when enabled (+12), it copies the passed depth
container into its global +16 field, iterates count +8 / array +0, dispatches
each object's first virtual callback with the global render phase, and clears
the global depth container. Native post currently re-reads that global.
`sub_8221CE78` (.70.cpp:7443), read completely, resolves/unbinds old post
containers; it is not a native image producer.

Implement native ordered effect scheduling for recognized whole-post callbacks,
passing the input depth directly and removing the wrapper's global depth copy,
virtual dispatch and cleanup execution. Preserve order and bounded ping-pong
outputs for multiple roots. Preflight the complete callback list and authored
plans before GPU writes; never replay a partially executed list on refusal.
DoF's property publication can change the shared focus used by the next root,
so refresh each later DoF input after its predecessor's publication.

Scene lifecycle -> temporary container creation, resolve links/exposure, engine
list/property producers, other callback types and output/UI publication remain
explicit boundaries. This is not complete native scene/frame ownership.

## Cumulative storage ledger / initial gate

Starting actual volume free: 54,448,750,592 bytes (50.71 GiB). Existing raw
archive is frozen over budget; no prior cleanup allowance is credited here.
Planned peak additional build/link/test space: at most 1 GiB in existing trees;
expected remaining free >49 GiB, minimum reserve 20 GiB. No guest rebuild,
dependency download, asset copy or duplicate configure tree.
Bound no-capture diagnostics to 75 seconds each, 100 MiB cumulative new logs /
perf / small reports. Explicit `bd_capture_after_s=0`, verify profile and config,
stop owned processes on timeout/errors/budget growth, restore original profile.
Incoming raw allowance is zero until fresh eligible cleanup is measured.
Produced bytes and ending free space will be reconciled after each large job.

## Source/build checkpoint

Native hook, explicit scene/depth entry, complete callback/plan preflight,
64-root bound, RAII-held alternating output pair, ordered focus refresh and
counted compatibility wrapper are implemented. No global depth copy or virtual
call executes for recognized lists. Direct compatibility post entry still
imports the global depth, as required for unconverted callers.
CPU sequence tests cover counts 0..64, overflow refusal, repeated ping-pong
reuse and independent eye payloads against an ordered reference. All 30 CTests
(29 texture/state + 1 material) and 36 source guards (23 post, 10 scene,
3 reflection) pass. Focus ordering is source-guarded; multi-root GPU coverage
is not yet established. Host target built successfully; codegen wrote zero
files, no guest TU or shader rebuilt. Only the existing host/test trees used.

Binary linked 18:49:01 EDT, 47,519,744 bytes; embedded `60293d656` with
local changes, SHA-256
`ff28a0de687b372b083c5fb1484d894447d42cee659258436d4e1dd1e19354c3`.
Post-build free 54,446,174,208 bytes; net volume increase 2,576,384 bytes
from starting preflight. New test binary and host objects replace/reuse existing
outputs; no raw captures produced. The running diagnostic has an explicit zero
capture delay and a 75-second owned-process timeout.

## Fresh capture budget and eligible cleanup

Read the directional-bloom and readiness retention evidence. Current
`native_readiness_flat` / `native_readiness_vr` supersedes the older normal
bloom pair. Delete only the 120 raws in each of `native_bloom_flat` and
`native_bloom_vr`, plus their exact automatic hard links. Keep all eight PNGs,
reports and logs. All directional previews/shared probes, readiness startup
probes and unresolved failure/control evidence remain protected.
Exact expected endpoints:

- Flat: `frame_1788645314_0.raw` .. `frame_1788645317_119.raw`, 8,294,420
  bytes/frame, 995,330,400 unique bytes.
- VR: `frame_1788645185_0.raw` .. `frame_1788645194_119.raw`, 18,247,700
  bytes/frame, 2,189,724,000 unique bytes.

Validate all 240 payloads / 480 exact paths, stopped renderer, reparse-free
workspace ancestry and exact NTFS two-link membership/all same-name references
before removal. Measure actual recovery. Historical raw pixels cannot be
recovered; rerunning the test is not byte-identical restoration.

Only after this fresh cleanup: capture `native_sequence_flat` (120 frames,
995,330,400 bytes) and `native_sequence_vr` (120 frames, 2,189,724,000 bytes).
Total new retained raw cap 3,185,054,400 bytes equals the removed raw bytes;
no historical archive growth and no retry allowance. Include automatic outputs
once by NTFS identity; isolated sets use hard links. Existing cumulative 100 MiB
small-output budget includes diagnostic logs, perf CSVs and endpoint/stereo PNGs.
Minimum reserve 20 GiB, stop at completion or 110 seconds. Stream all adjacent
frames, export only endpoints, inspect flat and both full eyes. These become
current normal evidence only after inspection; retain the readiness normal pair
as previous control and review at the next replacement checkpoint.

No-capture diagnostic log 804, owned PID 26244, 18:50:24-18:51:41:
all five settings audited, full 1673 archives / 119346 records mounted,
zero new raws and zero checked error/critical/VK_ERROR/device-loss/exception/
assertion/fatal markers. Final sample 3,601 native sequences / roots,
zero original sequences, zero refusals and zero original post scopes. Maximum
one root exercised; no multi-root GPU or image qualification claim.
The owned process and build session are terminal.

Fresh cleanup completed: all 240 payloads / 480 exact paths validated then
removed; all eight PNGs, reports/logs and protected evidence remain.
Actual free 54,442,467,328 -> 57,628,499,968 bytes, recovery 3,186,032,640
bytes (2.97 GiB). Removed unique raw bytes 3,185,054,400. New raw consumed
so far zero; the full unchanged 3,185,054,400-byte replacement allowance
remains, split exactly 995,330,400 flat + 2,189,724,000 VR. This is fresh
reclamation, not credit from the readiness checkpoint. Net volume reduction
from initial preflight is currently 3,179,749,376 bytes before new captures.

Implementation `7e17427` committed/pushed before image qualification. Same
18:49:01 executable, no rebuild for documentation stamps.
VR log 805, owned PID 24668, 18:55:03-18:56:16: all 16 settings audited;
native sun/shadows, layered multiview, camera mode 2, height zero, mirror/legacy
stereo off, XR scale 1, process-local simulator 1440x1584 per eye. Capture
delay 60 / minimum 450 / count 120. Last sample 8,101 native sequences/roots,
zero original sequences/post scopes/refusals, max roots one.
Captures `frame_1788648965_0.raw` .. `frame_1788648973_119.raw`, frames
7984-8103, stacked 1440x3168. Exactly 2,189,724,000 unique raw bytes retained
with hard-link isolation. Analysis/pixel inspection in progress.

Before the next producer: free after VR 55,436,337,152 bytes (51.63 GiB).
Removed 3,185,054,400 unique raw bytes; consumed 2,189,724,000; remaining
995,330,400, exactly the planned 120-frame flat set. No retry or cap increase.
Small exports/logs share the original 100 MiB budget; no other large output.

VR streaming analysis completed: 0/119 changes above 6%, maximum .52079%,
zero cyan pixels. First/last stereo bands 44/52/62/72/82/90/95% are
-1/-2/-3/-5/-6/-8/-9 pixels in both captures: correctly crossed depth,
near-far -8, spread 8. All four full eye PNGs inspected: coherent orange sky,
village, stairs, ground/rocks, moving windmill and shadows. Existing distant
blur remains, and this framing does not qualify Shu's shadow. No new visual
defect identified in this short normal window. No authored multi-root,
late-scene, title-artwork or full-game qualification claim.

## Flat verification and final retention

Flat log 806, owned PID 24420, 18:57:51-18:58:58: original five-setting
profile restored and audited (delay 60 / minimum 600 / count 120). Last sample
2,701 native sequences/roots, zero original sequences/post scopes/refusals,
max roots one. Captures `frame_1788649133_0.raw` ..
`frame_1788649137_119.raw`, frames 2833-2952, 1920x1080.
Streaming analysis: 0/119 changes above 6%, maximum 3.34505%, no cyan hits
(maximum .02083%, median .01119%). Both full endpoint PNGs inspected:
recognizable Shu and cast silhouette, vegetation, ground, windmill and changing
shadows; existing distant DoF remains. No new defect identified in this window.

All three logs mount 1673 archives / 119346 records and contain zero checked
error/critical/VK_ERROR/device-loss/exception/assertion/fatal markers. All owned
renderer, build and analysis sessions are terminal; original profile read back,
no runtime registry change and no Quest/Thor run. Guest-source established the
container/caller relationship and ordered focus side effect; devloop reused
the existing desktop/test trees; vrsim verified both final eyes on desktop.
No authored multi-root, empty/disabled-list or unknown-callback GPU coverage;
capacity/order are CPU tested and refusal/focus ordering source guarded.

Final new retained evidence: 240 unique raws / 3,185,054,400 bytes, eight
endpoint/stereo PNGs / 9,772,550 bytes, app logs / 952,019 bytes, six perf
files / 2,482,512 bytes. No every-frame PNG export. Incoming raw allowance
is fully consumed, with zero retry/growth allowance. Current normal baseline
is `native_sequence_flat` / `native_sequence_vr`; keep the readiness normal
pair as previous control, eligible for review at the next replacement, not
automatic deletion. Keep readiness startup probes, every effect preview/shared
probe and unresolved failure/control evidence until their explicit replacement.

Final scoped NTFS-identity inventory across automatic and isolated capture
roots: 29,565 paths / 28,331 unique raw files, 263,324,814,100 logical and
233,564,594,672 allocated bytes. Both unique counts and byte totals are unchanged
from the readiness checkpoint. The historical archive remains over the 10 GiB
target; its inventory/review obligation and no-growth gate remain in force.
The older bloom pair now has zero raws; all eight existing PNGs remain.

Ending measured free space: 54,396,416,000 bytes (50.66 GiB). Net volume
usage increased 52,334,592 bytes (49.91 MiB) from initial preflight, despite
the 2.97 GiB gross cleanup: replacement captures consumed the reclaimed raw
allowance. Small evidence and build updates remain within their recorded caps.
A scoped, reparse-free changed-file inspection of the runtime/build tree found
the expected executable/PDB/host objects, build metadata, logs and perf files;
no additional large runtime payload. The volume delta is not a precise sum of
repo artifacts; the residual is unattributed, not claimed as raw capture growth
or reclaimed space. No further producer launched after this reconciliation.

Next: remove the scene scheduler's temporary post-container construction and
cleanup, pass explicit native scene colour/depth/exposure through the frame,
and remove downstream resolve-link/UI publication dependencies. Engine list,
property/camera/focus/light/visibility/animation/material/scene producers and
unknown callbacks still need conversion. Title artwork, VR blur, late-scene
failures and representative fields/battles/cutscenes/menus/transitions/reloads
in both eyes remain unqualified. This is another removed guest rendering
execution boundary, not a completed native frame. Full goal stays active;
Quest optimization has not started.
