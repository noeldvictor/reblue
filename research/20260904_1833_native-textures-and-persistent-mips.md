# Native textures and persistent mip cooking

2026-09-04. Advances the native asset boundary toward the complete host frame;
does not claim full rendering ownership or Quest qualification.

## Boundary and implementation

Inspected `native_texture_mirror.cpp`, `texture_upload.cpp`, `host_mips.cpp`,
the resource/bindless retirement paths and `config/hooks/physical_buffers.toml`.
Static content was already untiled into CPU-owned block/texel buffers before
upload. This is now the import boundary for address-free native texture data.
No generated guest source or hook configuration was changed.

`NativeTextureData` stores BC1/BC2/BC3/RGBA8 UNORM, explicit dimensions/mips and
tightly packed images. Cube faces and volume slices are preserved. The checked
`.bdtex` v1 format and content ID include all subresources, not just a tiled
base or a fetch constant containing allocation addresses. Row/slice padding
is stripped before hashing and restored only for host upload footprints.
See `docs/NATIVE_TEXTURE_FORMAT.md` for the exact format.

`NativeTextureLibrary` owns shared immutable CPU assets and loads by ID without
the guest, SDK or GPU. A 128 MiB accounted storage budget and 8192-entry ceiling
bound residency; only unpinned LRU entries may be evicted. Write errors,
invalid files and budget refusals are observable. Unsupported/refused imports
retain an explicitly tracked compatibility path.

The mip decoder/filter/compressor in `host_mips.cpp` now takes native format
and asset data, with no SDK/Xenos dependency. Its existing BC alpha behavior,
box filter and stop-at-4x4 rule are preserved. `host_mips_bridge.cpp` contains
the remaining old-format/upload adapter. The default native path uses a
versioned base-content-to-cooked-ID recipe index: after restart it validates
the native result and base bytes, then loads the mip chain without generation.
This is actual reuse, not merely writing a cache after recomputing every mip.

`BuildNativeTexture` accepts the native asset, reconstructs host footprints and
uploads it. The result remains a temporary `GuestTexture` bridge, which pins
the CPU asset and retains existing fence-safe GPU image/view/descriptor
lifetime. GPU images are not yet shared/deduplicated, and scene/material
bindings still come from the original resource/draw producers. Their removal
is still required. Converted recorder texture identities now use the stable
native ID; legacy fallback identities retain their old limitations.

## Standalone verification

New texture CTest 1/1 passed. It covers all four formats, byte order, every
truncated prefix/single-byte corruption, rechecksummed invalid geometry,
trailing bytes, transactional failure, padded volume row/slice removal,
cube face preservation/identity, mip round trips, unchanged bases, unsupported
generation, source-free loading, recipe restart reuse, changed-base recooking,
pinned byte budgets, eviction and interrupted-cache repair.

The standalone CLI was run twice on a synthetic native BC base. First run:
`09ff242bdbe328f6.bdtex`, two levels, one generated chain. Second run: same ID,
zero generated, one cached. `--verify` then loaded the result independently.

The tool also loaded the actual desktop game's native files by ID with no
game/runtime/GPU dependency: **621 textures, 4996 images, four cubes, seven
volumes, 95,360,656 file bytes**. There were 185 mip recipe reference files.
These are this slice's derived assets, not a whole-game budget or GPU memory
measurement. No game-derived files are committed.

## Renderer build and pixels

Reused `out/build/win-amd64-release`, target `reblue`, Clang 22.1.8, D3D12 off,
OpenXR on. The renderer linked successfully. Resource-header changes rebuilt
dependent host sources; codegen reported up to date and no guest translation
units rebuilt. Existing unrelated deprecation/designator warnings remain.
Native material CTest 1/1, mesh CTest 1/1 and stereo Python tests 2/2 passed too.

Both runs used autoplay, capture after 60 seconds, minimum 600 draws and
120 final 1920x1080 frames. `bd_native_materials_verify=true` checked the prior
material conversion. Cold explicitly enabled `bd_native_textures`; warm used
the final binary's default-on setting with no texture override. No performance
A/B, resolution reduction or new filtering-quality tradeoff was made.

| Run | Assets and source checks | Captures |
| --- | --- | --- |
| Cold `reblue_635.log`, `out/verification/native_texture_cold` | Last periodic upload report: 614 cooked, 0 loaded, 180 generated mip chains / 8 cached. Zero invalid/write/budget failures. Final material wrong/checked diffuse 0/489423, specular 0/462075 | 120 frames, 119 pairs, zero jumps above 6%, zero cyan patch frames. Village image inspected |
| Warm `reblue_636.log`, `out/verification/native_texture_warm` | Last periodic upload report: 0 cooked, 614 loaded, 0 generated mip chains / 188 cached. Zero invalid/write/budget failures. Final material wrong/checked diffuse 0/659672, specular 0/622724 | 120 frames, 119 pairs, zero jumps above 6%, zero cyan patch frames. Village image inspected |

Upload reports occur every 64 requests, so those snapshots are not asserted to
be the final whole-run counters. The independent loader verified all 621 files;
none of the `.bdtex` files had a write time in the warm run. Images retain the
character, terrain/foliage/building textures and shadows without a new visible
artifact in this field slice. No error/critical, Vulkan-error or device-loss
messages were found in either log. Reflection *material constants* were not
exercised by the flat capture checks; cube/volume texture data was verified
by the asset tests/loader, not a dedicated visual reflection/fur scene.

Both test processes were stopped and the original five-line profile restored.
The new setting remains enabled by default in source.

## Remaining work

Native scene loading, material-to-texture associations and sampler definitions,
shared GPU image ownership, remaining texture imports and headset-specific
formats are not complete. Original archive/creation discovery still supplies
the initial untiling boundary. Guest draw/pass producers, shaders and dynamic
rendering ownership still need replacement across the full frame.

The 64-frame multiview lighting defect remains open. This turn did not perform
a new stereo-depth qualification or run on Quest/Thor. Fields, battles,
cutscenes, UI, transitions/reloads and both eyes still gate device work.
Commits remain local pending the existing push-approval review; no alternate
network push route was attempted.
