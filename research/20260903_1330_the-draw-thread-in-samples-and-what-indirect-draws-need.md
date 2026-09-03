# The Draw Thread in samples, and what indirect draws need

2026-09-03, 12:30-13:30, desktop only. The first symbolised desktop CPU profile of the guest's
Draw Thread, the five cuts it named, the host frustum cull, and the measurement that settles
the design of indirect draws.

## The instrument

`bd_sample_profiler = true` on a desktop run dumps raw PCs; they are offsets from the module
base, and `llvm-symbolizer --obj=reblue_vk.exe --relative-address` resolves them against the
PDB. `tools/symbolize_profile_win.sh` does it and prints the hottest functions. Three traps:

- The profile covers the guest's own threads. The IO thread (`sub_8272BE80` under
  `bdIOThreadReadFile` / `bdPackReadEntry`) and the loaders (`sub_8217AE90` under the texlist
  and CSV parsers) are in it and are not the frame.
- `dropped` counts PCs outside our module: the Vulkan driver and the runtime (313-815 of
  ~5,500 samples). The per-draw driver cost is in there, invisible by function.
- The 11:33 profile symbolised against a later exe read plausible names for wrong functions
  in the anonymous namespace; symbolise against the exe of the same build only.

## What the Draw Thread was doing (12:38, 5,977 samples)

| function | samples | what it was |
| --- | --- | --- |
| `InsetQuadUVs` | 585 (9.8%) | a half-texel UV inset over **two four-vertex quads a frame** |
| `try_translate` | 188 (3.1%) | one guest address validation per field read, walk and replay |
| `sub_82287788` | 185 (3.1%) | the guest's visibility test, once per node |
| `HostDrawReplay` | 120 | the replay itself |
| `UploadSharedConstants` | 93 | |
| `CopyGuestPixelBlock` + `CopyGuestVertexBlock` | 84 | the 4 KB swaps, once per replayed sub-draw |
| `bdSceneCullDistanceHook` + `bdSceneCullBiasHook` | 103 | the census hooks, reading the centre back from guest scratch |
| `MaskedHash`, `FindPhysicalBufferByStruct`, `IsRegistered`, `XXH3` | ~150 | keys and lookups per draw |

The inset was the surprise. A `[up]` counter proved it runs twice a frame on four vertices in
a field scene; the profile still put ~0.5 ms a frame in its min/max reads. The reads were
of `alloc.memory`: the upload ring's mapped memory, which is host-visible GPU memory -
VMA's AUTO + SEQUENTIAL_WRITE puts it in the write-combined BAR heap on a desktop, and it is
uncached on the Quest's unified memory. **Never read the upload ring back.** The other
readers (`ConstantBlockBytes` in the queue's diag and the recorder) are one-shots.

## The cuts (commits `ee062ce`, `218437c`)

- The user-pointer vertex fix-ups (canvas fit, inset) run on a host scratch; the result is
  uploaded once. The glyph batch is cached by content hash as well.
- The walk translates a node once (`try_at<GuestDrawNode>`), not five fields.
- The replay swaps the live constant blocks once per template, not per sub-draw (scalar on
  the Quest's ARM64 tail; the x86 path is SSE).
- `NativeTexturesByName` caches per name against the registry sequence (the glyph stamps ask
  every frame).
- **`bd_host_cull`** (default on): the walk's visibility test on the host for the guest's
  default view path.

## The host cull

`sub_82287788(centre, radius)`: if the cull-off switch (`lis -32036; addi -5536; +520`) is
set, visible. Otherwise by render view (`0x8277405C`): 1 and 4 transform the sphere by a
matrix and test a depth range and four planes; 7 and 9 another table; 0, 5 and 8 a point
test (`sub_82287EE0`); **every other view builds (x, y, z, r) and asks `sub_821CE028`
whether `dot((x, y, z, 1), plane) > r` for any of the six planes at the global table
(`lis -32033; addi -30608` + 64..144)**, visible when not. The centre is in view space (the
bias hook's distance is its length). The host does that last path with the same planes, read
once per walk, and calls the guest for the others. `bd_host_cull_diag` runs both and counts:
**2,897 nodes a frame host-tested, 111 of 223 walks a frame, zero disagreements** over three
70 s runs. Within-run A/B: -1.6% CPU per draw, GPU flat. The centre never goes through guest
scratch on the host path (`bdSceneCullBiasHost`, `bdSceneCullDistanceHost` over host floats).

The other half of the walks (the reflection and shadow views) still call the guest test:
4% of the Draw Thread. Their paths need a per-view decode of the matrix and range steps.

## After (13:20, 5,255 samples)

`other_ms` 4.91 -> 4.67 ms across runs (the cross-run spread is 8%; the profile shares are
the evidence). What is left of the host's own per-draw work is spread thin: the replay
2.4%, the shared upload 1.5%, the hashes and lookups ~2%, translation 4% (the replay's guest
reads and the walk's remaining two per node: the matrix and the mesh).

## Indirect draws: the number that settles the design

`[draw-queue] indirect diag` (one-shot, beside the instancing diag): draws grouped by
instanced pipeline, PS and shared constants, index buffer and format, every stream's buffer
and stride, viewport and scissor - everything an indirect command cannot change:

| flush | indexed instanced draws | coarse groups | groups of >1 | draws in them | commandable by vertexOffset |
| --- | --- | --- | --- | --- | --- |
| village scene | 565 | **65** | 35 | 535 | **0** |
| transition | 181 | 39 | 26 | 168 | 0 |

One `drawIndexedIndirect` per group would be 565 -> 65 draw calls, 8.7x. But no draw in
any group has its streams at a whole number of strides from a group base: the meshes of a
model are packed into one physical block at arbitrary offsets, and a command's single
`vertexOffset` cannot express per-stream byte offsets. So **indirect draws need vertex
pulling**: the vertex shader reads its attributes from the instance record's stream
addresses (`RawBufferLoad` from a device address, the way the recompiled shaders already read
constants) at `vertexId * stride + element offset`, decoding the declaration's formats
in-shader. That is a XenosRecomp change (a pulled-vertex twin under a spec constant, the
element decode from the declaration), an instance record with stream addresses and strides,
`drawIndexedIndirect` in the plume fork (it has none today), and the queue writing commands
per group. The plain pipeline stays for everything else.
