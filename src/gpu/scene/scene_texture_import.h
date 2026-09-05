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

// Unlike model reflection selection, these images always come from the scene
// table. The active offset applies only when that table is the active table.
// A null selection is a legacy no-op at publication, never an unbind.
template <class Read>
std::optional<uint32_t> ReadSceneTextureSelection(SceneTextureRole role, Read read) {
  const auto table = read(kSceneTextureTable);
  if (!table)
    return {};
  if (!*table)
    return 0;
  const auto active_table = read(kActiveTextureTable + 4);
  const auto count = read(uint64_t(*table));
  if (!active_table || !count)
    return {};
  uint32_t index = uint32_t(role);
  if (*table == *active_table) {
    const auto offset = read(kActiveTextureTable);
    if (!offset)
      return {};
    index += *offset; // source compares the low 32 bits, including next wrap
  }
  if (index >= *count)
    return read(kActiveTextureTable + 32);
  const auto entries = read(uint64_t(*table) + 4);
  if (!entries || !*entries)
    return {};
  const uint64_t address = uint64_t(*entries) + uint64_t(index) * 28 + 24;
  if (address > UINT32_MAX - 3)
    return {};
  return read(address);
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
