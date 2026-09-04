/**
 * @file    native_texture_data.h
 * @brief   Portable, tightly packed native texture assets, including mip
 * chains.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license   BSD 3-Clause License
 */
#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace bd::gpu::scene {
enum class NativeTextureFormat : uint32_t {
  BC1 = 1,
  BC2 = 2,
  BC3 = 3,
  RGBA8 = 4
};
enum class NativeTextureDimension : uint32_t {
  Image2D = 1,
  Cube = 2,
  Volume = 3
};
struct NativeTextureData {
  NativeTextureFormat format = NativeTextureFormat::BC1;
  NativeTextureDimension dimension = NativeTextureDimension::Image2D;
  uint32_t width = 0, height = 0, depth = 1, mip_levels = 1;
  // Layer-major, then mip. One layer for 2D/volume, six for cube. Volume
  // slices are contiguous within a mip. No upload-ring row padding on disk.
  std::vector<std::vector<uint8_t>> images;
  bool operator==(const NativeTextureData &) const = default;
};
constexpr uint64_t kNativeTextureMaxBytes = 64ull << 20;
uint32_t NativeTextureBlockBytes(NativeTextureFormat format);
uint32_t NativeTextureBlockEdge(NativeTextureFormat format);
uint32_t NativeTextureLayers(const NativeTextureData &data);
uint64_t NativeTextureImageBytes(NativeTextureFormat format, uint32_t width,
                                 uint32_t height, uint32_t depth);
bool ValidateNativeTexture(const NativeTextureData &data);
// Strip source row/slice padding. Source is CPU-owned, never mapped GPU memory.
bool ImportNativeTextureImage(NativeTextureFormat format, uint32_t width,
                              uint32_t height, uint32_t depth,
                              std::span<const uint8_t> source,
                              uint64_t row_pitch, uint64_t slice_pitch,
                              std::vector<uint8_t> &out);
bool EncodeNativeTexture(const NativeTextureData &data,
                         std::vector<uint8_t> &file);
bool DecodeNativeTexture(std::span<const uint8_t> file,
                         NativeTextureData &data);
uint64_t NativeTextureContentId(std::span<const uint8_t> file);
// BC1/2/3 box-filter recipe v1; preserves the existing alpha-aware compressor.
// Input is one 2D base level, output stops at the 4x4 block level as before.
bool GenerateNativeTextureMips(const NativeTextureData &base,
                               NativeTextureData &out);
} // namespace bd::gpu::scene
