/**
 * @file    native_lighting_bridge.h
 * @brief   Temporary engine source and shader boundaries for native lighting.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include "gpu/scene/native_lighting.h"
namespace bd::gpu::scene {
struct NodeTag;
std::optional<LightingVector> NativeNodeShadowSampling(const NodeTag &tag);
void CheckNativeShadowSampling(const LightingVector &expected,
                                const uint8_t *pixel_constants);
void NoteNativeShadowSamplingReplay();
} // namespace bd::gpu::scene
