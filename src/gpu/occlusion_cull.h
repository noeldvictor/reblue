/**
 * @file    gpu/occlusion_cull.h
 * @brief   Host occlusion culling on the scene walk: at the end of the scene
 *          pass a view-space cube proxy per visible node is drawn under an
 *          occlusion query, depth-tested against the pass's own depth; a
 *          node whose proxy passed no sample two frames running is not
 *          drawn. Conservative by the proxy's margin, one frame of latency.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <plume_render_interface.h>
#include <rex/types.h>

namespace bd::gpu {

struct VideoState;

// Frame slot lifecycle, beside the fragment census: reset at command list
// begin, results read after the slot's fence.
void OcclusionCullFrameBegin(plume::RenderDevice *device,
                             plume::RenderCommandList *cmd, u32 slot);
void OcclusionCullCollect(u32 slot);

// The walk, camera view only: whether a node drew occluded last time, and
// this frame's sphere for the query. `key` identifies the node across
// frames (its draw-node address).
bool OcclusionCullOccluded(u64 key);
void OcclusionCullNote(u64 key, const float centre[3], float radius);

// At the scene pass's end, with its framebuffer still bound: draws this
// frame's proxies under queries. No-op unless bd_occlusion_cull and the
// scene pass's projection are known.
void OcclusionCullEmit(VideoState &s);

} // namespace bd::gpu
