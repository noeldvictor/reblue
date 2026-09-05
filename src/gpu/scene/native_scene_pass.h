/**
 * @file    native_scene_pass.h
 * @brief   Native scene attachment sizing and authored colour policy.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>
namespace bd::gpu::scene {
struct SceneExtent {
  uint32_t width = 0, height = 0;
  bool operator==(const SceneExtent &) const = default;
};
inline std::optional<SceneExtent> ScaleSceneExtent(SceneExtent output,
                                                  int supersampling, int percent) {
  if (!output.width || !output.height || percent <= 0)
    return {};
  const uint64_t factor = uint32_t(std::max(1, supersampling));
  uint64_t width = output.width * factor, height = output.height * factor;
  // The old adapter's arithmetic would wrap. Refuse that input before allocation.
  if (width > UINT32_MAX || height > UINT32_MAX)
    return {};
  if (percent < 100) {
    if (width * uint32_t(percent) > UINT32_MAX ||
        height * uint32_t(percent) > UINT32_MAX)
      return {};
    width = std::max(uint64_t(1), width * uint32_t(percent) / 100);
    height = std::max(uint64_t(1), height * uint32_t(percent) / 100);
  }
  return SceneExtent{uint32_t(width), uint32_t(height)};
}
inline uint32_t SceneClearColor(uint32_t authored_argb, bool primary_view) {
  return primary_view ? authored_argb | 0xFF000000u : authored_argb;
}
inline uint32_t SceneColorWriteMask(bool primary_view) {
  return primary_view ? 7u : 15u;
}
// Preserve the authored HDR/post exposure contract, without console resolve bits.
inline float SceneOutputExposure(bool hdr) { return hdr ? 0.25f : 1.0f; }
} // namespace bd::gpu::scene
