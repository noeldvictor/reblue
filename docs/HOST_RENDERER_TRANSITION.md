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
shaders, and its discovery is attached to replayed node draws. Explicit native
material/layout definitions, asset-level loading, dynamic geometry, cache
streaming/eviction and replacement of the guest frame and draw producers are
still work to do. `bd_native_meshes` is on by default after the desktop checks
recorded in `research/20260904_1713_native-mesh-assets-and-capture-ownership.md`.
The counters cover indexed replays, not every draw in the game.

The diorama control exposes a remaining 64-frame lighting flash in the existing
template path, present with native meshes disabled too. The presented eyes in
that distant view do not establish a stereo-depth verdict. Neither limitation
is a completed VR qualification or a reason to claim the host transition done.

Shared working instructions live in [AGENTS.md](../AGENTS.md). The former
CLAUDE.md is preserved as a [historical snapshot](archive/CLAUDE_2026-09-04.md),
not current guidance. Use current code, dated evidence and this scope when
deciding what remains. Never claim that a desktop timing proves a Quest
performance result.
