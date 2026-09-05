/**
 * @file    post_scanline.h
 * @brief   Native scanline intent, independent animation and shared shader math.
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

struct ScanlineParameters {
  float strength = 0;
  float phase = 0;
  bool enabled = false;
};

// Original rand() returned 15 bits, but the gate and phase used 65536.
// Preserve that distribution: intervals <= 2 always roll, phase stays < .5.
inline float ScanlinePhase(float interval, bool noise, uint32_t roll, uint32_t jitter) {
  if (!noise) return 0;
  const float divisor = interval < 1.0f ? 1.0f : interval;
  const float threshold = floor((1.0f / divisor) * 65536.0f);
  return float(roll & 0x7fffu) < threshold ? float(jitter & 0x7fffu) / 65536.0f : 0;
}

// Render-frame identity only: no gameplay RNG, guest TLS, wall clock or eye ID.
inline uint32_t ScanlineRandom(uint32_t value) {
  value ^= value >> 16;
  value *= 0x7feb352du;
  value ^= value >> 15;
  value *= 0x846ca68bu;
  return value ^ (value >> 16);
}
inline float ScanlineFramePhase(float interval, bool noise, uint32_t frame) {
  return ScanlinePhase(interval, noise, ScanlineRandom(frame),
                       ScanlineRandom(frame ^ 0x9e3779b9u));
}
#endif

inline float ScanlineWave(float y, float height, float strength, float phase) {
  if (phase == 0.0f || strength == 0.0f) return 0.0f;
  const float position = ((y + phase) * height) * strength;
  const float cycle = ((position * 0.1f) * phase) * 0.159154937f + 0.5f;
  return sin((cycle - floor(cycle)) * 6.28318548f - 3.14159274f);
}
inline float ScanlineOffset(float wave, float strength, float exponent) {
  // A defined zero avoids log2(0), without changing the signed odd-power curve.
  if (wave == 0.0f) return 0.0f;
  const float sign = wave > 0.0f ? 1.0f : -1.0f;
  return (sign * pow(abs(wave), exponent) * strength) * 0.01f;
}
#ifdef __cplusplus
} // namespace bd::gpu
#endif
