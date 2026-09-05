/**
 * @file    post_adjustments.h
 * @brief   Address-free post-effect intent and shared C++/HLSL optical math.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#ifdef __cplusplus
#include <cmath>
namespace bd::gpu {
using std::floor;
using std::sin;

struct PostAdjustments {
  float fisheye = 0;
  float reverse_strength = 0;
  float reverse_pivot = 1;
  bool fisheye_enabled = false;
  bool reverse_enabled = false;
  bool Active() const { return fisheye_enabled || reverse_enabled; }
};
#endif

// Radius is measured in output-width units, with y scaled by height/width.
// This retains the authored radial curve, not the old fixed 720/1280 canvas.
// The center has no displacement; avoid the original reciprocal-zero chain.
inline float FisheyeOffsetScale(float radius, float strength) {
  if (radius <= 0.0f || strength == 0.0f) return 0.0f;
  if (strength < 0.0f) {
    const float distance = radius * 1.41421199f;
    return 0.5f * distance * distance * strength / radius;
  }
  const float cycle = radius * 0.707085073f + 0.5f;
  const float angle = (cycle - floor(cycle)) * 6.28318548f - 3.14159274f;
  return -0.2f * sin(angle) * strength / radius;
}

inline float ReverseColor(float color, float strength, float pivot) {
  return color + ((0.5f * pivot - color) * 2.0f) * strength;
}

#ifdef __cplusplus
} // namespace bd::gpu
#endif
