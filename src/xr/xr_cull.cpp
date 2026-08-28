/**
 * @file    xr/xr_cull.cpp
 * @license BSD 3-Clause, see LICENSE
 */
#include "xr/xr_cull.h"

#include <algorithm>
#include <cmath>

namespace bd::xr {

namespace {

// Below this the pull-back would divide by something near zero and throw the
// apex to infinity. A frustum this narrow is not a real headset; clamp rather
// than produce a volume that culls everything.
constexpr f32 kMinTangent = 1e-3f;

// The pull-back below is derived to put the outermost corner of each eye
// frustum exactly on the boundary. Exactly is not good enough in float: the
// corner lands a few parts per million outside and gets culled. Widening the
// tangents by a hair costs nothing and makes the containment strict, which is
// the direction a culling volume should always err in - a false positive is a
// draw call, a false negative is a hole in the world.
constexpr f32 kEdgeSlack = 1.0001f;

Plane MakePlane(Vec3 normalInView, const Quat &orientation, Vec3 apex) {
  const Vec3 n = Normalize(Rotate(orientation, Normalize(normalInView)));
  return {n, -Dot(n, apex)};
}

} // namespace

CullVolume CullVolume::FromEyes(const Pose &leftEye, const Fov &leftFov,
                                const Pose &rightEye, const Fov &rightFov,
                                f32 margin) {
  CullVolume out;

  // Widest tangent on each side, so the union of both eyes' angular coverage
  // is enclosed rather than their intersection.
  const f32 scale = std::max(margin, 1.0f) * kEdgeSlack;
  const f32 tanL = std::min(std::tan(leftFov.angleLeft),
                            std::tan(rightFov.angleLeft)) * scale;
  const f32 tanR = std::max(std::tan(leftFov.angleRight),
                            std::tan(rightFov.angleRight)) * scale;
  const f32 tanD = std::min(std::tan(leftFov.angleDown),
                            std::tan(rightFov.angleDown)) * scale;
  const f32 tanU = std::max(std::tan(leftFov.angleUp),
                            std::tan(rightFov.angleUp)) * scale;

  // Orientation from the left eye. The runtime keeps both eyes on a common
  // orientation in every sane configuration, and averaging two quaternions
  // properly is more work than the difference could ever justify.
  const Quat orientation = leftEye.orientation;

  const Vec3 midpoint = (leftEye.position + rightEye.position) * 0.5f;

  // How far apart the eyes are, measured across the volume's own right axis:
  // that is the direction the offset actually displaces the frustums along.
  const Vec3 right = Rotate(orientation, Vec3{1.0f, 0.0f, 0.0f});
  const Vec3 up = Rotate(orientation, Vec3{0.0f, 1.0f, 0.0f});
  const Vec3 separation = rightEye.position - leftEye.position;
  const f32 halfX = std::fabs(Dot(separation, right)) * 0.5f;
  const f32 halfY = std::fabs(Dot(separation, up)) * 0.5f;

  // Pull the apex back until the widened frustum swallows an eye sitting
  // half a separation off-axis. At distance z from the apex the frustum is
  // half-width tan*z, so an offset of halfX is covered once z >= halfX / tan.
  // Taking the tightest tangent on each axis covers every side at once.
  const f32 tightestX =
      std::max(std::min(std::fabs(tanL), std::fabs(tanR)), kMinTangent);
  const f32 tightestY =
      std::max(std::min(std::fabs(tanD), std::fabs(tanU)), kMinTangent);
  const f32 pullBack = std::max(halfX / tightestX, halfY / tightestY);

  const Vec3 forward = Rotate(orientation, Vec3{0.0f, 0.0f, 1.0f});
  out.apex_ = midpoint - forward * pullBack;

  // View-space inward normals. Inside the left plane is x/z >= tanL, which
  // rearranges to x - tanL*z >= 0, and so on around the four sides.
  out.planes_[0] = MakePlane({1.0f, 0.0f, -tanL}, orientation, out.apex_);
  out.planes_[1] = MakePlane({-1.0f, 0.0f, tanR}, orientation, out.apex_);
  out.planes_[2] = MakePlane({0.0f, 1.0f, -tanD}, orientation, out.apex_);
  out.planes_[3] = MakePlane({0.0f, -1.0f, tanU}, orientation, out.apex_);

  return out;
}

bool CullVolume::TestSphere(Vec3 centre, f32 radius) const {
  for (const Plane &p : planes_) {
    if (Dot(p.normal, centre) + p.d < -radius)
      return false;
  }
  return true;
}

} // namespace bd::xr
