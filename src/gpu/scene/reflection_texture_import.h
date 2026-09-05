/**
 * @file    reflection_texture_import.h
 * @brief   Explicit, temporary engine association boundary for reflection.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include "gpu/scene/native_material_data.h"

namespace bd::gpu::scene {
// Engine addresses never enter NativeReflectionRecipe. This short-lived
// adapter snapshots the node's pass/table selection, not a previous draw.
struct ReflectionTextureImport {
  uint32_t pass_default = 0;
  uint32_t table_offset = 0, table_count = 0, table_entries = 0, fallback = 0;
  bool has_table = false;
};
constexpr uint32_t kReflectionPassDefault = (uint32_t(-32036) << 16) - 22280 + 68;
constexpr uint32_t kReflectionTableState = (uint32_t(-32036) << 16) - 7864;

// The scene-target binder is both a virtual callback and a direct helper of
// another material callback. Neither leaves slot 5 under model-command control.
inline bool ModelReflectionCallbackSupported(uint32_t callback) {
  return callback && callback != 0x8221E618 && callback != 0x82454C08;
}

// Read takes a checked 64-bit address and returns a host-endian optional word.
template <class Read>
std::optional<ReflectionTextureImport> ReadReflectionTextureImport(Read read) {
  ReflectionTextureImport result;
  const auto pass = read(kReflectionPassDefault);
  const auto table = read(kReflectionTableState + 4);
  if (!pass || !table)
    return {};
  result.pass_default = *pass;
  if (!*table)
    return result;
  const auto offset = read(kReflectionTableState);
  const auto count = read(*table);
  const auto entries = read(uint64_t(*table) + 4);
  const auto fallback = read(kReflectionTableState + 32);
  if (!offset || !count || !entries || !fallback)
    return {};
  result.has_table = true;
  result.table_offset = *offset;
  result.table_count = *count;
  result.table_entries = *entries;
  result.fallback = *fallback;
  return result;
}

template <class Read>
std::optional<uint32_t> SelectReflectionTextureImport(
    const ReflectionTextureImport &inputs, const NativeReflectionRecipe &recipe,
    Read read) {
  if (recipe.source == ReflectionTextureSource::PassDefault)
    return inputs.pass_default;
  if (recipe.source != ReflectionTextureSource::Table)
    return {};
  // A missing table returns null, NOT the fallback/default texture. The
  // original lookup compares the low 32 bits of offset + material index.
  if (!inputs.has_table)
    return 0;
  const uint32_t index = inputs.table_offset + uint32_t(recipe.table_index);
  if (index >= inputs.table_count)
    return inputs.fallback;
  const uint64_t address = uint64_t(inputs.table_entries) + uint64_t(index) * 28 + 24;
  if (!inputs.table_entries || address > UINT32_MAX - 3)
    return {};
  return read(address);
}
} // namespace bd::gpu::scene
