/**
 * @file    post_heat.h
 * @brief   Native heat-shimmer intent and shared depth-aware displacement math.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#ifdef __cplusplus
#include <cmath>
#include <cstdint>
namespace bd::gpu {
using std::abs;
using std::floor;
using std::pow;
using std::sin;

struct HeatShimmerParameters {
  float amplitude_x = .03f, amplitude_y = .03f;
  float noise_scale = 1, depth_power = 5;
  float phase = 0;
  bool enabled = false;
};

// One phase per render frame, shared by both eyes. Deliberately independent of
// the old mutable per-invocation phase array and of authored activation history.
inline float HeatShimmerFramePhase(uint32_t frame) {
  return float(double(frame) * .03);
}
#endif

struct HeatUV { float u, v; };

inline HeatUV HeatShimmerNoiseUV(float u, float v, float scale, float phase, float tap) {
  const float cycle = (phase * 1.57075f + tap * .75f) * .159154937f + .5f;
  const float wave = sin((cycle - floor(cycle)) * 6.28318548f - 3.14159274f);
  HeatUV result = {(u + wave * .01f) * 3.0f * scale,
                   (v + (phase * .2f + tap * .02f)) * scale};
  return result;
}

inline float HeatShimmerDepthWeight(float depth, float exponent) {
  if (exponent == 0.0f) return 1.0f;
  const float distance = abs(1.0f - pow(abs(1.0f - depth), .2f));
  return pow(distance, exponent);
}

inline HeatUV HeatShimmerDisplace(float u, float v, float sum_x, float sum_y,
                                  float depth_weight, float amplitude_x, float amplitude_y) {
  HeatUV result = {u + ((sum_x * .5f - 1.0f) * depth_weight) * amplitude_x,
                   v + ((sum_y * .5f - 1.0f) * depth_weight) * amplitude_y};
  return result;
}

inline bool HeatShimmerAcceptDepth(float original, float displaced) {
  // Equal depth is accepted. NaN is rejected, like the original comparison.
  return displaced >= original;
}
#ifdef __cplusplus
} // namespace bd::gpu
#endif
