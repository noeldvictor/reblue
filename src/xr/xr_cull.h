/**
 * @file    xr/xr_cull.h
 * @brief   One culling volume covering both eyes, for bdCameraViewFrustumTest.
 *
 * @copyright Copyright (c) 2026 re:Blue contributors
 * @license   BSD 3-Clause - see LICENSE
 */
#pragma once

#include <rex/types.h>

#include "xr/xr_math.h"

namespace bd::xr {

// Why one volume rather than two frustums, or one eye's frustum widened by a
// fudge factor:
//
// An object occluded for one eye can be visible to the other, so testing a
// single eye is simply wrong - geometry vanishes from one side of the stereo
// image and the effect reads as flicker rather than as a culling bug. Testing
// both eyes is correct and costs twice as much. A single volume enclosing both
// is correct for both viewpoints at roughly the price of one, which is what the
// VR industry converged on.
//
// The construction: take the widest of the two eyes' tangents on each side,
// place the apex at the midpoint between them, then pull that apex backwards
// far enough that the resulting frustum provably contains both eye frustums.
// Without the pull-back a frustum at the midpoint clips the outer edges of each
// eye, because the eyes are offset from the point it is built around.
//
// The margin on top is for head motion: the guest culls once per frame but the
// head keeps moving until the moment of display, so the volume has to cover
// where the player is about to be looking, not where they were.

// Inward-facing half-space. A point is inside when Dot(normal, p) + d >= 0.
struct Plane {
  Vec3 normal;
  f32 d = 0.0f;
};

class CullVolume {
public:
  // Eye poses are in game space, already converted. margin scales the
  // tangents: 1.0 is exactly what is visible now and pops the instant the head
  // turns; bd_vr_cull_expand defaults to 1.5.
  static CullVolume FromEyes(const Pose &leftEye, const Fov &leftFov,
                             const Pose &rightEye, const Fov &rightFov,
                             f32 margin);

  // Conservative: a sphere touching the volume counts as visible. The guest
  // gives bounding volumes, so false positives cost a draw call and false
  // negatives cost a hole in the world.
  bool TestSphere(Vec3 centre, f32 radius) const;

  bool TestPoint(Vec3 p) const { return TestSphere(p, 0.0f); }

  // Left, right, bottom, top. No near or far plane: distance culling is the
  // guest's own business and it already does it against its LOD tables.
  const Plane &SidePlane(u32 i) const { return planes_[i]; }

  Vec3 Apex() const { return apex_; }

private:
  Plane planes_[4];
  Vec3 apex_;
};

} // namespace bd::xr
