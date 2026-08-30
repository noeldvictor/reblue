/**
 * @file    xr/xr_pad.cpp
 * @brief   See xr_pad.h.
 * @license BSD 3-Clause, see LICENSE
 */
#include "xr/xr_pad.h"

#include <atomic>
#include <cmath>
#include <mutex>

#include "core/logging.h"
#include "ui/vr_options.h"

namespace bd::xr {

namespace {
// The session syncs on the render thread and the guest polls on its own, so
// unlike the camera latch this really does cross threads. A mutex rather than
// an atomic because PadState is far too big to be lock-free, and a torn pad
// state is a phantom button press.
std::mutex g_mutex;
PadState g_pad;
std::atomic<bool> g_everConnected{false};
} // namespace

// Both grips squeezed with both triggers, held. Blue Dragon never asks for
// that shape, so it cannot be hit by accident during play, and it needs no
// button taken away from the game.
//
// The alternative was the menu button, which the runtime reserves, or a face
// button, which the game uses for everything.
bool MenuGesture(const PadState &s) {
  return s.leftGrip > 0.8f && s.rightGrip > 0.8f && s.leftTrigger > 0.8f &&
         s.rightTrigger > 0.8f;
}

// Stick-driven navigation with a re-arm, so one flick is one step rather than
// one step per frame at 60 Hz.
void DriveOptions(const PadState &s) {
  static bool vertical_armed = true;
  static bool horizontal_armed = true;
  static bool close_armed = true;
  constexpr f32 kOn = 0.6f;
  constexpr f32 kOff = 0.3f;

  if (std::abs(s.rightStickY) < kOff && std::abs(s.leftStickY) < kOff)
    vertical_armed = true;
  if (std::abs(s.rightStickX) < kOff && std::abs(s.leftStickX) < kOff)
    horizontal_armed = true;
  if (!s.b)
    close_armed = true;

  const f32 y = std::abs(s.rightStickY) > std::abs(s.leftStickY) ? s.rightStickY
                                                                 : s.leftStickY;
  const f32 x = std::abs(s.rightStickX) > std::abs(s.leftStickX) ? s.rightStickX
                                                                 : s.leftStickX;
  if (vertical_armed && std::abs(y) > kOn) {
    vertical_armed = false;
    // Stick up is +1 in OpenXR and up the list is -1.
    bd::ui::vr_options::Move(y > 0.0f ? -1 : 1);
  }
  if (horizontal_armed && std::abs(x) > kOn) {
    horizontal_armed = false;
    bd::ui::vr_options::Adjust(x > 0.0f ? 1 : -1);
  }
  if (close_armed && s.b) {
    close_armed = false;
    bd::ui::vr_options::SetOpen(false);
  }
}

void SubmitPad(const PadState &state) {
  // The gesture toggles on release, so holding it does not flap the panel.
  static bool gesture_held = false;
  const bool gesture = MenuGesture(state);
  if (gesture_held && !gesture) {
    bd::ui::vr_options::Toggle();
    BD_INFO("[xr] VR options panel {}",
            bd::ui::vr_options::IsOpen() ? "opened" : "closed");
  }
  gesture_held = gesture;

  // While the panel is up the pad drives it and the guest sees neutral -
  // otherwise the character walks around underneath the menu, which is exactly
  // what a stick-driven overlay does if you forget this.
  PadState forwarded = state;
  if (bd::ui::vr_options::IsOpen()) {
    DriveOptions(state);
    const bool connected = forwarded.connected;
    forwarded = PadState{};
    forwarded.connected = connected;
  }

  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pad = forwarded;
  }
  if (state.connected && !g_everConnected.exchange(true, std::memory_order_relaxed))
    BD_INFO("[xr] controller connected");

  // Logged on change only. Bindings that silently go to the wrong button are
  // the usual way an OpenXR input map is wrong, and that is invisible from
  // inside the game - the guest just does something unexpected. One line per
  // press makes it checkable without a headset on someone's face.
  static u32 previous = 0;
  const u32 pressed = (state.a ? 1u : 0u) | (state.b ? 2u : 0u) |
                      (state.x ? 4u : 0u) | (state.y ? 8u : 0u) |
                      (state.menu ? 16u : 0u) |
                      (state.leftThumbClick ? 32u : 0u) |
                      (state.rightThumbClick ? 64u : 0u);
  if (pressed != previous) {
    previous = pressed;
    BD_INFO("[xr] pad 0x{:02X} sticks ({:.2f},{:.2f}) ({:.2f},{:.2f}) "
            "trig {:.2f}/{:.2f} grip {:.2f}/{:.2f}",
            pressed, state.leftStickX, state.leftStickY, state.rightStickX,
            state.rightStickY, state.leftTrigger, state.rightTrigger,
            state.leftGrip, state.rightGrip);
  }
}

bool CurrentPad(PadState &out) {
  if (!g_everConnected.load(std::memory_order_relaxed))
    return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  out = g_pad;
  return true;
}

} // namespace bd::xr
