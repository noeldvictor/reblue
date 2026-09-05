/**
 * @file    native_sun_camera.h
 * @brief   Current-view, texel-stabilized directional shadow camera.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include "gpu/scene/native_transform.h"
#include "gpu/scene/native_frustum.h"
#include <algorithm>
#include <limits>

namespace bd::gpu::scene {
using SunVector = std::array<double, 3>;
inline double SunDot(const SunVector &a, const SunVector &b) {
  return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}
struct NativeSunCamera {
  RenderMatrix scene_view_projection{};
  RenderMatrix view{}, projection{}, view_projection{};
  RenderFrustum frustum;
  SunVector eye{}, focus{};
  std::array<SunVector, 8> receivers{};
  double half_extent = 0, world_texel = 0, depth_range = 0;
};

inline bool IntersectsSunVolume(const RenderFrustum &volume, const SunVector &center,
                                 double radius) {
  if (!std::isfinite(radius) || radius < 0) return false;
  for (const auto value : center) if (!std::isfinite(value)) return false;
  for (const auto &plane : volume.planes)
    if (center[0]*plane[0] + center[1]*plane[1] + center[2]*plane[2] + plane[3] > radius)
      return false;
  return true;
}

// Double precision avoids far-plane cancellation in translated scene cameras.
// The result is native CPU math, not a translated register instruction stream.
inline std::optional<std::array<double, 16>> InverseSunInput(const RenderMatrix &m) {
  double a[4][8]{};
  for (size_t r = 0; r < 4; ++r) {
    for (size_t c = 0; c < 4; ++c) {
      if (!std::isfinite(m[r*4+c])) return {};
      a[r][c] = m[r*4+c];
    }
    a[r][r+4] = 1;
  }
  for (size_t c = 0; c < 4; ++c) {
    size_t pivot = c;
    for (size_t r = c+1; r < 4; ++r)
      if (std::abs(a[r][c]) > std::abs(a[pivot][c])) pivot = r;
    if (std::abs(a[pivot][c]) < 1e-15) return {};
    if (pivot != c)
      for (size_t k = 0; k < 8; ++k) std::swap(a[c][k], a[pivot][k]);
    const double divisor = a[c][c];
    for (auto &value : a[c]) value /= divisor;
    for (size_t r = 0; r < 4; ++r) {
      if (r == c) continue;
      const double factor = a[r][c];
      for (size_t k = 0; k < 8; ++k) a[r][k] -= factor*a[c][k];
    }
  }
  std::array<double, 16> result;
  for (size_t r = 0; r < 4; ++r)
    for (size_t c = 0; c < 4; ++c) result[r*4+c] = a[r][c+4];
  return result;
}
inline std::array<double, 4> TransformSunPoint(const SunVector &v, double w,
                                              const auto &matrix) {
  std::array<double, 4> result;
  for (size_t c = 0; c < 4; ++c)
    result[c] = v[0]*matrix[c] + v[1]*matrix[4+c] + v[2]*matrix[8+c] + w*matrix[12+c];
  return result;
}

// Independent of camera handedness and aspect ratio. Infinite-far projections
// contribute homogeneous ray directions instead of an invented finite plane.
inline std::optional<std::array<SunVector, 8>> SunReceiverCorners(
    const RenderMatrix &view, const RenderMatrix &projection, double reach) {
  if (!std::isfinite(reach) || reach <= 0) return {};
  const auto inverse_view = InverseSunInput(view);
  const auto inverse_projection = InverseSunInput(projection);
  if (!inverse_view || !inverse_projection) return {};
  std::array<SunVector, 8> result;
  size_t index = 0;
  for (int y : {-1, 1}) for (int x : {-1, 1}) {
    const auto n = TransformSunPoint({double(x), double(y), 0}, 1, *inverse_projection);
    const auto f = TransformSunPoint({double(x), double(y), 1}, 1, *inverse_projection);
    if (std::abs(n[3]) < 1e-12) return {};
    SunVector near_point, direction;
    for (size_t k = 0; k < 3; ++k) {
      near_point[k] = n[k]/n[3];
      direction[k] = std::abs(f[3]) < 1e-12 ? f[k] : f[k]/f[3] - near_point[k];
    }
    const double length = std::sqrt(SunDot(direction, direction));
    if (!std::isfinite(length) || length <= 1e-12) return {};
    const double distance = std::abs(f[3]) < 1e-12 ? reach : std::min(reach, length);
    SunVector far_point;
    for (size_t k = 0; k < 3; ++k) far_point[k] = near_point[k] + direction[k]*(distance/length);
    for (size_t side = 0; side < 2; ++side) {
      const auto world = TransformSunPoint(side ? far_point : near_point, 1, *inverse_view);
      if (std::abs(world[3]) < 1e-12) return {};
      for (size_t k = 0; k < 3; ++k) {
        const double value = world[k]/world[3];
        if (!std::isfinite(value)) return {};
        result[index + side*4][k] = value;
      }
    }
    ++index;
  }
  return result;
}

// Sun pitch/yaw are authored world-space radians. A rotation-only basis avoids
// a pole singularity and preserves roll-free sun direction at every angle.
// Caster padding extends upstream and downstream without widening receivers.
inline std::optional<NativeSunCamera> BuildNativeSunCamera(
    const RenderMatrix &view, const RenderMatrix &projection, double pitch,
    double yaw, uint32_t dimension, double reach, double caster_padding) {
  if (!std::isfinite(pitch) || !std::isfinite(yaw) || dimension < 8 ||
      !std::isfinite(caster_padding) || caster_padding < 0) return {};
  const auto corners = SunReceiverCorners(view, projection, reach);
  if (!corners) return {};
  NativeSunCamera result;
  result.scene_view_projection = MultiplyRenderMatrices(view, projection);
  result.receivers = *corners;
  for (const auto &point : *corners)
    for (size_t k = 0; k < 3; ++k) result.focus[k] += point[k]/8;
  double radius = 0;
  for (const auto &point : *corners) {
    SunVector delta;
    for (size_t k = 0; k < 3; ++k) delta[k] = point[k] - result.focus[k];
    radius = std::max(radius, std::sqrt(SunDot(delta, delta)));
  }
  // Quantized spherical fit: camera rotation does not continuously resize the
  // square. Two texels cover filtering and the half-texel centre snap.
  radius = std::ceil(radius*16)/16;
  result.half_extent = radius/(1 - 4.0/dimension);
  if (!(result.half_extent > 0) || !std::isfinite(result.half_extent)) return {};
  result.world_texel = 2*result.half_extent/dimension;
  const double sp = std::sin(pitch), cp = std::cos(pitch);
  const double sy = std::sin(yaw), cy = std::cos(yaw);
  const SunVector right{cy, 0, -sy}, up{sy*sp, cp, cy*sp}, back{sy*cp, -sp, cy*cp};
  const double cx = std::round(SunDot(result.focus, right)/result.world_texel)*result.world_texel;
  const double cy_light = std::round(SunDot(result.focus, up)/result.world_texel)*result.world_texel;
  const double cz = SunDot(result.focus, back) + result.half_extent + caster_padding;
  for (size_t k = 0; k < 3; ++k) {
    result.eye[k] = right[k]*cx + up[k]*cy_light + back[k]*cz;
    result.view[k*4] = float(right[k]);
    result.view[k*4+1] = float(up[k]);
    result.view[k*4+2] = float(back[k]);
  }
  result.view[12] = float(-cx);
  result.view[13] = float(-cy_light);
  result.view[14] = float(-cz);
  result.view[15] = 1;
  result.depth_range = 2*(result.half_extent + caster_padding);
  result.projection[0] = result.projection[5] = float(1/result.half_extent);
  result.projection[10] = float(-1/result.depth_range); // RH, depth [0,1]
  result.projection[15] = 1;
  result.view_projection = MultiplyRenderMatrices(result.view, result.projection);
  // Outward planes directly from native clip inequalities. Perspective slopes
  // cannot represent an orthographic shadow volume.
  for (size_t p = 0; p < 6; ++p) {
    auto &plane = result.frustum.planes[p];
    for (size_t r = 0; r < 4; ++r) {
      const auto *row = result.view_projection.data() + r*4;
      plane[r] = p == 0 ? -row[2] : p == 1 ? row[2]-row[3] :
                 p == 2 ? row[0]-row[3] : p == 3 ? -row[0]-row[3] :
                 p == 4 ? row[1]-row[3] : -row[1]-row[3];
    }
    const double length = std::sqrt(double(plane[0])*plane[0] +
        double(plane[1])*plane[1] + double(plane[2])*plane[2]);
    if (!(length > 0) || !std::isfinite(length)) return {};
    for (auto &value : plane) {
      value = float(value/length);
      if (!std::isfinite(value)) return {};
    }
  }
  for (const auto *m : {&result.scene_view_projection, &result.view,
                        &result.projection, &result.view_projection})
    for (float value : *m) if (!std::isfinite(value)) return {};
  return result;
}
} // namespace bd::gpu::scene
