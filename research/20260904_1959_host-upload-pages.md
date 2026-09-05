# Host upload pages and constant-window overflow

2026-09-04. This separates native resource staging from shader-register
compatibility storage. The full host renderer remains incomplete.

## Cause and implementation

The previous `reblue_646.log` reports slot 1 wrapping at 33,637,888 bytes at
19:40:53.915, during a burst of native texture imports. Source inspection shows
`UploadHostBytes` and guest bulk uploads used the same `Allocate` function as
shader constants. It allowed individual requests up to the whole shared buffer
size, despite a smaller per-slot span, and rewound to offset zero on overflow.
That could overwrite both constants and earlier texture subresources before
their commands had even been submitted.

The earlier binding note's 615-upload/605-resident GPU snapshot describes the
initial capture, not the full run: later log 646 reports additional uploads,
including 734 uploaded and 656 resident immediately after the overflow.

New `HostUploadAllocation` and `UploadHostData` use a native page arena with no
guest resource or shader-register dynamic offset. Native textures call it
directly, as does the native ImGui overlay. Bulk compatibility adapters share
its safe storage. The arena owns
independent pages per recording slot, reusing/retiring only after that slot's
completed fence. Four-MiB ordinary pages, at most 64 MiB per request, and a
256 MiB aggregate page budget bound growth. Idle/one-off large pages retire;
failed allocation leaves existing copy sources intact. User-pointer uploads
open their recording list and acquire the renderer lock before reserving/writing.

The remaining shader buffer cannot wrap. Checked allocation refusal is distinct
from an unchanged content hit; affected immediate/legacy-eye/deferred-record
draws are rejected. Record-budget fallback uploads the actual node block.
UP-vertex upload failure also rejects the draw. Host constant payloads reserve
and initialize the largest fixed descriptor range, preventing an end-of-buffer
descriptor overrun. These are failure safeguards, not permission to count an
incomplete frame as correct. The retained shader/register ABI still needs
replacement. See [the upload contract](../docs/HOST_UPLOAD_ARENA.md).

## Verification so far

Used the repository devloop and vrsim instructions, current transition scope
and prior binding evidence. Inspected native uploads, compatibility uploads,
constant publication, immediate/deferred draws and the actual frame fence/reset
call chain. No generated source, shader translation, guest configuration,
dependency gitlink or game-derived assets changed.

Clang 22.1.8, configured Vulkan-only desktop target `reblue`, OpenXR enabled.
Build linked; codegen wrote zero files and no guest translation units rebuilt.
Texture CTest 4/4, material 1/1, mesh 1/1 and stereo checker unit tests 2/2 pass.
The new production-policy test validates every byte across a 160 MiB burst,
80 MiB per slot, plus a single 64 MiB request. It tests exact bounds, invalid
alignment, integer overflow, allocation failure, budget accounting and slot
isolation/retirement without a GPU. Runtime resource exhaustion itself is not
injected into Vulkan.

Initial run `reblue_647.log`, 19:53:59 start, captured 120 final 1920x1080 frames
after 60 seconds with the original five-setting profile. Isolated directory:
`out/verification/host_upload_initial_flat`. Analysis: 119 pairs, zero jumps
above 6%, zero cyan patches, median cyan 0.011%, maximum 0.02%. Inspected the
character, terrain, village buildings, foliage and shadows in `first.png`.
This run preceded the final UP locking/recording and failure-path refinements;
it does not alone qualify the final code or the later loading burst.

## Longer run and remaining image failures

`reblue_648.log` ran from 19:56:30 to about 20:02:40. All five profile values
audited; the capture delay was 270 seconds and minimum draws 30. It waited at
20 draws before recording seq 0 at 20:01:55.064, frame 14650. The complete
120-frame sequence is isolated in `host_upload_late_flat`.

The loading burst completed with zero upload refusals/failures, constant-window
errors, device-loss or retire-race messages. Post-fence staging returned to
8,388,608 bytes after a 41,943,040-byte total peak; peak single-slot staging was
32,700,928 bytes. The final periodic report showed 22 pages created / 20 retired.
The last reported shader-slot peak was 7,061,856 / 33,554,432 bytes. These are
allocation-accounting observations, not headset performance or full-game proof.

**The late pixels fail correctness:** 73/119 pairs jump above 6%, despite zero
cyan patches (median 0.000%, maximum 0.01%). Inspected seq 2, 44 and 45: very dark
or missing geometry, damaged text and block-like silhouettes. This is not an
acceptable scene transition and must not be dismissed as ordinary camera motion.
Eliminating upload wrapping did not qualify the later scene.

First multiview run `reblue_649.log`, started 20:03:05, used the standard 13
diorama settings: VR on, legacy stereo off, multiview/layered textures on,
final-eye capture, mirror off, camera mode 2, height 0; capture after 60 seconds,
minimum 450 draws, 120 frames. Process-local xrsim recommended 1440x1584 per eye,
but the actual final capture was stacked 936x2060. Instance/session creation and
different game/eye cameras confirm XR composition. The sequence completed at
20:04:13.135, frame 21017, in `host_upload_initial_vr`.

Analysis: 10 jumps at 7/8/10/11/13 and 71/72/74/75/77, exactly 64 frames apart;
zero cyan patches (median/max 0.000%). Both eyes were inspected: distant blurred
terrain with letterboxing. Stereo checker exits 2, INCONCLUSIVE, with only one
textured matched band. This reproduces the earlier lighting cadence, not a VR
pass. There was no Quest/Thor run.

Review after these runs found additional transient-buffer lifetime gaps: raw
IA bindings survived page rewind, vertex-pulling heap entries survived page
destruction, and captured draw recipes could retain frame-local streams.
Final code scrubs bindings on rewind, forgets heap entries before retirement,
declares storage usage and refuses to freeze transient geometry into a
cross-frame recipe. The production-policy test covers callback order and live
resource identity. Those changes require a new pixel check; the failed later
sequence above is not retroactively a pass, nor is its exact cause isolated.

## Final short flat check

After those lifetime changes, rebuilt the renderer and reran all four texture
tests successfully. `reblue_650.log` started at 20:08:39 with all five original
profile settings audited. Its 120 final 1920x1080 frames completed at
20:09:44.839, frame 2955, isolated in `host_upload_final_flat`. All 119 pairs
stay below the 6% jump threshold; zero cyan patches, median cyan 0.011%, maximum
0.02%. Inspected character, terrain, village buildings, foliage and shadows.
No error/critical/device-loss/retire-race message was found. Last periodic
staging report: 8,388,608 reserved, 29,360,128 total peak, 22,247,936 single-slot
peak, 18 pages created / 16 retired, zero failures. Reported shader-slot peak:
1,838,944 / 33,554,432 bytes. The process was stopped after the capture.

The independent arena/tests were committed and pushed as `199349d`. Final
multiview verification is running separately; the longer later-scene failure
still needs a new run after transient-binding fixes and remains an open gate.
