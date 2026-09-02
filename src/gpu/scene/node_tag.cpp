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
thread_local NodeTag t_tag;
} // namespace

const NodeTag &CurrentNodeTag() { return t_tag; }

void SetCurrentNodeTag(const NodeTag &tag) { t_tag = tag; }

void ClearCurrentNodeTag() { t_tag = NodeTag{}; }

} // namespace bd::gpu::scene
