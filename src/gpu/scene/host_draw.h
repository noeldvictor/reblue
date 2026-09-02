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

namespace bd::gpu::scene {

struct NodeTag;

bool HostDrawEnabled();

// From the DrawSingle hook, before the interpreter runs for a node: snapshots
// the guest's register files and fetch constants so the capture can see what
// the interpreter wrote.
void HostDrawSnapshotBefore();
// From Video::SetTexture: slot `index` was bound while a node's interpreter
// run is being captured. A binding that does not change the pointer is still
// a binding the replay has to make.
void NoteTextureSet(u32 index);
// From the D3DDevice_Set*ShaderConstantFN hooks: registers [start, start +
// count) of the vertex (or pixel) file were written while a node's run is
// being captured - written, whether or not the value moved.
void NoteConstantsSet(bool vertex, u32 start, u32 count);
// From the bdSetSamplerState hook: sampler `slot` was set (or asked for the
// value it already held) while a node's run is being captured.
void NoteSamplerSet(u32 slot);
// After the interpreter returned for the node: the draws it issued since the
// snapshot become (or refresh, or invalidate) the node's template.
void HostDrawCommit(const NodeTag &tag);

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

} // namespace bd::gpu::scene
