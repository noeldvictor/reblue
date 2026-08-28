/**
 * @file    xr/xr_game_camera.cpp
 * @brief   Head pose in, guest view matrix out. See xr_game_camera.h.
 * @license BSD 3-Clause, see LICENSE
 */
#include "xr/xr_game_camera.h"

#include <cstring>

#include "core/logging.h"
#include "xr/xr_camera.h"
#include "xr/xr_settings.h"

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

} // namespace

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

  SyncTuning();
  Camera &camera = Camera::Get();
  const GameCamera game = GameCameraFromView(gameView);
  camera.SubmitGameCamera(game);
  const Mat4 view = camera.ComposeEye(g_eye.pose, g_eye.fov).view;
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
