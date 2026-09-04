/**
 * @file    gpu/scene/host_draw.h
 * @brief   Host-issued node draws: the per-node interpreter
 *          (bdSceneNodeDrawSingle, 1,935 guest instructions marshalling a
 *          material into big-endian memory for the host to read back) is
 *          skipped for a node whose draw the host has already seen. Stage 2b
 *          of "The direction" in CLAUDE.md.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/types.h>

namespace bd::gpu {
struct VideoState;
struct QueuedDraw;
} // namespace bd::gpu

struct PPCContext;

namespace bd::gpu {
struct PipelineState;
} // namespace bd::gpu

namespace bd::gpu::scene {

struct NodeTag;

bool HostDrawEnabled();
// True on this thread while a host-issued node draw is being dispatched.
bool HostDrawReplaying();

// From the DrawSingle hook, before the interpreter runs for a node: snapshots
// the guest's register files and fetch constants so the capture can see what
// the interpreter wrote.
void HostDrawSnapshotBefore();
// Whether a node's interpreter run is worth snapshotting at all: false for a
// node known not to replay (its vertex shader reads the bone palette, or its
// template went volatile). The snapshot and diff cost 8 KB of copies per
// node, which the Quest's cores felt.
bool HostDrawWantsCapture(const NodeTag &tag);
// From Video::SetTexture: slot `index` was bound while a node's interpreter
// run is being captured. A binding that does not change the pointer is still
// a binding the replay has to make.
void NoteTextureSet(u32 index);
// From the D3DDevice_Set*ShaderConstantFN hooks: registers [start, start +
// count) of the vertex (or pixel) file were written while a node's run is
// being captured - written, whether or not the value moved.
void NoteConstantsSet(bool vertex, u32 start, u32 count);
// The same write with its source: the guest address the values were copied
// from. Where that lands (inside the visual, the mesh, the node's palette
// slot, the traverse context, or elsewhere) is what lets the host read the
// value itself instead of replaying the interpreter's copy of it.
void NoteConstantsSource(bool vertex, u32 start, u32 count, u32 src_va);
// From the bdSetSamplerState hook: sampler `slot` was set (or asked for the
// value it already held) while a node's run is being captured.
void NoteSamplerSet(u32 slot);
// After the interpreter returned for the node: the draws it issued since the
// snapshot become (or refresh, or invalidate) the node's template.
void HostDrawCommit(const NodeTag &tag);
// Whether the run since HostDrawSnapshotBefore issued any draw.
bool HostDrawHasDraws();

// From the draw hook, once the queued draw is complete, under the video
// state's mutex: remembers what the interpreter produced for the tagged node
// (mesh, render view, technique) so the next frames can skip it.
void HostDrawCapture(const VideoState &s, const QueuedDraw &q, u32 device_guest,
                     u32 primitive_type);

// From the DrawSingle hook, before the interpreter: true when the node's
// draw was issued by the host from its template and the interpreter must
// not run. False when there is no usable template - the interpreter runs,
// and its draw refreshes the template.
bool HostDrawReplay(const NodeTag &tag);

// The guest's deferred render list, built without the interpreter.
//
// A sorted or translucent node's bdSceneNodeDrawSingle run issues no draw:
// it allocates one entry per sub-draw from the global render list
// (sub_8227DB50) and fills it - a per-node constant image (the mesh record,
// draw parameters, pass flags, bone table) plus the node matrix (r5, at +16)
// and the traverse context's palette pointer (+268). The host records the
// entries such a run produced and re-emits them next time: the guest's own
// allocator, the recorded bytes, a fresh matrix and palette.
u32 RenderListCount();
void HostListBuildCapture(const NodeTag &tag, u32 count_before);
// A node's two parts are replayed together or not at all: the direct draws
// from its template and the render-list entries from its list record. What
// the store holds for each (2026-09-03: a node with both lost its list part
// on every replayed frame - the ground light at the village rock came and
// went with the refresh cadence).
bool HostDrawHasDrawTemplate(const NodeTag &tag);
// 0 no list record, 1 fresh (replayable), 2 stale (the interpreter runs).
u32 HostListBuildStatus(const NodeTag &tag);
bool HostListBuildReplay(const NodeTag &tag, PPCContext &ctx, uint8_t *base);

// Whether a pipeline state's vertex shader reads the bone palette
// (c60..c155): the draw is a skinned node - a character.
bool PipelineReadsBones(const bd::gpu::PipelineState &st);

} // namespace bd::gpu::scene
