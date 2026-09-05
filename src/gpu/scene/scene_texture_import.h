/**
 * @file    scene_texture_import.h
 * @brief   Checked temporary boundary for current/next scene images.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include "gpu/scene/scene_texture_recipe.h"
#include <array>
#include <cstdint>
#include <optional>

namespace bd::gpu::scene {
constexpr uint32_t kSceneTextureTable = (uint32_t(-32137) << 16) + 29804 + 4;
constexpr uint32_t kActiveTextureTable = (uint32_t(-32036) << 16) - 7864;

constexpr SceneTextureProducer ImportSceneTextureProducer(uint32_t callback) {
  return callback == 0x8221E618 ? SceneTextureProducer::Images
      : callback == 0x82454C08 ? SceneTextureProducer::ImagesAndBlend
      : SceneTextureProducer::None;
}

// Temporary source stamp, never stored in a native recipe. Equal images from
// different table rows are not evidence that the selection path is unchanged.
struct SceneTextureSelection {
  uint32_t table = 0, active_table = 0, count = 0, offset = 0, index = 0;
  uint32_t source_word = 0, image = 0;
  bool operator==(const SceneTextureSelection &) const = default;
};
using SceneTextureSelections = std::array<SceneTextureSelection, 2>;

// Unlike model reflection selection, these images always come from the scene
// table. The active offset applies only when that table is the active table.
// A null selection is a legacy no-op at publication, never an unbind.
template <class Read>
std::optional<SceneTextureSelection> ReadSceneTextureSelectionSource(
    SceneTextureRole role, Read read) {
  const auto table = read(kSceneTextureTable);
  if (!table)
    return {};
  if (!*table)
    return SceneTextureSelection{};
  const auto active_table = read(kActiveTextureTable + 4);
  const auto count = read(uint64_t(*table));
  if (!active_table || !count)
    return {};
  SceneTextureSelection result;
  result.table = *table;
  result.active_table = *active_table;
  result.count = *count;
  result.index = uint32_t(role);
  if (*table == *active_table) {
    const auto offset = read(kActiveTextureTable);
    if (!offset)
      return {};
    result.offset = *offset;
    result.index += *offset; // source compares low 32 bits, including next wrap
  }
  if (result.index >= *count) {
    result.source_word = kActiveTextureTable + 32;
  } else {
    const auto entries = read(uint64_t(*table) + 4);
    if (!entries || !*entries)
      return {};
    const uint64_t address = uint64_t(*entries) + uint64_t(result.index) * 28 + 24;
    if (address > UINT32_MAX - 3)
      return {};
    result.source_word = uint32_t(address);
  }
  const auto image = read(result.source_word);
  if (!image)
    return {};
  result.image = *image;
  return result;
}

template <class Read>
std::optional<uint32_t> ReadSceneTextureSelection(SceneTextureRole role, Read read) {
  const auto result = ReadSceneTextureSelectionSource(role, read);
  return result ? std::optional(result->image) : std::nullopt;
}

template <class Read>
std::optional<SceneTextureSelections> ReadSceneTextureSources(Read read) {
  const auto current = ReadSceneTextureSelectionSource(SceneTextureRole::Current, read);
  const auto next = ReadSceneTextureSelectionSource(SceneTextureRole::Next, read);
  if (!current || !next)
    return {};
  return std::array{*current, *next};
}

template <class Read>
std::optional<std::array<uint32_t, 2>> ReadSceneTexturePair(Read read) {
  const auto current = ReadSceneTextureSelection(SceneTextureRole::Current, read);
  const auto next = ReadSceneTextureSelection(SceneTextureRole::Next, read);
  if (!current || !next)
    return {};
  return std::array{*current, *next};
}
} // namespace bd::gpu::scene
