# The per-draw ABI cut: four descriptor sets, content-keyed constants, instancing on the queue

2026-09-02, 16:30. Desktop measurements only; the Quest run for these three
steps is pending the headset being attached.

The morning's render-stage trace settled that the scene pass is draw-bound
(~36 us of GPU per draw, 535 draws in 19.5 ms) and that nothing fragment-side
moves it. The plan agreed with the owner in the afternoon: cut the per-draw ABI
on the existing deferred draw queue first, before any guest scene structure is
read, because the queue already holds every draw's whole state. Three steps
shipped today, each verified on a desktop autoplay capture of the village.

## Step A. Four real descriptor sets

The Vulkan layout bound one 4096-entry texture set three times (one per HLSL
register space), put the three dynamic guest-constant ranges ahead of the
sampler array, and dropped the occlusion set on Android for the four-set
limit. Now:

| set | contents | bound |
| --- | --- | --- |
| 0 | `Texture2DArray[]` b0, `Texture3D[]` b1, `TextureCube[]` b2 - three heaps as three bindings, all update-after-bind and partially bound | once per command list |
| 1 | `SamplerState[]` b0 | once per command list |
| 2 | the vertex, pixel and shared blocks as dynamic UBOs (b0-2), the instance record buffer (b3, static) | per draw, three offsets |
| 3 | the sun-occlusion counter, back on Android | while counting |

plume flags every texture and sampler range of a boundless set as
update-after-bind now, not only the last (fork commit 3855087), and reports
the descriptor limits in its capabilities so `BuildPipelineLayout` logs them
and refuses politely: on the desktop `sets=32 sampled images/set=1048576`. Each
texture descriptor is written into all three heaps, which is exactly what one
array read through three spaces gave, so no caller needs a view's dimension.
The spec violation the layer reported (VUID 03001, a dynamic buffer beside an
update-after-bind binding) is gone by construction; the desktop has no
validation layer installed, so that check is the Quest's `validate_quest.sh`.

Capture `frame_1788378195` (821 draws/frame) matches the previous build's
village frame.

## Step B. Content-gated uploads

The guest dirties the vertex and pixel blocks on every `Set*ShaderConstant`
call, not on a change of value, and the scene walk re-sets a material's
registers for every node wearing it. Both uploads now byte-swap into scratch
and compare with the block bound on this command list, like the shared block
has since the dynamic-UBO change. A new `[perf]` line counts them:

| | before | after B |
| --- | --- | --- |
| VS blocks uploaded / frame | ~640 | 320 |
| PS blocks uploaded / frame | ~520 | ~90 |

The 320 vertex uploads left were the per-node world matrices.

## Step C. Instancing on the queue, and what it took to make draws equal

**Shader side** (XenosRecomp `reblue` branch, commits 27b4c5b and 0cab968):
`SPEC_CONSTANT_INSTANCED` bit 2; every guest vertex shader reads its float4
constants through `BD_VSC(reg)`, which under the bit returns
`g_Instances[g_InstanceIndex].regs[reg]` from a structured buffer at binding 3
of the constant set and otherwise folds to the `g_VSC[reg]` read it always did;
every vertex shader declares `SV_InstanceID` and copies it into
`g_InstanceIndex` in the prologue. DXC's default lowering is `InstanceIndex`
(firstInstance included): 42 of the 141 compiled modules decorate it, none
declare `BaseInstance`, none declare `Int64`. The cache entry gained
`constantRegisterMask[8]`, the float4 registers a shader declares.

**Host side**: the pipeline's instanced twin is `specConstants |
kSpecConstantInstanced` (no new `PipelineState` field; the predictor emits
the twin beside every base). A deferred draw with a twin stages its whole
vertex block as a 4 KB `InstanceRecord` (CPU staging, 2048 per frame) and does
not upload it; the queue groups consecutive draws with equal `group_key`
(instanced pipeline, framebuffer, viewport, index and vertex views, draw
parameters, pixel and shared offsets), commits their records contiguously into
a 32 MB storage buffer (two slots of 4096 records) and issues one
`drawIndexedInstanced`. Inside runs of order-independent draws (depth-tested
LESS/LEQUAL, no stencil, opaque or blended-with-depth-write) equal keys are
brought together first; a run never crosses a draw that has to keep its place.

Three findings on the way, each from a one-shot `[draw-queue] instancing diag`
line that counts distinct key components over a scene-sized flush:

1. **Offsets did not mean content.** The gate of step B remembered only the
   last block, so A-B-A-B materials got fresh offsets every time and the key
   saw 611 distinct shared-block offsets for 387 distinct meshes. Fix: every
   upload is keyed by content within the frame slot (xxhash maps: full block for
   PS and shared, masked block for VS). 393 after.
2. **The whole vertex block differed for every draw** (393 -> 645) because the
   guest writes registers no shader reads; comparing over the shader's declared
   registers (`constantRegisterMask`) took VS uploads from 320 to 65 a frame.
3. **Then the shaders' own per-node registers**: the register diag named c20-22
   (world) and **c57**, `g_colliVec` in the foliage vertex shaders, a per-node
   collision vector. Slicing four named ranges into the record would never
   cover what the guest chooses to write per node, so the record is the
   **whole 256-register block**; an instanced draw never reads the uniform
   window at all, and the vertex offset left the key.

And one about order: a 666-draw transition flush had 316 distinct keys and
**zero reorderable draws**, because every one blended - the guest leaves
blending on for opaque and cut-out materials (the 64% blended depth-writers
of the LRZ note). Blended depth-writers now count as reorderable
(`bd_draw_instancing_reorder_blended`, default on; approximate where two
overlap, accepted by the owner). Real transparencies write no depth and keep
their place.

**Desktop result** (village, `bd_xr_autoplay`, mono):

| | before | after C |
| --- | --- | --- |
| VS blocks uploaded / frame | 320 | 3 |
| PS blocks uploaded / frame | ~90 | ~40 |
| scene flush, draws recorded -> issued | 262 -> 262 | 262 -> 239 |
| transition flush, distinct keys | - | 316 of 666 |

Capture `frame_1788380709` is the same village frame as before every step.
The village has few repeated meshes; the field scene on the Quest is the
number that matters, and it is pending.

## What the Quest run has to answer

`bash tools/verify_quest.sh` with the defaults (the morning's configuration,
so the render-stage trace compares), then `bash tools/gpu_drawtrace_quest.sh`
and `python tools/gpu_trace_summary.py`: the scene pass Render ms against
19.5 ms at 535 draws, the draws per pass, and the `[draw-queue] instancing`
tally. Separately, `bash tools/validate_quest.sh` for VUID 03001.

## Sources

- `research/20260902_0930_the-scene-pass-is-the-gpu-and-a-depth-prepass.md` (the draw-bound finding)
- plume fork `noeldvictor/plume` main 3855087; XenosRecomp fork `noeldvictor/XenosRecomp` reblue 27b4c5b, 0cab968
- `src/gpu/bindless_allocator.h` (the layout note), `src/gpu/constant_buffers.cpp` (the gates and caches), `src/gpu/draw_queue.cpp` (the grouping and the diag)

## Later the same day: the recorder and the host walk (stages 1 and 2a)

**The recorder** (`gpu/scene/scene_recorder.cpp`, `bd_scene_record_after_s`): the
`bdSceneNodeDrawSingle` hook sets a per-thread node tag (mesh, matrix index, palette slot,
traverse context, visual, render view, technique) and the draw hook hands every queued draw to
the recorder while the window is open. Keys: the mesh by the model block's content hash and
the view offsets, the material by shader hashes, the pointer-free pipeline state, the pixel
block over the registers the PS declares and the bound textures' content hashes (new
`GuestTexture::contentHash`). Village, 8 frames:

| pass | draws / frame | distinct meshes | distinct materials |
| --- | --- | --- | --- |
| 0, view 1 (shadow) | 149 | 141 | 35 |
| 0, view 3 (scene) | 227 | 209 | 83 |
| 12 (view 1 / 3) | 44 + 44 | 44 | 1 / 4 |

464 tagged node draws a frame, 330 untagged (effects, UI, post), **26 repeated mesh-and-
material draws a frame** - which is what the queue's instancing already merges (23). The
village is not a repeat-heavy scene; the field is.

**The host walk** (`gpu/scene/host_walk.cpp`, `bd_host_walk`, default on): a `REX_HOOK_RAW`
on `bdSceneNodeCullTraverse` walks the guest's draw nodes iteratively (prune, no-draw,
has-geometry flags; child then sibling), transforms the mesh centre by the palette slot,
calls the two cull hooks and the guest's own visibility test `sub_82287788` on a frame laid
out like the guest's, keeps the render-view-1 per-node counter, and hands survivors to the
host's `bdSceneNodeDrawSingle`. Village run: 847-850 draws a frame against 819-860 with the
guest walk, the same instancing tally, the same frame. The walk is ours now; the interpreter
behind each node is still the guest's, which is stage 2b.

## Evening: the host-issued node draw (stage 2b), and what the interpreter really writes

`bd_node_write_diag` (a register-file diff across 4000 `bdSceneNodeDrawSingle` calls) and
then the setter hooks settled what the per-node interpreter writes: vertex c0-c4 and c20-c23
on every node, pixel c0-c13 on every node, c57 in the foliage shaders, the bone palette
c60-c151 for skinned nodes; five textures and eleven sampler states through the hooks. The
world rows are the palette slot transposed with the translation in .w (3728 of 3728 recorded
draws; `tools/scene_walk_dump.py` prints the comparison).

Three wrong versions on the way, each visible on the desktop within a minute: (1) one draw
per node - a node's run issues one draw per material range, so the rock behind Shu vanished;
(2) a value diff as the delta - a same-value write is invisible, so a replay after a different
material inherited that material's register (lighting flicker); (3) whole-block templates -
the view-projection moves every frame and every template went volatile. The version that
holds: per sub-draw the host state and the registers the setter hooks saw written; a register
whose value moved between sightings is "per visual" and comes from the latest interpreted node
of the same visual this frame (one node per visual per frame is interpreted for that);
everything else is the template's. `gpu/scene/host_draw.cpp`.

Village: **111 of 420 node draws a frame host-issued**, 17 interpreted for fresh visual
values, 0 volatile templates, 0 dropped draws, frame identical (`frame_1788384614`). The
remaining 290 are foliage (c57) and skinned nodes, which stage 6 takes, and the 14 pixel and
5 vertex per-visual registers, which are the lighting and camera terms the host should compute
from the visual itself - the owner has cleared deep rewrites of the camera and lighting.

## Night: the Quest numbers, and what they retired

Verify defaults (side-by-side, shadows and reflections off, cull 350, 60 Hz), the same
configuration as yesterday's 37.5 ms:

| build | frame | GPU total | scene pass (trace) | CPU "elsewhere" |
| --- | --- | --- | --- | --- |
| yesterday (before today's steps) | 50.0 ms | 37.5 | 19.5 (mono) | 12-14 |
| A+B+C+recorder+walk+host draw, every scene draw instanced | 50.1 | 45.0 | 28.0 | 27.5 |
| singles through the plain pipeline | 61.5 | 49.6 | 25.2 / 24.0 | 29 |
| A/B reorder on vs off (one run) | 66.7 / 50.2 | **52.9 / 44.9** | - | 29 |
| A/B instancing on vs off (reorder off) | 66.7 / 66.7 | 53.0 / 52.7 | - | 27 |

Retired by the within-run A/Bs:

- **The instancing reorder costs 8 ms of GPU.** Sorting a run of order-independent draws by
  pipeline and key scatters near and far geometry, and on this direct-mode pass the shaded
  fragments follow the draw order: the guest's tree order is nearer to front-to-back than
  anything keyed by pipeline. Off by default. Instances have to be brought together with a
  depth-aware grouping, not a pipeline sort.
- **Instancing itself, with singles on the plain pipeline, is flat** (52.7 vs 53.0 ms): the
  field scene autoplay lands in has few consecutive repeats, so the record path neither helps
  nor hurts there. The first measurement, with every scene draw through the instanced variant,
  put the scene pass at 28.0 ms - the storage-buffer constant reads on Adreno, the path the
  2026-08-29 note measured slow. The record path is for real groups only now.

Not yet explained: the systematic gap between yesterday's 37.5 ms and today's 45-53 ms with
instancing and reorder off. The remaining suspects are the host-issued draw (A/B running) and
the instancing machinery every vertex shader now carries even in its plain variant (the
`SV_InstanceID` input, the `BD_VSC` spec-constant branch and the storage-buffer binding); a
probe APK built with `XENOS_RECOMP_NO_INSTANCING` removes the latter. Cross-run drift is
30-70% here (the instancing A/B's own baseline sat 8 ms above the reorder A/B's), so only
within-run arms and same-build traces count.

The CPU side doubled (13 -> 27 ms "elsewhere") on every run today, including with the twin
built off the render thread; the sampling profile's libreblue share is 1.2%, so the time is
not in host code as the profiler sees it. Open.
