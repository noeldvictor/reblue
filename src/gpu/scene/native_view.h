/**
 * @file    native_view.h
 * @brief   Address-free camera frustum production and view-cache ownership.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include "gpu/scene/native_frustum.h"
#include "gpu/scene/native_transform.h"
#include <algorithm>
#include <bit>
#include <limits>
#include <unordered_map>

namespace bd::gpu::scene {
using FrustumClipPoints = std::array<std::array<float, 4>, 6>;
inline constexpr FrustumClipPoints kViewClipPoints{{
    {1,0,1,1}, {-1,0,1,1}, {0,1,1,1}, {0,-1,1,1}, {0,0,0,1}, {0,0,1,1}}};

inline float RefinedReciprocal(float value) {
#pragma clang fp contract(off)
  const float initial = 1.0f / value;
  const float first = initial * -(value * initial - 1.0f) + initial;
  const float second = first * -(value * first - 1.0f) + first;
  return std::isnan(first) ? initial : second;
}

// General row-vector inverse, including off-centre and non-rigid cameras.
// Singular/nonfinite inputs retain IEEE results; inventing an identity camera
// here changes startup visibility and masks invalid producers.
inline RenderMatrix InverseRenderMatrix(const RenderMatrix &m) {
#pragma clang fp contract(off)
  // Pair minors by column. The ordered cofactor expansion matters when a far
  // clip unprojection subtracts almost equal inverse coefficients: an
  // algebraically equivalent row expansion moved a live far plane by 48 units.
  const float a = -(m[14]*m[11] - m[10]*m[15]);
  const float b = -(m[14]*m[7] - m[6]*m[15]);
  const float c = -(m[10]*m[7] - m[6]*m[11]);
  const float d = -(m[10]*m[3] - m[2]*m[11]);
  const float e = -(m[14]*m[3] - m[2]*m[15]);
  const float f = -(m[6]*m[3] - m[2]*m[7]);
  const float g = -(m[12]*m[9] - m[8]*m[13]);
  const float h = -(m[12]*m[5] - m[4]*m[13]);
  const float i = -(m[8]*m[5] - m[4]*m[9]);
  const float j = -(m[8]*m[1] - m[0]*m[9]);
  const float k = -(m[12]*m[1] - m[0]*m[13]);
  const float l = -(m[4]*m[1] - m[0]*m[5]);
  RenderMatrix adjugate{
      m[13]*c - (m[9]*b - m[5]*a),
      -m[1]*a - (m[13]*d - m[9]*e),
      m[13]*f - (m[5]*e - m[1]*b),
      -m[1]*c - (m[9]*f - m[5]*d),
      -m[4]*a - (m[12]*c - m[8]*b),
      m[12]*d - (m[8]*e - m[0]*a),
      -m[0]*b - (m[12]*f - m[4]*e),
      m[8]*f - (m[4]*d - m[0]*c),
      m[15]*i - (m[11]*h - m[7]*g),
      -m[3]*g - (m[15]*j - m[11]*k),
      m[15]*l - (m[7]*k - m[3]*h),
      -m[3]*i - (m[11]*l - m[7]*j),
      -m[6]*g - (m[14]*i - m[10]*h),
      m[14]*j - (m[10]*k - m[2]*g),
      -m[2]*h - (m[14]*l - m[6]*k),
      m[10]*l - (m[6]*j - m[2]*i)};
  const float determinant = (adjugate[3]*m[12] + adjugate[2]*m[8]) +
                            (adjugate[1]*m[4] + adjugate[0]*m[0]);
  const float inverse = RefinedReciprocal(determinant);
  for (auto &value : adjugate)
    value *= inverse;
  return adjugate;
}

inline std::array<float, 3> NormalizeViewDirection(std::array<float, 3> vector,
                                                 float threshold = 0) {
#pragma clang fp contract(off)
  const float squared = vector[2] * vector[2] +
                        (vector[1] * vector[1] + vector[0] * vector[0]);
  if (squared <= threshold)
    return {};
  const float initial = 1.0f / std::sqrt(squared);
  const float refined = initial * -((squared * 0.5f) * (initial * initial) - 0.5f) + initial;
  for (auto &value : vector)
    value *= refined;
  return vector;
}

// Preserve the authored direction-to-angle approximation, including endpoints.
// These are scalar polynomial coefficients, not an emulated vector instruction
// stream or reads from a process-addressed constant table.
inline float ViewDirectionAsin(float value) {
#pragma clang fp contract(off)
  constexpr std::array<uint32_t, 12> coefficients{
      0xBD6DD42D, 0xBED65553, 0x3E663246, 0x400B1889,
      0x3F1DD7B6, 0x408980BD, 0xBF983F2F, 0xC0D1360E,
      0xBFAF4418, 0xC08F6AD9, 0x3FB58485, 0x40AF6AD8};
  value = std::clamp(value, -1.0f, 1.0f);
  const float magnitude = std::abs(value);
  const float product = magnitude * value;
  const float cube = product * value;
  const float ratio = (value - product) /
                      std::sqrt(std::bit_cast<float>(0x3F800001u) - magnitude);
  std::array<float, 4> polynomial;
  for (size_t i = 0; i < 4; ++i)
    polynomial[i] = magnitude * (magnitude * std::bit_cast<float>(coefficients[i]) +
                                 std::bit_cast<float>(coefficients[i + 4])) +
                    std::bit_cast<float>(coefficients[i + 8]);
  return (polynomial[0] * (cube * ratio) + polynomial[1] * ratio) +
         (polynomial[2] * (cube * value) + polynomial[3] * value);
}

inline float WrapViewAngle(float angle) {
  constexpr float pi = std::bit_cast<float>(0x40490FDBu);
  constexpr float two_pi = std::bit_cast<float>(0x40C90FDBu);
  // The producer's double divisor is the float-authored 2*pi extended to double.
  angle = float(std::fmod(double(angle), double(two_pi)));
  if (angle > pi)
    angle -= two_pi;
  if (angle < -pi)
    angle += two_pi;
  return angle;
}

inline std::array<float, 2> ViewHalfAngleSinCos(float angle) {
#pragma clang fp contract(off)
  // Inputs are already wrapped and halved. Retain the producer's ordered
  // polynomial: even a one-ulp quaternion change is amplified by a translated
  // camera when a plane passes near the world origin.
  constexpr std::array<uint32_t, 12> sine{
      0x3F800000, 0xBE2AAAAB, 0x3C088889, 0xB9500D01,
      0x3638EF1D, 0xB2D7322B, 0x2F309231, 0xAB573F9F,
      0x274A963C, 0xA317A4DA, 0x1EB8DC78, 0x9A3B0DA1};
  constexpr std::array<uint32_t, 12> cosine{
      0x3F800000, 0xBF000000, 0x3D2AAAAB, 0xBAB60B61,
      0x37D00D01, 0xB493F27E, 0x310F76C8, 0xAD49CBA5,
      0x29573F9F, 0xA53413C3, 0x20F2A15D, 0x9C8671CB};
  std::array<float, 24> powers;
  powers[0] = 1;
  powers[1] = angle;
  for (size_t i = 2; i < powers.size(); ++i)
    powers[i] = powers[(i + 1) / 2] * powers[i / 2];
  float sin_value = angle, cos_value = 1;
  for (size_t i = 1; i < sine.size(); ++i) {
    sin_value = std::bit_cast<float>(sine[i]) * powers[2*i+1] + sin_value;
    cos_value = std::bit_cast<float>(cosine[i]) * powers[2*i] + cos_value;
  }
  return {sin_value, cos_value};
}

inline FrustumShape BuildViewFrustumShape(const RenderMatrix &view,
                                         const RenderMatrix &projection,
                                         const FrustumClipPoints &clip_points = kViewClipPoints) {
#pragma clang fp contract(off)
  const auto inverse_projection = InverseRenderMatrix(projection);
  std::array<std::array<float, 4>, 6> points;
  for (size_t i = 0; i < points.size(); ++i) {
    for (size_t c = 0; c < 4; ++c)
      points[i][c] =
          (clip_points[i][0] * inverse_projection[c] +
           clip_points[i][1] * inverse_projection[4 + c]) +
          (clip_points[i][2] * inverse_projection[8 + c] +
           clip_points[i][3] * inverse_projection[12 + c]);
    const float divisor = RefinedReciprocal(points[i][i < 4 ? 2 : 3]);
    for (auto &value : points[i])
      value *= divisor;
  }
  FrustumShape shape;
  shape.right = -points[0][0];
  shape.left = -points[1][0];
  // Authored vertical culling guard band is 10%, not a projection flip alone.
  shape.top = points[2][1] * -1.1f;
  shape.bottom = points[3][1] * -1.1f;
  shape.near_distance = -points[4][2];
  shape.far_distance = -points[5][2];
  const auto inverse_view = InverseRenderMatrix(view);
  std::copy_n(inverse_view.begin() + 12, 3, shape.origin.begin());
  std::array<float, 3> direction;
  for (size_t c = 0; c < 3; ++c)
    direction[c] = -(0.0f * inverse_view[c] +
                       (0.0f * inverse_view[4 + c] + (inverse_view[8 + c] + 0.0f)));
  direction = NormalizeViewDirection(direction, std::numeric_limits<float>::min());
  direction = NormalizeViewDirection(direction);
  const float pitch = WrapViewAngle(-ViewDirectionAsin(direction[1]));
  direction[1] = 0;
  direction = NormalizeViewDirection(direction);
  constexpr float pi = std::bit_cast<float>(0x40490FDBu);
  float yaw = ViewDirectionAsin(direction[0]);
  if (!(direction[2] > 0))
    yaw = -yaw;
  else
    yaw += direction[0] > 0 ? pi : -pi;
  yaw = WrapViewAngle(yaw + pi);
  const auto [sp, cp] = ViewHalfAngleSinCos(pitch * 0.5f);
  const auto [sy, cy] = ViewHalfAngleSinCos(yaw * 0.5f);
  // Roll is intentionally zero. Keep all terms so nonfinite inputs propagate.
  shape.orientation = {sp * cy + (cp * sy) * 0.0f,
                       cp * sy + (-sp * cy) * 0.0f,
                       (cp * cy) * 0.0f - sp * sy,
                       cp * cy + (sp * sy) * 0.0f};
  return shape;
}

class NativeViewCache {
public:
  const FrustumShape *Get(uint32_t view) const {
    const auto found = views_.find(view);
    return found == views_.end() ? nullptr : &found->second;
  }
  void Publish(uint32_t view, const FrustumShape &shape) { views_[view] = shape; }
  void Invalidate(uint32_t view) { views_.erase(view); }
  void Clear() { views_.clear(); }
private:
  std::unordered_map<uint32_t, FrustumShape> views_;
};
} // namespace bd::gpu::scene
