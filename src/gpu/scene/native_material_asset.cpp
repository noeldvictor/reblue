/**
 * @file    native_material_asset.cpp
 * @brief   Canonical, checked native material serialization.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license   BSD 3-Clause License
 */
#include "gpu/scene/native_material_asset.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace bd::gpu::scene {
namespace {
constexpr uint8_t kMagic[8] = {'B', 'D', 'M', 'A', 'T', 0, 1, 0};
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);

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
bool Colour(std::vector<uint8_t> &out, std::span<const float> values, bool known) {
  for (float value : values) {
    if (!known)
      value = 0;
    if (!std::isfinite(value) || value < 0 || value > 1)
      return false;
    Put(out, value == 0 ? 0 : std::bit_cast<uint32_t>(value));
  }
  return true;
}
} // namespace

NativeMaterialId NativeMaterialContentId(std::span<const uint8_t> file) {
  // FNV-1a over canonical bytes, stable on every host. This detects accidental
  // corruption and names derived assets; it is not a security/authenticity hash.
  uint64_t hash = 14695981039346656037ull;
  for (uint8_t byte : file)
    hash = (hash ^ byte) * 1099511628211ull;
  return hash;
}

bool EncodeNativeMaterial(const NativeMaterialAsset &asset,
                          std::vector<uint8_t> &file) {
  if (asset.lighting_model != NativeLightingModel::OriginalLit &&
      asset.lighting_model != NativeLightingModel::Cel)
    return false;
  const auto &m = asset.properties;
  std::vector<uint8_t> result(std::begin(kMagic), std::end(kMagic));
  result.reserve(kNativeMaterialFileBytes);
  Put(result, 0, 8);
  Put(result, uint32_t(asset.lighting_model));
  Put(result, uint32_t(m.modulate_diffuse) | (uint32_t(m.has_diffuse_multiplier) << 1) |
      (uint32_t(m.has_specular_colour) << 2) | (uint32_t(m.has_reflection_colour) << 3) |
      (uint32_t(m.has_shininess) << 4));
  Put(result, m.has_shininess ? m.shininess : 0);
  if (!Colour(result, m.diffuse_multiplier, m.has_diffuse_multiplier) ||
      !Colour(result, m.specular_colour, m.has_specular_colour) ||
      !Colour(result, m.reflection_colour, m.has_reflection_colour))
    return false;
  const uint64_t checksum = NativeMaterialContentId(std::span(result).subspan(16));
  for (unsigned i = 0; i < 8; ++i)
    result[8 + i] = uint8_t(checksum >> (i * 8));
  file = std::move(result);
  return true;
}

bool DecodeNativeMaterial(std::span<const uint8_t> file,
                          NativeMaterialAsset &asset) {
  if (file.size() != kNativeMaterialFileBytes ||
      !std::equal(std::begin(kMagic), std::end(kMagic), file.begin()) ||
      Get(file, 8, 8) != NativeMaterialContentId(file.subspan(16)))
    return false;
  NativeMaterialAsset result;
  result.lighting_model = NativeLightingModel(uint32_t(Get(file, 16)));
  auto &m = result.properties;
  const uint32_t flags = uint32_t(Get(file, 20));
  const uint32_t shininess = uint32_t(Get(file, 24));
  if ((flags & ~31u) || shininess > 255)
    return false;
  m.modulate_diffuse = flags & 1;
  m.has_diffuse_multiplier = flags & 2;
  m.has_specular_colour = flags & 4;
  m.has_reflection_colour = flags & 8;
  m.has_shininess = flags & 16;
  m.shininess = uint8_t(shininess);
  size_t offset = 28;
  for (auto values : {std::span(m.diffuse_multiplier), std::span(m.specular_colour)})
    for (float &value : values) {
      value = std::bit_cast<float>(uint32_t(Get(file, offset)));
      offset += 4;
    }
  for (float &value : m.reflection_colour) {
    value = std::bit_cast<float>(uint32_t(Get(file, offset)));
    offset += 4;
  }
  // Reject noncanonical encodings too: one logical asset has one identity.
  std::vector<uint8_t> canonical;
  if (!EncodeNativeMaterial(result, canonical) ||
      !std::equal(canonical.begin(), canonical.end(), file.begin(), file.end()))
    return false;
  asset = result;
  return true;
}

uint32_t ComposeNativeMaterialAsset(const NativeMaterialAsset &asset,
                                   const std::array<float, 4> &object_colour,
                                   bool writes_shininess,
                                   std::array<float, 4> values[3]) {
  if (asset.lighting_model != NativeLightingModel::OriginalLit)
    return 0;
  return ComposeNativeMaterial(asset.properties, object_colour, writes_shininess,
                               values[0], values[1], values[2]);
}
} // namespace bd::gpu::scene
