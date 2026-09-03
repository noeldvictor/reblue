# Vertex pulling and indirect draws, on the desktop

2026-09-03, 13:00-14:00, desktop only. Stage 8's first half: the recompiled vertex shaders can
pull their attributes from the instance record's streams, the pulled pipeline twin draws with
no per-mesh stream binds, and batches of pulled draws go out as one `drawIndexedIndirect`.
Commits `b387ca8` (pulling), `71c31c9` (indirect), plume fork `4695e1b`, XenosRecomp fork
`a3b45cf`.

## Why pulling

`[draw-queue] indirect diag` (the 13:30 note): the village's 565 instanced draws fall into 65
groups an indirect call could each cover, and none of them can be addressed by a command's
single `vertexOffset`, because a model's meshes are packed into one physical block at
arbitrary per-stream byte offsets. So the shader has to fetch its own vertices.

## The pieces

- **XenosRecomp** (`shader_common.h`, `shader_recompiler.cpp`): `SPEC_CONSTANT_PULLED (1 << 3)`
  on every vertex shader beside the instanced bit; `SV_VertexID` in the prologue; one local per
  vertex attribute, `vXxxN = (spec & PULLED) ? BD_PullF(location) : iXxxN`, and the fetch sites
  read the local. `BD_PullF`/`BD_PullU` read `g_PullInfo[g_InstanceIndex]` (a stream table per
  record: heap slot, base offset, stride), `g_DeclTable[decl * 16 + location]` (a packed entry
  per attribute location: format code, slot, byte offset) and `g_VertexBufferHeap[slot]` (a
  `ByteAddressBuffer` array, binding 3 of the texture set), at `vertexId * stride + offset`,
  any alignment, decoding the format the input assembler would have converted from (`BD_PULL_*`
  codes for the sixteen formats the declaration builder emits). No `Int64`: `spv_caps.py` is
  clean.
- **Host** (`gpu/vertex_pull.*`): the block heap (256 slots, a buffer registered on first sight
  as a stream, released after the frames in flight when its block is freed), the declaration
  table (8192 x 16 entries, written on a declaration's first use from what the builder decided
  per element), the per-record pull info staged beside the instance record and committed at the
  same GPU index, the pulled pipeline twin (the instanced state plus the bit, the dummy input
  layout: every location on slot 15, stride 0, a 64-byte zero buffer), the precache building it
  beside the instanced twin.
- **plume fork**: `drawIndexedIndirect`, `RenderBufferFlag::INDIRECT`, `multiDrawIndirect`
  capability, storage-buffer ranges of a boundless set flagged update-after-bind.
- **The queue**: `bd_draw_pull` draws record groups through the pulled twin (slot 15 bound once,
  no stream binds); `bd_draw_indirect` batches consecutive pulled draws sharing the batch key
  (pipeline, PS and shared constants, index buffer and format, pass geometry) into one
  `drawIndexedIndirect`, a command per instancing group, records contiguous, the index buffer
  bound at offset zero and each command's `firstIndex` carrying the mesh's own offset.

## What the desktop says

| run (village) | draws in -> issued a flush | pulled | indirect | frame |
| --- | --- | --- | --- | --- |
| pull on, singles plain | 258 -> 252 | 25 groups | - | correct |
| pull on, singles on records | 258 -> 252 | 85 | - | correct (`cap_pull3`) |
| pull + records for replayed draws | 258 -> 240 | 235 of 240 | - | correct but one cyan patch |
| pull + indirect, singles on records | 257 -> 251 | 85 | **91 draws in 35 calls** | correct |

760 records a frame stage with every one pullable (no declaration refused, no stream unbound,
no heap slot short); `[pull] per frame` prints the refusals. `other_ms` 4.60 in the indirect
run, the lowest of the day (4.91 this morning).

## What limits the coverage

Only draws with an instance record can pull, and the host-replayed draws (581 of 660 node
draws a frame) stay off the record path by default (`bd_host_draw_records`), whose
intermittent artefact - a node hidden, this time a cyan patch on the ground - predates this
work. Two probes (replay off; records off) and a re-run of the failing configuration all
rendered correctly, so the fault is intermittent and in the replay's record contents, not in
the pulled decode. Naming it needs a per-frame instrument on that path; until then the pulled
and indirect paths cover the interpreted draws and the instancing groups.

## A capture trap, for the record

Two captures came back solid sky blue (88, 198, 255) with 790+ draws in the frame. The CSV's
`gpu_draw_ms` for those frames read 1.2 ms against 3.3 ms in the neighbours: the autoplay
camera was on open sky. A legitimate frame, and a reminder that a one-shot capture at a fixed
time compares scenes, not code, across runs.

## What the Quest will need before this ships there

The instanced twin's whole-block record reads were measured on the Quest (2026-09-02): every
scene draw through them put the scene pass at 28 ms against 19.5, because Adreno preloads
uniform constants and pays a memory load per storage-buffer read. The pulled and indirect paths
carry that cost per pulled draw. The fix is a per-record register mask: the record holds only
the registers that differ from the batch's bound uniform block (the world rows, the palette,
the foliage vector), `BD_VSC` takes the record for those and the uniform block for the rest.
That is the next piece of this stage, designed on the desktop, measured once on the Quest with
the rest of the host-owned frame.
