/**
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause License
 */
#pragma once
#include <cstdint>
#include <unordered_map>

namespace bd::gpu::scene {
struct SceneImportEpoch {
  uint64_t textures = 0, geometry = 0;
  bool operator==(const SceneImportEpoch &) const = default;
};
inline bool HasDirectRecipe(bool has_draws, bool volatile_material) {
  return has_draws || volatile_material;
}
// A node's direct draws and deferred entries are a compound recipe. Retiring
// only its draws makes the remaining list appear to be the entire node.
template <class Draws, class Lists, class Retire>
size_t PruneNodeRecipes(Draws &draws, Lists &lists, uint32_t frame,
                       uint32_t keep_frames, Retire retire) {
  return std::erase_if(draws, [&](const auto &item) {
    if (uint32_t(frame - item.second.used_frame) <= keep_frames)
      return false;
    lists.erase(item.first);
    retire(item.first);
    return true;
  });
}
} // namespace bd::gpu::scene
