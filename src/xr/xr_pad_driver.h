/**
 * @file    xr/xr_pad_driver.h
 * @brief   Presents the Touch controllers to the guest as an Xbox 360 pad.
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once

#include <memory>

#include <rex/input/input_driver.h>

namespace bd::xr {

// The SDK's driver interface is declared from inside rex::input, where these
// names need no qualifying. This driver lives in bd::xr, so it imports them
// once here rather than spelling out a namespace on every signature.
using rex::X_RESULT;
using rex::X_STATUS;
using rex::input::X_INPUT_CAPABILITIES;
using rex::input::X_INPUT_KEYSTROKE;
using rex::input::X_INPUT_STATE;
using rex::input::X_INPUT_VIBRATION;

// Reports one device while an OpenXR runtime is delivering controller state,
// and none otherwise - so a desktop build with this driver installed behaves
// exactly as if it were not.
class PadDriver final : public rex::input::InputDriver {
public:
  PadDriver(rex::ui::Window *window, size_t window_z_order);
  ~PadDriver() override;

  X_STATUS Setup() override;
  void EnumerateDevices(std::vector<rex::input::DeviceInfo> &out) override;
  X_RESULT GetDeviceState(rex::input::DeviceId id,
                          X_INPUT_STATE *out_state) override;
  X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t flags,
                                 X_INPUT_CAPABILITIES *out_caps) override;
  X_RESULT SetDeviceVibration(rex::input::DeviceId id,
                              X_INPUT_VIBRATION *vibration) override;
  X_RESULT GetDeviceKeystroke(rex::input::DeviceId id, uint32_t flags,
                              X_INPUT_KEYSTROKE *out_keystroke) override;
};

} // namespace bd::xr
