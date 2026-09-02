/**
 * @file    gpu/scene/node_tag.h
 * @brief   Which scene node the draws now reaching the D3D layer belong to.
 *          The bdSceneNodeDrawSingle hook sets it on entry and clears it on
 *          return; the recorder reads it when a draw is queued.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/types.h>

namespace bd::gpu::scene {

// Everything the draw path cannot see on its own: identity. The draw itself
// (pipeline, streams, constants, textures) is already resolved by the time
// the queue records it; what is missing is which mesh, which node of which
// visual, and which pass, and those are the four arguments of the guest's
// per-node draw plus what its context points at.
struct NodeTag {
  u32 mesh_va = 0;     // r3
  u32 node_index = 0;  // r4
  u32 matrix_va = 0;   // r5, palette + index * 64
  u32 ctx_va = 0;      // r6, GuestTraverseCtx
  u32 visual_va = 0;   // ctx->visual
  u32 palette_va = 0;  // ctx->palette
  u32 render_view = 0; // the guest's render-view id at the time
  u32 tech = 0;        // visual + kVisualTech
  u32 seq = 0;         // node draw ordinal within the process
  bool valid = false;
};

// Per thread: the walk runs on the guest thread that issues the draws.
const NodeTag &CurrentNodeTag();
void SetCurrentNodeTag(const NodeTag &tag);
void ClearCurrentNodeTag();

} // namespace bd::gpu::scene
