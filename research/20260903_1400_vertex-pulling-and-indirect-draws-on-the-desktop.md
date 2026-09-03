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

## The register mask, shipped (14:10, commit `465e153`, XenosRecomp fork `2df2ff5`)

`BDInstanceRecord` carries `uint4 mask[2]`; `BD_VSC` reads the record only for a register whose
bit is set and the uniform block otherwise. `CommitInstanceRecords` sets the bits where a
record differs from the group's base block - its first record's - and the group's (or the
indirect batch's) uniform window is that base, uploaded as an ordinary window. Identical by
construction; the default, pulled and indirect configurations rendered correctly. On the
Quest an instanced vertex then loads the world rows and the palette from storage and takes the
other twenty-odd constants from preloaded registers, which is the 28 -> 19.5 ms difference
measured on 2026-09-02. `bd_record_mask=false` writes all-ones masks (the old read).

## The sky-blue captures, settled

The step from 3.4 to 1.13 ms of GPU draw time on one frame, flat for a second and a half,
recurring about every seven seconds, looked like geometry vanishing. The CSVs of every run of
the day say otherwise: last night's 02:26 run and this morning's 09:54, before any change,
spend 13% and 5% of their field frames in the same stretches. It is the autoplay camera's
cut to a cheap view (the zenith, a uniform blue with no clouds), and a one-shot capture at a
fixed second lands in it about one time in five. Read `gpu_draw_ms` beside a capture before
calling it lost, and re-capture rather than reason.

## The replayed draws' artefact, named (14:50, commit `ee11470`)

With `bd_capture_frames` capturing 300 consecutive frames and `tools/capture_cyan.py` counting
the artefact colour per frame: the default configuration had the patch in 186 of 300 frames
at a 0.3% threshold and whole frames of it (the "solid sky-blue" captures were this too when
they were not the zenith); with the host-issued draws off, none. The replay kept a template's
render-target texture slots by inheriting whatever the previous host-ordered draw left bound,
because a pooled surface changes pointer every frame; the order differs from the interpreter's,
so the reflection map landed on the ground. The fix: the visual's interpreted node in the same
pass records the surfaces it bound this frame (`VisualRegs::tex`) and the replay takes those.
After: 0 patch frames of 300, twice, in the default configuration; 1 borderline frame (a sliver
of sky at 1.09%) in the replayed-records one. Whole-frame readings with the host-issued draws
off (87 of 300 in one run) are the zenith sky, and only those.

## Everything on (15:00, commit `0f47166`)

Replayed draws on records, singles on the record path, pulling and indirect draws all default
on. Village: 258 draws a flush in -> 240 issued -> **80 indirect calls, 235 pulled**. A
300-frame sequence with no artefact frame. Within-run A/Bs: `bd_draw_indirect` -2.9% CPU per
draw, `bd_draw_pull` (the whole pulled path) -3.2%, GPU flat. The desktop driver's per-draw
cost is small; the Quest's is what these are for, and that run comes with the host-owned frame.

## Addendum (16:00): the sun frustum fitted to the view, and two more guards

`gpu/shadow_fit.*` (commit `6b2912e`). The guest's constant setter turned out to be the wrong
seam - under the host-issued draws the shadow pass never calls it, and every view it did see
was a camera - so the fit lives at the host's vertex block fetch. The diag settled the
convention: the shadow pass's c32-35 is an orthographic box (row 3 = 0 0 0 1) applied as
clip = M * v, the camera's c32-35 has the unit view direction and the eye distance in its
fourth row, and the camera frustum out to 300 units lands at x [-0.72, 0.42], y [-0.95, 0.25]
of the box. The fit pre-multiplies both light matrices by a clip-space recentre-and-zoom with
the centre snapped to the map's texel grid; captures at 300 and 150 units show the same
shadows as the guest's, no corruption, and the PCF scale follows the zoom.

The cyan patch came back once in a one-shot capture after the surface-slot fix, so two more
guards: a replay whose render-target slot has no binding from the visual's interpreted node
this frame interprets instead, and a template's ordinary texture pointer is checked against
the content hash it had at capture (a reused GuestTexture object keeps its pointer). The
stress: templates refreshed every 600 frames, 300 captured frames, zero frames with 2-60%
of the artefact colour; the readings near 2% are sky slivers.
