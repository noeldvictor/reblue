/**
 * @file    native_raster_bridge.h
 * @brief   Import engine raster updates; supply live native intent to draws.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include "gpu/scene/native_raster.h"
struct PPCContext;
namespace bd::gpu::scene {
void UpdateRasterImport(PPCContext &ctx, uint8_t *base);
void ResetRasterImport();
RasterState CurrentRasterIntent();
} // namespace bd::gpu::scene
