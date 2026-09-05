/**
 * @file    native_view_bridge.h
 * @brief   Native view-cache publication at the remaining engine boundary.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include <cstdint>
#include "gpu/scene/native_frustum.h"
struct PPCContext;
namespace bd::gpu::scene {
bool PublishCachedViewFrustum(PPCContext &ctx, uint32_t view);
bool PublishNativeViewVolume(uint32_t view, const RenderFrustum &volume);
}
