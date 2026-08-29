/**
 * @file    xr/xr_camera.h
 * @brief   Turns a head pose and the game's own camera into the per-eye view
 *          and projection matrices the guest renderer is handed.
 *
 * @copyright Copyright (c) 2026 re:Blue contributors
 * @license   BSD 3-Clause - see LICENSE
 */
#pragma once

#include <rex/types.h>

#include "xr/xr_math.h"

namespace bd::xr {

// Where the camera sits relative to the party. The mode decides the anchor;
// the head pose always offsets freely from whatever that anchor turns out to
// be, because taking control away from someone's neck is how you make them
// sick.
enum class CameraMode : i32 {
  // Anchor at the player character's head. Blue Dragon's animations are
  // authored for a camera that is somewhere else entirely, so this needs the
  // heaviest smoothing of the four and is still going to be a novelty.
  FirstPerson = 0,
  // Anchor floats behind and above the character. The default: it keeps the
  // game's framing legible while letting the player lean in and look around.
  ThirdPerson = 1,
  // Anchor detaches from the character and the world scales down hard. The
  // most comfortable mode by a distance, because it sidesteps every problem
  // caused by the game's authored camera framing.
  Diorama = 2,
  // Not a 3D camera at all: the flat image on a world-locked screen. Handled
  // by the compositor rather than here, and listed so the enum covers every
  // value bd_vr_camera_mode can hold.
  Cinema = 3,
};

// What the game itself wanted this frame, read off bdCameraViewSetMatrices
// before we overwrite it. Kept because several modes still want the game's
// opinion: its position is the anchor in Diorama, and its yaw is what a
// recentre lines the player up with.
struct GameCamera {
  Vec3 position;
  // Forward and up as the guest computed them, already in game space.
  Vec3 forward{0.0f, 0.0f, 1.0f};
  Vec3 up{0.0f, 1.0f, 0.0f};
};

// The party leader's world transform, for the anchor in the two attached
// modes. Supplied by the engine layer; the camera does not go looking for it.
struct CharacterAnchor {
  Vec3 position;   // feet, game units
  f32 eyeHeight = 0.0f; // game units above position
  f32 facingYaw = 0.0f; // radians about +Y
  bool valid = false;
};

// Derives the party leader's transform from the game's own follow camera.
//
// Blue Dragon's field camera sits behind the leader and looks at them, so the
// leader lies on the camera's forward axis at roughly the follow distance and
// the camera's yaw is the leader's facing. That is the only source available:
// the direct route, hooking the player object, is written and never fires (see
// src/xr/xr_player_anchor.cpp), which is why the anchored modes silently fell
// back to the game camera for so long.
//
// Pure and dependency-free on purpose, so tools/xr_math_test can check the
// convention - getting the yaw or the height sign wrong here is invisible in a
// symmetric test pose and has cost this port three separate bugs.
//
// distance <= 0 returns an invalid anchor, which is the documented opt-out.
CharacterAnchor CharacterFromFollowCamera(const GameCamera &game, f32 distance,
                                          f32 eyeHeight);

// One eye's worth of output, ready for the hook to byte-swap into guest memory.
struct EyeMatrices {
  Mat4 view;
  Mat4 projection;
};

// The settings the composition actually reads, passed in rather than pulled
// from the cvar singleton. Keeping it that way is what lets xr_camera.cpp
// depend on nothing but xr_math.h, and therefore what lets tools/xr_math_test
// exercise the camera modes without the SDK present. Do not reintroduce a
// reach into bd::xr::Settings from here.
struct CameraTuning {
  // Game units per real-world metre. A property of Blue Dragon we have to
  // measure, not a preference.
  f32 unitsPerMetre = 1.0f;
  // How large the world feels. 1.0 is life size; small values read as a
  // tabletop.
  f32 worldScale = 1.0f;
  // Third-person anchor offset, in the character's own frame.
  Vec3 thirdOffset{0.0f, 1.5f, -3.0f};
  // How far above the scene the diorama anchor floats.
  f32 dioramaHeight = 8.0f;
};

// Stateless per frame apart from the recentre offset and the smoothed anchor,
// both of which have to persist across frames by definition.
class Camera {
public:
  static Camera &Get();

  // Pushed from bd::xr::Settings whenever a cvar changes, so the composition
  // never reaches for a global.
  void SetTuning(const CameraTuning &tuning) { tuning_ = tuning; }
  const CameraTuning &Tuning() const { return tuning_; }

  // Latches what the game asked for. Called from the bdCameraViewSetMatrices
  // hook before the override is composed.
  void SubmitGameCamera(const GameCamera &cam);

  // Latches the party leader's transform. Called from the field update once
  // per frame; safe to never call, in which case the attached modes fall back
  // to the game camera's own position.
  void SubmitCharacter(const CharacterAnchor &anchor);

  // Latches the head pose for this frame, in OpenXR space and metres. Converts
  // and stores; does not compose.
  void SubmitHeadPose(const Pose &openxrPose);

  // Composes one eye. eyePose is the runtime's per-eye pose, in OpenXR space,
  // and already carries the IPD offset, so we never reconstruct it ourselves.
  EyeMatrices ComposeEye(const Pose &openxrEyePose, const Fov &fov) const;

  // Yaw the player should walk relative to: the head's yaw, flattened, plus
  // the recentre offset. Locomotion resolves the stick against this rather
  // than against the game camera, or "forward" means whatever the game last
  // decided and the player fights it.
  f32 LocomotionYaw() const;

  // Aligns the player's current facing with the game camera's, and drops the
  // accumulated positional drift. Bound to a button, and called once on mode
  // change so a switch does not leave the player facing a wall.
  void Recenter();

  // Snap turn, in radians, applied to the recentre yaw.
  void ApplyTurn(f32 radians);

  // Drops the smoothed anchor so the next frame snaps rather than travelling.
  // Wanted whenever the anchor is about to move somewhere unrelated: a session
  // restart, an area load, a mode change. Sliding smoothly across a level
  // transition is worse than cutting.
  void ResetSmoothing() { smoothedValid_ = false; }

  CameraMode Mode() const { return mode_; }
  void SetMode(CameraMode mode);

private:
  Camera() = default;

  // Anchor position and yaw in game space for the current mode, before the
  // head pose is added.
  Pose ComposeAnchor() const;

  // Head offset in metres becomes an offset in game units. Two factors, and
  // they mean different things: units-per-metre is a property of the game we
  // have to measure, world scale is a preference about how big the world
  // should feel.
  f32 HeadOffsetScale() const;

  CameraMode mode_ = CameraMode::ThirdPerson;
  CameraTuning tuning_;

  GameCamera gameCamera_;
  CharacterAnchor character_;

  // Head pose, already converted to game space, still in metres.
  Pose head_;

  // Applied to everything the head contributes, so the player can face the way
  // the game intends without physically turning around.
  f32 recenterYaw_ = 0.0f;
  Vec3 recenterOffset_;

  // Low-passed anchor, because the raw one snaps whenever the game cuts its
  // camera and an unsmoothed cut straight into the eyes is the single most
  // reliable way to make someone take the headset off.
  mutable Vec3 smoothedAnchor_;
  mutable bool smoothedValid_ = false;
};

} // namespace bd::xr
