/**
 * @file    native_blend_bridge.h
 * @brief   Produce live host blend intent at the engine state boundary.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include "gpu/scene/native_blend.h"
#include <cstdint>
struct PPCContext;
namespace bd::gpu::scene {
// False means a different subsystem owns this state; no effects occurred.
bool UpdateBlendImport(PPCContext &ctx, uint8_t *base);
void ResetBlendImport();
BlendState CurrentBlendIntent(uint32_t device);
} // namespace bd::gpu::scene
