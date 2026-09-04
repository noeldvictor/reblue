/**
 * @file    native_material_asset.h
 * @brief   Portable material assets and stable, content-based identities.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license   BSD 3-Clause License
 */
#pragma once

#include "gpu/scene/native_material_data.h"

namespace bd::gpu::scene {

enum class NativeLightingModel : uint32_t {
  OriginalLit = 0,
  Cel = 1, // format slot; the native cel shader is not implemented yet
};

struct NativeMaterialAsset {
  NativeMaterialProperties properties;
  NativeLightingModel lighting_model = NativeLightingModel::OriginalLit;
  bool operator==(const NativeMaterialAsset &) const = default;
};

using NativeMaterialId = uint64_t;
constexpr size_t kNativeMaterialFileBytes = 68;

// Version 1 stores colour/shininess recipes, not textures or guest draw state.
// All integers and IEEE-754 binary32 values are little endian. No raw C++
// struct layout, guest addresses, geometry records or shader register numbers.
// Unknown values and negative zero are canonicalized before hashing/writing.
bool EncodeNativeMaterial(const NativeMaterialAsset &asset,
                          std::vector<uint8_t> &file);
bool DecodeNativeMaterial(std::span<const uint8_t> file,
                          NativeMaterialAsset &asset);
NativeMaterialId NativeMaterialContentId(std::span<const uint8_t> file);

// Never silently shade an unsupported lighting model as OriginalLit.
uint32_t ComposeNativeMaterialAsset(const NativeMaterialAsset &asset,
                                   const std::array<float, 4> &object_colour,
                                   bool writes_shininess,
                                   std::array<float, 4> values[3]);

} // namespace bd::gpu::scene
