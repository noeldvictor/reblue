# Native receiver-shadow inputs, 2026-09-04

## Outcome and ownership

The host now evaluates a named receiver-shadow policy using current scene inputs
and a decoded per-material disable flag. Supported direct-tree phase-0 replays
no longer retain this decision from an older draw template. The pure policy,
control-record decoding and tests were committed and pushed as `cf45af2`;
this note accompanies their renderer integration.

This is a bounded ownership step, not a complete native shadow renderer or a
fix for the 64-frame multiview defect. Incoming pass enable, visibility stamps,
their producer and thread-specific frame counters still come from guest scene
state. A temporary shader-ABI adapter publishes the result into PS bool bit 5.
No guest state is written by this composer. List-entry draws, phase 1,
technique 11 and missing/ambiguous imports retain the tracked compatibility
path. The control-record index is discovery metadata; `.bdmat` v1 remains
unchanged and does not persist this shadow policy.

`bd_native_shadow_inputs` defaults on. The input checker compares supported
interpreted draws with the pure host result; replay counters separately count
compositions and changes from the legacy composed bit. These are not counts
of fully host-owned frames, nor proof of all later inherited state.

## Source evidence

Used the exact translated C++ and PPC comments in `generated/`, not a new
binary/decompiler import. Generated source and hook definitions were not edited.

- `generated/reblue_recomp.40.cpp`, `bdSceneNodeDrawSingle` at `0x8227FEE8`:
  `0x822802DC..0x822803D8` saves incoming pass shadow state, selects a receiver
  record and conditionally filters the node. Visual offsets 3380/3376 select
  the per-node pointer table; the default receiver is visual + 3132.
- `generated/reblue_recomp.94.cpp`, `sub_82189E00`: select the 12-byte visibility
  slot, read its 16-bit stamp at +8, reject stamps at or above `0x8000`, then
  compare with the current engine frame. Do not truncate that frame to 16 bits.
- `generated/reblue_recomp.34.cpp`, `sub_82184A38`: compare the current guest
  thread ID with the primary-thread ID, then select frame-owner word 12007 or
  12008. Host presentation frame count is not an equivalent source.
- `generated/reblue_recomp.40.cpp`, `0x822813CC..0x82281430`: an E000 command
  selects a 16-byte model-control record through scene-graph +8. Its decoded
  fourth output byte disables receiving shadows; otherwise use node visibility.
  The function restores incoming pass shadow state at `0x82281CC0`.
- `generated/reblue_recomp.94.cpp`, `sub_8228AB40`, and
  `generated/reblue_recomp.79.cpp`, `sub_8228AAB0`: presence-mask bit 0 gates
  the first payload's four feature bits; payload bit 3 is shadow disable.
  An absent control table is a no-op, not a shadow-disable request.
- `generated/reblue_recomp.60.cpp`, `sub_821739B0`: the global receiver filter
  gates staging writes. `generated/reblue_recomp.67.cpp`, `sub_821981E0`,
  flushes bool staging; `bd_normal_lit.hlsl` names bool 133 `g_bShadowMap`
  (PS bit 5). The same named input appears in the wind-lit shader.

The pure equation is `pass_enabled && (!receiver_filter_enabled ||
(receiver_visible && !material_disables_shadow))`. Unreadable inputs return
an unknown import, not an invented false value. Repeated geometry under
different control policies is also refused as ambiguous. Existing command
discovery bounds/generation handling are shared with the material importer.

## Build and standalone checks

Reused `out/build/win-amd64-release`: Vulkan-only `reblue` target,
`reblue_vk.exe`, OpenXR and PCH on, Clang 22.1.8. Both incremental host builds
completed successfully; codegen reported no generated changes, and no guest
translation units rebuilt. A standalone Ninja build stalled under restricted
process creation; the exact CMake/Ninja processes were identified and stopped,
then the same build succeeded with the required execution permissions.

Final checks passed:

- Native material tests: 1/1, including control-record selection, all 16 policy
  combinations, stamp validity/staleness/full-width frame comparison, and
  control presence-mask semantics. Existing v1 file/cooker tests still pass.
- Native texture/lifetime/binding/upload tests: 4/4.
- Native mesh tests: 1/1.
- Stereo-check utility tests: 2/2. These test the tool, not the VR capture.

## Runtime evidence

All logs below are under `out/build/win-amd64-release/logs/`. Captures were
isolated per run under `out/verification/`, with 120 raw files per completed
sequence. `capture_seq.py` used its 6% neighbouring-frame threshold;
`capture_cyan.py` used its default cyan classification. Times are local EDT.

### Late baseline after upload lifetime fixes: log 652

The pre-shadow binary from the final upload checkpoint (`9770991` documentation,
`7347972` implementation) launched at 20:17:57. Capture delay 270 seconds,
minimum 30 draws, 120 frames; autoplay and perf CSV enabled. Sequence 119 was
written at 20:23:26, frame 14742. Output:
`out/verification/shadow_inputs_late_baseline`.

77/119 frame pairs exceeded 6%; no cyan patch frames. Inspected sequences
44/45 show black/red block-like geometry, mostly missing scenery and damaged
text. This is a visual failure after the previous upload lifetime fixes, not
just evidence from the earlier overflow build. There were no error/critical,
Vulkan-error, retirement-race or overflow matches in the log. Upload counters
reported zero failures. Texture CPU-budget refusals occurred during loading;
their causal role in the broken scene is not established.

### Sampled input verification in VR: log 654

The first launch, log 653 at 20:28:21, mistakenly used `bd_vr` instead of
`bd_vr_enabled`; the config audit exposed it. That process was stopped and is
not VR evidence. The corrected launch began at 20:29:28 with all 14 settings
applied, including `bd_host_draw_verify_every = 17`.

Shared VR settings: autoplay/perf CSV on, capture after 60 seconds, minimum
450 draws, 120 frames; VR enabled, legacy stereo off, multiview and layered
textures on, scene-array capture off, mirror off, camera mode 2, diorama height
0. Process-local `XR_RUNTIME_JSON` selected the repository simulator with an
absolute library path; recommendations 1440x1584, head height 0. OpenXR instance,
session, 936x1030x2 swapchain and an active composed eye pose were confirmed.

Sequence 119 completed at 20:30:35, frame 20379, with final stacked 936x2060
eyes in `out/verification/native_shadow_sampled_vr`. The receiver checker
reported 636334 supported draws and zero mismatches. The broader replay
verifier still reported 98804 wrong nodes out of 521265 and 115766 wrong draws
out of 660124, including bool, texture and other mismatches. This is not a
clean whole-renderer comparison.

The sequence had 51/119 jumps over 6% and no cyan patches. Its first image was
inspected: distant blurred scenery, orange sky, black bars and a visible
horizontal strip. Sampled interpretation changes refresh frequency, so this is
not the normal replay path's stability result.

### Normal multiview: log 655

Final binary launched at 20:32:27, all 13 normal VR settings applied (no sampled
verifier). Sequence 119 completed at 20:33:34, frame 21034. Output:
`out/verification/native_shadow_normal_vr`.

337041 supported input checks, zero mismatches, 311013 receiving-shadow
decisions; 5450237 replay compositions changed the old composed bit 110526
times. Thus live input is used, rather than merely reproducing cached values.

Nevertheless 10/119 pairs exceed 6%, at destination frames 16, 17, 19, 20, 22
and 80, 81, 83, 84, 86: the same 64-frame cadence as the earlier upload-page
verification. No cyan patches. Actual stacked frames 0/16/17 were inspected
in both eyes: scenery alternates between a more legible ground plane and
blurred/horizontally banded output. This defect remains unresolved.

`stereo_check.py --raw <seq0> --stacked --out <isolated-dir>/stereo` returned
INCONCLUSIVE: only 44% of the examined band matched at zero shift, with too few
bounded depth matches. The distant framing is not a stereo-depth qualification.
No error/critical, Vulkan-error, retirement-race or overflow matches were found.

### Final flat check: log 656

Final binary launched at 20:34:42 after restoring the original five settings:
autoplay/perf CSV on, capture after 60 seconds, minimum 600 draws, 120 frames.
The audit confirmed all five. Sequence 0 was written at 20:35:44, frame 2841;
sequence 119 at 20:35:48, frame 2960. Output:
`out/verification/native_shadow_final_flat`, 1920x1080.

0/119 jumps over 6%; no cyan patches (median 0.012%, maximum 0.02%). Actual
first/last frames show the character, terrain, vegetation and shadows intact.
This passes the short flat slice, not later scenes. At the last receiver report
there were 707790 checked draws, zero wrong, 666603 receiving; 1294547 replay
compositions changed 63141 legacy decisions. The run continued into a later
low-draw/loading interval, but no second late-scene capture was taken from this
binary. Do not claim its late-scene visual qualification.

The log contains no error/critical, Vulkan-error, retirement-race or overflow
matches. The final renderer process was stopped at 20:39:35 after exact-path
validation and successful process-exit confirmation. All verification renderer
processes are stopped; the original five-setting profile is restored. No
generated sources, game assets, saves, binaries or derived captures are staged.

## Remaining work

Replace guest pass/visibility and deferred-list producers with native scene
and pass data, persist complete material feature policy, and remove the
shader-register adapter. The 64-frame multiview defect, later scene corruption,
full-game scene coverage and actual stereo-depth verification remain open.
Quest optimization remains gated on complete host-owned desktop rendering.
