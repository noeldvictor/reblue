/**
 * @file    lighting_shader_bridge.h
 * @brief   Temporary engine staging ABI; not the native lighting contract.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include "gpu/scene/native_lighting.h"
#include <bit>

namespace bd::gpu::scene {
using LightingStagingImage = std::array<uint32_t, 103>;
inline LightingStagingImage PackLightingStaging(const NativeLightingPass &pass) {
  LightingStagingImage result{};
  const auto &inputs = pass.inputs;
  auto vector = [&](size_t offset, const LightingVector &value) {
    for (size_t i = 0; i < 4; ++i)
      result[offset / 4 + i] = std::bit_cast<uint32_t>(value[i]);
  };
  vector(0, inputs.ambient);
  vector(16, inputs.camera_position);
  vector(80, inputs.ambient);
  vector(96, inputs.camera_position);
  vector(112, inputs.color_scale);
  vector(224, pass.shadow_sampling);
  vector(288, pass.scene_sampling);
  result[340 / 4] = inputs.specular;
  result[348 / 4] = inputs.receiver_filter;
  result[352 / 4] = inputs.secondary_shadow;
  result[360 / 4] = inputs.shadow_mode;
  result[364 / 4] = inputs.light_count > 0;
  result[368 / 4] = inputs.light_count > 1;
  for (size_t offset = 372; offset <= 384; offset += 4)
    result[offset / 4] = 1;
  for (size_t offset = 396; offset <= 404; offset += 4)
    result[offset / 4] = std::bit_cast<uint32_t>(1.0f);
  // The compatibility consumer compares this modification serial. Reset plus
  // seven ambient/camera/colour/bias writes, two scene writes, optional extent
  // writes, and each nonzero feature transition. No native consumer needs it.
  result[408 / 4] = 9 + (inputs.sample_extent ? 2 : 0) +
      (inputs.receiver_filter != 0) + (inputs.secondary_shadow != 0) +
      (inputs.shadow_mode != 0) + (inputs.specular != 0) +
      (inputs.light_count > 0) + (inputs.light_count > 1);
  return result;
}
} // namespace bd::gpu::scene
