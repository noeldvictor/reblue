/**
 * @file    xr/xr_game_camera.cpp
 * @brief   Head pose in, guest view matrix out. See xr_game_camera.h.
 * @license BSD 3-Clause, see LICENSE
 */
#include "xr/xr_game_camera.h"

#include <cmath>
#include <cstring>

#include "core/logging.h"
#include "xr/xr_camera.h"
#include "xr/xr_settings.h"

REXCVAR_DECLARE(double, bd_vr_anchor_distance);
REXCVAR_DECLARE(double, bd_vr_anchor_eye_height);

namespace bd::xr {

namespace {

// The latched eye. Written by the present path and read by the guest's render
// thread inside bdBuildViewMatrix, both of which are the same thread here:
// bdCameraRenderSetup is where the render thread does its camera work, and the
// eye is latched before the guest is let near it. Kept plain rather than atomic
// so that stays true by construction - if a second thread ever reads this, the
// tearing would be a torn matrix, which reads as a single wrong frame and is
// very hard to attribute.
struct LatchedEye {
  Pose pose;
  Fov fov;
  bool valid = false;
};

LatchedEye g_eye;

// The frustum the guest is told to render with. Written once per frame by the
// session, read by the projection hook on the guest's render thread.
struct RenderFovState {
  f32 halfVertical = 0.0f;
  f32 aspect = 0.0f;
  bool valid = false;
};

RenderFovState g_renderFov;

// Reconstructs the camera-to-world transform the guest's view matrix inverts.
// The rotation block is the transpose of the camera basis and the translation
// row is the negated projection of the position onto it, so reading the basis
// off the columns and undoing the projection recovers both.
GameCamera GameCameraFromView(const f32 v[16]) {
  const Vec3 right{v[0], v[4], v[8]};
  const Vec3 up{v[1], v[5], v[9]};
  const Vec3 forward{v[2], v[6], v[10]};

  GameCamera cam;
  cam.forward = forward;
  cam.up = up;
  cam.position = -(right * v[12] + up * v[13] + forward * v[14]);
  return cam;
}

void WriteMat4(const Mat4 &m, f32 out[16]) {
  std::memcpy(out, &m.m[0][0], sizeof(f32) * 16);
}

// Pushes the current cvar values into the camera. Cheap, and doing it per frame
// rather than on cvar change means a setting altered from the console takes
// effect on the next frame with no subscription plumbing.
void SyncTuning() {
  Settings &settings = Settings::Get();
  Camera &camera = Camera::Get();
  camera.SetTuning(settings.Tuning());
  if (camera.Mode() != settings.Mode()) {
    // SetMode resets the anchor smoothing itself, so a mode change cuts rather
    // than sliding the player across the level.
    camera.SetMode(settings.Mode());
  }
}

// Derives the party leader's transform from the game's own follow camera, and
// feeds it to the anchored camera modes.
//
// The direct route - hooking the player object and reading its position - is
// written (src/xr/xr_player_anchor.cpp) and does not work: the guest never
// calls bdPlayerFieldMovementUpdate, even with the character verifiably
// walking. See research/20260829_1420_autoplay-walks-and-the-anchor-hook-is-dead.md.
//
// This works instead because Blue Dragon's field camera is a follow camera: it
// sits behind the party leader and looks at them, so the leader is on the
// camera's forward axis at roughly the follow distance, and the camera's yaw is
// the leader's facing. That is approximate, and it is derived from data that
// provably updates every frame - the camera was watched tracing the circle that
// autoplay walks.
//
// No feedback risk: the hook hands the guest a scratch copy of the view matrix
// and the guest keeps its own camera, so nothing computed here can reach the
// follow-camera controller.
void SubmitCharacterFromGameCamera(const GameCamera &game) {
  const CharacterAnchor anchor = CharacterFromFollowCamera(
      game, f32(REXCVAR_GET(bd_vr_anchor_distance)),
      f32(REXCVAR_GET(bd_vr_anchor_eye_height)));
  // An invalid anchor is left unsubmitted rather than submitted-as-invalid, so
  // a bad frame during a load keeps the last good one instead of dropping the
  // camera back to the game's for one frame and jolting.
  if (anchor.valid)
    Camera::Get().SubmitCharacter(anchor);
}
} // namespace

void SetRenderFov(f32 halfVerticalRadians, f32 aspect) {
  g_renderFov.halfVertical = halfVerticalRadians;
  g_renderFov.aspect = aspect;
  g_renderFov.valid = halfVerticalRadians > 0.0f && aspect > 0.0f;
}

bool RenderFov(f32 &halfVerticalRadians, f32 &aspect) {
  if (!g_renderFov.valid || !ViewOverrideActive())
    return false;
  halfVerticalRadians = g_renderFov.halfVertical;
  aspect = g_renderFov.aspect;
  return true;
}

void SubmitEye(const Pose &openxrEyePose, const Fov &fov) {
  g_eye.pose = openxrEyePose;
  g_eye.fov = fov;
  g_eye.valid = true;
}

void ClearEye() { g_eye.valid = false; }

bool ViewOverrideActive() {
  if (!g_eye.valid || !Settings::Get().Enabled())
    return false;
  // Cinema keeps the game's own camera untouched: the whole point of that mode
  // is that the flat image is exactly what the flat game would have drawn.
  return Settings::Get().Mode() != CameraMode::Cinema;
}

bool ComposeView(const f32 gameView[16], f32 out[16]) {
  if (!ViewOverrideActive())
    return false;

  // The guest hands out a matrix full of NaN on some frames - a load or a
  // camera cut, where its own struct is not yet populated. It draws nothing on
  // those frames so it never notices. We would: the anchor is low-passed, and
  // Lerp(NaN, x, t) is NaN, so a single poisoned frame sticks to the camera for
  // the rest of the session and every frame after it renders black.
  //
  // So this is rejected before it reaches any retained state, not clamped
  // after.
  for (int i = 0; i < 16; ++i) {
    if (std::isfinite(gameView[i]))
      continue;
    static bool told = false;
    if (!told) {
      told = true;
      BD_INFO("[xr] guest view matrix was non-finite; passing the frame "
              "through untouched. Expected during loads.");
    }
    return false;
  }

  SyncTuning();
  Camera &camera = Camera::Get();
  const GameCamera game = GameCameraFromView(gameView);
  camera.SubmitGameCamera(game);
  SubmitCharacterFromGameCamera(game);
  const Mat4 view = camera.ComposeEye(g_eye.pose, g_eye.fov).view;

  // A single non-finite element here blanks the entire frame - every vertex
  // transforms to NaN and clips away - so the whole scene disappears with no
  // error anywhere. Falling back to the guest's own matrix costs head tracking
  // for that frame and keeps the game visible, which is a far better failure
  // than a black headset.
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      if (std::isfinite(view.m[i][j]))
        continue;
      static bool told = false;
      if (!told) {
        told = true;
        const CameraTuning &t = camera.Tuning();
        BD_ERROR("[xr] view went non-finite at m[{}][{}]; falling back to the "
                 "guest camera. units/m={} worldScale={} dioramaH={} "
                 "eye=({}, {}, {}) fov=({}, {}, {}, {}) game=({}, {}, {})",
                 i, j, t.unitsPerMetre, t.worldScale, t.dioramaHeight,
                 g_eye.pose.position.x, g_eye.pose.position.y,
                 g_eye.pose.position.z, g_eye.fov.angleLeft,
                 g_eye.fov.angleRight, g_eye.fov.angleUp, g_eye.fov.angleDown,
                 game.position.x, game.position.y, game.position.z);
      }
      return false;
    }
  }

  WriteMat4(view, out);

  // Periodic rather than one-shot: a single line proves the seam is reached,
  // but only a changing head position proves the runtime is actually tracking,
  // and that is the part worth being able to see from a log.
  static u32 calls = 0;
  if ((calls++ % 600) == 0) {
    BD_INFO("[xr] cam: game ({:.1f}, {:.1f}, {:.1f})  head m ({:.3f}, {:.3f}, "
            "{:.3f})  eye ({:.1f}, {:.1f}, {:.1f})",
            game.position.x, game.position.y, game.position.z,
            g_eye.pose.position.x, g_eye.pose.position.y,
            g_eye.pose.position.z, -view.m[3][0], -view.m[3][1], -view.m[3][2]);
  }
  return true;
}

bool ComposeProjection(f32 out[16]) {
  if (!ViewOverrideActive())
    return false;

  SyncTuning();
  WriteMat4(Camera::Get().ComposeEye(g_eye.pose, g_eye.fov).projection, out);
  return true;
}

} // namespace bd::xr
