/**
 * @file    native_pass_bridge.h
 * @brief   Typed pass entry/exit with temporary engine getter publications.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include <cstddef>
#include <cstdint>
namespace bd::gpu {
struct GuestTexture;
namespace scene {
// No attachment effects. Native producers cannot take the engine overflow no-op.
bool CanEnterNativePass();
bool EnterNativePass(GuestTexture *color, GuestTexture *depth, uint32_t &result);
bool LeaveNativePass(uint32_t &result);
std::size_t NativePassDepth();
} // namespace scene
} // namespace bd::gpu
