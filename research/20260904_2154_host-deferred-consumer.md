# Host deferred-list consumer, 2026-09-04

## Ownership and boundary

The normal `sub_8227F360` path now calls `ConsumeDeferredList` instead of the
translated consumer. Native C++ owns list import, ordering and iteration, visual
switch scheduling, material begin/end scheduling, CPU bone gathering, constant
publication, winding/depth policy, ordinary/fur/stencil expansion, direct draw
issuance and list cleanup. This is an intermediate ownership checkpoint, not a
complete native renderer or permission to start Quest work.

`bd_native_deferred_consumer` defaults on. Off, or a failed initial import,
calls the original consumer and increments `fallback`. Import refusal happens
before engine side effects. Nonfinite ordering keys retain submission order and
increment `refused` rather than restarting an already-started consumer.
Preflight checks list capacity/alignment/cursors, entry ranges, declarations,
top-level visual ranges, bone source ranges, device storage, material dispatch
entries and callback ABI stack space. It does not validate every pointer that
the remaining engine callbacks may follow.

Native ordering operates on imported records without republishing a sorted
engine pointer array. These records still identify big-endian engine entries;
they are not an address-free native scene asset. Material constants are read
after the live material callback, which may change the entry. Bone matrices are
gathered into ordinary CPU storage and published as one range starting at VS60,
so existing capture bookkeeping sees the full palette. No upload-ring memory is
read back. Temporary float dirty masks and low-bit boolean packing live in a
separately tested shader-ABI helper; these are explicitly compatibility details.

Host-issued ordinary draws call `DispatchHostNodeDraw` directly, preserving the
triangle-strip count/start-index and wireframe save/restore contract. Existing
list-entry capture/replay remains in use. Replayed entries bypass setup only
under its existing same-visual guard; this checkpoint does not prove inherited
state correctness for subsequent interpreted entries.

Visual begin/end, material callbacks, world composition, fur shader selection,
render/sampler state and resource association remain engine adapters. Their six
call categories are reported independently from direct draws/replay/fallback.
Several resource callbacks are already host-hooked: bridge calls are not an
exact count of guest instructions. Zero consumer fallbacks therefore does not
mean zero guest rendering or a fully host-owned frame. Full material/pass/scene
production, shader ABI removal, native animation and GPU skinning, frame
scheduling and the other transition gates remain required.

## Source contract and tests

Read the complete translated consumer in `generated/reblue_recomp.84.cpp`,
`config/hooks/render_list.toml`, and the scene-node/list-capture hooks before
replacement. Followed `bdSceneNodeDrawIndexed` in module 65, declaration binding
in module 25, palette publishing in module 23, float constant publishing in
modules 9/5, and boolean packing in modules 23/1. Host device layout, draw
dispatch, state hooks and capture bookkeeping establish the adapter boundary.
No generated source or hook TOML was changed.

- Visual transitions and material end calls retain their ordering, including
  the material-begin skip result. Initial/terminal state and arena cleanup are
  retained, including restoration of object mode before ending the visual.
- Fur shell selectors are signed bytes: negative values still select fur
  setup/cleanup but issue no shells. Positive shells use fractions 1/N through
  N/N. Stencil work is a two-draw sequence with the pixel boolean transition;
  pending stencil survives fur entries and clears only after that pair.
- Culling is a named native face policy; D3D values are confined to the bridge.
- Constant-mask tests enumerate every valid range within 256 vectors, including
  empty/full/end ranges and overflow refusal. Boolean tests cover all 32 bits,
  several previous words and zero/even/odd values while preserving other bits.
- Surface tests enumerate all signed-byte shell selectors, both stencil states,
  every supported positive shell count/slice, all byte sidedness values,
  reversed winding and invalid/nonfinite fur inputs.

The surface core was committed and pushed separately as `e0288bc` before
consumer integration. All six standalone texture/lifetime/binding/upload/
deferred/surface tests pass, including the final shader-helper additions
(0.40 seconds total). Release assertions are enabled. The configured Vulkan-only
desktop target `reblue` linked successfully with OpenXR/PCH on and Clang 22.1.8.
Codegen reported zero writes; no guest translation units rebuilt. The final
build adds checked shader ranges and stricter initial list/stack validation to
the first runtime binary. No build tree was deleted.

## First runtime binary

### Flat, log 663

PID 22164 launched 21:41:40 EDT; original five profile settings: autoplay/perf
CSV on, capture after 60 seconds, minimum 600 draws, 120 frames. Sequence 119
completed 21:42:46.593 at frame 2961, 1920x1080. Output isolated in
`out/verification/native_consumer_flat`. 0/119 pairs exceed 6%; no cyan patches
(median 0.011%, maximum 0.02%). First and last images were inspected: character,
terrain, rocks, vegetation, building and shadows remain intact.

Last periodic report: 73009 lists, 2040025 entries, 1674048 replays, 358849 direct
draws, zero shells/stencil/fallback/refusal. Bridge calls: visual 657429,
material 731954, state 2358272, world 365977, resource 1643804, shader zero.
These are cumulative repeated-work counts, including empty list passes, not
unique scenes/assets. Exact-path validated process stopped and confirmed exited
at 21:44:19.

### Multiview, log 664 (including rotated .1.log)

PID 23692 launched 21:44:34. All 13 profile settings applied: autoplay/perf CSV,
60-second capture delay, minimum 450 draws, 120 frames; VR/multiview/layered
textures on; legacy stereo, scene-array capture and mirror off; camera mode 2,
diorama height 0. Process-local simulator settings requested 1440x1584 and
height 0 through the absolute `out/xrsim-build/reblue_xrsim.json` manifest.
Logs confirm the instance, session and 936x1030x2 swapchain.

Final stacked-eye sequence 119 completed 21:45:42.806, frame 20926, 936x2060.
Output isolated in `out/verification/native_consumer_vr`. 10/119 pairs exceed
6%, at destination frames 25, 26, 28, 29, 31 and 89, 90, 92, 93, 95: the same
64-frame cadence remains. No cyan patches (median/maximum 0%). Both eyes of
frames 0/119 and 25/26 were inspected. The latter jump changes a more readable
terrain plane into a blurred/banded result. Large black bars and poor framing
remain. Stereo check exited 2, INCONCLUSIVE: only one bounded textured band.
This is a failed visual stability check, not a VR qualification.

Last report: 351238 lists, 12858281 entries, 11134684 replays, 1685162 direct
draws; zero shells/stencil/fallback/refusal. Bridge calls: visual 3547425,
material 3447194, state 11321040, world 1723597, resource 8209080, shader zero.
The exact-path validated process stopped and was confirmed exited at 21:50:27.
Both first-binary logs, including rotation, have no error/critical, Vulkan
error/failure, overflow, exhaustion or retirement-race matches. Neither capture
exercises fur/stencil GPU paths. This longer process lifetime is not a captured
late-scene qualification: the previous missing-geometry/text failure stays open.

## Final-binary verification

### Flat, log 665

Final binary launched 21:53:17, PID 24536; the original five profile settings
all applied. Native consumer/depth remain on by default and depth verification
off. Sequence 119 completed 21:54:23.111 at frame 2953, 1920x1080. Isolated output:
`out/verification/native_consumer_final_flat`. 0/119 jumps over 6%, no cyan
patches (median 0.011%, maximum 0.02%). First and last images were inspected:
character, terrain, foliage, building and shadows are intact.

Last report: 70318 lists, 1960504 entries, 1608763 replays, 344925 direct draws;
zero shells/stencil/fallback/refusal. Bridge calls: visual 632598, material
703482, state 2267587, world 351741, resource 1580427, shader zero. No
error/critical, Vulkan error/failure, overflow, exhaustion or retirement-race
matches. The exact-path validated process stopped and was confirmed exited
at 21:55:50. This verifies that the final bounds checks accept the captured
field; it is not all-scene or fur/stencil GPU qualification.

Local profiles, logs, captures, binaries, generated code and game data are
excluded from commits.

### Multiview, log 666

The integrated consumer was committed and pushed as `7021ccc` after the final
flat check. The same final binary launched at 21:57:14, PID 23104, with the
13-setting profile and simulator environment described for log 664. All 13
settings applied; the log confirms instance/session, the 936x1030x2 swapchain
and composed eyes differing from the game camera.

Sequence 119 completed 21:58:22.235 at frame 20297, 936x2060 stacked eyes.
Output isolated in `out/verification/native_consumer_final_vr`. 10/119 pairs
exceed 6%, at destination frames 20, 21, 23, 24, 26 and 84, 85, 87, 88, 90.
The 64-frame defect remains. No cyan patches (median/maximum 0%). Both eyes of
0/119 and 20/21 were inspected: the flagged jump changes the terrain plane to
the known blurred/banded image. Stereo check exited 2, INCONCLUSIVE, with only
one bounded textured band. This is not a successful VR stability/depth check.

Last periodic report: 151156 lists, 4293914 entries, 3794220 replays, 494870
direct draws; zero shells/stencil/fallback/refusal. Bridge calls: visual
1086309, material 999388, state 3535644, world 499694, resource 2453383, shader
zero. No error/critical, Vulkan error/failure, overflow, exhaustion or
retirement-race matches. The exact-path validated process stopped and was
confirmed exited at 21:58:59.

All renderer processes are stopped and the original five-setting profile is
restored: autoplay/perf CSV on, 60-second capture delay, minimum 600 draws and
120 frames. No later-scene capture was repeated with this checkpoint. Fur,
stencil, battles, cutscenes, menus, transitions/reloads and full both-eye
coverage remain unqualified. The next ownership work is replacement of the
remaining visual/material/world/state adapters and entry/pass producers with
native scene/material/pass data, while keeping the multiview and late-scene
pixel failures open. No Quest work was performed.
