/**
 * @file    native_texture_binding_bridge.h
 * @brief   Borrow immutable native image ownership from a live resource adapter.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include "gpu/scene/native_texture_binding.h"
namespace bd::gpu { struct GuestTexture; }
namespace bd::gpu::scene {
NativeTextureBinding CaptureNativeTexture(const GuestTexture *texture);
} // namespace bd::gpu::scene
