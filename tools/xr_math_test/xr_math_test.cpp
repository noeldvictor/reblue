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
#include <limits>
#include <cstdio>
#include <numbers>

#include "xr/xr_camera.h"
#include "xr/xr_cull.h"
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

// --- camera composition ---
//
// Reachable only because xr_camera.cpp takes its settings as a pushed
// CameraTuning instead of reading the cvar singleton. Keep it that way.

// Where the composed camera ended up, recovered by asking which world point
// lands at the view-space origin. Cheaper than inverting the matrix and it
// checks the matrix rather than trusting it.
bool CameraSitsAt(const EyeMatrices &eye, Vec3 expected) {
  const Vec4 v = Mul(Vec4{expected.x, expected.y, expected.z, 1.0f}, eye.view);
  return Near(v.x, 0.0f) && Near(v.y, 0.0f) && Near(v.z, 0.0f);
}

// Neutral inputs: identity head, identity eye, game camera looking down +Z
// from the origin. Recenter against these is a no-op, which keeps each case
// independent of the ones before it.
void ResetCamera(Camera &c, CameraMode mode, const CameraTuning &tuning) {
  c.SetTuning(tuning);
  c.SubmitGameCamera(GameCamera{});
  c.SubmitCharacter(CharacterAnchor{});
  c.SubmitHeadPose(Pose{});
  c.SetMode(mode);
  c.Recenter();
  c.ResetSmoothing();
}

const Fov kFov{-0.7f, 0.7f, 0.7f, -0.7f};

void TestCameraModes() {
  std::printf("camera modes\n");

  Camera &c = Camera::Get();
  CameraTuning t;

  // Third person: the anchor sits at the offset, expressed in the character's
  // own frame, so it stays behind them as they turn.
  ResetCamera(c, CameraMode::ThirdPerson, t);
  CharacterAnchor ch;
  ch.valid = true;
  ch.position = {0.0f, 0.0f, 0.0f};
  ch.eyeHeight = 1.6f;
  ch.facingYaw = 0.0f;
  c.SubmitCharacter(ch);
  Check(CameraSitsAt(c.ComposeEye(Pose{}, kFov), Vec3{0.0f, 1.5f, -3.0f}),
        "third person: anchor sits at the offset behind the character");

  // Facing +90 degrees about Y takes the character's forward from +Z to +X,
  // so the camera behind them must swing to -X.
  ResetCamera(c, CameraMode::Diorama, t); // force a real mode change back
  ResetCamera(c, CameraMode::ThirdPerson, t);
  ch.facingYaw = kPi * 0.5f;
  c.SubmitCharacter(ch);
  Check(CameraSitsAt(c.ComposeEye(Pose{}, kFov), Vec3{-3.0f, 1.5f, 0.0f}),
        "third person: the offset rotates with the character's facing");

  // First person: anchor at the character's eyes.
  ResetCamera(c, CameraMode::FirstPerson, t);
  ch.facingYaw = 0.0f;
  c.SubmitCharacter(ch);
  Check(CameraSitsAt(c.ComposeEye(Pose{}, kFov), Vec3{0.0f, 1.6f, 0.0f}),
        "first person: anchor sits at the character's eye height");

  // Diorama detaches from the character entirely and floats above the game's
  // own camera.
  ResetCamera(c, CameraMode::Diorama, t);
  GameCamera gc;
  gc.position = {10.0f, 0.0f, 20.0f};
  c.SubmitGameCamera(gc);
  c.ResetSmoothing();
  Check(CameraSitsAt(c.ComposeEye(Pose{}, kFov), Vec3{10.0f, 8.0f, 20.0f}),
        "diorama: anchor floats above the game camera, ignoring the character");

  // No character: the attached modes fall back to the game camera rather than
  // snapping to the origin. This happens during menus and event scenes.
  ResetCamera(c, CameraMode::ThirdPerson, t);
  c.SubmitGameCamera(gc);
  c.SubmitCharacter(CharacterAnchor{}); // valid = false
  c.ResetSmoothing();
  Check(CameraSitsAt(c.ComposeEye(Pose{}, kFov), gc.position),
        "no character: third person falls back to the game camera");
}

void TestWorldScale() {
  std::printf("world scale\n");

  Camera &c = Camera::Get();
  CameraTuning t;
  t.unitsPerMetre = 1.0f;
  t.worldScale = 1.0f;

  // A head one metre to the right, life size, moves the camera one game unit.
  ResetCamera(c, CameraMode::Diorama, t);
  Pose eye;
  eye.position = {1.0f, 0.0f, 0.0f};
  Check(CameraSitsAt(c.ComposeEye(eye, kFov), Vec3{1.0f, 8.0f, 0.0f}),
        "life size: one metre of head movement is one game unit");

  // Shrink the world tenfold and the same metre has to cover ten times the
  // game distance, or the world stops reading as small.
  t.worldScale = 0.1f;
  ResetCamera(c, CameraMode::Diorama, t);
  Check(CameraSitsAt(c.ComposeEye(eye, kFov), Vec3{10.0f, 8.0f, 0.0f}),
        "world scale 0.1: the same metre covers ten game units");

  // Units-per-metre is the other factor and multiplies rather than divides.
  t.worldScale = 1.0f;
  t.unitsPerMetre = 4.0f;
  ResetCamera(c, CameraMode::Diorama, t);
  Check(CameraSitsAt(c.ComposeEye(eye, kFov), Vec3{4.0f, 8.0f, 0.0f}),
        "units per metre scales head movement independently");

  // A zero world scale must not divide by zero and blow the camera to NaN.
  t.worldScale = 0.0f;
  t.unitsPerMetre = 1.0f;
  ResetCamera(c, CameraMode::Diorama, t);
  const EyeMatrices m = c.ComposeEye(eye, kFov);
  Check(std::isfinite(m.view.m[3][0]) && std::isfinite(m.view.m[3][2]),
        "zero world scale degrades to life size rather than NaN");
}

void TestRecentreAndTurn() {
  std::printf("recentre and turn");
  std::printf("\n");

  Camera &c = Camera::Get();
  CameraTuning t;
  ResetCamera(c, CameraMode::Diorama, t);

  // Facing 90 degrees away physically, recentring should line the player back
  // up with the game's forward.
  c.SubmitHeadPose(Pose{{0, 0, 0}, RotAboutY(kPi * 0.5f)});
  c.Recenter();
  Check(Near(c.LocomotionYaw(), 0.0f),
        "recentre lines locomotion up with the game camera");

  // A snap turn moves locomotion forward with it.
  c.ApplyTurn(kPi * 0.5f);
  Check(Near(c.LocomotionYaw(), kPi * 0.5f), "snap turn offsets locomotion yaw");

  // Recentring cancels where the player has wandered to in the play space,
  // but must not cancel the vertical - standing up should still raise the
  // view, and zeroing it is what makes a recentre feel like the floor moved.
  //
  // Composing with the eye at the same place the head was recentred from is
  // the meaningful case: the horizontal drift should vanish completely and the
  // height should survive untouched.
  ResetCamera(c, CameraMode::Diorama, t);
  const Pose stood{{0.5f, 1.7f, 0.25f}, Quat{}};
  c.SubmitHeadPose(stood);
  c.Recenter();
  const EyeMatrices m = c.ComposeEye(stood, kFov);
  Check(CameraSitsAt(m, Vec3{0.0f, 8.0f + 1.7f, 0.0f}),
        "recentre zeroes horizontal drift and keeps height");

  // And a step away from the recentre point still moves the camera, by the
  // step. Expressed in game space, so the Z is mirrored from the OpenXR input:
  // half a metre further back in OpenXR (+Z) is half a metre back in game
  // space (-Z).
  const EyeMatrices stepped =
      c.ComposeEye(Pose{{1.0f, 1.7f, 0.75f}, Quat{}}, kFov);
  Check(CameraSitsAt(stepped, Vec3{0.5f, 8.0f + 1.7f, -0.5f}),
        "moving off the recentre point moves the camera by that much");
}

// --- culling volume ---

void TestCullVolume() {
  std::printf("cull volume\n");

  // A plausible headset: 64 mm IPD, both eyes level and parallel, asymmetric
  // per-eye frustums because no real headset has centred optics.
  Pose left, right;
  left.position = {-0.032f, 0.0f, 0.0f};
  right.position = {0.032f, 0.0f, 0.0f};
  const Fov lf{-0.9f, 0.8f, 0.8f, -0.85f};
  const Fov rf{-0.8f, 0.9f, 0.8f, -0.85f}; // mirrored, as they usually are

  const CullVolume v = CullVolume::FromEyes(left, lf, right, rf, 1.0f);

  // The property the whole construction exists for: every point either eye can
  // see must be inside the shared volume. If this fails, geometry disappears
  // from one side of the stereo image and reads as flicker, not as a culling
  // bug, which is a horrible thing to chase.
  bool contained = true;
  const Pose eyes[] = {left, right};
  const Fov fovs[] = {lf, rf};
  const f32 depths[] = {0.05f, 0.5f, 1.0f, 25.0f, 1000.0f};
  for (int e = 0; e < 2; ++e) {
    const f32 tx[] = {std::tan(fovs[e].angleLeft), std::tan(fovs[e].angleRight)};
    const f32 ty[] = {std::tan(fovs[e].angleDown), std::tan(fovs[e].angleUp)};
    for (f32 z : depths) {
      for (f32 x : tx) {
        for (f32 y : ty) {
          const Vec3 corner{eyes[e].position.x + x * z,
                            eyes[e].position.y + y * z, z};
          if (!v.TestPoint(corner))
            contained = false;
        }
      }
    }
  }
  Check(contained, "combined volume contains both eye frustums, 40 corners");

  Check(v.TestPoint(Vec3{0.0f, 0.0f, 10.0f}), "accepts geometry straight ahead");
  Check(!v.TestPoint(Vec3{0.0f, 0.0f, -100.0f}),
        "rejects geometry behind the viewer");
  Check(!v.TestPoint(Vec3{1000.0f, 0.0f, 1.0f}),
        "rejects geometry far off to the side");
  Check(!v.TestPoint(Vec3{0.0f, 1000.0f, 1.0f}),
        "rejects geometry far overhead");

  // Conservative on purpose: the guest hands over bounding volumes, and a
  // false positive costs a draw call where a false negative costs a hole in
  // the world.
  Check(v.TestSphere(Vec3{1000.0f, 0.0f, 1.0f}, 1200.0f),
        "a large enough sphere straddling the edge counts as visible");

  // The margin is what stops geometry popping the instant the head turns.
  const Vec3 justOutside{-3.0f, 0.0f, 1.0f};
  Check(!v.TestPoint(justOutside), "point outside the tight volume is rejected");
  const CullVolume wide = CullVolume::FromEyes(left, lf, right, rf, 3.0f);
  Check(wide.TestPoint(justOutside), "the same point is inside a widened one");

  // A margin below 1 would cull inside what is actually visible, so it clamps.
  const CullVolume clamped = CullVolume::FromEyes(left, lf, right, rf, 0.1f);
  Check(clamped.TestPoint(Vec3{0.0f, 0.0f, 10.0f}),
        "a margin below 1 clamps rather than culling the view away");

  // Turning the head must carry the volume with it: what was ahead is now off
  // to the side, and what was to the side is now ahead.
  Pose tl = left, tr = right;
  tl.orientation = tr.orientation = RotAboutY(kPi * 0.5f);
  tl.position = Rotate(tl.orientation, left.position);
  tr.position = Rotate(tr.orientation, right.position);
  const CullVolume turned = CullVolume::FromEyes(tl, lf, tr, rf, 1.0f);
  Check(turned.TestPoint(Vec3{10.0f, 0.0f, 0.0f}),
        "turned volume accepts what is now ahead");
  Check(!turned.TestPoint(Vec3{0.0f, 0.0f, -10.0f}),
        "turned volume rejects what is now behind");

  // Eyes offset vertically as well, to exercise the other pull-back axis.
  Pose vl = left, vr = right;
  vl.position = {-0.032f, 0.01f, 0.0f};
  vr.position = {0.032f, -0.01f, 0.0f};
  const CullVolume tilted = CullVolume::FromEyes(vl, lf, vr, rf, 1.0f);
  bool tiltedContained = true;
  const Pose tiltedEyes[] = {vl, vr};
  for (int e = 0; e < 2; ++e) {
    const f32 tx[] = {std::tan(fovs[e].angleLeft), std::tan(fovs[e].angleRight)};
    const f32 ty[] = {std::tan(fovs[e].angleDown), std::tan(fovs[e].angleUp)};
    for (f32 z : depths) {
      for (f32 x : tx) {
        for (f32 y : ty) {
          const Vec3 corner{tiltedEyes[e].position.x + x * z,
                            tiltedEyes[e].position.y + y * z, z};
          if (!tilted.TestPoint(corner))
            tiltedContained = false;
        }
      }
    }
  }
  Check(tiltedContained, "containment holds with vertically offset eyes");
}

// The guest hands out a view matrix full of NaN on some frames - loads, camera
// cuts - and draws nothing on those, so it never notices. The anchor here is
// low-passed, and Lerp(NaN, x, t) is NaN, so before this was guarded a single
// poisoned frame stuck to the camera for the rest of the session and every
// frame after it rendered black inside the headset.
//
// The bug that motivated this test was found on hardware, from a log line,
// after a black screen. It is exactly the kind of thing this harness should
// have caught first: it needs no headset and no renderer.
void TestNaNRecovery() {
  std::printf("NaN recovery\n");

  const f32 nan = std::numeric_limits<f32>::quiet_NaN();
  Camera &c = Camera::Get();
  CameraTuning t;
  ResetCamera(c, CameraMode::ThirdPerson, t);

  GameCamera good;
  good.position = {10.0f, 20.0f, 30.0f};

  // Settle on a real camera first, so the anchor holds something valid.
  for (int i = 0; i < 16; ++i)
    c.SubmitGameCamera(good);
  EyeMatrices eye = c.ComposeEye(Pose{}, kFov);
  Check(std::isfinite(eye.view.m[3][0]), "a normal game camera composes finite");

  // One poisoned frame.
  GameCamera poisoned;
  poisoned.position = {nan, nan, nan};
  c.SubmitGameCamera(poisoned);
  eye = c.ComposeEye(Pose{}, kFov);
  Check(std::isfinite(eye.view.m[3][0]),
        "a NaN game camera does not produce a NaN view");

  // And - the part that actually mattered - the damage must not persist. Every
  // later frame is good, so every later frame must compose.
  bool recovered = true;
  for (int i = 0; i < 8; ++i) {
    c.SubmitGameCamera(good);
    eye = c.ComposeEye(Pose{}, kFov);
    for (int r = 0; r < 4 && recovered; ++r)
      for (int col = 0; col < 4; ++col)
        if (!std::isfinite(eye.view.m[r][col]))
          recovered = false;
  }
  Check(recovered, "the camera recovers on the next good frame");
}

// The anchor derivation. Every one of these is a convention that is invisible
// in a symmetric pose, which is the class of bug this file exists to catch.
void TestFollowCameraAnchor() {
  using namespace bd::xr;

  // Camera behind the leader, looking along +Z, 1.5m up in 100-units-per-metre
  // game units.
  GameCamera game;
  game.position = {0.0f, 150.0f, -300.0f};
  game.forward = {0.0f, 0.0f, 1.0f};
  game.up = {0.0f, 1.0f, 0.0f};

  const CharacterAnchor a = CharacterFromFollowCamera(game, 300.0f, 150.0f);
  Check(a.valid, "a follow camera yields a valid anchor");
  // Leader is one follow distance along forward, and the feet sit eyeHeight
  // below the look-at point - so on the ground at the origin.
  Check(Near(a.position.x, 0.0f) && Near(a.position.y, 0.0f) &&
            Near(a.position.z, 0.0f),
        "the leader's feet land on the ground under the look-at point");
  Check(Near(a.eyeHeight, 150.0f), "eye height is carried through");
  Check(Near(a.facingYaw, 0.0f), "forward +Z is yaw zero");

  // Off-axis, because a symmetric case cannot see a sign error. Camera looking
  // along +X means the leader faces +X, which is yaw +pi/2 under the same
  // convention YawOf uses.
  game.position = {0.0f, 150.0f, 0.0f};
  game.forward = {1.0f, 0.0f, 0.0f};
  const CharacterAnchor b = CharacterFromFollowCamera(game, 300.0f, 150.0f);
  Check(Near(b.position.x, 300.0f), "the leader is along the camera's forward");
  Check(Near(b.facingYaw, 1.5707964f), "+X forward is yaw +pi/2");

  // FirstPerson puts the eye at position.y + eyeHeight; that must land back on
  // the camera's look-at height, or the player's head sits in the floor.
  Check(Near(b.position.y + b.eyeHeight, 150.0f),
        "eye height returns to the look-at point");

  // The documented opt-out, and the NaN guard.
  Check(!CharacterFromFollowCamera(game, 0.0f, 150.0f).valid,
        "distance 0 opts out");
  game.forward = {std::nanf(""), 0.0f, 1.0f};
  Check(!CharacterFromFollowCamera(game, 300.0f, 150.0f).valid,
        "a non-finite camera yields no anchor");
}

} // namespace

int main() {
  std::printf("xr_math checks\n\n");
  TestHandednessInvariant();
  TestViewMatrix();
  TestProjection();
  TestYaw();
  TestQuatBasics();
  TestCameraModes();
  TestFollowCameraAnchor();
  TestWorldScale();
  TestRecentreAndTurn();
  TestCullVolume();
  TestNaNRecovery();

  std::printf("\n%s\n", g_failures == 0 ? "all checks passed"
                                        : "FAILURES PRESENT");
  return g_failures == 0 ? 0 : 1;
}
