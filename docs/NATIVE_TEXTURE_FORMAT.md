# Native texture assets, version 1

Native textures contain ordinary tightly packed, little-endian BC1/BC2/BC3 or
RGBA8 UNORM data, independent of Xbox format/fetch constants, tiling, physical
addresses and upload-row alignment. Original supplied mips, cube faces and
volume slices are preserved. The format and cooker require no game runtime,
SDK or GPU. Derived game assets are not distributed with the repository.

## File contract

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 8 | Magic/version: `42 44 54 45 58 00 01 00` |
| 8 | 8 | FNV-1a-64 checksum of all bytes from offset 16 |
| 16 | 4 | Format: 1 BC1, 2 BC2, 3 BC3, 4 RGBA8, all UNORM |
| 20 | 4 | Dimension: 1 2D, 2 cube, 3 volume |
| 24 | 4 | Base width |
| 28 | 4 | Base height |
| 32 | 4 | Base depth; 1 for 2D/cube |
| 36 | 4 | Mip count |
| 40 | remaining | Images in layer-major, then mip order |

All integer fields are unsigned little endian. Cubes have six layers; other
dimensions have one. Each mip's dimensions are `max(base >> level, 1)`.
Volume slices are contiguous inside each mip. BC images have ceil(width/4)
by ceil(height/4) blocks, 8 bytes per BC1 block or 16 per BC2/3 block. RGBA8
has four bytes per texel. Rows and slices contain no alignment padding.

Width/height are limited to 16384, depth to 2048, mip count to the dimension's
complete chain, and the entire file to 64 MiB. Cubes must be square. The
decoder requires exactly the implied payload size and rejects unknown formats,
dimensions, versions, malformed geometry, truncation, trailing data and bad
checksums transactionally.

The filename is the zero-padded, lower-case, 16-digit FNV-1a-64 of the entire
canonical file plus `.bdtex`. FNV uses offset basis 14695981039346656037 and
prime 1099511628211, with modulo-2^64 arithmetic. Identity covers every mip,
face and slice, never source addresses, row padding or sampler state. This is
a content/corruption check, not authentication. Conflicting content under an
already resolved ID is refused.

## Mip cooking and ownership

`NativeTextureLibrary::Resolve(data, true)` cooks a missing BC1/2/3 2D chain.
Recipe v1 preserves the existing alpha-aware box filter and BC compressor,
stopping at the 4x4 block level. The base is unchanged. Inputs must be at least
8x8; generation is limited to 16 million base texels to bound scratch memory.
Other mip-generation formats/dimensions remain unsupported, not silently
converted to a different format or resolution.

`mips-v1/<base-content-id>.ref` contains an 8-byte magic
`42 44 4d 49 50 00 01 00` followed by the cooked texture ID as little-endian u64.
The base ID hashes its canonical one-level native file. A restart validates
the referenced asset, its dimensions, format, expected mip count and exact
base bytes, then loads it without running the mip generator. A missing or
invalid derived result is recooked when source data is available. A change to
the filter/compressor recipe must change this recipe version.

The library owns shared immutable CPU assets, with a default 128 MiB accounted
storage budget and an 8192-entry ceiling. It evicts only unpinned least-recently
used entries; a full pinned library refuses growth. Invalid files, write
failures and budget refusals are counted. These bounds are not a whole-game
CPU/GPU budget or proof of the 1.5 GB headset asset target.

The desktop path is enabled by `bd_native_textures`. Native scene code can use
`AcquireNativeTextureGpu(asset)` directly: it returns a shared native image,
view and bindless descriptor, with no guest resource object or format. Matching
content IDs reuse the same GPU objects and avoid another upload. The temporary
`BuildNativeTexture` adapter borrows this binding for the existing draw path;
destroying one adapter does not retire another owner's descriptor.

The GPU store belongs to the host device. It accounts a 256 MiB image-payload
budget and 8192 entries, including images waiting for a fence. These bytes do
not include driver allocation alignment/metadata. Unpinned entries are marked
after a frame slot's entry drain and released only at that slot's next completed
fence. Reacquisition cancels retirement. Descriptor nulling/free precedes view
and image destruction. Native handles must remain live through command
recording; callers reacquire by asset through the renderer lock, not by promoting
weak references concurrently with reclamation. Each padded upload subresource
is limited to 64 MiB of staging scratch.

GPU copy sources now use [host upload pages](HOST_UPLOAD_ARENA.md), not the
shader-register ring. Native texture uploads cannot wrap over shader constants
or earlier subresources in a loading burst. Per-slot fences cover page reuse
and retirement; the shared 256 MiB staging budget is separate from image
residency and CPU asset budgets.

Standalone tests exercise that same residency implementation, with instrumented
image destruction and descriptor lifetime. This does **not** replace the guest
draw/pass producers or supply asset-level native scene loading.
Unsupported/failed imports retain the tracked compatibility path. Runtime GPU
uploads, reuse, retirement and refusals are reported separately from CPU cooking.

## Native material sampling boundary

`NativeTextureBinding` owns the primary GPU asset and explicit volume-slice or
cube companions. Converted material slots publish these descriptors directly;
their captured guest pointer and allocation address are empty. Stable sampler
recipes are imported once into `RenderSamplerDesc`, then resolved using host
anisotropy/mip policy and a padding-independent key covering every descriptor
field. Those slots do not read/decode guest fetch words during normal replay.
Verification and recording can still compose the compatibility words.

This conversion covers explicitly bound, immutable material textures only.
Inherited slots, mutable surfaces, aliases, unresolved companions and unstable
samplers remain compatibility inputs. They cannot safely be frozen as static
assets. The existing shader heap/register ABI also remains. The default-on
`bd_native_texture_bindings` switch may be disabled for correctness comparisons;
restart the process for a clean comparison with newly captured recipes.

Temporary node recipes hold strong native handles through command recording.
Texture replacement/eviction and geometry invalidation expire imported recipes
at lookup; they do not erase unrelated visual/pass producer history. Untouched
recipes retire after 300 frames, with a 4096-node ceiling. Direct draws and their
deferred entries retire together, and volatile direct parts must not be mistaken
for list-only nodes. Native GPU release still follows the fence policy above.
These are bounded compatibility recipes, not a finished native scene database.

The binding tests cover dimensional descriptor mapping, explicit companions,
ownership after importer release, fence retirement, compound-recipe pruning,
volatile direct parts and complete sampler identity. They use vendored Plume
type headers, but no game runtime or GPU. See the dated
[binding evidence](../research/20260904_1946_native-material-texture-bindings.md)
for the desktop slice and remaining correctness limitations.

## Standalone tool

```sh
cmake -S tools/native_texture_test -B out/native_texture_test -G Ninja
cmake --build out/native_texture_test
ctest --test-dir out/native_texture_test --output-on-failure

# Validate and load every native texture by content ID:
out/native_texture_test/native_texture_cook --verify <native-texture-directory>

# Cook the base of a native 2D asset into a persistent BC mip chain:
out/native_texture_test/native_texture_cook --mips <input.bdtex> <output-directory>
```

Use the configured Clang toolchain; on Windows executable names end in `.exe`.
The renderer stores files in `<cache_root>/native_textures/v1/`. Archive
extraction, initial untiling, material-to-texture associations and companion
cube/volume bindings still come from the temporary resource importer. Native
asset-level scene loading and headset-specific texture formats remain work.
