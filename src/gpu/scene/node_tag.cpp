/**
 * @file    gpu/scene/node_tag.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/scene/node_tag.h"

namespace bd::gpu::scene {

namespace {
// Not thread-local: the walk and the draws it produces are the same thread,
// and a stale value only costs a merge, never correctness (an unknown or wrong
// sphere is treated as overlapping everything by the gather's veto).
float g_node_sphere[4] = {0.0f, 0.0f, 0.0f, 0.0f};
} // namespace

void PublishNodeSphere(const float centre[3], float radius) {
  g_node_sphere[0] = centre[0];
  g_node_sphere[1] = centre[1];
  g_node_sphere[2] = centre[2];
  g_node_sphere[3] = radius;
}

bool LastNodeSphere(float out[4]) {
  if (!(g_node_sphere[3] > 0.0f))
    return false;
  for (u32 i = 0; i < 4; ++i)
    out[i] = g_node_sphere[i];
  return true;
}


namespace {
thread_local NodeTag t_tag;
} // namespace

const NodeTag &CurrentNodeTag() { return t_tag; }

void SetCurrentNodeTag(const NodeTag &tag) { t_tag = tag; }

void ClearCurrentNodeTag() { t_tag = NodeTag{}; }

} // namespace bd::gpu::scene
