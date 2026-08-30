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

// The compositor paces to submultiples of the display rate, so a frame that
// cannot hold 13.9ms at 72Hz is dropped to 27.8 or 41.7. Asking for 60Hz makes
// the tiers 16.7/33.3/50 and lets a ~20ms frame land on 33.3ms - 30fps, which
// is what Blue Dragon originally ran at. 0 leaves the runtime alone.
// The party leader's position, derived from the game's own follow camera.
//
// The direct route - hooking the player object - is written and does not fire
// (see xr_player_anchor.cpp), so ThirdPerson and FirstPerson had no anchor at
// all and silently fell back to the game camera. Blue Dragon's field camera
// sits behind the leader and looks at them, so the leader is on the camera's
// forward axis: position = camera + forward * distance. Approximate, and built
// on data that provably updates every frame.
//
// Distance is in game units, where the field camera sits about 150 above the
// ground - so roughly 100 units to the metre. 0 disables the derivation and
// restores the old fall-back behaviour.
// Detail culling: the smallest a node may project before it is dropped.
//
// The one kind of culling Blue Dragon has no reason to do. A Xenon command
// processor made draws nearly free, so the engine submits every node its
// frustum keeps however small it lands; on an Adreno each of those is a full
// trip through bdSceneNodeDrawSingle, the single largest consumer of guest CPU
// on device. 0 disables. Start around 2-4 pixels: below that a node cannot
// resolve to more than a speck even before the VR render scale shrinks it.
REXCVAR_DEFINE_DOUBLE(bd_cull_min_pixels, 0.0, kCvarGroup,
                      "Drop scene nodes whose projected radius is under this "
                      "many pixels. 0 disables.")
    .range(0.0, 64.0);

REXCVAR_DEFINE_DOUBLE(bd_vr_anchor_distance, 300.0, kCvarGroup,
                      "Game units from the follow camera to the party leader, "
                      "used to anchor the third- and first-person VR camera "
                      "modes. 0 falls back to the game camera.")
    .range(0.0, 5000.0);

REXCVAR_DEFINE_DOUBLE(bd_vr_anchor_eye_height, 150.0, kCvarGroup,
                      "Height of the leader's eyes above their feet, in game "
                      "units. The follow camera looks at about eye level, so "
                      "this is how far down the anchor's origin sits.")
    .range(0.0, 1000.0);

REXCVAR_DEFINE_DOUBLE(bd_xr_refresh_rate, 0.0, kCvarGroup,
                      "Display refresh rate to request, in Hz. 0 leaves it to "
                      "the runtime. The nearest supported rate at or below the "
                      "request is used. Requires restart.")
    .range(0.0, 120.0)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(bd_vr_camera_mode,
                     static_cast<i32>(bd::xr::CameraMode::ThirdPerson),
                     kCvarGroup,
                     "VR camera: 0 = first person, 1 = third person, "
                     "2 = diorama, 3 = flat image on a world-locked screen.")
    .range(0, static_cast<i32>(bd::xr::CameraMode::Cinema));

// 100, measured, not 1. Blue Dragon's field camera sits at y ~ 150 with the
// ground at 0, which is an eye height of 1.5 m - so a game unit is a
// centimetre. The default was 1.0, and that is what made every anchored camera
// mode wrong: the head's 1.6 m of height became 1.6 cm of game movement, and
// the third-person offset below put the camera 3 cm behind the character
// instead of 3 m. What that renders is the inside of the character's head,
// which is exactly what a capture showed.
REXCVAR_DEFINE_DOUBLE(bd_vr_units_per_metre, 100.0, kCvarGroup,
                      "Blue Dragon world units per real-world metre. A "
                      "property of the game, not a preference: measure it "
                      "against a character of known height and leave it "
                      "alone. Wrong here makes every other VR setting lie.")
    .range(0.001, 1000.0);

REXCVAR_DEFINE_DOUBLE(bd_vr_world_scale, 1.0, kCvarGroup,
                      "How large the world feels. 1.0 is life size. Below 1 "
                      "shrinks it, so 0.1 reads as a tabletop diorama.")
    .range(0.01, 10.0);

// These are game units - centimetres - so the old (0, 1.5, -3.0) put the camera
// 3 cm behind and 1.5 cm above the character. They were metres by mistake. The
// ranges were +/-50 too, which is half a metre, so the values could not even be
// tuned out of the bug from a config file.
REXCVAR_DEFINE_DOUBLE(bd_vr_third_offset_x, 0.0, kCvarGroup,
                      "Third-person anchor offset, right of the character, in "
                      "game units (about 100 to the metre).")
    .range(-2000.0, 2000.0);

// Zero, not eye height: the head pose already carries the player's own 1.6 m
// above the anchor, so putting eye height here too stacks them and the camera
// ends up 3.7 m up looking down on the character - measured, from a capture.
// This is the offset of the *anchor*, and the anchor belongs on the ground.
REXCVAR_DEFINE_DOUBLE(bd_vr_third_offset_y, 0.0, kCvarGroup,
                      "Third-person anchor offset, above the character, in "
                      "game units. 0 puts the anchor at their feet and lets "
                      "the player's own height supply eye level.")
    .range(-2000.0, 2000.0);

REXCVAR_DEFINE_DOUBLE(bd_vr_third_offset_z, -300.0, kCvarGroup,
                      "Third-person anchor offset along the character's "
                      "facing, in game units. Negative sits behind them; -300 "
                      "is about three metres back.")
    .range(-2000.0, 2000.0);

// Battles are stationary set-pieces: the party and the enemies stand in fixed
// ranks and nobody walks anywhere, so a follow camera has nothing to follow and
// spends the fight jittering against the game's own battle camera. A fixed
// diorama view is what the scene is actually shaped for - you look down on the
// arena like a tabletop, which is also the most comfortable thing to do with a
// camera the player does not drive.
REXCVAR_DEFINE_BOOL(bd_vr_battle_diorama, true, kCvarGroup,
                    "Switch to the diorama camera for the duration of a "
                    "battle, whatever mode the field is using. Off keeps the "
                    "field camera mode throughout.");

REXCVAR_DEFINE_DOUBLE(bd_vr_diorama_height, 800.0, kCvarGroup,
                      "How far above the scene the diorama anchor floats, in "
                      "game units. 800 is about eight metres.")
    .range(0.0, 20000.0);

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
