/**
 * @file    views.cpp
 * @brief   Camera inverse, projection shape, orientation and native cache tests.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/scene/native_view.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <cstdlib>
// Debug CRT assertions can open a modal dialog in unattended Windows runs.
#undef assert
#define assert(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
  std::exit(EXIT_FAILURE); } } while (false)

using namespace bd::gpu::scene;
constexpr RenderMatrix identity{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
constexpr FrustumClipPoints clips{{{1,0,1,1}, {-1,0,1,1}, {0,1,1,1},
                                   {0,-1,1,1}, {0,0,0,1}, {0,0,1,1}}};
bool Near(double a, double b, double epsilon = 1e-5) {
  return std::abs(a - b) <= epsilon * (1 + std::abs(b));
}
void CheckInverse(const RenderMatrix &matrix) {
  const auto inverse = InverseRenderMatrix(matrix);
  for (unsigned r = 0; r < 4; ++r)
    for (unsigned c = 0; c < 4; ++c) {
      double left = 0, right = 0;
      for (unsigned k = 0; k < 4; ++k) {
        left += double(matrix[r*4+k]) * inverse[k*4+c];
        right += double(inverse[r*4+k]) * matrix[k*4+c];
      }
      assert(Near(left, r == c));
      assert(Near(right, r == c));
    }
}
int main() {
  CheckInverse(identity);
  CheckInverse({2,0,0,0, 0,3,0,0, 0,0,-4,0, 200,-50,12,1});
  uint32_t seed = 0x582469ab;
  for (unsigned trial = 0; trial < 1000; ++trial) {
    RenderMatrix matrix;
    for (unsigned i = 0; i < 16; ++i) {
      seed = seed * 1664525u + 1013904223u;
      matrix[i] = float(int32_t(seed >> 16) - 32768) / 65536.0f;
      if (i % 5 == 0)
        matrix[i] += 4; // well-conditioned general, non-affine matrix
    }
    CheckInverse(matrix);
  }
  for (int i = -10000; i <= 10000; ++i) {
    const float x = float(i) / 10000;
    assert(Near(ViewDirectionAsin(x), std::asin(double(x))));
    const float angle = x * 1.57079632679f;
    const auto [sine, cosine] = ViewHalfAngleSinCos(angle);
    assert(Near(sine, std::sin(double(angle)), 5e-7));
    assert(Near(cosine, std::cos(double(angle)), 5e-7));
  }
  assert(Near(WrapViewAngle(17.0f), std::remainder(17.0f, 2 * 3.141592653589793)));
  assert(std::isnan(WrapViewAngle(std::numeric_limits<float>::infinity())));
  assert((NormalizeViewDirection({0,0,0}) == std::array<float,3>{}));
  assert((NormalizeViewDirection({1e-25f,0,0}) == std::array<float,3>{}));
  assert(Near(NormalizeViewDirection({3,4,0})[0], 0.6));

  RenderMatrix projection{2,0,0,0, 0,4,0,0, 0,0,-1.01f,-1, 0,0,-1.01f,0};
  CheckInverse(projection);
  auto shape = BuildViewFrustumShape(identity, projection, clips);
  assert(Near(shape.right, 0.5) && Near(shape.left, -0.5));
  assert(Near(shape.top, 0.275) && Near(shape.bottom, -0.275));
  assert(Near(shape.near_distance, 1) && Near(shape.far_distance, 101, 3e-5));
  assert((shape.origin == std::array<float,3>{}));
  assert(Near(std::abs(shape.orientation[1]), 1));
  assert(Near(shape.orientation[0], 0) && Near(shape.orientation[2], 0));
  const RenderMatrix distant{2.4142134f,0,0,0, 0,4.291935f,0,0,
                              0,0,1.00005f,1, 0,0,-1.00005f,0};
  const auto far_shape = BuildViewFrustumShape(identity, distant);
  // Projection recorded in the second desktop comparison: shape checks pass
  // before a separate translated-plane trigonometry mismatch. The first run's
  // -20020.547 observation belonged to a different projection, not this fixture.
  assert(Near(far_shape.far_distance, -19996.6797, 1e-5));
  // Off-centre stereo projection changes horizontal bounds independently.
  projection[8] = 0.2f;
  shape = BuildViewFrustumShape(identity, projection, clips);
  assert(Near(shape.right, 0.6) && Near(shape.left, -0.4));
  auto view = identity;
  view[12] = -5; view[13] = 3; view[14] = -7;
  shape = BuildViewFrustumShape(view, projection, clips);
  assert(Near(shape.origin[0], 5) && Near(shape.origin[1], -3) && Near(shape.origin[2], 7));
  // A roll-only view leaves the roll-free culling orientation unchanged.
  view = {0,1,0,0, -1,0,0,0, 0,0,1,0, 0,0,0,1};
  shape = BuildViewFrustumShape(view, projection, clips);
  assert(Near(std::abs(shape.orientation[1]), 1) && Near(shape.orientation[2], 0));
  const auto singular = InverseRenderMatrix({});
  for (float value : singular)
    assert(std::isnan(value));
  shape = BuildViewFrustumShape({}, {}, clips);
  for (float value : shape.orientation)
    assert(std::isnan(value));
  assert(std::isnan(shape.near_distance) && std::isnan(shape.right));

  NativeViewCache cache;
  assert(!cache.Get(0));
  shape = BuildViewFrustumShape(identity, projection, clips);
  cache.Publish(0, shape);
  cache.Publish(900, shape); // native capacity is not the seven-slot adapter
  shape.origin[0] = 123;
  cache.Publish(1, shape);
  assert(cache.Get(0)->origin[0] == 0 && cache.Get(1)->origin[0] == 123);
  cache.Invalidate(0);
  assert(!cache.Get(0) && cache.Get(900));
  cache.Clear();
  assert(!cache.Get(1) && !cache.Get(900));
}
