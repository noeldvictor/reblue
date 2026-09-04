# Persistent native material assets and source-free loading

2026-09-04. Continues the complete host-renderer goal; this is not a completed
frame, full-game verification or Quest qualification. The previous turn
established the phase-0 material decoder and its limits in
`20260904_1748_native-material-properties.md`.

## What changed

Material colour/shininess recipes now live in shared immutable native assets,
not optional property copies embedded in every captured subdraw. The
`NativeMaterialLibrary` deduplicates by canonical content ID and supports
`Load(id)` with only a directory, no guest pointers, game runtime or GPU.
Runtime imports resolve these assets once per discovered model range; replay
holds a pin and composes from the asset. Warm resolution uses the decoded
native file after validating it against the requested content.

The `.bdmat` v1 contract is documented in `docs/NATIVE_MATERIAL_FORMAT.md`.
It is 68 explicit little-endian bytes with version magic, checksum, lighting
model, known-field flags and normalized colour/shininess properties. There
are no source buffer records, guest addresses, per-object tint or shader
register numbers in the file. Unknown values and negative zero canonicalize
before hashing. Content identity includes the version and lighting-model slot.
Cel is reserved but not implemented; unsupported composition refuses it.

Library residency is limited to 16384 assets, with unreferenced LRU eviction.
Active draw/discovery pins cannot be evicted. A fully pinned library refuses
growth and counts it. The temporary model-command discovery cache separately
limits vector storage to 8 MiB and entry count to 4096. These component bounds
do not prove the whole game's 1.5 GB headset asset budget.

An independent desktop tool cooks supported big-endian command streams and
validates/loads real native material files by ID. It uses the same format and
library as the renderer, with no SDK/runtime dependency. It does not extract
archives or cook a complete model/scene by itself.

## Tests and desktop evidence

Existing Windows Vulkan tree, target `reblue`, OpenXR on, D3D12 off, Clang
22.1.8. Final incremental build linked `reblue_vk.exe`; codegen reported the
module up to date and no guest translation units rebuilt.

Material CTest 1/1 passed, now covering the original command decoder plus:

- Explicit byte order and a cross-implementation golden content ID.
- Every truncated prefix and every single-byte corruption, trailing bytes,
  unknown model/flags, excessive shininess, invalid floats, and rechecksummed
  noncanonical encodings.
- Transactional read/write encoding, unknown-field and negative-zero
  canonicalization, and unsupported lighting-model refusal.
- Cooking, pointer deduplication, source-free disk-only restart loading,
  asset lifetime after library destruction, pinned-capacity refusal and
  unreferenced eviction.
- Interrupted writes, recooking/reloading a corrupted derived file, valid
  bytes under a wrong ID, observable write failure and a zero-capacity library.

Native mesh CTest 1/1 and stereo Python tests 2/2 also passed. These tests do
not claim a new stereo image qualification.

The command-line cook path was exercised using the synthetic command fixture
from the tests (including operand bytes equal to the terminator). It produced
two native materials; `--verify` independently loaded and composed all three
known fields for both. Then the same tool loaded all 10 actual game-derived
files under `out/build/win-amd64-release/cache/native_materials/v1/`, with no
game process dependency: diffuse/specular/reflection composable counts 10/10/0.
Those files total 680 payload bytes; filesystem allocation overhead is not
included, and this is not a complete material/texture asset size.

Both desktop runs used the original flat-view profile plus
`bd_native_materials_verify=true`. Native meshes/materials remained on by
default. Capture after 60 seconds, minimum 600 draws, 120 final 1920x1080
frames. No performance A/B or resolution reduction was used.

| Run | Native assets / source checks | Images |
| --- | --- | --- |
| Cold, `reblue_633.log`, `out/verification/native_material_cold` | 10 cooked, 0 loaded, 10 resident, 513 memory hits; zero invalid files/write failures/budget refusals. Last wrong/checked diffuse 0/314782, specular 0/297034; replay compositions 527758/527506/0 | 120 frames, 119 pairs, zero jumps above 6%, zero cyan patch frames. Village image inspected |
| Restart, `reblue_634.log`, `out/verification/native_material_warm` | 0 cooked, 10 loaded, 10 resident, 513 memory hits; zero invalid files/write failures/budget refusals. Last wrong/checked diffuse 0/511634, specular 0/483086; replay compositions 860181/859929/0 | 120 frames, 119 pairs, zero jumps above 6%, zero cyan patch frames. Village image inspected |

The images retain the character, foliage, buildings, terrain and shadows;
no new visible material artifact was found in this field slice. Reflection
properties are covered by synthetic tests but were not exercised in these
flat game captures. No errors/critical messages, Vulkan errors or device-loss
messages were found in these two logs. Test processes were stopped and the
original five-line desktop profile restored. Game-derived files and captures
remain ignored, not committed.

## Still required

This advances the persistent asset boundary; it does not replace the current
draw/pass producers. Runtime discovery still reads model commands to identify
the material, per-object tint still crosses the guest scene boundary, and
known properties still reach the existing shader-register ABI at the final
compatibility step. Native texture/lighting definitions, asset-level
mesh-to-material bindings and scene loading, native shaders, list/phase-1
recipes, and all remaining frame ownership are still required.

The existing 64-frame multiview lighting defect remains open. This turn made
no new multiview-depth or Quest claim and did not run on Quest/Thor. Full
fields/battles/cutscenes/UI/transitions/reloads and both eyes must still pass
the desktop gate before device optimization.

The previous push was blocked by automatic review pending explicit approval.
This work proceeds locally; no alternative push route was attempted.
