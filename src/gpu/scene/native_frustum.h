/**
 * @file    native_frustum.h
 * @brief   Native view volumes and plane construction, independent of the
 * engine, shader registers and graphics SDKs.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

namespace bd::gpu::scene {
struct FrustumShape {
  std::array<float, 3> origin{};
  std::array<float, 4> orientation{0, 0, 0, 1}; // xyzw, not implicitly normalized
  float right = 1, left = -1, top = 1, bottom = -1;
  float near_distance = 1, far_distance = 1000;
};
using FrustumPlane = std::array<float, 4>; // outward normal: n . x + d <= 0
struct RenderFrustum {
  // Near, far, right, left, top, bottom. Packing for a backend belongs elsewhere.
  std::array<FrustumPlane, 6> planes{};
};

// Arithmetic also preserves exceptional transitional engine values: NaNs do
// not reject spheres, and a zero-length normal yields a zero plane. Native
// camera asset validation belongs at ingestion, not in this arithmetic layer.
inline RenderFrustum BuildFrustumPlanes(const FrustumShape &shape) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
  RenderFrustum result{{{{0, 0, -1, shape.near_distance},
                        {0, 0, 1, -shape.far_distance},
                        {1, 0, -shape.right, 0},
                        {-1, 0, shape.left, 0},
                        {0, 1, -shape.top, 0},
                        {0, -1, shape.bottom, 0}}}};
  const auto [x, y, z, w] = shape.orientation;
  for (auto &plane : result.planes) {
    const auto [nx, ny, nz, d] = plane;
    // q * (n, 0) * conjugate(q). Pairwise, non-contracted arithmetic
    // retains the established CPU convention, including non-unit q scaling.
    const float tx = (w * nx - z * ny) + (y * nz - x * 0.0f);
    const float ty = (w * ny + z * nx) + (-y * 0.0f - x * nz);
    const float tz = (w * nz - z * 0.0f) + (-y * nx + x * ny);
    const float dot = (w * 0.0f + z * nz) + (y * ny + x * nx);
    plane[0] = (dot * x + tz * y) + (-ty * z + tx * w);
    plane[1] = (dot * y - tz * x) + (ty * w + tx * z);
    plane[2] = (dot * z + tz * w) + (ty * x - tx * y);
    plane[3] = d - (plane[2] * shape.origin[2] +
                    (plane[1] * shape.origin[1] + plane[0] * shape.origin[0]));
    const float squared = plane[2] * plane[2] +
                          (plane[1] * plane[1] + plane[0] * plane[0]);
    if (squared == 0) {
      plane = {}; // a degenerate normal contributes no rejection
      continue;
    }
    float reciprocal = 1.0f / std::sqrt(squared);
    const float half_squared = squared * 0.5f;
    const float reciprocal_squared = reciprocal * reciprocal;
    const float correction = -(half_squared * reciprocal_squared - 0.5f);
    reciprocal = reciprocal * correction + reciprocal;
    for (auto &value : plane)
      value *= reciprocal;
  }
  return result;
}

// The pass producer must publish in the current frame. A reload or a missing
// producer must not silently reuse the previous scene's native view volume.
class FrameFrustum {
public:
  void Publish(uint32_t frame, const RenderFrustum &frustum) {
    frame_ = frame;
    frustum_ = frustum;
  }
  void Invalidate() { frustum_.reset(); }
  const RenderFrustum *Get(uint32_t frame) const {
    return frame_ == frame && frustum_ ? &*frustum_ : nullptr;
  }
private:
  uint32_t frame_ = 0;
  std::optional<RenderFrustum> frustum_;
};
} // namespace bd::gpu::scene
