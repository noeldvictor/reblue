/**
 * @file    native_image_layers.h
 * @brief   Preserve both eyes in sampleable colour AND depth attachments.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include <cstdint>
namespace bd::gpu {
constexpr uint32_t AttachmentTextureLayers(bool cube, bool volume, bool color_target,
                                          bool depth_target, bool multiview, bool layered) {
  return !cube && !volume && (color_target || depth_target) && multiview && layered ? 2u : 1u;
}
} // namespace bd::gpu
