# Host renderer transition

The owner's active goal is to move **all rendering** to the host, remove the
Xbox 360 rendering model, and use modern GPU and VR techniques on desktop.
Gameplay remains recompiled. Preserve Blue Dragon's art style and readability;
materials, lighting, geometry, effects and asset formats may change for performance.
Quest 2 optimization follows the completed desktop transition.

## Completion requirements

All of these remain required; shipping an intermediate component is not completion.

- Host frame scheduling and pass construction, with no guest rendering execution
  or per-draw D3D/Xenos state translation in the finished frame.
- Host scene and object data, materials, animation and GPU skinning, shadows,
  reflections, particles/effects, post-processing, UI and presentation.
- Native asset conversion with explicit versioned formats, stable identities,
  desktop cooking and persistent output. Meshes, textures/mips/compression and
  materials must not depend on transient guest allocation addresses.
- Multiview stereo and correctly sized layered targets; host frustum and
  occlusion culling, instancing, indirect draws and suitable generated LODs,
  merged geometry and impostors.
- Remove EDRAM allocation, tile matching, seed copies and emulated resolves
  from the frame. Ordinary GPU resolves needed for native MSAA are distinct.
- Verify representative fields, battles, cutscenes, menus, scene transitions
  and reloads on desktop, including both eyes and animated effects. Record
  remaining guest rendering calls and resource dependencies explicitly.
- Only after that desktop gate: Quest 2 qualification and VR optimization,
  including foveation, toward the recorded 72 Hz / 1440x1584-per-eye target.

## Current conversion

Scene-image producer checkpoint (2026-09-05): current/next scene-table selection
and the complete scene-image binding callback now execute on the host, with
explicit native image handles, live dynamic adapters and counted null no-ops.
Both bindings are preflighted outside the video lock before publication.
All 15 material/texture/state CTests pass. Producer comparison is underway;
scene-table production, persistent scene associations, retained replay recipes,
the wrapper's blend/constants and full visual qualification remain. See
`research/20260905_0301_native-scene-textures.md`.

Reflection binding checkpoint (2026-09-05): supported direct phase-0 draws now
decode explicit selection and enable recipes, resolve current pass/table inputs
before submission, and discard the retained slot-5 image. All sub-draw bindings
are preflighted before dispatch. Null-selection inheritance remains an explicit
compatibility refusal, not an invented unbind; ordinary/animated overrides,
deferred/nonzero-phase recipes and persistent native scene associations also
remain. See `research/20260905_0144_native-reflection-selection.md` for source,
the initial diagnostic failure, corrected integration and verification scope.
This is not a completed reflection pass or fully native frame.
The corrected sampled transition has 490655 matching source checks, 179
unsupported scene-target callback draws and no slot-5 differences in the
bounded replay log. Supported GPU coverage here is disabled, pass-default
reflection selection; table-selected/enabled/dynamic cases remain unqualified.
The subsequent normal late run deadlocked between capture's texture lookup and
an IO upload, before producing captures. Source validation now snapshots at
draw time and resolves outside the video lock before template publication;
14 CTests and two source-boundary guards pass. The corrected normal run advances
through loading, with 1214021 matching source checks (including 6701 enabled
pass-default draws), but later rock-wall popping and damaged text remain:
108/119 large frame changes. Table/dynamic bindings remain unqualified. Normal
final-eye multiview has 0/119 large jumps but blurred/letterboxed, below-target
936x1030 eyes and inconclusive depth. See
`research/20260905_0235_reflection-validation-lock-order.md`.

Lighting checkpoint (2026-09-05): the complete lighting setup producer and its
reset/dimension helper execution now run on the host. Address-free records hold
ambient/camera/colour and shadow sampling inputs; supported direct phase-0
replays use the explicit shadow sampling record instead of retained constants.
The corrected short run has 13538 matching full publications and 200650 matching
direct-node input checks, with 0/119 large frame jumps. All 13 standalone
upload/state/verification/lighting tests pass. Engine scene/light descriptors,
texture associations, material staging/flush, other draw recipes and full-game
verification remain. The normal late run has 43580 host publications with zero
compatibility/reset calls and 700323 matching direct-node checks, but still
loses scenery and damages text (107/119 large frame changes). Normal multiview
has 0/119 large jumps but blurred/letterboxed eyes, inconclusive depth and the
same below-target 936x1030 eyes. See
`research/20260905_0121_native-lighting-pass.md`.

Verification follow-up (2026-09-05): replay diagnostics now retain bounded
examples in later scenes, report declared shader-input differences separately,
compare buffer fields without padding noise and flag incomplete compared-draw
counts. All 12 standalone upload/state/verification tests pass. Recurring reports
identify camera and animated-UV input mismatches; the sampled late baseline
still loses background surfaces and has damaged text. This is a diagnostic
checkpoint, not a new native producer or a fix for those pixels. See
`research/20260905_0053_recurring-draw-verification.md` for exact scope and runs.

Latest skin checkpoint (2026-09-05): explicit per-draw joint bindings now come
from model commands or deferred-entry indices. The host gathers each draw's
current palette before submission; matrix-value identity guessing and the
single final-node bone table are removed. Independent tests cover equal poses
that diverge, different per-draw bindings, capacity and transactional failures.
The normal late run records 787878 source checks with zero mismatches and
481158 replayed palettes. Inspected character stretching is gone, superseding
that specific failure in the earlier packet checkpoint below. Background
surfaces and text still fail: 110/119 frame pairs exceed the 6% jump threshold.
Normal final-eye multiview has 0/119 large jumps but inconclusive stereo depth.
See `research/20260905_0025_native-skin-bindings.md` for both runs and the
replay-off control. Skeleton/animation evaluation, pose sources, persistent
skin scene assets, discovery/list adapters and the shader-register ABI remain
explicit conversion boundaries; this is not a fully native skinned frame.

Packet checkpoint (2026-09-05): host draw packets now retain authoritative shader,
declaration and raster/blend/alpha intent throughout dispatch. Engine bind/setter
history no longer overwrites replay packets, and shared vertex decoding uses the
packet declaration. The new SDK-independent ownership regression test passes
alongside the other ten upload/state tests, and the desktop renderer linked
without rebuilding guest objects.

With replay enabled, the latest short flat and final-eye multiview sequences each
have 0/119 jumps over 6% and no cyan patches. Inspected eyes no longer have broad
horizontal banding. This supersedes the short-field flicker findings in earlier
checkpoints below; it does not qualify other scenes. Stereo depth remains
INCONCLUSIVE, blur/letterboxing remains, and actual eyes are 936x1030 instead of
the 1440x1584 target. At that checkpoint, a longer run using the prior late-scene
capture settings failed with deformed characters, disappearing scenery and
damaged text. See `research/20260905_0010_native-draw-late-scene.md`; zero allocation
failures and zero cyan patches do not qualify those pixels.
See `research/20260904_2348_native-draw-intent.md` for the replay-off control,
consumer overwrite trace, normal-path captures and remaining producer boundaries.
Earlier raster/blend/alpha draw-application counters included replay flushes;
their zero-mismatch setter checks did not establish packet ownership. The replay
comparator also did not dispatch its expected packet, so a zero pipeline-state
mismatch count could not detect this consumer bug.

The subsystem checkpoints below retain their historical verification outcomes.
Their remaining conversion boundaries still apply unless explicitly superseded.

`gpu/scene/native_mesh*` starts the native geometry asset boundary: loaded
model indices become triangle lists, GPU-ready vertex streams are persisted,
and native assets upload into shared host geometry arenas. Existing generated
LOD lists feed that same importer. The format contains no guest addresses.

This does **not** yet remove the draw-template interpreter dependency. The
importer currently retains packed vertex layouts understood by the existing
shaders, and its discovery is attached to replayed node draws. Complete native
material/layout definitions, asset-level loading, dynamic geometry, cache
streaming/eviction and replacement of the guest frame and draw producers are
still work to do. `bd_native_meshes` is on by default after the desktop checks
recorded in `research/20260904_1713_native-mesh-assets-and-capture-ownership.md`.
The counters cover indexed replays, not every draw in the game.

`gpu/scene/native_material*` now decodes named diffuse, specular/shininess and
reflection-colour properties from model commands into host-owned records. The
supported direct-tree phase-0 draws compose these with the object's colour,
without reading a sibling draw or the shared material staging globals.
`bd_native_materials` is on by default. Unsupported/ambiguous cases retain the
tracked compatibility path; this is not a complete native material system.
Materials are now shared immutable assets with stable content IDs, a checked
little-endian `.bdmat` format, independent cooking/loading and bounded residency.
The lighting-model slot includes a reserved Cel value; no native cel shader is
claimed. See [the format and cooker](NATIVE_MATERIAL_FORMAT.md). Complete texture
associations, mesh/material scene-asset loading, list-entry/phase-1 recipes, complete
lighting/shader definitions and shader-ABI replacement remain required. See
`research/20260904_1748_native-material-properties.md` for exact coverage and
correctness-only comparisons, and
`research/20260904_1806_persistent-native-material-assets.md` for independent
loading, cold/warm desktop captures and the persistent-asset tests.

`gpu/scene/native_shadow*` now composes a named receiver-shadow policy from
current node visibility and decoded model controls, instead of retaining that
decision in an old draw template. `bd_native_shadow_inputs` is on by default
for supported direct-tree phase-0 draws. The pure policy and stamp checks have
standalone tests; sampled and normal desktop checks found no input-composition
mismatches. This is still an import boundary: the pass enable, visibility stamps
and frame counters remain guest-produced, and the result still enters the old
shader ABI. List/phase-1 recipes and persistent shadow policy in native material
assets remain unconverted. It does not fix the recurring multiview defect.
See `research/20260904_2041_native-shadow-receiver-inputs.md` for exact coverage,
flat captures and the failed/inconclusive VR checks.

Deferred depth ordering and bounded allocation/batch planning now execute on
the host (`gpu/scene/deferred_work.h` and the temporary `deferred_list.cpp`
bridge). Replayed batches refresh world/palette and relocate their material
self-reference instead of retaining the original pooled pointer. The native
core has standalone capacity, ordering and relocation tests. This still
publishes big-endian entry images; remaining entry fields, material/pass records
and engine storage require conversion. The consumer replacement below now owns
the consuming loop. See `research/20260904_2055_host-deferred-work.md` for the
earlier allocation/sort checkpoint.
Its short flat check passes, but multiview still shows 10/119 jumps and the
later scene 79/119 with missing scenery/damaged text. Neither allocation/sort
conversion nor pointer relocation resolves those visual failures.

`gpu/scene/deferred_depth.h` now produces initial and replay depth on the host
from explicit bounds/far-extent or fixed policies. Replayed keys use current
world/view inputs, not old numeric depth. Whole-batch preflight includes depth
validation, and every entry must agree with its recipe's matrix source.
`bd_native_deferred_depth` defaults on. The input comparison recorded 20483
checks with zero mismatches; the final normal-path flat sequence has 0/119
jumps over 6% and no cyan patches. Multiview still has 10/119 jumps at the
64-frame cadence, with an inconclusive stereo-depth check. See
`research/20260904_2122_live-native-deferred-depth.md`.
The object/view transforms remain engine-produced; bounds/policy discovery is
not yet native scene-asset loading. Engine storage and other entry fields remain
tracked boundaries. The prior later-scene failure has
not been requalified by this short-run checkpoint.

`gpu/scene/deferred_consumer.cpp` now owns deferred-list iteration, visual-switch
scheduling, CPU bone gathering, material constant publication, ordinary/fur/
stencil surface expansion, direct draw issuance and list cleanup. Its valid-input
path replaces the original `sub_8227F360` loop. `bd_native_deferred_consumer`
defaults on; the explicit compatibility switch/import fallback is counted.
Standalone surface-policy and shader-ABI packing tests pass. The final flat
sequence has 0/119 jumps and no cyan patches; final multiview still has 10/119 jumps
at a 64-frame cadence and an inconclusive stereo-depth result. See
`research/20260904_2154_host-deferred-consumer.md` for verification and limits.

This is not a fully native frame: visual/material/shader callbacks and
state/resource adapters remain, with separate bridge counters. Some resource
adapters already route to host hooks, so these are boundary-call counts, not a
precise guest-instruction census. Engine entry storage, resource/declaration
associations, shader-register packing and replay's retained-state assumptions
still need replacement. Fur/stencil policies have standalone coverage but the
captured field does not exercise those GPU paths. The known later-scene failure
and full-game/both-eye acceptance gates remain open.

Object/pass transform publication is now host-produced too
(`gpu/scene/native_transform*`). The normal `bdBuildViewMatrix` path replaces
the guest producer, its default callback, transpose/multiply helpers and
constant setter. Camera interpolation/XR view composition feeds it directly
from native memory. `bd_native_transforms` defaults on; comparisons and
compatibility calls are counted independently. The final comparison recorded
826215 checks with no cache/constant/mask mismatches or compatibility calls,
including 203 nonfinite loading updates previously refused. Native assets
still use strict finite-value validation. See
`research/20260904_2216_native-render-transforms.md`.
The final normal flat sequence has 0/119 jumps and no cyan patches. Normal
desktop multiview produced 2387514 native transform updates with zero
comparison/compatibility calls, but still has 10/119 jumps at the 64-frame
cadence, blurred/banded eyes and an inconclusive stereo-depth result.
Engine object/camera/projection sources, inherited matrix cache and the
shader-register publication ABI remain temporary boundaries. This does not
replace native scene/pass scheduling or fix/qualify the previously documented
multiview and later-scene failures.

Raster/depth/stencil intent now lives in named host state (`native_raster*`).
The normal path replaces 15 `bdSetRenderState` setters and copies live native
fields at draw time, removing the per-draw engine raster-cache read/conversion.
`bd_native_raster` defaults on; diagnostic comparison defaults off. The live
comparison recorded 1491692 setter checks and 3070903 ordinary draw-state
checks, with zero publication mismatches/cache drift and one bootstrap import.
The normal flat sequence has 0/119 jumps and no cyan patches. Normal desktop
multiview still has 10/119 jumps at the 64-frame cadence, blurred/banded eyes
and an inconclusive stereo-depth result; it does not qualify VR correctness.
See `research/20260904_2238_native-raster-intent.md` for tests and captures.
Getter/cache/register shadows remain explicit engine adapters. Sampler,
other-state and material/pass producers, CCW stencil behavior, replay recipes and native
scene/pass assets remain unconverted. Field captures do not exercise stencil
operation/mask setters. This is not full frame or both-eye qualification.

Blend intent now also lives in named host state (`native_blend*`), with eight
host setters and no normal per-draw blend-register read/conversion.
`bd_native_blend` defaults on; comparison defaults off. The comparison recorded
4002268 setter checks and 3073105 ordinary draw checks without publication
mismatches, untracked blend writes or compatibility calls, using one bootstrap
import. Its short flat sequence has 0/119 jumps and no cyan patches.
The normal flat path also has 0/119 jumps and no cyan patches, with 4007188
native blend updates and no blend comparison/compatibility calls.
Normal desktop multiview records 8979675 host blend updates without blend
comparison/compatibility calls, but still reproduces 10/119 jumps at the 64-frame
cadence, banded/blurred eyes and inconclusive stereo depth. This is not a VR pass.
The source trace did not substantiate the earlier claim of inline device blend
writers outside the SDK setters; unrelated matching object offsets are not D3D
device writes. Verification still explicitly checks for untracked writers.
See `research/20260904_2302_native-blend-intent.md`. This is not a complete native
material/pass producer: getter/cache shadows, blend constants,
other-state execution and retained replay recipes remain. Separate-alpha and
operation setters have standalone coverage but no field GPU exercise so far.

Alpha cutout/coverage intent is now host-owned too (`native_alpha*`), with four
host setters and live ordinary-draw composition instead of retained pipeline
intent. `bd_native_alpha` defaults on; verification defaults off. The shared
C++/HLSL predicate supports all eight compare modes through specialization, and
the reference uses the SDK's exact 1/255 scale, correcting the former 1/256 hook.
CPU tests and regenerated SPIR-V verify the comparison contract, including
explicit ordered-NaN behavior. Publication comparison recorded 7196829 setter
checks and 7108657 draw-intent checks with zero mismatches/drift/compatibility
calls. The final normal flat path records 2274942 native updates without alpha
comparison/compatibility calls, 0/119 frame jumps and no cyan patches.
Normal desktop multiview records 13662279 native alpha updates without
comparison/compatibility calls, but the final-eye sequence still has 5/119 jumps
in one flicker cluster, blurred/banded eyes and inconclusive stereo depth. This
window does not establish recurrence or improvement over earlier two-cluster
captures. Actual eyes are 936x1030, not the requested 1440x1584 target.
See `research/20260904_2327_native-alpha-policy.md`. Engine getter/cache shadows,
native material/pass producers, replay recipes and the shader-register ABI remain.
The field exercises only GE and no alpha-to-coverage requests; other comparison
GPU paths, multisample coverage output and custom coverage offsets are not
qualified. This does not establish full frame or both-eye correctness.

Static textures now cross a persistent native boundary too: `.bdtex` files
preserve BC/RGBA data, mips, cube faces and volume slices with address-free
content IDs. The SDK-independent mip cooker persists missing chains; subsequent
loads use a versioned recipe cache without regenerating them. `bd_native_textures`
is on by default. See [the native texture contract](NATIVE_TEXTURE_FORMAT.md).
CPU assets are shared and budgeted. A device-owned native GPU store now shares
images, views and descriptors by content ID, with native handles and fence-gated
retirement independent of guest wrappers. The remaining resource bridge only
borrows those bindings. Explicit immutable material slots now hold native image
handles directly, including cube/volume companions. Stable sampler recipes use
native descriptors without per-replay fetch decoding. Inherited/dynamic inputs,
asset-level scene loading and guest draw/pass replacement remain required.
Cold/warm desktop and independent-loader evidence is recorded in
`research/20260904_1833_native-textures-and-persistent-mips.md`.
Shared GPU lifetime tests, runtime reuse/retirement and flat/multiview captures
are recorded in `research/20260904_1854_shared-native-texture-gpu-ownership.md`.
The native binding checkpoint, compound-recipe lifetime fixes and 120-frame
flat capture are recorded in
`research/20260904_1946_native-material-texture-bindings.md`. Its capture has no
jumps above 6% or cyan patches, but the longer run later exhausted a 32 MiB
constant-buffer slot. That checkpoint was not a clean long-session qualification;
the wrapping hazard is addressed by the upload separation below.

Resource staging now uses bounded, fence-reclaimed **host upload pages**,
independent of the shader-register buffer. Native textures and the native UI
use the host API directly; compatibility bulk adapters share it. Shader storage
no longer wraps on exhaustion, and transient vertex streams cannot be cached
as immutable cross-frame geometry. See [the upload contract](HOST_UPLOAD_ARENA.md)
and `research/20260904_1959_host-upload-pages.md`. A longer loading run no longer
reported overflow, but its later scene still had severe dark/missing-geometry
frames. This remains a correctness failure, not full transition qualification.
The longer baseline was rerun after the final transient-stream lifetime fixes:
77 of 119 frame pairs exceeded the 6% jump threshold and inspected frames still
showed broken geometry/text. Upload separation did not solve that scene.

The earlier diorama control exposed a 64-frame lighting flash in the existing
template path, present with native meshes or native materials disabled too.
The upload-page checkpoint's multiview check reproduced that cadence: 10 jumps in
119 frame pairs, with no upload or constant-storage errors. The presented eyes
in that distant view still do not establish a stereo-depth verdict.
The native packet-ownership fix above removes those jumps and broad banding in
its short capture, but still does not establish stereo depth or full-game
correctness. The host transition is not complete.

Shared working instructions live in [AGENTS.md](../AGENTS.md). The former
CLAUDE.md is preserved as a [historical snapshot](archive/CLAUDE_2026-09-04.md),
not current guidance. Use current code, dated evidence and this scope when
deciding what remains. Never claim that a desktop timing proves a Quest
performance result.
