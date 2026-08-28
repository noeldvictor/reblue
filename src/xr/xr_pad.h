/**
 * @file    xr/xr_pad.h
 * @brief   Touch controller state, carried from the XR session to the input
 *          driver without either end seeing the other's headers.
 *
 * Quest controllers are not Android gamepads. SDL never sees them: they exist
 * only as OpenXR actions, which is why a headset with two controllers in your
 * hands reports "no controllers" to a normal SDL app and the game sits on its
 * title screen forever.
 *
 * The session fills this in from xrSyncActions; the input driver turns it into
 * an X_INPUT_STATE. Keeping the struct here means the driver compiles in every
 * configuration, including the ones built without a loader, where it simply
 * reports no device.
 *
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once

#include <rex/types.h>

namespace bd::xr {

// Analogue values are 0..1 for triggers and grips, -1..1 for sticks, with the
// runtime's own deadzone already applied - OpenXR guarantees a resting stick
// reads exactly zero, so re-deadzoning here would only widen it.
struct PadState {
  bool a = false;
  bool b = false;
  bool x = false;
  bool y = false;
  bool menu = false;
  bool leftThumbClick = false;
  bool rightThumbClick = false;

  f32 leftStickX = 0.0f;
  f32 leftStickY = 0.0f;
  f32 rightStickX = 0.0f;
  f32 rightStickY = 0.0f;

  f32 leftTrigger = 0.0f;
  f32 rightTrigger = 0.0f;
  f32 leftGrip = 0.0f;
  f32 rightGrip = 0.0f;

  // False until a sync has actually returned data. The driver reports no
  // device while this is false, rather than a pad stuck at neutral, so the
  // game's own "no controller" handling still works.
  bool connected = false;
};

// Called from the session once per frame after xrSyncActions.
void SubmitPad(const PadState &state);

// Latest state. Returns false when no controller has ever reported.
bool CurrentPad(PadState &out);

} // namespace bd::xr
