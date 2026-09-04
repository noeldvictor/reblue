/**
 * @file    native_texture_gpu.h
 * @brief   Host-owned immutable texture images, views and bindless bindings.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license   BSD 3-Clause License
 */
#pragma once
#include "gpu/scene/native_texture_library.h"
#include <plume_render_interface.h>

namespace bd::gpu {
struct VideoState;
namespace scene {
struct NativeTextureGpu {
  NativeTextureHandle asset;
  // Declaration order ensures the view dies before its image.
  std::unique_ptr<plume::RenderTexture> image;
  std::unique_ptr<plume::RenderTextureView> view;
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  plume::RenderTextureViewDimension dimension =
      plume::RenderTextureViewDimension::UNKNOWN;
  uint32_t descriptor = ~uint32_t{0};
  uint8_t alpha_opaque = 0;
};
using NativeTextureGpuHandle = std::shared_ptr<const NativeTextureGpu>;

// Native source in, shared native binding out. No guest wrapper, fetch
// constant, resource address or format is needed. The device's store owns
// residency and releases descriptors/images only after the last handle AND the
// correct fence.
NativeTextureGpuHandle
AcquireNativeTextureGpu(const NativeTextureHandle &asset);

// Renderer lock held; first at the proven fence, second after slot-entry
// drains.
void DrainNativeTextureGpuLocked(VideoState &s, uint32_t slot);
void MarkUnusedNativeTextureGpuLocked(VideoState &s, uint32_t recording_slot);
} // namespace scene
} // namespace bd::gpu
