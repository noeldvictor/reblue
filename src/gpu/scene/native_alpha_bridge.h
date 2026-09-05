/**
 * @file    native_alpha_bridge.h
 * @brief   Import engine alpha updates; supply live native policy to draws.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include "gpu/scene/native_alpha.h"
struct PPCContext;
namespace bd::gpu::scene {
bool UpdateAlphaImport(PPCContext &ctx, uint8_t *base);
void ResetAlphaImport();
AlphaState CurrentAlphaIntent();
} // namespace bd::gpu::scene
