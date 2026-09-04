/**
 * @file    native_texture_data.cpp
 * @brief   Checked native texture geometry, padding removal and file format.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license   BSD 3-Clause License
 */
#include "gpu/scene/native_texture_data.h"
#include <algorithm>
#include <bit>
#include <cstring>

namespace bd::gpu::scene {
namespace {
constexpr uint8_t kMagic[8] = {'B', 'D', 'T', 'E', 'X', 0, 1, 0};
void Put(std::vector<uint8_t> &out, uint64_t value, unsigned count = 4) {
  for (unsigned i = 0; i < count; ++i)
    out.push_back(uint8_t(value >> (i * 8)));
}
uint64_t Get(std::span<const uint8_t> file, size_t offset, unsigned count = 4) {
  uint64_t value = 0;
  for (unsigned i = 0; i < count; ++i)
    value |= uint64_t(file[offset + i]) << (i * 8);
  return value;
}
bool Shape(const NativeTextureData &data) {
  if (!NativeTextureBlockBytes(data.format) || !data.width || !data.height ||
      !data.depth || data.width > 16384 || data.height > 16384 ||
      data.depth > 2048 || !data.mip_levels ||
      data.mip_levels >
          std::bit_width(std::max({data.width, data.height, data.depth})))
    return false;
  switch (data.dimension) {
  case NativeTextureDimension::Image2D:
    return data.depth == 1;
  case NativeTextureDimension::Cube:
    return data.depth == 1 && data.width == data.height;
  case NativeTextureDimension::Volume:
    return true;
  default:
    return false;
  }
}
} // namespace

uint32_t NativeTextureBlockBytes(NativeTextureFormat format) {
  switch (format) {
  case NativeTextureFormat::BC1:
    return 8;
  case NativeTextureFormat::BC2:
  case NativeTextureFormat::BC3:
    return 16;
  case NativeTextureFormat::RGBA8:
    return 4;
  default:
    return 0;
  }
}
uint32_t NativeTextureBlockEdge(NativeTextureFormat format) {
  return format == NativeTextureFormat::RGBA8 ? 1 : 4;
}
uint32_t NativeTextureLayers(const NativeTextureData &data) {
  return data.dimension == NativeTextureDimension::Cube ? 6 : 1;
}
uint64_t NativeTextureImageBytes(NativeTextureFormat format, uint32_t width,
                                 uint32_t height, uint32_t depth) {
  if (!width || !height || !depth || width > 16384 || height > 16384 ||
      depth > 2048)
    return 0;
  const auto edge = NativeTextureBlockEdge(format);
  return uint64_t((width + edge - 1) / edge) * ((height + edge - 1) / edge) *
         depth * NativeTextureBlockBytes(format);
}
bool ValidateNativeTexture(const NativeTextureData &data) {
  if (!Shape(data) ||
      data.images.size() != NativeTextureLayers(data) * data.mip_levels)
    return false;
  uint64_t bytes = 40;
  for (size_t i = 0; i < data.images.size(); ++i) {
    const auto mip = i % data.mip_levels;
    const uint64_t size = NativeTextureImageBytes(
        data.format, std::max(data.width >> mip, 1u),
        std::max(data.height >> mip, 1u), std::max(data.depth >> mip, 1u));
    bytes += size;
    if (!size || data.images[i].size() != size ||
        bytes > kNativeTextureMaxBytes)
      return false;
  }
  return true;
}
bool ImportNativeTextureImage(NativeTextureFormat format, uint32_t width,
                              uint32_t height, uint32_t depth,
                              std::span<const uint8_t> source,
                              uint64_t row_pitch, uint64_t slice_pitch,
                              std::vector<uint8_t> &out) {
  const auto size = NativeTextureImageBytes(format, width, height, depth);
  if (!size || size > kNativeTextureMaxBytes ||
      row_pitch > kNativeTextureMaxBytes ||
      slice_pitch > kNativeTextureMaxBytes)
    return false;
  const uint64_t rows = (height + NativeTextureBlockEdge(format) - 1) /
                        NativeTextureBlockEdge(format);
  const uint64_t row_bytes = size / depth / rows;
  if (row_pitch < row_bytes || slice_pitch < row_pitch * rows ||
      (depth - 1) * slice_pitch + (rows - 1) * row_pitch + row_bytes >
          source.size())
    return false;
  std::vector<uint8_t> result(static_cast<size_t>(size));
  for (uint32_t z = 0; z < depth; ++z)
    for (uint64_t y = 0; y < rows; ++y)
      std::memcpy(result.data() + (z * rows + y) * row_bytes,
                  source.data() + z * slice_pitch + y * row_pitch,
                  size_t(row_bytes));
  out = std::move(result);
  return true;
}
uint64_t NativeTextureContentId(std::span<const uint8_t> file) {
  uint64_t hash = 14695981039346656037ull;
  for (uint8_t byte : file)
    hash = (hash ^ byte) * 1099511628211ull;
  return hash;
}
bool EncodeNativeTexture(const NativeTextureData &data,
                         std::vector<uint8_t> &file) {
  if (!ValidateNativeTexture(data))
    return false;
  std::vector<uint8_t> result(std::begin(kMagic), std::end(kMagic));
  Put(result, 0, 8);
  Put(result, uint32_t(data.format));
  Put(result, uint32_t(data.dimension));
  Put(result, data.width);
  Put(result, data.height);
  Put(result, data.depth);
  Put(result, data.mip_levels);
  for (const auto &image : data.images)
    result.insert(result.end(), image.begin(), image.end());
  const uint64_t sum = NativeTextureContentId(std::span(result).subspan(16));
  for (unsigned i = 0; i < 8; ++i)
    result[8 + i] = uint8_t(sum >> (8 * i));
  file = std::move(result);
  return true;
}
bool DecodeNativeTexture(std::span<const uint8_t> file,
                         NativeTextureData &data) {
  if (file.size() < 40 || file.size() > kNativeTextureMaxBytes ||
      !std::equal(std::begin(kMagic), std::end(kMagic), file.begin()) ||
      Get(file, 8, 8) != NativeTextureContentId(file.subspan(16)))
    return false;
  NativeTextureData result;
  result.format = NativeTextureFormat(uint32_t(Get(file, 16)));
  result.dimension = NativeTextureDimension(uint32_t(Get(file, 20)));
  result.width = uint32_t(Get(file, 24));
  result.height = uint32_t(Get(file, 28));
  result.depth = uint32_t(Get(file, 32));
  result.mip_levels = uint32_t(Get(file, 36));
  if (!Shape(result))
    return false;
  size_t cursor = 40;
  for (uint32_t i = 0; i < NativeTextureLayers(result) * result.mip_levels;
       ++i) {
    const auto mip = i % result.mip_levels;
    const auto size = NativeTextureImageBytes(
        result.format, std::max(result.width >> mip, 1u),
        std::max(result.height >> mip, 1u), std::max(result.depth >> mip, 1u));
    if (!size || size > file.size() - cursor)
      return false;
    result.images.emplace_back(file.begin() + cursor,
                               file.begin() + cursor + size);
    cursor += size;
  }
  if (cursor != file.size() || !ValidateNativeTexture(result))
    return false;
  data = std::move(result);
  return true;
}
} // namespace bd::gpu::scene
