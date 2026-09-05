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
// The world-space bounding sphere of the node the draws after this belong to,
// published by the host walk. The draw queue reads it to decide whether two
// blended draws can swap: spheres that do not overlap in the view never write
// the same pixel. Radius 0 means unknown, and unknown never moves.
void PublishNodeSphere(const float centre[3], float radius);
bool LastNodeSphere(float out[4]);

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
  // From the guest's deferred render list (sub_8227F360) rather than a
  // direct node draw: mesh_va is then a hash of the entry's draw identity,
  // matrix_va the entry's inline world matrix, ctx_va/palette_va unset.
  bool from_list = false;
  // A render-list entry names its bones outright: palette_va is the entry's
  // palette pointer (+268) and bone_table_va its index table (+800, one u32
  // per bone, bone_count of them from +289). Direct nodes import per-draw
  // joint bindings from model commands, never by comparing pose values.
  u32 bone_table_va = 0;
  u32 bone_count = 0;
  bool valid = false;
};

// Per thread: the walk runs on the guest thread that issues the draws.
const NodeTag &CurrentNodeTag();
void SetCurrentNodeTag(const NodeTag &tag);
void ClearCurrentNodeTag();

} // namespace bd::gpu::scene
