/**
 * @file    scene_textures.cpp
 * @brief   Current/next scene selection is distinct from model table selection.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/scene/scene_texture_import.h"
#include <stdexcept>
#include <unordered_map>
using namespace bd::gpu::scene;
namespace {
void Require(bool value) {
  if (!value)
    throw std::runtime_error("scene texture selection check failed");
}
}
int main() {
  std::unordered_map<uint64_t, uint32_t> words{
      {kSceneTextureTable, 1000}, {kActiveTextureTable + 4, 1000},
      {kActiveTextureTable, 2}, {kActiveTextureTable + 32, 99},
      {1000, 4}, {1004, 2000},
      {2024, 10}, {2052, 11}, {2080, 12}, {2108, 13}};
  auto read = [&](uint64_t address) -> std::optional<uint32_t> {
    const auto it = words.find(address);
    return it == words.end() ? std::nullopt : std::optional(it->second);
  };
  auto pair = [&] { return ReadSceneTexturePair(read); };
  Require(pair() == std::array<uint32_t, 2>{12, 13});
  const auto original_sources = ReadSceneTextureSources(read);
  Require(original_sources.has_value());
  words[kActiveTextureTable] = 0;
  words[2024] = 12;
  words[2052] = 13;
  Require(pair() == std::array<uint32_t, 2>{12, 13}); // identical images, different rows
  Require(original_sources != ReadSceneTextureSources(read));
  words[kActiveTextureTable] = 2;
  words[1004] = 4000;
  words[4080] = 12;
  words[4108] = 13;
  Require(pair() == std::array<uint32_t, 2>{12, 13}); // relocated row array also differs
  Require(original_sources != ReadSceneTextureSources(read));
  words[1004] = 2000;
  words[2024] = 10;
  words[2052] = 11;
  Require(original_sources == ReadSceneTextureSources(read));
  words[kActiveTextureTable + 4] = 3000;
  Require(pair() == std::array<uint32_t, 2>{10, 11}); // scene table, not active table
  words[kActiveTextureTable + 4] = 1000;
  words[kActiveTextureTable] = 3;
  Require(pair() == std::array<uint32_t, 2>{13, 99});
  words[kActiveTextureTable] = 4;
  Require(pair() == std::array<uint32_t, 2>{99, 99});
  words[kActiveTextureTable] = UINT32_MAX;
  Require(pair() == std::array<uint32_t, 2>{99, 10}); // next wraps independently
  words[kActiveTextureTable] = 0;
  const auto snapshot = pair();
  words[2024] = 20;
  words[2052] = 0;
  Require(snapshot == std::array<uint32_t, 2>{10, 11});
  Require(pair() == std::array<uint32_t, 2>{20, 0}); // null is a real no-op selection
  words.erase(2052);
  Require(!pair()); // unreadable next must not publish current partially
  Require(ReadSceneTextureSelection(SceneTextureRole::Current, read) == 20);
  words[1004] = 0;
  Require(!pair());
  words[1004] = UINT32_MAX - 4;
  Require(!pair()); // checked address overflow before reading
  words[1004] = 2000;
  words[1000] = 0;
  Require(pair() == std::array<uint32_t, 2>{99, 99}); // empty table uses fallback
  words.erase(kActiveTextureTable + 32);
  Require(!pair());
  words[kSceneTextureTable] = 0;
  words.erase(kActiveTextureTable + 4);
  Require(pair() == std::array<uint32_t, 2>{0, 0}); // absent table does not read fallback
  words.erase(kSceneTextureTable);
  Require(!pair());
}
