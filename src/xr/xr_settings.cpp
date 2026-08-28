/**
 * @file    xr/xr_settings.cpp
 * @license BSD 3-Clause, see LICENSE
 */
#include "xr/xr_settings.h"

#include <numbers>
#include <string_view>

#include <rex/cvar.h>

#include "core/settings.h" // kCvarGroup

REXCVAR_DECLARE(bool, bd_vr_enabled);
REXCVAR_DECLARE(i32, bd_vr_camera_mode);
REXCVAR_DECLARE(double, bd_vr_units_per_metre);
REXCVAR_DECLARE(double, bd_vr_world_scale);
REXCVAR_DECLARE(double, bd_vr_third_offset_x);
REXCVAR_DECLARE(double, bd_vr_third_offset_y);
REXCVAR_DECLARE(double, bd_vr_third_offset_z);
REXCVAR_DECLARE(double, bd_vr_diorama_height);
REXCVAR_DECLARE(bool, bd_vr_snap_turn);
REXCVAR_DECLARE(double, bd_vr_turn_degrees);
REXCVAR_DECLARE(bool, bd_vr_comfort_vignette);
REXCVAR_DECLARE(i32, bd_vr_hud_mode);
REXCVAR_DECLARE(double, bd_vr_hud_distance);
REXCVAR_DECLARE(double, bd_vr_hud_scale);
REXCVAR_DECLARE(i32, bd_vr_cutscene_policy);
REXCVAR_DECLARE(double, bd_vr_cull_expand);

REXCVAR_DEFINE_BOOL(bd_vr_enabled, false, kCvarGroup,
                    "Open an OpenXR session and render in stereo. Off runs the "
                    "ordinary flat renderer. Requires restart.");

REXCVAR_DEFINE_INT32(bd_vr_camera_mode,
                     static_cast<i32>(bd::xr::CameraMode::ThirdPerson),
                     kCvarGroup,
                     "VR camera: 0 = first person, 1 = third person, "
                     "2 = diorama, 3 = flat image on a world-locked screen.")
    .range(0, static_cast<i32>(bd::xr::CameraMode::Cinema));

REXCVAR_DEFINE_DOUBLE(bd_vr_units_per_metre, 1.0, kCvarGroup,
                      "Blue Dragon world units per real-world metre. A "
                      "property of the game, not a preference: measure it "
                      "against a character of known height and leave it "
                      "alone. Wrong here makes every other VR setting lie.")
    .range(0.001, 1000.0);

REXCVAR_DEFINE_DOUBLE(bd_vr_world_scale, 1.0, kCvarGroup,
                      "How large the world feels. 1.0 is life size. Below 1 "
                      "shrinks it, so 0.1 reads as a tabletop diorama.")
    .range(0.01, 10.0);

REXCVAR_DEFINE_DOUBLE(bd_vr_third_offset_x, 0.0, kCvarGroup,
                      "Third-person anchor offset, right of the character, in "
                      "game units.")
    .range(-50.0, 50.0);

REXCVAR_DEFINE_DOUBLE(bd_vr_third_offset_y, 1.5, kCvarGroup,
                      "Third-person anchor offset, above the character.")
    .range(-50.0, 50.0);

REXCVAR_DEFINE_DOUBLE(bd_vr_third_offset_z, -3.0, kCvarGroup,
                      "Third-person anchor offset along the character's "
                      "facing. Negative sits behind them.")
    .range(-50.0, 50.0);

REXCVAR_DEFINE_DOUBLE(bd_vr_diorama_height, 8.0, kCvarGroup,
                      "How far above the scene the diorama anchor floats, in "
                      "game units.")
    .range(0.0, 200.0);

REXCVAR_DEFINE_BOOL(bd_vr_snap_turn, true, kCvarGroup,
                    "Turn in fixed steps rather than smoothly. Smooth turning "
                    "is the more common cause of discomfort, so this is on.");

REXCVAR_DEFINE_DOUBLE(bd_vr_turn_degrees, 30.0, kCvarGroup,
                      "Degrees per snap turn, or degrees per second when snap "
                      "turning is off.")
    .range(5.0, 180.0);

REXCVAR_DEFINE_BOOL(bd_vr_comfort_vignette, true, kCvarGroup,
                    "Narrow the view while the character is moving. Costs "
                    "peripheral vision, buys a great deal of comfort.");

REXCVAR_DEFINE_INT32(bd_vr_hud_mode, static_cast<i32>(bd::xr::HudMode::HeadLocked),
                     kCvarGroup,
                     "The 2D layer in VR: 0 = follows the head, 1 = pinned in "
                     "the world, 2 = hidden.")
    .range(0, static_cast<i32>(bd::xr::HudMode::Hidden));

REXCVAR_DEFINE_DOUBLE(bd_vr_hud_distance, 2.0, kCvarGroup,
                      "Metres from the eye to the 2D layer. Closer than about "
                      "a metre is uncomfortable to focus on.")
    .range(0.5, 20.0);

REXCVAR_DEFINE_DOUBLE(bd_vr_hud_scale, 1.0, kCvarGroup,
                      "Size of the 2D layer.")
    .range(0.1, 5.0);

REXCVAR_DEFINE_INT32(bd_vr_cutscene_policy,
                     static_cast<i32>(bd::xr::CutscenePolicy::HeadOffsetOnly),
                     kCvarGroup,
                     "Scripted camera moves in VR: 0 = ignore them and keep "
                     "the head in control, 1 = drop to diorama for the scene, "
                     "2 = honour them as authored. 2 will make most people "
                     "ill.")
    .range(0, static_cast<i32>(bd::xr::CutscenePolicy::Honour));

REXCVAR_DEFINE_DOUBLE(bd_vr_cull_expand, 1.5, kCvarGroup,
                      "Margin on the culling volume. The volume covers both "
                      "eyes plus room to lean; 1.0 culls to exactly what is "
                      "visible and pops geometry the moment the head turns.")
    .range(1.0, 8.0);

namespace bd::xr {

Settings &Settings::Get() {
  static Settings instance;
  return instance;
}

f32 Settings::TurnRadians() const {
  return turnDegrees_ * static_cast<f32>(std::numbers::pi) / 180.0f;
}

CameraTuning Settings::Tuning() const {
  return {unitsPerMetre_, worldScale_, thirdOffset_, dioramaHeight_};
}

namespace {
// Every adopt that feeds the camera ends here, so a cvar write reaches the
// composition without the camera ever looking anything up.
void PushTuning() { Camera::Get().SetTuning(Settings::Get().Tuning()); }
} // namespace

void Settings::AdoptEnabled() { enabled_ = REXCVAR_GET(bd_vr_enabled); }

void Settings::AdoptMode() {
  mode_ = static_cast<CameraMode>(REXCVAR_GET(bd_vr_camera_mode));
  // SetMode recentres, so this must not fire before the first head pose has
  // arrived. Init runs long before a session exists, which is fine: recentring
  // against an identity pose is a no-op.
  Camera::Get().SetMode(mode_);
}

void Settings::AdoptUnitsPerMetre() {
  unitsPerMetre_ = static_cast<f32>(REXCVAR_GET(bd_vr_units_per_metre));
  PushTuning();
}

void Settings::AdoptWorldScale() {
  worldScale_ = static_cast<f32>(REXCVAR_GET(bd_vr_world_scale));
  PushTuning();
}

void Settings::AdoptThirdOffset() {
  thirdOffset_ = {static_cast<f32>(REXCVAR_GET(bd_vr_third_offset_x)),
                  static_cast<f32>(REXCVAR_GET(bd_vr_third_offset_y)),
                  static_cast<f32>(REXCVAR_GET(bd_vr_third_offset_z))};
  PushTuning();
}

void Settings::AdoptDioramaHeight() {
  dioramaHeight_ = static_cast<f32>(REXCVAR_GET(bd_vr_diorama_height));
  PushTuning();
}

void Settings::AdoptSnapTurn() { snapTurn_ = REXCVAR_GET(bd_vr_snap_turn); }

void Settings::AdoptTurnDegrees() {
  turnDegrees_ = static_cast<f32>(REXCVAR_GET(bd_vr_turn_degrees));
}

void Settings::AdoptComfortVignette() {
  comfortVignette_ = REXCVAR_GET(bd_vr_comfort_vignette);
}

void Settings::AdoptHudMode() {
  hudMode_ = static_cast<HudMode>(REXCVAR_GET(bd_vr_hud_mode));
}

void Settings::AdoptHudDistance() {
  hudDistance_ = static_cast<f32>(REXCVAR_GET(bd_vr_hud_distance));
}

void Settings::AdoptHudScale() {
  hudScale_ = static_cast<f32>(REXCVAR_GET(bd_vr_hud_scale));
}

void Settings::AdoptCutscenePolicy() {
  cutscenePolicy_ =
      static_cast<CutscenePolicy>(REXCVAR_GET(bd_vr_cutscene_policy));
}

void Settings::AdoptCullExpand() {
  cullExpand_ = static_cast<f32>(REXCVAR_GET(bd_vr_cull_expand));
}

void Settings::Init() {
  AdoptEnabled();
  AdoptMode();
  AdoptUnitsPerMetre();
  AdoptWorldScale();
  AdoptThirdOffset();
  AdoptDioramaHeight();
  AdoptSnapTurn();
  AdoptTurnDegrees();
  AdoptComfortVignette();
  AdoptHudMode();
  AdoptHudDistance();
  AdoptHudScale();
  AdoptCutscenePolicy();
  AdoptCullExpand();
  PushTuning();

  auto reg = [](const char *name, void (Settings::*adopt)()) {
    rex::cvar::RegisterChangeCallback(
        name, [adopt](std::string_view, std::string_view) {
          (Settings::Get().*adopt)();
        });
  };
  // bd_vr_enabled is deliberately absent: the session is opened once at
  // startup, so a live write would leave the setting and the renderer
  // disagreeing. It is marked as requiring a restart for the same reason.
  reg("bd_vr_camera_mode", &Settings::AdoptMode);
  reg("bd_vr_units_per_metre", &Settings::AdoptUnitsPerMetre);
  reg("bd_vr_world_scale", &Settings::AdoptWorldScale);
  reg("bd_vr_third_offset_x", &Settings::AdoptThirdOffset);
  reg("bd_vr_third_offset_y", &Settings::AdoptThirdOffset);
  reg("bd_vr_third_offset_z", &Settings::AdoptThirdOffset);
  reg("bd_vr_diorama_height", &Settings::AdoptDioramaHeight);
  reg("bd_vr_snap_turn", &Settings::AdoptSnapTurn);
  reg("bd_vr_turn_degrees", &Settings::AdoptTurnDegrees);
  reg("bd_vr_comfort_vignette", &Settings::AdoptComfortVignette);
  reg("bd_vr_hud_mode", &Settings::AdoptHudMode);
  reg("bd_vr_hud_distance", &Settings::AdoptHudDistance);
  reg("bd_vr_hud_scale", &Settings::AdoptHudScale);
  reg("bd_vr_cutscene_policy", &Settings::AdoptCutscenePolicy);
  reg("bd_vr_cull_expand", &Settings::AdoptCullExpand);
}

} // namespace bd::xr
