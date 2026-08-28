/**
 * @file    xr/xr_math.h
 * @brief   The small amount of vector and matrix maths the XR layer needs, and
 *          the one place the OpenXR and Blue Dragon coordinate conventions are
 *          reconciled.
 *
 * @copyright Copyright (c) 2026 re:Blue contributors
 * @license   BSD 3-Clause - see LICENSE
 */
#pragma once

#include <cmath>

#include <rex/types.h>

namespace bd::xr {

// Handedness, written down once, because getting it wrong produces a world that
// looks almost right and is mirrored, which is a miserable thing to debug at
// three in the morning wearing a headset.
//
// OpenXR is right-handed: +X right, +Y up, -Z forward. Positions are metres.
//
// Blue Dragon is a Direct3D 9 era title and left-handed: +X right, +Y up,
// +Z forward, row-vector convention (v * M), row-major storage.
//
// The two differ by a mirror on Z. Everything crossing that boundary goes
// through FromOpenXRPosition / FromOpenXRRotation below, never by hand at the
// call site.

struct Vec3 {
  f32 x = 0.0f;
  f32 y = 0.0f;
  f32 z = 0.0f;
};

// Rotation, xyzw, same component order OpenXR uses.
struct Quat {
  f32 x = 0.0f;
  f32 y = 0.0f;
  f32 z = 0.0f;
  f32 w = 1.0f;
};

// A rigid transform. Camera-to-world unless the name says otherwise.
struct Pose {
  Vec3 position;
  Quat orientation;
};

// Asymmetric field of view as four signed angles in radians, matching XrFovf.
// Left and down are negative, right and up positive. Per eye, and not
// symmetric on any real headset, which is why this is four angles and not one
// FOV plus an aspect ratio the way the flat renderer works.
struct Fov {
  f32 angleLeft = 0.0f;
  f32 angleRight = 0.0f;
  f32 angleUp = 0.0f;
  f32 angleDown = 0.0f;
};

// Row-major, row-vector (v * M). Written to match what the guest already
// stores, so a composed matrix can go straight back through the hook. Byte
// order is the hook's problem, not this file's: guest memory is big-endian.
struct Mat4 {
  f32 m[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
};

// --- Vec3 ---

constexpr Vec3 operator+(Vec3 a, Vec3 b) {
  return {a.x + b.x, a.y + b.y, a.z + b.z};
}
constexpr Vec3 operator-(Vec3 a, Vec3 b) {
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}
constexpr Vec3 operator*(Vec3 v, f32 s) { return {v.x * s, v.y * s, v.z * s}; }
constexpr Vec3 operator-(Vec3 v) { return {-v.x, -v.y, -v.z}; }

constexpr f32 Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

constexpr Vec3 Cross(Vec3 a, Vec3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
          a.x * b.y - a.y * b.x};
}

inline f32 Length(Vec3 v) { return std::sqrt(Dot(v, v)); }

inline Vec3 Normalize(Vec3 v) {
  const f32 len = Length(v);
  return len > 0.0f ? v * (1.0f / len) : Vec3{};
}

// --- Quat ---

constexpr Quat operator*(Quat a, Quat b) {
  return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
          a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
          a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

constexpr Quat Conjugate(Quat q) { return {-q.x, -q.y, -q.z, q.w}; }

inline Quat Normalize(Quat q) {
  const f32 len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (len <= 0.0f)
    return Quat{};
  const f32 inv = 1.0f / len;
  return {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

// v' = q v q*, via the cross-product form rather than building a matrix.
constexpr Vec3 Rotate(Quat q, Vec3 v) {
  const Vec3 axis{q.x, q.y, q.z};
  const Vec3 t = Cross(axis, v) * 2.0f;
  return v + t * q.w + Cross(axis, t);
}

// Rotation about +Y. The only axis the recentre and snap-turn paths need, and
// worth having in closed form so they do not reach for a general axis-angle.
inline Quat QuatFromYaw(f32 radians) {
  const f32 half = radians * 0.5f;
  return {0.0f, std::sin(half), 0.0f, std::cos(half)};
}

// Yaw about +Y, ignoring any pitch and roll. Used to flatten a head pose into
// something locomotion can be resolved against: walking forward should not
// depend on whether the player is looking at their feet.
inline f32 YawOf(Quat q) {
  const Vec3 forward = Rotate(q, Vec3{0.0f, 0.0f, 1.0f});
  return std::atan2(forward.x, forward.z);
}

// --- OpenXR to game space ---

// Mirror on Z. Metres in, metres out: the game-unit conversion is world scale's
// job and lives in xr_camera.cpp, because it is a policy decision and this is
// not.
constexpr Vec3 FromOpenXRPosition(Vec3 v) { return {v.x, v.y, -v.z}; }

// Mirroring the Z axis conjugates the rotation by diag(1, 1, -1), which in
// quaternion terms negates x and y and leaves z and w alone.
constexpr Quat FromOpenXRRotation(Quat q) { return {-q.x, -q.y, q.z, q.w}; }

constexpr Pose FromOpenXRPose(const Pose &p) {
  return {FromOpenXRPosition(p.position), FromOpenXRRotation(p.orientation)};
}

// --- Matrices ---

// World-to-view for a camera at pose, left-handed, row-vector. The rotation
// block is the transpose of the camera basis and the translation row is the
// negated projection of the position onto it, which is what a rigid inverse
// comes out as.
inline Mat4 ViewFromPose(const Pose &pose) {
  const Vec3 right = Rotate(pose.orientation, Vec3{1.0f, 0.0f, 0.0f});
  const Vec3 up = Rotate(pose.orientation, Vec3{0.0f, 1.0f, 0.0f});
  const Vec3 forward = Rotate(pose.orientation, Vec3{0.0f, 0.0f, 1.0f});
  const Vec3 p = pose.position;

  Mat4 out;
  out.m[0][0] = right.x;   out.m[0][1] = up.x;   out.m[0][2] = forward.x;
  out.m[1][0] = right.y;   out.m[1][1] = up.y;   out.m[1][2] = forward.y;
  out.m[2][0] = right.z;   out.m[2][1] = up.z;   out.m[2][2] = forward.z;
  out.m[3][0] = -Dot(right, p);
  out.m[3][1] = -Dot(up, p);
  out.m[3][2] = -Dot(forward, p);
  out.m[0][3] = out.m[1][3] = out.m[2][3] = 0.0f;
  out.m[3][3] = 1.0f;
  return out;
}

// Off-centre perspective from the runtime's per-eye tangents, left-handed with
// depth in [0, 1]. This is XMMatrixPerspectiveOffCenterLH with the near-plane
// distance cancelled out, since l = near * tan(angleLeft) and so on.
//
// The asymmetry is the whole point: an HMD's eye frustums are not centred, and
// forcing a symmetric projection here is what makes a stereo image feel subtly
// wrong without ever looking obviously broken.
inline Mat4 ProjectionFromFov(const Fov &fov, f32 nearZ, f32 farZ) {
  const f32 tanL = std::tan(fov.angleLeft);
  const f32 tanR = std::tan(fov.angleRight);
  const f32 tanU = std::tan(fov.angleUp);
  const f32 tanD = std::tan(fov.angleDown);
  const f32 tanWidth = tanR - tanL;
  const f32 tanHeight = tanU - tanD;

  Mat4 out;
  out.m[0][0] = 2.0f / tanWidth;
  out.m[1][1] = 2.0f / tanHeight;
  out.m[2][0] = (tanR + tanL) / tanWidth;
  out.m[2][1] = (tanU + tanD) / tanHeight;
  out.m[2][2] = farZ / (farZ - nearZ);
  out.m[2][3] = 1.0f;
  out.m[3][2] = -(farZ * nearZ) / (farZ - nearZ);
  out.m[3][3] = 0.0f;
  out.m[0][1] = out.m[0][2] = out.m[0][3] = 0.0f;
  out.m[1][0] = out.m[1][2] = out.m[1][3] = 0.0f;
  out.m[3][0] = out.m[3][1] = 0.0f;
  return out;
}

} // namespace bd::xr
