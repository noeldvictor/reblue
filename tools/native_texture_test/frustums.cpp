/**
 * @file    frustums.cpp
 * @brief   Native view volumes against independent double rotation math.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/scene/native_frustum.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <limits>

using namespace bd::gpu::scene;
using DPlane = std::array<double, 4>;
DPlane ReferencePlane(const FrustumShape &shape, DPlane plane) {
  const double x = shape.orientation[0], y = shape.orientation[1];
  const double z = shape.orientation[2], w = shape.orientation[3];
  // Independent quadratic rotation matrix; no production quaternion helper.
  const double matrix[3][3] = {
      {w*w + x*x - y*y - z*z, 2*(x*y - z*w), 2*(x*z + y*w)},
      {2*(x*y + z*w), w*w - x*x + y*y - z*z, 2*(y*z - x*w)},
      {2*(x*z - y*w), 2*(y*z + x*w), w*w - x*x - y*y + z*z}};
  DPlane result{0, 0, 0, plane[3]};
  for (size_t row = 0; row < 3; ++row) {
    for (size_t col = 0; col < 3; ++col)
      result[row] += matrix[row][col] * plane[col];
    result[3] -= result[row] * shape.origin[row];
  }
  const double length = std::sqrt(result[0]*result[0] + result[1]*result[1] +
                                  result[2]*result[2]);
  if (length == 0)
    return {};
  for (auto &value : result)
    value /= length;
  return result;
}
void Check(const FrustumShape &shape) {
  const std::array<DPlane, 6> local{{{0, 0, -1, shape.near_distance},
                                    {0, 0, 1, -shape.far_distance},
                                    {1, 0, -shape.right, 0},
                                    {-1, 0, shape.left, 0},
                                    {0, 1, -shape.top, 0},
                                    {0, -1, shape.bottom, 0}}};
  const auto actual = BuildFrustumPlanes(shape);
  for (size_t i = 0; i < 6; ++i) {
    const auto expected = ReferencePlane(shape, local[i]);
    for (size_t k = 0; k < 4; ++k)
      assert(std::abs(actual.planes[i][k] - expected[k]) <
             0.0001 * (1 + std::abs(expected[k])));
  }
}
bool Visible(const RenderFrustum &frustum, std::array<float, 3> centre,
             double radius) {
  for (const auto &p : frustum.planes)
    if (double(p[0]*centre[0] + p[1]*centre[1] + p[2]*centre[2] + p[3]) > radius)
      return false;
  return true;
}
int main() {
  FrustumShape shape;
  shape.far_distance = 10;
  Check(shape);
  auto volume = BuildFrustumPlanes(shape);
  assert((volume.planes[0] == FrustumPlane{0, 0, -1, 1}));
  assert((volume.planes[1] == FrustumPlane{0, 0, 1, -10}));
  assert(Visible(volume, {0, 0, 5}, 0));
  // Inside/tangent/outside for every face; positive-radius intersections stay.
  for (auto point : {std::array<float, 3>{0,0,0}, {0,0,11}, {6,0,5},
                     {-6,0,5}, {0,6,5}, {0,-6,5}}) {
    assert(!Visible(volume, point, 0));
    assert(Visible(volume, point, 1.01));
  }
  assert(Visible(volume, {0,0,1}, 0));
  assert(Visible(volume, {0,0,10}, 0));
  shape.origin = {20, -5, 3};
  shape.orientation = {0, std::sqrt(0.5f), 0, std::sqrt(0.5f)};
  Check(shape);
  volume = BuildFrustumPlanes(shape);
  assert(Visible(volume, {25, -5, 3}, 0));
  assert(!Visible(volume, {15, -5, 3}, 0));
  shape.orientation = {0, 0, 0, 2}; // not normalized: near distance becomes 1/4
  Check(shape);
  assert(std::abs(BuildFrustumPlanes(shape).planes[0][3] - 3.25f) < 1e-6f);
  shape.orientation = {};
  Check(shape);
  assert(BuildFrustumPlanes(shape).planes == RenderFrustum{}.planes);
  shape = {};
  shape.near_distance = std::numeric_limits<float>::infinity();
  auto exceptional = BuildFrustumPlanes(shape);
  assert(std::isinf(exceptional.planes[0][3]));
  assert(!Visible(exceptional, {0, 0, 5}, 0));
  shape.origin[0] = std::numeric_limits<float>::quiet_NaN();
  exceptional = BuildFrustumPlanes(shape);
  for (const auto &plane : exceptional.planes)
    assert(std::isnan(plane[3]));
  assert(Visible(exceptional, {0, 0, 5}, 0));
  shape = {};
  shape.orientation[0] = std::numeric_limits<float>::infinity();
  exceptional = BuildFrustumPlanes(shape);
  for (const auto &plane : exceptional.planes)
    for (float value : plane)
      assert(std::isnan(value));

  uint32_t random = 0x17273ab1;
  auto sample = [&] {
    random = random * 1664525u + 1013904223u;
    return float(int32_t(random >> 16) - 32768) / 8192.0f;
  };
  for (unsigned trial = 0; trial < 10000; ++trial) {
    for (auto &value : shape.origin)
      value = sample() * 250;
    for (auto &value : shape.orientation)
      value = sample();
    shape.right = sample(); shape.left = sample();
    shape.top = sample(); shape.bottom = sample();
    shape.near_distance = sample(); shape.far_distance = sample() * 1000;
    Check(shape); // asymmetric, arbitrary non-unit rotation and signed distances
  }
  FrameFrustum current;
  assert(!current.Get(0));
  current.Publish(7, volume);
  assert(current.Get(7)->planes == volume.planes);
  assert(!current.Get(6) && !current.Get(8));
  current.Invalidate();
  assert(!current.Get(7));
  current.Publish(UINT32_MAX, volume);
  assert(current.Get(UINT32_MAX) && !current.Get(0));
  current.Publish(0, {});
  assert(current.Get(0)->planes == RenderFrustum{}.planes);
  assert(!current.Get(UINT32_MAX));
}
