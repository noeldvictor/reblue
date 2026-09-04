/**
 * @file    host_mips_bridge.cpp
 * @brief   Temporary format/upload adapter around the native mip cooker.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license   BSD 3-Clause License
 */
#include "gpu/host_mips.h"
#include "gpu/scene/native_texture_data.h"
#include <algorithm>
#include <cstring>
#include <rex/graphics/xenos.h>

namespace bd::gpu {
namespace {
scene::NativeTextureFormat Format(u32 value) {
  using rex::graphics::xenos::TextureFormat;
  switch (TextureFormat(value)) {
  case TextureFormat::k_DXT1:
    return scene::NativeTextureFormat::BC1;
  case TextureFormat::k_DXT2_3:
    return scene::NativeTextureFormat::BC2;
  case TextureFormat::k_DXT4_5:
    return scene::NativeTextureFormat::BC3;
  default:
    return scene::NativeTextureFormat(0);
  }
}
} // namespace
bool HostMipsSupported(u32 format) {
  return scene::NativeTextureBlockBytes(Format(format)) != 0;
}
bool GenerateHostMips(u32 format, u32 width, u32 height, const u8 *base,
                      size_t size, u32 row_bytes, u32 row_width,
                      HostMipChain &out) {
  out.storage.clear();
  out.levels.clear();
  if (!base || !HostMipsSupported(format))
    return false;
  scene::NativeTextureData data;
  data.format = Format(format);
  data.width = width;
  data.height = height;
  data.images.resize(1);
  if (!scene::ImportNativeTextureImage(
          data.format, width, height, 1, {base, size}, row_bytes,
          uint64_t(row_bytes) * ((height + 3) / 4), data.images[0]))
    return false;
  scene::NativeTextureData chain;
  if (!scene::GenerateNativeTextureMips(data, chain))
    return false;
  out.storage.resize(chain.mip_levels - 1);
  out.levels.push_back({base, size, width, height, row_width});
  const auto block_bytes = scene::NativeTextureBlockBytes(data.format);
  for (u32 i = 1; i < chain.mip_levels; ++i) {
    const auto w = std::max(width >> i, 1u), h = std::max(height >> i, 1u);
    const auto tight = ((w + 3) / 4) * block_bytes,
               pitch = (tight + 255u) & ~255u;
    auto &image = out.storage[i - 1];
    image.resize(pitch * ((h + 3) / 4));
    for (u32 y = 0; y < (h + 3) / 4; ++y)
      std::memcpy(image.data() + y * pitch, chain.images[i].data() + y * tight,
                  tight);
    out.levels.push_back(
        {image.data(), image.size(), w, h, (pitch / block_bytes) * 4});
  }
  return true;
}
} // namespace bd::gpu
