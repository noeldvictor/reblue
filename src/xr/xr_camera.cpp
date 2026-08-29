/**
 * @file    xr/xr_camera.cpp
 * @license BSD 3-Clause, see LICENSE
 */
#include "xr/xr_camera.h"

#include <cmath>

namespace bd::xr {

namespace {

// Depth range for the composed projection. The guest's own near and far are
// tuned for a 27 degree vertical view on a television and clip visibly once the
// player can lean; these are wider on both ends. Far is generous because the
// skybox and distant terrain are the first things to betray a near-plane cut.
constexpr f32 kNearZ = 0.05f;
constexpr f32 kFarZ = 20000.0f;

// Exponential smoothing weight for the anchor, per frame at 72 Hz. Low enough
// that a hard camera cut arrives as a fast slide instead of a jump, high enough
// that ordinary follow-camera motion does not feel like it is dragging.
constexpr f32 kAnchorSmoothing = 0.25f;

Vec3 Lerp(Vec3 a, Vec3 b, f32 t) { return a + (b - a) * t; }

} // namespace

Camera &Camera::Get() {
  static Camera instance;
  return instance;
}

void Camera::SubmitGameCamera(const GameCamera &cam) { gameCamera_ = cam; }

CharacterAnchor CharacterFromFollowCamera(const GameCamera &game, f32 distance,
                                          f32 eyeHeight) {
  CharacterAnchor anchor;
  if (!(distance > 0.0f))
    return anchor; // invalid; caller falls back to the game camera

  const Vec3 look = game.position + game.forward * distance;
  if (!std::isfinite(look.x) || !std::isfinite(look.y) ||
      !std::isfinite(look.z))
    return anchor;

  // position is the feet and the camera looks at about eye level, so drop by
  // the height the anchor's consumers then add back - FirstPerson puts the eye
  // at position.y + eyeHeight, which lands exactly on the look-at point.
  anchor.position = {look.x, look.y - eyeHeight, look.z};
  anchor.eyeHeight = eyeHeight;
  // Same convention as YawOf in xr_math.h: forward (0,0,1) is yaw zero.
  anchor.facingYaw = std::atan2(game.forward.x, game.forward.z);
  anchor.valid = true;
  return anchor;
}

void Camera::SubmitCharacter(const CharacterAnchor &anchor) {
  character_ = anchor;
}

void Camera::SubmitHeadPose(const Pose &openxrPose) {
  head_ = FromOpenXRPose(openxrPose);
}

void Camera::SetMode(CameraMode mode) {
  if (mode == mode_)
    return;
  mode_ = mode;
  // A mode change moves the anchor a long way. Recentring here means the
  // player comes out of the switch facing what they were facing, and stops the
  // smoother from spending half a second travelling between two unrelated
  // points.
  smoothedValid_ = false;
  Recenter();
}

f32 Camera::HeadOffsetScale() const {
  // Game units per metre of real head movement. Dividing by world scale is
  // what makes a small world feel large: at 0.1 the world reads as a tabletop,
  // so a 10 cm lean has to cover ten times the game distance to match.
  const f32 scale = tuning_.worldScale;
  return scale > 0.0f ? tuning_.unitsPerMetre / scale : tuning_.unitsPerMetre;
}

Pose Camera::ComposeAnchor() const {
  Vec3 target;
  f32 yaw = 0.0f;

  switch (mode_) {
  case CameraMode::FirstPerson:
    if (character_.valid) {
      target = character_.position;
      target.y += character_.eyeHeight;
      yaw = character_.facingYaw;
    } else {
      target = gameCamera_.position;
    }
    break;

  case CameraMode::ThirdPerson:
    if (character_.valid) {
      yaw = character_.facingYaw;
      // The offset is authored in the character's frame, so it stays behind
      // them as they turn rather than swinging around the world axes.
      const Quat facing = QuatFromYaw(yaw);
      target = character_.position + Rotate(facing, tuning_.thirdOffset);
    } else {
      // No character this frame: fall back to the game's own camera rather
      // than snapping to the origin. Happens during menus and event scenes.
      target = gameCamera_.position;
    }
    break;

  case CameraMode::Diorama:
    // Detached. The game camera's position is a reasonable centre for the
    // action, raised so the player looks down into the scene rather than
    // standing in it.
    target = gameCamera_.position;
    target.y += tuning_.dioramaHeight;
    break;

  case CameraMode::Cinema:
    // The compositor draws the flat image; nothing here contributes. Return
    // the game camera unchanged so anything still reading this gets something
    // sane rather than the origin.
    target = gameCamera_.position;
    break;
  }

  // Second line of defence, and worth having even though xr_game_camera now
  // rejects a non-finite guest matrix upstream: this state is retained across
  // frames through a low-pass, and Lerp(NaN, x, t) is NaN, so anything that
  // ever gets NaN in here keeps it for the life of the session. Recovering by
  // snapping is right - the alternative is a permanently black headset.
  const bool finite = std::isfinite(target.x) && std::isfinite(target.y) &&
                      std::isfinite(target.z);
  const bool smoothedFinite =
      std::isfinite(smoothedAnchor_.x) && std::isfinite(smoothedAnchor_.y) &&
      std::isfinite(smoothedAnchor_.z);
  if (!finite) {
    target = smoothedFinite ? smoothedAnchor_ : Vec3{};
  }

  if (!smoothedValid_ || !smoothedFinite) {
    smoothedAnchor_ = target;
    smoothedValid_ = true;
  } else {
    smoothedAnchor_ = Lerp(smoothedAnchor_, target, kAnchorSmoothing);
  }

  return {smoothedAnchor_, QuatFromYaw(yaw + recenterYaw_)};
}

EyeMatrices Camera::ComposeEye(const Pose &openxrEyePose,
                               const Fov &fov) const {
  const Pose eye = FromOpenXRPose(openxrEyePose);
  const Pose anchor = ComposeAnchor();

  // The runtime's eye pose already carries the IPD offset and whatever the
  // headset's optics do, so it is used whole. Scaling only its translation is
  // deliberate: scaling the rotation would be meaningless, and scaling the IPD
  // along with the world is exactly what makes world scale read as size rather
  // than as the player having shrunk.
  const f32 scale = HeadOffsetScale();
  const Vec3 localOffset = (eye.position + recenterOffset_) * scale;

  Pose out;
  out.position = anchor.position + Rotate(anchor.orientation, localOffset);
  out.orientation = anchor.orientation * eye.orientation;

  return {ViewFromPose(out), ProjectionFromFov(fov, kNearZ, kFarZ)};
}

f32 Camera::LocomotionYaw() const {
  return YawOf(head_.orientation) + recenterYaw_;
}

void Camera::Recenter() {
  // Line the player's physical facing up with the game's, then zero the
  // positional drift that accumulates as they wander around the play space.
  const f32 gameYaw =
      std::atan2(gameCamera_.forward.x, gameCamera_.forward.z);
  recenterYaw_ = gameYaw - YawOf(head_.orientation);
  recenterOffset_ = -head_.position;
  // Keep the vertical alone: standing up should still raise the view, and
  // cancelling it is what makes a recentre feel like the floor moved.
  recenterOffset_.y = 0.0f;
}

void Camera::ApplyTurn(f32 radians) { recenterYaw_ += radians; }

} // namespace bd::xr
