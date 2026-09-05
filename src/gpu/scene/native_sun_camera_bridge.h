/**
 * @file    native_sun_camera_bridge.h
 * @brief   Native sun camera ownership and temporary engine output adapters.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include "gpu/scene/native_sun_camera.h"
namespace bd::gpu::scene {
// Called after the current scene view is produced, before installing light view.
bool ProduceNativeSunCamera(uint32_t source, uint32_t target, uint32_t dimension);
std::optional<NativeSunCamera> GetNativeSunCamera();
void InvalidateNativeSunCamera();
} // namespace bd::gpu::scene
