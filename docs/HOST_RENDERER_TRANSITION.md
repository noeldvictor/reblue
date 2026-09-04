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
claimed. See [the format and cooker](NATIVE_MATERIAL_FORMAT.md). Native texture
bindings, mesh/material scene-asset loading, list-entry/phase-1 recipes, complete
lighting/shader definitions and shader-ABI replacement remain required. See
`research/20260904_1748_native-material-properties.md` for exact coverage and
correctness-only comparisons, and
`research/20260904_1806_persistent-native-material-assets.md` for independent
loading, cold/warm desktop captures and the persistent-asset tests.

The diorama control exposes a remaining 64-frame lighting flash in the existing
template path, present with native meshes or native materials disabled too.
The presented eyes in that distant view do not establish a stereo-depth verdict.
Neither limitation is a completed VR qualification or a reason to claim the
host transition done.

Shared working instructions live in [AGENTS.md](../AGENTS.md). The former
CLAUDE.md is preserved as a [historical snapshot](archive/CLAUDE_2026-09-04.md),
not current guidance. Use current code, dated evidence and this scope when
deciding what remains. Never claim that a desktop timing proves a Quest
performance result.
