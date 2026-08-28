/**
 * @file    tools/xr_math_test/xr_math_test.cpp
 * @brief   Standalone checks for xr/xr_math.h.
 *
 * This tree cannot do a full build - there is no assets/default.xex and no
 * ReXGlue SDK, so codegen never runs. xr_math.h is deliberately written to
 * depend on nothing but the integer aliases, which means the one piece of the
 * VR work most likely to be silently wrong is also the one piece that can be
 * compiled and run on its own. That is the entire reason this file exists.
 *
 * Build and run:
 *   cmake -S tools/xr_math_test -B out/xr_math_test -G Ninja
 *   cmake --build out/xr_math_test && ./out/xr_math_test/xr_math_test
 *
 * @copyright Copyright (c) 2026 re:Blue contributors
 * @license   BSD 3-Clause - see LICENSE
 */
#include <cmath>
#include <cstdio>
#include <numbers>

#include "xr/xr_math.h"

using namespace bd::xr;

namespace {

int g_failures = 0;

constexpr f32 kEps = 1e-4f;

void Check(bool ok, const char *what) {
  if (!ok) {
    ++g_failures;
    std::printf("  FAIL  %s\n", what);
  } else {
    std::printf("  ok    %s\n", what);
  }
}

bool Near(f32 a, f32 b) { return std::fabs(a - b) < kEps; }

bool Near(Vec3 a, Vec3 b) {
  return Near(a.x, b.x) && Near(a.y, b.y) && Near(a.z, b.z);
}

// Row-vector convention: v' = v * M. Written out here rather than added to
// xr_math.h because the shipping code composes matrices for the guest to use
// and never transforms points itself.
struct Vec4 {
  f32 x, y, z, w;
};

Vec4 Mul(Vec4 v, const Mat4 &m) {
  Vec4 o{};
  o.x = v.x * m.m[0][0] + v.y * m.m[1][0] + v.z * m.m[2][0] + v.w * m.m[3][0];
  o.y = v.x * m.m[0][1] + v.y * m.m[1][1] + v.z * m.m[2][1] + v.w * m.m[3][1];
  o.z = v.x * m.m[0][2] + v.y * m.m[1][2] + v.z * m.m[2][2] + v.w * m.m[3][2];
  o.w = v.x * m.m[0][3] + v.y * m.m[1][3] + v.z * m.m[2][3] + v.w * m.m[3][3];
  return o;
}

constexpr f32 kPi = static_cast<f32>(std::numbers::pi);

Quat RotAboutY(f32 radians) {
  const f32 half = radians * 0.5f;
  return {0.0f, std::sin(half), 0.0f, std::cos(half)};
}

// --- the handedness invariant ---
//
// The one property that has to hold for the whole conversion to be correct:
// converting a rotated vector must equal rotating the converted vector by the
// converted rotation. If this passes, the mirror-on-Z is right; if it fails,
// every VR scene will be subtly mirrored and nothing downstream will explain
// why.
void TestHandednessInvariant() {
  std::printf("handedness invariant\n");

  const Vec3 samples[] = {
      {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f, 0.0f},  {0.0f, 1.0f, 0.0f},
      {0.3f, -0.7f, 0.6f}, {-2.0f, 0.5f, 1.5f},
  };
  const Quat rotations[] = {
      RotAboutY(kPi * 0.5f),
      RotAboutY(-kPi * 0.25f),
      Normalize(Quat{0.2f, 0.4f, -0.1f, 0.8f}),
      Normalize(Quat{-0.5f, 0.1f, 0.3f, 0.7f}),
  };

  bool allOk = true;
  for (const Quat &qXr : rotations) {
    for (const Vec3 &vXr : samples) {
      // Rotate in OpenXR space, then convert.
      const Vec3 lhs = FromOpenXRPosition(Rotate(qXr, vXr));
      // Convert both, then rotate in game space.
      const Vec3 rhs =
          Rotate(FromOpenXRRotation(qXr), FromOpenXRPosition(vXr));
      if (!Near(lhs, rhs))
        allOk = false;
    }
  }
  Check(allOk, "convert(R*v) == convert(R)*convert(v) over 20 pairs");

  // A concrete case, worked by hand, so a regression says which way it broke.
  // +90 degrees about Y in OpenXR takes -Z (forward) to -X.
  const Vec3 turned = Rotate(RotAboutY(kPi * 0.5f), Vec3{0.0f, 0.0f, -1.0f});
  Check(Near(turned, Vec3{-1.0f, 0.0f, 0.0f}),
        "OpenXR: +90 about Y takes forward(-Z) to -X");
  Check(Near(FromOpenXRPosition(Vec3{0.0f, 0.0f, -1.0f}), Vec3{0, 0, 1}),
        "OpenXR forward(-Z) converts to game forward(+Z)");
}

void TestViewMatrix() {
  std::printf("view matrix\n");

  // Identity camera at the origin: a point ten units ahead should still be ten
  // units ahead in view space.
  const Mat4 v = ViewFromPose(Pose{});
  const Vec4 ahead = Mul(Vec4{0.0f, 0.0f, 10.0f, 1.0f}, v);
  Check(Near(ahead.x, 0.0f) && Near(ahead.y, 0.0f) && Near(ahead.z, 10.0f),
        "identity pose leaves a forward point on +Z");

  // Translate the camera and the point should come back relative to it.
  Pose moved;
  moved.position = {0.0f, 2.0f, 5.0f};
  const Vec4 rel = Mul(Vec4{0.0f, 2.0f, 15.0f, 1.0f}, ViewFromPose(moved));
  Check(Near(rel.x, 0.0f) && Near(rel.y, 0.0f) && Near(rel.z, 10.0f),
        "translated camera subtracts its own position");

  // Turn the camera 90 degrees left about Y (game space, left-handed). A point
  // that was on +X should end up straight ahead.
  Pose turned;
  turned.orientation = RotAboutY(kPi * 0.5f);
  const Vec4 side = Mul(Vec4{10.0f, 0.0f, 0.0f, 1.0f}, ViewFromPose(turned));
  Check(Near(side.z, 10.0f) && Near(side.x, 0.0f),
        "rotating the camera brings a side point to +Z");

  // The rotation block must stay orthonormal, or the scene shears.
  const Vec3 r{v.m[0][0], v.m[1][0], v.m[2][0]};
  const Vec3 u{v.m[0][1], v.m[1][1], v.m[2][1]};
  const Vec3 f{v.m[0][2], v.m[1][2], v.m[2][2]};
  Check(Near(Length(r), 1.0f) && Near(Length(u), 1.0f) && Near(Length(f), 1.0f),
        "view basis vectors are unit length");
  Check(Near(Dot(r, u), 0.0f) && Near(Dot(r, f), 0.0f) && Near(Dot(u, f), 0.0f),
        "view basis vectors are orthogonal");
}

void TestProjection() {
  std::printf("projection\n");

  const f32 nearZ = 0.05f;
  const f32 farZ = 20000.0f;
  const f32 q = kPi * 0.25f; // 45 degrees, so a symmetric 90 degree frustum
  const Fov symmetric{-q, q, q, -q};
  const Mat4 p = ProjectionFromFov(symmetric, nearZ, farZ);

  // Depth must land in [0, 1]: D3D convention, not OpenGL's [-1, 1]. Getting
  // this wrong gives a scene that renders but z-fights everywhere.
  const Vec4 atNear = Mul(Vec4{0.0f, 0.0f, nearZ, 1.0f}, p);
  const Vec4 atFar = Mul(Vec4{0.0f, 0.0f, farZ, 1.0f}, p);
  Check(Near(atNear.z / atNear.w, 0.0f), "near plane maps to depth 0");
  Check(Near(atFar.z / atFar.w, 1.0f), "far plane maps to depth 1");

  // w must carry view-space depth, or the perspective divide does nothing.
  Check(Near(atFar.w, farZ), "w receives view-space z");

  // At 90 degrees horizontal, a point at 45 degrees is exactly on the edge.
  const Vec4 edge = Mul(Vec4{1.0f, 0.0f, 1.0f, 1.0f}, p);
  Check(Near(edge.x / edge.w, 1.0f), "45 degrees lands on the right edge");

  const Vec4 centre = Mul(Vec4{0.0f, 0.0f, 1.0f, 1.0f}, p);
  Check(Near(centre.x / centre.w, 0.0f) && Near(centre.y / centre.w, 0.0f),
        "a point straight ahead lands at the centre");

  // The asymmetric case is the one that actually ships: no headset has a
  // centred frustum, and a symmetric approximation is subtly wrong rather than
  // obviously broken, which is worse.
  const Fov asym{-0.9f, 0.7f, 0.8f, -0.85f};
  const Mat4 pa = ProjectionFromFov(asym, nearZ, farZ);
  const Vec4 l = Mul(Vec4{std::tan(asym.angleLeft), 0.0f, 1.0f, 1.0f}, pa);
  const Vec4 r = Mul(Vec4{std::tan(asym.angleRight), 0.0f, 1.0f, 1.0f}, pa);
  const Vec4 up = Mul(Vec4{0.0f, std::tan(asym.angleUp), 1.0f, 1.0f}, pa);
  Check(Near(l.x / l.w, -1.0f), "asymmetric: left tangent maps to -1");
  Check(Near(r.x / r.w, 1.0f), "asymmetric: right tangent maps to +1");
  Check(Near(up.y / up.w, 1.0f), "asymmetric: up tangent maps to +1");
}

void TestYaw() {
  std::printf("yaw extraction\n");

  Check(Near(YawOf(Quat{}), 0.0f), "identity has zero yaw");
  Check(Near(YawOf(RotAboutY(kPi * 0.5f)), kPi * 0.5f), "+90 about Y reads back");
  Check(Near(YawOf(RotAboutY(-kPi * 0.25f)), -kPi * 0.25f), "-45 about Y reads back");

  // Yaw must ignore pitch, or walking forward while looking down steers you
  // into the floor.
  const Quat pitched = RotAboutY(kPi * 0.5f) * Quat{std::sin(0.2f), 0, 0, std::cos(0.2f)};
  Check(Near(YawOf(pitched), kPi * 0.5f), "pitch does not disturb yaw");

  Check(Near(YawOf(QuatFromYaw(1.1f)), 1.1f), "QuatFromYaw round-trips");
}

void TestQuatBasics() {
  std::printf("quaternion basics\n");

  const Quat a = RotAboutY(kPi * 0.5f);
  Check(Near(Rotate(a * Conjugate(a), Vec3{1, 2, 3}), Vec3{1, 2, 3}),
        "q * conj(q) is the identity rotation");

  // Composition order: applying a then b must equal rotating by (a*b).
  const Quat b = Normalize(Quat{0.1f, 0.3f, 0.2f, 0.9f});
  const Vec3 v{0.4f, -1.2f, 2.0f};
  Check(Near(Rotate(a, Rotate(b, v)), Rotate(a * b, v)),
        "Rotate(a, Rotate(b, v)) == Rotate(a*b, v)");

  Check(Near(Length(Rotate(a, v)), Length(v)), "rotation preserves length");
}

} // namespace

int main() {
  std::printf("xr_math checks\n\n");
  TestHandednessInvariant();
  TestViewMatrix();
  TestProjection();
  TestYaw();
  TestQuatBasics();

  std::printf("\n%s\n", g_failures == 0 ? "all checks passed"
                                        : "FAILURES PRESENT");
  return g_failures == 0 ? 0 : 1;
}
