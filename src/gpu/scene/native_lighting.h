/**
 * @file    native_lighting.h
 * @brief   Address-free lighting inputs and host pass composition.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include <array>
#include <cstdint>
#include <optional>

namespace bd::gpu::scene {
using LightingVector = std::array<float, 4>;
struct LightingExtent {
  uint32_t width = 0, height = 0;
};
struct NativeLightingInputs {
  bool receivers_enabled = true;
  // Preserve the source byte values at the temporary engine ABI boundary.
  uint8_t receiver_filter = 0, secondary_shadow = 0, shadow_mode = 0;
  uint8_t specular = 0;
  int32_t light_count = 0;
  LightingVector ambient{}, camera_position{}, color_scale{};
  float shadow_bias = 0, shadow_threshold = 0;
  float shadow_kernel_scale = 0.25f;
  std::optional<LightingExtent> sample_extent;
  std::array<float, 3> scene_origin{};
  float scene_range = 1;
};
struct NativeLightingPass {
  NativeLightingInputs inputs;
  LightingVector shadow_sampling{};
  LightingVector scene_sampling{};
};

inline NativeLightingPass ComposeNativeLighting(NativeLightingInputs inputs) {
  if (!inputs.receivers_enabled) {
    inputs.receiver_filter = 0;
    inputs.specular = 0;
  }
  NativeLightingPass result{inputs};
  result.shadow_sampling = {inputs.shadow_bias, inputs.shadow_threshold, 0, 0};
  if (inputs.sample_extent) {
    result.shadow_sampling[2] = float(inputs.sample_extent->width) * inputs.shadow_kernel_scale;
    result.shadow_sampling[3] = float(inputs.sample_extent->height) * inputs.shadow_kernel_scale;
  }
  result.scene_sampling = {inputs.scene_origin[0], inputs.scene_origin[1],
                           inputs.scene_origin[2], 1.0f / inputs.scene_range};
  return result;
}
} // namespace bd::gpu::scene
