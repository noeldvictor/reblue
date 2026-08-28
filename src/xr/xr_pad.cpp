/**
 * @file    xr/xr_pad.cpp
 * @brief   See xr_pad.h.
 * @license BSD 3-Clause, see LICENSE
 */
#include "xr/xr_pad.h"

#include <atomic>
#include <mutex>

#include "core/logging.h"

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

void SubmitPad(const PadState &state) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pad = state;
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
