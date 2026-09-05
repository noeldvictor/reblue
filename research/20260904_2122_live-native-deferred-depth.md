# Live native deferred depth, 2026-09-04

## Ownership and remaining boundary

`deferred_depth.h` owns explicit bounds/far-extent and fixed-depth policies.
The pure calculation takes host values and current world/view transforms;
it has no guest addresses, shader registers or retained numeric depth. The
initial calculation and its standalone tests were pushed as `92aa80f` before
runtime integration.

The `sub_8227EFC8` host replacement now produces initial depth without executing
the original function on its normal, valid-input path. Imported policy/bounds
are owned with their corresponding deferred entry recipe. Replay recalculates
depth from the current object matrix and current engine view before publishing
the complete batch. The old image's numeric key is only a diagnostic reference,
not the native producer's source. Fixed policy is explicit, never inferred from
an old key.

The temporary import map is bounded by the 5140-entry list capacity, clears at
the first allocation after a drain and erases a reused entry before importing.
Captured recipes own values, not references into that map. Every captured entry
must match the replay matrix source; the old aggregate majority check is no
longer sufficient to qualify an individual recipe. Mixed direct/deferred nodes
preflight depth as well as capacity before issuing any direct draws. Unknown
policy or nonfinite/invalid inputs refuse the whole replay batch without a
partial publication. Counters count published replay, not speculative preflight.

`bd_native_deferred_depth` defaults on. Off is a correctness comparison only;
the initial producer calls the original and replay uses the old captured key.
`bd_native_deferred_depth_verify` defaults off. When enabled, it runs the
original producer for comparison before publishing the native result. Unknown
initial inputs have a counted compatibility fallback and no replayable policy.
`checked` counts diagnostic original calls; `compatibility` counts the off or
fallback calls. Zero compatibility alone does not mean zero guest execution
when verification is enabled.

This is not a complete native deferred renderer. Object/view transforms are
still engine-produced, bounds/policy discovery is still tied to interpreted
entries, and bounds are not yet persisted/associated through the native scene
asset loader. Most entry fields, material/pass records, engine list storage,
visual switches and the guest list consumer remain. The host frame scheduler,
remaining draw producers and representative full-game qualification are still
required. No Quest run or headset performance claim is made.

## Source contract

Read `config/hooks/render_list.toml`, the complete translated
`generated/reblue_recomp.41.cpp` / `sub_8227EFC8`, its call in
`generated/reblue_recomp.40.cpp` / `bdSceneNodeDrawSingle`, and `GuestMesh` in
`src/gpu/scene/guest_scene.h`. No generated source or hook TOML was edited.

- r3: entry, r4: world matrix, r5: mesh, r6: fixed-policy selector.
- Nonzero selector or null mesh chooses the fixed scalar at
  `(uint32_t(-32251) << 16) + 20912`.
- Ordinary policy reads mesh centre +20/+24/+28 and radius +32. The view matrix
  is at `(uint32_t(-32034) << 16) - 19936 + 65536 - 10816`.
- With row-major matrices and translation in row 3, transform centre by
  world*view, then return `-(radius + view_space_z)`. The radius is added as
  supplied, not multiplied by a newly invented maximum-axis scale.
- The native expression uses paired matrix dot sums and Z/Y/X point composition
  with Clang FP contraction disabled, matching the translated producer's order.
  Runtime comparison uses tolerance `1e-5 * (1 + abs(host_depth))`; it is not a
  bit-identical or denormal/FP-mode qualification.
- The compatibility bridge writes the result big-endian at entry +276 only
  after all depths and destination ranges validate.

## Build and tests

Reused the configured Vulkan-only desktop `reblue` target (OpenXR and PCH on,
Clang 22.1.8). Both incremental runtime builds linked successfully; codegen
reported zero writes and no guest translation units rebuilt. The first attempt
to run standalone Ninja under sandbox process permissions stalled; its exact
identified Ninja child was stopped and the same build/tests completed with
normal process permissions. No build tree was deleted.

All five standalone texture/lifetime/binding/upload/deferred tests pass with
assertions enabled in the deferred Release test. New cases cover identity,
object/camera movement, camera rotation, nonuniform scaling, far extent,
explicit fixed policy independent of matrices, 1000 randomized affine
transforms checked against independent double-precision point math, unknown
policy, nonfinite values in every matrix/centre component, finite overflow,
live changes to sort order, unchanged output on a partially invalid batch,
empty work and exact big-endian depth-byte publication without other changes.

## Initial producer comparison, log 660

Launched 21:18:05 EDT, PID 8876. Original five settings plus
`bd_native_deferred_depth_verify = true`; all six audited as applied. Autoplay
and perf CSV on, capture after 60 seconds, minimum 600 draws, 120 frames.
Capture sequence 119 completed 21:19:10 at frame 2947, 1920x1080.
Isolated output: `out/verification/native_depth_verify`.

No jumps over 6% in 119 pairs; no cyan patches (median 0.011%, maximum 0.02%).
First and last images inspected: character, terrain, vegetation and shadows
intact. Last periodic depth report: 20484 imports / 4928 fixed policies,
20483 native initial publications, 898837 replay publications / 213822 changed
keys, 20483 checks, zero mismatches, compatibility calls or refusals. Periodic
snapshots can occur between import and publication. These are repeated-work
counts, not unique assets or fully host-owned frames.

This run predates extraction of the already-used transactional batch evaluation
and depth-byte write into standalone-tested helpers. Those helpers preserve
successful evaluation/publication behavior and were rebuilt for the normal
runs. PID 8876 was stopped after exact executable-path validation and confirmed
exited before rebuilding. No error/critical, Vulkan-error, overflow or
retirement-race matches were found in the log.

## Normal-path verification

### Flat, log 661

Final binary launched 21:20:37, PID 21660. Original five settings all applied;
native depth is on and verification off by default. Sequence 119 completed
21:21:43 at frame 2952, 1920x1080. Isolated output:
`out/verification/native_depth_flat`.

0/119 jumps above 6%; no cyan patches (median 0.011%, maximum 0.02%). Inspected
first and last images: the character, foliage, terrain and shadows are intact.
Last report: 38179 imports / 9161 fixed policies, 38178 initial publications,
1741580 replay publications / 330858 changed keys, zero checks, compatibility
calls or refusals. The host allocation/sort bridge also reports zero refusals.
No error/critical, Vulkan-error, overflow or retirement-race matches. Exact-path
validated process stopped and confirmed exited at 21:23:02.

The integrated producer/replay checkpoint was committed and pushed as
`a8f232c` after this normal-path check. A short flat check is not full-game or
headset qualification.

### Multiview, log 662

Launched 21:23:06, PID 20212, using the same final binary with native depth on
and its verifier off. All 13 profile settings applied: autoplay/perf CSV,
capture after 60 seconds with minimum 450 draws and 120 frames; VR/multiview
and layered textures on, legacy stereo/scene-array capture/mirror off, camera
mode 2 and diorama height 0. Process-local simulator settings requested
1440x1584 with height 0 and the absolute `out/xrsim-build/reblue_xrsim.json`
manifest. Logs confirm instance/session, 936x1030x2 swapchain and composed
camera eyes differing from the game camera.

Final stacked-eye sequence 119 completed 21:24:14, frame 20808, 936x2060.
Isolated output: `out/verification/native_depth_vr`. 10/119 pairs exceed 6%,
at destination frames 4, 5, 7, 8, 10 and 68, 69, 71, 72, 74: the 64-frame
cadence remains. No cyan patches (median/max 0%). First and last stacked images
were inspected in both eyes: distant blurred terrain, conspicuous horizontal
bands and large black bars persist. The stereo check returned exit 1,
INCONCLUSIVE, with no usable bounded depth bands. Neither native depth nor
clean fallback counters resolve or qualify these failing pixels.
Frames 4 and 5 were also inspected across a flagged jump: both eyes change
from a more legible terrain plane to the horizontally banded/blurred result.

Last periodic depth report: 120618 imports / 10833 fixed policies, 120617
initial publications, 5713129 replay publications / 1057516 changed keys,
zero checks, compatibility calls or refusals. The host allocation/sort bridge
also reports zero refusals. No error/critical, Vulkan-error, overflow or
retirement-race matches. Exact-path validated PID 20212 was stopped and
confirmed exited at 21:25:25.

All renderer processes are stopped. The original five-setting profile is
restored (60-second delay, minimum 600 draws, 120 frames, autoplay/perf CSV).
No generated code, local profiles, logs, captures, binaries or game data are
committed. No late-scene run was repeated for this checkpoint; the previous
documented dark/missing-geometry failure remains unqualified, not fixed.

## Next ownership boundary

Replace the remaining entry/material/pass producers and guest list consumer,
including visual switching and cleanup; persist/associate object bounds and
policy through native scene assets. Current world/view import must become
native object/pass ownership. Keep the known multiview and later-scene pixel
failures open while making those replacements. This is progress toward the
full renderer goal, not completion or authorization to begin Quest work.
