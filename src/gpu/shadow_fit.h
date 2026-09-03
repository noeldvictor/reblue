/**
 * @file    gpu/shadow_fit.h
 * @brief   The sun shadow frustum fitted to the view on the host: the
 *          guest's world-to-light-clip matrix, recentred and zoomed in its
 *          own clip space onto the camera frustum's near part, applied to
 *          the registers the shadow pass (c32-35, g_mViewProj under the
 *          depth-only shadow target) and the scene pass (c36-39,
 *          g_mViewToLightProj) read.
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

// From the vertex block fetch, on the host-endian block every draw uploads
// (interpreted and replayed alike): classifies the pass by the bound
// targets, records the scene camera's view-projection, and under
// bd_shadow_fit rewrites the light matrices in the block.
void ShadowFitOnVertexBlock(float *regs, const VideoState &s);

// The zoom the last fit applied (1 = the guest's own coverage), for the PCF
// kernel's world-space scale.
f64 ShadowFitZoom();

} // namespace bd::gpu
