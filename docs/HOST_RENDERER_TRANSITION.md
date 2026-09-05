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

This is not a fully native frame: visual/material/world/shader callbacks and
state/resource adapters remain, with separate bridge counters. Some resource
adapters already route to host hooks, so these are boundary-call counts, not a
precise guest-instruction census. Engine entry storage, resource/declaration
associations, shader-register packing and replay's retained-state assumptions
still need replacement. Fur/stencil policies have standalone coverage but the
captured field does not exercise those GPU paths. The known later-scene failure
and full-game/both-eye acceptance gates remain open.

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

The diorama control exposes a remaining 64-frame lighting flash in the existing
template path, present with native meshes or native materials disabled too.
The final upload-page multiview check reproduces that cadence: 10 jumps in
119 frame pairs, with no upload or constant-storage errors. The presented eyes
in that distant view still do not establish a stereo-depth verdict.
Neither limitation is a completed VR qualification or a reason to claim the
host transition done.

Shared working instructions live in [AGENTS.md](../AGENTS.md). The former
CLAUDE.md is preserved as a [historical snapshot](archive/CLAUDE_2026-09-04.md),
not current guidance. Use current code, dated evidence and this scope when
deciding what remains. Never claim that a desktop timing proves a Quest
performance result.
