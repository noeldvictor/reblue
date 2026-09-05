/**
 * @brief Native colour grading and frame-coherent grain parameters.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#ifdef __cplusplus
#include <cmath>
#include <cstdint>
namespace bd::gpu {
using std::abs;
using std::pow;
#endif

struct GradeColor { float r, g, b; };
inline float GradePower(float value, float exponent) {
  // The original log clamp makes black^0 equal one; do not emit 0 * -inf.
  if (exponent == 0) return 1;
  if (value == 0 && exponent > 0) return 0;
  return pow(abs(value), exponent);
}

inline float GradeLuminance(GradeColor color) {
  return (color.b * .0721f + color.r * .2125f) + color.g * .7154f;
}
inline GradeColor GradeDiscolor(GradeColor color, float strength) {
  const float light = pow(abs(GradeLuminance(color)), 1.1f);
  GradeColor result;
  result.r = color.r + (light * .9f - color.r) * strength;
  result.g = color.g + (light * .78f - color.g) * strength;
  result.b = color.b + (light * .72f - color.b) * strength;
  return result;
}
inline GradeColor GradeCorrect(GradeColor color, float gamma, float saturation,
                               GradeColor gain, GradeColor bias,
                               GradeColor target, float blend) {
  GradeColor powered;
  powered.r = GradePower(color.r, gamma);
  powered.g = GradePower(color.g, gamma);
  powered.b = GradePower(color.b, gamma);
  const float light = GradeLuminance(powered);
  GradeColor result;
  result.r = ((powered.r - light) * saturation + light) * gain.r + bias.r;
  result.g = ((powered.g - light) * saturation + light) * gain.g + bias.g;
  result.b = ((powered.b - light) * saturation + light) * gain.b + bias.b;
  result.r += (target.r - result.r) * blend;
  result.g += (target.g - result.g) * blend;
  result.b += (target.b - result.b) * blend;
  return result;
}

#ifdef __cplusplus
struct GradeParameters {
  GradeColor gain{1, 1, 1}, bias{0, 0, 0}, target{0, 0, 0};
  float gamma = 1, saturation = 1, blend = 0;
  float discolor_strength = 0, grain_strength = 0;
  float phase_x = 0, phase_y = 0;
  uint32_t grain_image = 0;
  bool discolor = false, grain = false, correction = false;
  bool Active() const { return discolor || grain || correction; }
};
inline bool GradeStrengthEnabled(uint8_t flag, float strength) {
  // The original packed producer uses a positive threshold, not abs(strength).
  return (flag & 1) && strength > .01f;
}
inline uint32_t GradeFrameHash(uint32_t value) {
  value ^= value >> 16;
  value *= 0x7feb352du;
  value ^= value >> 15;
  value *= 0x846ca68bu;
  return value ^ (value >> 16);
}
inline void AnimateGradeGrain(GradeParameters &parameters, uint32_t frame, bool alternate) {
  // Preserve the original 15-bit / 65536 phase range without gameplay RNG,
  // per-owner rotating state or eye-dependent animation.
  parameters.phase_x = float(GradeFrameHash(frame) & 0x7fff) / 65536.0f;
  parameters.phase_y = float(GradeFrameHash(frame ^ 0x9e3779b9u) & 0x7fff) / 65536.0f;
  parameters.grain_image = frame % 3 + (alternate ? 3 : 0);
}
} // namespace bd::gpu
#endif
