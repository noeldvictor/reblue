# Native scene-to-post handoff

2026-09-05, Windows Vulkan desktop, EDT. Base `6231c4c`.

## Scope and source evidence

Previous goal turn made progress: cumulative storage rules were updated and
pushed. Guest-source and devloop skills read completely; no delegation/device
work. Current normal baseline remains `native_sequence_flat` / `_vr`.

Read the complete post setup/cleanup/epilogue region of `bdRenderViewSubmit`
(`generated/reblue_recomp.16.cpp`), its entry and main scene descriptor setup,
and the complete `sub_8221C9A0` constructor (.57.cpp). The view input r18 holds
colour/depth getters at +0/+4; each getter's texture is at +4. The scene begin
adapter consumes those same fields. The post constructor copies that handle
to +0 and allocates surface-level wrappers and binding-index vectors, all
destroyed after the effect sequence. The scene owner already retains the
image for the synchronous submission, so those temporary wrappers are redundant.

Hook before 0x821865B8, after final focus store 0x821865B4. Import the explicit
scene/depth images once and run the native sequence, whose core no longer
accepts container addresses. On success jump to 0x821867E8, beyond both full
destructors. That block reloads saved flags and their owner before its calls;
it consumes neither skipped temporary data nor condition-register state. The
skipped GPR-allocation restore is already `BD_NOOP` in `gpu/hooks/device.cpp`.
Refusal leaves PPC registers and temporary stack memory unchanged; complete
list preflight and no partial replay retain the existing sequence contract.

This removes two guest container lifecycles and the normal wrapper invocation,
not scene-output getters, resolve-link/exposure publication, the engine list /
camera/focus producers, UI or the remaining parent rendering scheduler. Those
are still migration boundaries; full desktop/game/both-eye gates remain open.

## Cumulative storage ledger / initial gate

Starting measured free: 54,395,469,824 bytes (50.66 GiB). Reuse the existing
desktop/test trees. Planned peak build/link/test additional space <=1 GiB;
expected reserve >49 GiB, minimum 20 GiB. Hook TOML is a legitimate codegen
input change: expect only affected partition/header/metadata regeneration,
not an unrelated full guest rebuild. Inspect emitted build work.

No new raw capture allowance: historical archive remains frozen over budget;
previous cleanup savings were fully spent. Early diagnosis explicitly disables
captures, stops owned processes at 75 seconds, and shares a cumulative 100 MiB
cap for logs/perf/small reports. No asset cooking/download/copy/duplicate tree.
Reconcile actual free space and outputs before another large producer. Fresh
eligible cleanup is required before any new image sequence.

## Source/build verification

All 30 existing CTests and 39 source guards pass (26 post, 10 scene, 3
reflection). The new optional owned-code test maps every PPC comment and
checks all branch labels, exact entry/focus/exit addresses and both skipped
constructor/release calls. The runtime-independent hook guard verifies its
configuration and the post-epilogue GPR no-op. This is not pixel qualification.
The first test attempt found Python lacks `tomllib`; the guard now uses only
the standard-library text/regex helpers already supported here, no download.

Build succeeded: codegen wrote one partition, 218 files unchanged, no deletes.
Only guest partition 16 rebuilt; no other guest TU or shader/asset generation.
Generated output shows the hook before the depth-getter load, jumping directly
to `loc_821867E8`. Host target remains the configured Vulkan/OpenXR executable.
Binary 47,522,304 bytes, linked 19:15:17 EDT; embedded base `6231c4cfb` with
local changes. SHA-256:
`8adfecfb06d530ebb662d18910aaf0380f48cbf1432196a48e9bf13f2390b6a1`.

Post-build free 54,394,572,800 bytes; net volume growth 897,024 bytes from
initial preflight, within the 1 GiB peak plan. Raw output remains zero.
Next is a bounded no-capture diagnostic using the existing runner with Count=0;
its older cumulative log cutoff is stricter than this checkpoint's 100 MiB cap.
Runtime/image verification and final retention pending.

## Fresh capture budget / cleanup decision

Read the full readiness worklog and current effect-sequence retention record.
The current sequence normal pair supersedes readiness normal flat/VR for the
same short field scene; readiness's distinct early-startup probes remain
protected. Reclaim only the two older normal sets' 120 raw payloads each and
their exact automatic hard links, keeping all eight PNGs, reports and logs:

- `native_readiness_flat`: `frame_1788647193_0.raw` through
  `frame_1788647196_119.raw`, 8,294,420 bytes/frame, 995,330,400 unique bytes.
- `native_readiness_vr`: `frame_1788647050_0.raw` through
  `frame_1788647060_119.raw`, 18,247,700 bytes/frame, 2,189,724,000 unique bytes.

Validate all exact paths and workspace ancestry, reject reparse points, verify
each NTFS link pair and all same-name references, and require the renderer
stopped before removal. Retain current sequence controls, readiness startup,
all effect previews/shared probes and unresolved failure/control evidence.
Historical raw pixels cannot be restored; new runs are not identical recovery.

Only after fresh measured reclamation: `native_scene_handoff_flat` / `_vr`,
120 frames each, total 3,185,054,400 unique raw bytes, exactly matching removed
payloads. No retry or growth allowance. Full sequences are required for temporal
checks; export only first/last and stereo PNGs. Keep cumulative small outputs
under 100 MiB including diagnostic log/perf; all runs bounded by the existing
110-second capture timeout. Helpers reuse the prior exact-path validator and
bounded runner with this checkpoint's names/endpoints/time cutoff; no large
copies. This is a fresh budget, not reused savings from earlier checkpoints.

Diagnostic log 807, owned PID 24060, 19:15:57-19:17:14: five settings audited,
capture delay zero verified, no new raws. Last sample 3,601 direct handoffs /
sequences / roots, zero original container scopes, original wrappers, original
post scopes or input/sequence refusals; maximum one root. Checked error/critical/
VK_ERROR/device-loss/exception/assertion/fatal markers absent. Process and runner
terminal. No multi-root or pixel qualification from this diagnostic.

Fresh cleanup completed: 240 unique payloads / 480 validated paths removed,
all eight existing PNGs retained. Actual volume free 54,392,713,216 ->
57,578,221,568 bytes; measured recovery 3,185,508,352 bytes (2.97 GiB).
Removed raw payload is 3,185,054,400 bytes. Incoming raw consumed zero;
remaining allowance exactly 995,330,400 flat + 2,189,724,000 VR, no retry.
This gross reclamation is not a claim of net savings after pending captures.
