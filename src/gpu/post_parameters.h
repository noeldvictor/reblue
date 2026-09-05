/**
 * @file    post_parameters.h
 * @brief   Native depth-of-field values and explicit preparation ownership.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include "gpu/scene/native_transform.h"
#include "gpu/post_bloom.h"
#include <algorithm>
#include <cstdint>

namespace bd::gpu {
struct DofParameters {
  float aperture = 0;
  float blur_scale = 0;
  float authored_range = 0;
  float focus_depth = 0;
};

struct BloomParameters {
  float threshold = 0, intensity = 0;
  std::array<float, 4> scene_weight{4, 4, 4, 4};
  std::array<float, 4> bloom_weight{};
  DirectionalBloom directional;
};

// Mode 1 adds two directional masks; other authored modes add one. The
// native composite averages those independent masks and uses their total weight.
inline BloomParameters MakeBloomParameters(float threshold, float intensity,
                                           bool enabled, int32_t mode) {
  const float weight = enabled ? (mode == 1 ? 2.0f : 1.0f) : 0.0f;
  return {threshold, intensity, {4, 4, 4, 4}, {weight, weight, weight, 0},
          {1, 1, 0, enabled && mode == 1}};
}

// Keep the authored curve, but produce its inputs without a shader-register
// block. The adapter supplies a world-space focus point and this view's native
// transforms. Projection is intentionally separate: the original convention
// treats the intermediate view-space point as xyz with w=1.
inline DofParameters MakeDofParameters(
    float aperture, float authored_blur, float authored_range,
    double strength, const std::array<float, 3> &focus,
    const scene::RenderMatrix &view, const scene::RenderMatrix &projection) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
  auto transform = [](const std::array<float, 3> &point,
                      const scene::RenderMatrix &matrix) {
    std::array<float, 4> result;
    for (std::size_t lane = 0; lane < 4; ++lane) {
      float value = point[2] * matrix[8 + lane] + matrix[12 + lane];
      value = point[1] * matrix[4 + lane] + value;
      result[lane] = point[0] * matrix[lane] + value;
    }
    return result;
  };
  const auto eye = transform(focus, view);
  const auto clip = transform({eye[0], eye[1], eye[2]}, projection);
  float blur = (authored_blur * 1.25f) * 0.001f;
  if (strength < 1.0)
    blur = float(double(blur) * std::sqrt(std::max(0.0, strength)));
  // Preserve IEEE values at the transitional boundary, including a zero clip
  // w. Do not silently change authored focus or suppress DoF to conceal blur.
  return {aperture, blur, authored_range * 0.010001f, clip[2] / clip[3]};
}

// Tokens identify only the remaining synchronous engine entry pair, never
// assets. One prepared atlas must not be consumed by another view or frame.
class DofPreparation {
public:
  bool Prepare(uint64_t owner, uint64_t source, uint32_t frame) {
    if (active_ || !owner || !source)
      return false;
    owner_ = owner;
    source_ = source;
    frame_ = frame;
    active_ = true;
    return true;
  }
  bool Consume(uint64_t owner, uint64_t source, uint32_t frame) {
    if (!active_ || owner != owner_ || source != source_ || frame != frame_)
      return false;
    active_ = false;
    return true;
  }
  bool Active() const { return active_; }
private:
  uint64_t owner_ = 0, source_ = 0;
  uint32_t frame_ = 0;
  bool active_ = false;
};
} // namespace bd::gpu
