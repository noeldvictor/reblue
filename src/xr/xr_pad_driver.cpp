/**
 * @file    xr/xr_pad_driver.cpp
 * @brief   See xr_pad_driver.h.
 * @license BSD 3-Clause, see LICENSE
 */
#include "xr/xr_pad_driver.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <rex/input/flags.h>

#include "core/logging.h"

#include <chrono>

#include <rex/cvar.h>

#include "core/settings.h" // kCvarGroup
#include "xr/xr_pad.h"

namespace bd::xr {

namespace {

// Drives the game from the log, with nobody wearing the headset.
//
// Every VR bug so far has needed a person in the headset to see it, which
// makes the loop minutes long and the report second-hand. This presses START
// and then A on a fixed schedule, which is enough to get from the title screen
// into the field, so a build can be deployed, screenshotted and measured
// without leaving the terminal. Off unless asked for; it would fight a real
// player.
REXCVAR_DEFINE_BOOL(bd_xr_autoplay, false, kCvarGroup,
                    "Synthesise pad presses to walk the game into a field "
                    "scene unattended, for screenshots and profiling.");

// One device, so its handle is a constant. 'XRPD'.
constexpr rex::input::DeviceId kPadDevice =
    static_cast<rex::input::DeviceId>(0x58525044);

// A grip or trigger counts as its shoulder button past halfway. Blue Dragon
// only ever asks whether the shoulder is down, so where exactly the line sits
// matters less than it being well clear of a resting finger.
constexpr f32 kButtonThreshold = 0.5f;

// The right stick doubles as the d-pad, and menus repeat on it, so the
// threshold is high enough that a diagonal push does not fire both axes.
constexpr f32 kDpadThreshold = 0.6f;

i16 ToThumb(f32 v) {
  const f32 clamped = std::clamp(v, -1.0f, 1.0f);
  // 32767 rather than 32768 so full deflection is representable in both
  // directions without wrapping.
  return static_cast<i16>(std::lround(clamped * 32767.0f));
}

u8 ToTrigger(f32 v) {
  return static_cast<u8>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

// START once the game has had time to reach its title, then A on a slow
// repeat. Blue Dragon's opening is START, then a confirm on the file menu,
// then dialogue - all of which A advances. Held for a few frames because the
// guest samples its pad at 30Hz and edge-detects.
void ApplyAutoplay(PadState &pad) {
  using Clock = std::chrono::steady_clock;
  static Clock::time_point start{};
  if (start.time_since_epoch().count() == 0)
    start = Clock::now();
  const double t = std::chrono::duration<double>(Clock::now() - start).count();

  if (t < 6.0)
    return; // let it finish booting
  if (t < 6.4) {
    pad.menu = true; // START
    return;
  }
  // A for 200ms out of every 1.2s thereafter.
  const double phase = std::fmod(t - 6.4, 1.2);
  pad.a = phase < 0.2;
}

u16 ButtonsFrom(const PadState &pad) {
  // The X_INPUT_GAMEPAD_* enumerators are unscoped and live in rex::input;
  // importing the namespace for the length of this function is a good deal
  // less noise than qualifying fourteen of them.
  using namespace rex::input;

  u16 buttons = 0;
  if (pad.a) buttons |= X_INPUT_GAMEPAD_A;
  if (pad.b) buttons |= X_INPUT_GAMEPAD_B;
  if (pad.x) buttons |= X_INPUT_GAMEPAD_X;
  if (pad.y) buttons |= X_INPUT_GAMEPAD_Y;
  if (pad.menu) buttons |= X_INPUT_GAMEPAD_START;

  // Touch has exactly one menu button, on the left controller, and the right
  // one's system button belongs to the runtime. So BACK - which Blue Dragon
  // uses throughout its menus - goes on the left stick click, and the left
  // thumb button the guest also has goes unbound rather than stealing it.
  if (pad.leftThumbClick) buttons |= X_INPUT_GAMEPAD_BACK;
  if (pad.rightThumbClick) buttons |= X_INPUT_GAMEPAD_RIGHT_THUMB;

  if (pad.leftGrip >= kButtonThreshold) buttons |= X_INPUT_GAMEPAD_LEFT_SHOULDER;
  if (pad.rightGrip >= kButtonThreshold) buttons |= X_INPUT_GAMEPAD_RIGHT_SHOULDER;

  // D-pad off the right stick, leaving the left one for movement.
  if (pad.rightStickY >= kDpadThreshold) buttons |= X_INPUT_GAMEPAD_DPAD_UP;
  if (pad.rightStickY <= -kDpadThreshold) buttons |= X_INPUT_GAMEPAD_DPAD_DOWN;
  if (pad.rightStickX <= -kDpadThreshold) buttons |= X_INPUT_GAMEPAD_DPAD_LEFT;
  if (pad.rightStickX >= kDpadThreshold) buttons |= X_INPUT_GAMEPAD_DPAD_RIGHT;
  return buttons;
}

} // namespace

PadDriver::PadDriver(rex::ui::Window *window, size_t window_z_order)
    : InputDriver(window, window_z_order) {}

PadDriver::~PadDriver() = default;

X_STATUS PadDriver::Setup() { return X_STATUS_SUCCESS; }

void PadDriver::EnumerateDevices(std::vector<rex::input::DeviceInfo> &out) {
  PadState pad;
  if (!CurrentPad(pad))
    return; // no runtime, or no controller has reported yet

  rex::input::DeviceInfo info;
  info.id = kPadDevice;
  info.name = "OpenXR Controllers";
  out.push_back(info);
}

X_RESULT PadDriver::GetDeviceCapabilities(rex::input::DeviceId id, uint32_t,
                                          X_INPUT_CAPABILITIES *out_caps) {
  if (id != kPadDevice)
    return X_ERROR_DEVICE_NOT_CONNECTED;
  if (out_caps) {
    std::memset(out_caps, 0, sizeof(*out_caps));
    out_caps->type = 0x01;     // XINPUT_DEVTYPE_GAMEPAD
    out_caps->sub_type = 0x01; // XINPUT_DEVSUBTYPE_GAMEPAD
    out_caps->gamepad.buttons = 0xFFFF;
    out_caps->gamepad.left_trigger = 0xFF;
    out_caps->gamepad.right_trigger = 0xFF;
    out_caps->gamepad.thumb_lx = static_cast<i16>(0x7FFF);
    out_caps->gamepad.thumb_ly = static_cast<i16>(0x7FFF);
    out_caps->gamepad.thumb_rx = static_cast<i16>(0x7FFF);
    out_caps->gamepad.thumb_ry = static_cast<i16>(0x7FFF);
    out_caps->vibration.left_motor_speed = 0xFFFF;
    out_caps->vibration.right_motor_speed = 0xFFFF;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT PadDriver::GetDeviceState(rex::input::DeviceId id,
                                   X_INPUT_STATE *out_state) {
  if (id != kPadDevice)
    return X_ERROR_DEVICE_NOT_CONNECTED;

  PadState pad;
  if (!CurrentPad(pad))
    return X_ERROR_DEVICE_NOT_CONNECTED;

  // One line, the first time the guest actually asks. Everything upstream of
  // here can be verified from the log already; this is the only link that
  // proves the guest found the device and is polling it, as opposed to reading
  // the NOP pad and ignoring us.
  static bool announced = false;
  if (!announced) {
    announced = true;
    BD_INFO("[xr] guest is polling the OpenXR pad");
  }

  if (REXCVAR_GET(bd_xr_autoplay))
    ApplyAutoplay(pad);

  if (out_state) {
    std::memset(out_state, 0, sizeof(*out_state));
    out_state->gamepad.buttons = ButtonsFrom(pad);
    out_state->gamepad.left_trigger = ToTrigger(pad.leftTrigger);
    out_state->gamepad.right_trigger = ToTrigger(pad.rightTrigger);
    out_state->gamepad.thumb_lx = ToThumb(pad.leftStickX);
    out_state->gamepad.thumb_ly = ToThumb(pad.leftStickY);
    out_state->gamepad.thumb_rx = ToThumb(pad.rightStickX);
    out_state->gamepad.thumb_ry = ToThumb(pad.rightStickY);
  }
  return X_ERROR_SUCCESS;
}

X_RESULT PadDriver::SetDeviceVibration(rex::input::DeviceId id,
                                       X_INPUT_VIBRATION *) {
  if (id != kPadDevice)
    return X_ERROR_DEVICE_NOT_CONNECTED;
  // Touch controllers do have haptics, through xrApplyHapticFeedback. Not
  // wired up yet; accepted silently so the guest's rumble calls are not errors.
  return X_ERROR_SUCCESS;
}

X_RESULT PadDriver::GetDeviceKeystroke(rex::input::DeviceId id, uint32_t,
                                       X_INPUT_KEYSTROKE *) {
  if (id != kPadDevice)
    return X_ERROR_DEVICE_NOT_CONNECTED;
  return X_ERROR_EMPTY;
}

} // namespace bd::xr
