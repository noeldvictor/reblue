/**
 * @file    xr/xr_settings.h
 * @brief   VR settings. Comfort defaults are chosen deliberately and are not
 *          the same thing as best-looking defaults.
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once

#include <rex/types.h>

#include "xr/xr_camera.h"
#include "xr/xr_math.h"

namespace bd::xr {

// How the flat 2D layer is placed once there is no screen to put it on.
enum class HudMode : i32 {
  // Follows the head at a fixed distance. Least likely to be lost, least
  // likely to make anyone ill, and therefore the default.
  HeadLocked = 0,
  // Pinned in the world at the point it was last placed. Better for reading
  // long text, worse for anything that needs to be glanceable.
  WorldLocked = 1,
  Hidden = 2,
};

// What happens when the game takes its camera back for a scripted scene.
enum class CutscenePolicy : i32 {
  // The scene plays, but only the head's offset moves the view. The scene's
  // own camera motion is ignored. Safest, and the default.
  HeadOffsetOnly = 0,
  // Drop to diorama for the duration and watch the scene play out below.
  ForceDiorama = 1,
  // Honour the scene's camera exactly as authored. Included because someone
  // will want it, and clearly labelled because it will make them ill.
  Honour = 2,
};

// The vertical field of view bdCameraInit seeds every camera with, in radians:
// 3*pi/20, or 27 degrees. Recorded here because the VR projection replaces it
// entirely and it is useful to know what is being replaced.
inline constexpr f32 kGuestVerticalFOV = 0.4712389f;

class Settings {
public:
  static Settings &Get();

  // Adopts current cvar values and registers a change callback per setting, so
  // console, config file and launch argument writes all reach here. Mirrors
  // bd::gpu::Settings::Init and is called from the same place.
  void Init();

  bool Enabled() const { return enabled_; }
  CameraMode Mode() const { return mode_; }

  // Metres of real movement to game units, and the world's apparent size.
  // Kept apart on purpose: one is a fact about Blue Dragon we have to measure,
  // the other is a preference.
  f32 UnitsPerMetre() const { return unitsPerMetre_; }
  f32 WorldScale() const { return worldScale_; }

  Vec3 ThirdPersonOffset() const { return thirdOffset_; }
  f32 DioramaHeight() const { return dioramaHeight_; }

  // Bundled for Camera::SetTuning. Pushed to the camera whenever one of these
  // cvars changes; the camera never reads this object, which is what keeps it
  // free of any dependency beyond xr_math.h and therefore testable.
  CameraTuning Tuning() const;

  bool SnapTurn() const { return snapTurn_; }
  f32 TurnRadians() const;
  bool ComfortVignette() const { return comfortVignette_; }

  HudMode Hud() const { return hudMode_; }
  f32 HudDistance() const { return hudDistance_; }
  f32 HudScale() const { return hudScale_; }

  CutscenePolicy Cutscenes() const { return cutscenePolicy_; }

  // Extra margin on the culling volume, as a multiplier. The volume covers
  // both eyes plus room for the player to lean before geometry pops; see
  // research/20260828_1404_vr-quest-tooling-and-perf.md for why this is one
  // combined volume rather than two per-eye frustums.
  f32 CullExpand() const { return cullExpand_; }

private:
  Settings() = default;

  void AdoptEnabled();
  void AdoptMode();
  void AdoptUnitsPerMetre();
  void AdoptWorldScale();
  void AdoptThirdOffset();
  void AdoptDioramaHeight();
  void AdoptSnapTurn();
  void AdoptTurnDegrees();
  void AdoptComfortVignette();
  void AdoptHudMode();
  void AdoptHudDistance();
  void AdoptHudScale();
  void AdoptCutscenePolicy();
  void AdoptCullExpand();

  bool enabled_ = false;
  CameraMode mode_ = CameraMode::ThirdPerson;
  f32 unitsPerMetre_ = 1.0f;
  f32 worldScale_ = 1.0f;
  Vec3 thirdOffset_{0.0f, 1.5f, -3.0f};
  f32 dioramaHeight_ = 8.0f;
  bool snapTurn_ = true;
  f32 turnDegrees_ = 30.0f;
  bool comfortVignette_ = true;
  HudMode hudMode_ = HudMode::HeadLocked;
  f32 hudDistance_ = 2.0f;
  f32 hudScale_ = 1.0f;
  CutscenePolicy cutscenePolicy_ = CutscenePolicy::HeadOffsetOnly;
  f32 cullExpand_ = 1.5f;
};

} // namespace bd::xr
