# Native material assets, version 1

This format is the first persistent **colour/shininess recipe** component of
the native material system. It is not a complete texture, shader or pipeline
definition. No game-derived files are distributed with the project.

`NativeMaterialAsset` contains named properties and a lighting-model slot.
`NativeMaterialLibrary` cooks, deduplicates and loads these assets without
guest memory, game/runtime headers or a GPU. Draws hold immutable shared
references; their lifetime does not depend on a guest allocation or discovery
cache entry. `Load(id)` needs only the material directory and content ID.

## File contract

Every `.bdmat` file is exactly 68 bytes. Integers and IEEE-754 binary32 floats
are explicitly little endian; no C++ struct padding is serialized.

| Byte offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 8 | Magic/version: `42 44 4d 41 54 00 01 00` |
| 8 | 8 | FNV-1a-64 of bytes 16..67 |
| 16 | 4 | Lighting model: 0 OriginalLit, 1 Cel |
| 20 | 4 | Property flags, below |
| 24 | 4 | Shininess, integer 0..255 |
| 28 | 12 | Diffuse RGB multiplier |
| 40 | 12 | Specular RGB |
| 52 | 16 | Reflection RGBA |

Flags: bit 0 enables diffuse modulation, bit 1 marks the diffuse multiplier
known, bit 2 specular RGB known, bit 3 reflection RGBA known, bit 4 shininess
known. Other bits are invalid. Colour components are finite in [0, 1].
Unknown fields encode as zero; negative zero encodes as positive zero.
An unknown field is **not** a white default or permission to use a sibling's
value. The decoder rejects noncanonical encodings, unknown versions/models,
bad flags, nonfinite/out-of-range values, checksum failures and wrong sizes,
without changing the caller's existing asset.

The filename is the lower-case, zero-padded 16-digit FNV-1a-64 of the **whole
canonical file**, followed by `.bdmat`. The offset basis is
14695981039346656037 and the prime is 1099511628211. Hashes wrap modulo 2^64.
Identity includes the lighting model and format version, not guest addresses,
source buffer records, per-object colour or shader register numbers. This is
an accidental-corruption/content-identity mechanism, not authentication. A
conflicting resident/loaded asset is refused, never silently aliased.

The Cel value reserves the requested optional lighting-model slot. Its native
shader is not implemented: the current composer refuses that model rather
than silently shading it as OriginalLit. Runtime imports use OriginalLit.

## Cooking and loading

The desktop renderer stores derived files in
`<cache_root>/native_materials/v1/`. Runtime discovery still reads model
commands to establish a draw's recipe and identity; matching and scene loading
are not yet independent native asset producers. Once resolved, shared native
material assets supply the replayed values. Dynamic object tint still crosses
the existing scene boundary.

The library defaults to 16384 resident assets. Only unreferenced,
least-recently-used assets may be evicted. Live draw/discovery references pin
their assets; a full pinned library refuses growth and reports it. The
temporary discovery cache has separate 4096-entry and 8 MiB vector-storage
limits. These are component limits, not a completed 1.5 GB whole-game budget.

Interrupted/invalid derived files are rejected and recooked from owned source
data when available. Write failures retain usable in-memory data but increment
an explicit counter. `Load(id)` never cooks or repairs a file on its own.

Build the standalone cooker with the material tests (Clang toolchain as needed):

```sh
cmake -S tools/native_material_test -B out/native_material_test -G Ninja
cmake --build out/native_material_test
ctest --test-dir out/native_material_test --output-on-failure

# Validate every material and load it by ID, with no game or GPU:
out/native_material_test/native_material_cook --verify <material-directory>

# Cook an extracted, big-endian model command stream from your own assets:
out/native_material_test/native_material_cook --commands-be <commands.bin> <material-directory>
```

On Windows the executable names end in `.exe`. Command input is bounded to
65536 words and follows the supported phase-0 decoder. The tool prints the
range-to-material-ID mapping; it does not claim to export a complete model,
extract archives or convert textures. Asset-level geometry/material binding,
textures, native lighting/shaders and the remaining scene recipes still need
conversion.
