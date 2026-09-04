/**
 * @file    native_texture_binding.h
 * @brief   Native sampled-image ownership, independent of guest resources.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license   BSD 3-Clause License
 */
#pragma once
#include "gpu/scene/native_texture_gpu.h"

namespace bd::gpu::scene {
// Companions are explicit native assets, not borrowed views of a guest object.
// They preserve the current shader ABI's volume slice and atlas/cube inputs
// until materials and shaders have explicit dimensional texture semantics.
struct NativeTextureBinding {
  NativeTextureGpuHandle primary;
  NativeTextureGpuHandle slice_2d;
  NativeTextureGpuHandle cube;

  bool operator==(const NativeTextureBinding &) const = default;
};

struct NativeTextureIndices {
  uint32_t image_2d;
  uint32_t image_3d;
  uint32_t image_cube;
};

inline NativeTextureIndices TextureIndices(const NativeTextureBinding &binding,
                                           NativeTextureIndices nulls) {
  if (!binding.primary)
    return nulls;
  const auto &gpu = *binding.primary;
  switch (gpu.dimension) {
  case plume::RenderTextureViewDimension::TEXTURE_3D:
    nulls.image_3d = gpu.descriptor;
    if (binding.slice_2d)
      nulls.image_2d = binding.slice_2d->descriptor;
    break;
  case plume::RenderTextureViewDimension::TEXTURE_CUBE:
    nulls.image_cube = gpu.descriptor;
    break;
  default:
    nulls.image_2d = gpu.descriptor;
    if (binding.cube)
      nulls.image_cube = binding.cube->descriptor;
    break;
  }
  return nulls;
}
} // namespace bd::gpu::scene
