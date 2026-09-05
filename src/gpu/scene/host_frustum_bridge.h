/**
 * @file    host_frustum_bridge.h
 * @brief   Transitional engine view input and native culling volume access.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include "gpu/scene/native_frustum.h"

namespace bd::gpu::scene {
// No engine plane import: returns only a volume produced natively this frame
// on the submitting thread. Other views/missing producers remain explicit.
bool GetNativeSceneFrustum(RenderFrustum &frustum);
}
