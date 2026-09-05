/**
 * @file    scene_texture_recipe.h
 * @brief   Address-free scene-image roles and current-input composition.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include <array>
#include <cstdint>
#include <optional>

namespace bd::gpu::scene {
enum class SceneTextureRole : uint8_t { Current, Next };
enum class SceneTextureProducer : uint8_t { None, Images, ImagesAndBlend };
constexpr std::array<uint32_t, 2> kSceneTextureSlots{5, 10}; // shader ABI only

struct SceneTextureRecipe {
  SceneTextureProducer producer = SceneTextureProducer::None;
  uint8_t roles = 0;
  bool operator==(const SceneTextureRecipe &) const = default;
  bool Uses(SceneTextureRole role) const { return roles & (1u << uint32_t(role)); }
  bool UsesSlot(uint32_t slot) const {
    for (uint32_t i = 0; i < kSceneTextureSlots.size(); ++i)
      if (slot == kSceneTextureSlots[i] && Uses(SceneTextureRole(i)))
        return true;
    return false;
  }
  uint32_t SlotMask() const {
    uint32_t result = 0;
    for (uint32_t i = 0; i < kSceneTextureSlots.size(); ++i)
      if (Uses(SceneTextureRole(i)))
        result |= 1u << kSceneTextureSlots[i];
    return result;
  }
  // Ordinary writes replace semantic ownership even when the image is equal.
  void OverrideSlot(uint32_t slot) {
    for (uint32_t i = 0; i < kSceneTextureSlots.size(); ++i)
      if (slot == kSceneTextureSlots[i])
        roles &= ~(1u << i);
    if (!roles)
      producer = SceneTextureProducer::None;
  }
  void Publish(SceneTextureRole role, SceneTextureProducer source, bool nonnull) {
    if (!nonnull)
      return; // a null source does not unbind or erase the preceding producer
    producer = source;
    roles |= 1u << uint32_t(role);
  }
};

// Preflight requested roles together; never publish one input before discovering
// that the next is unavailable. Null-as-inheritance requires a separate recipe.
template <class Binding, class Valid>
std::optional<std::array<Binding, 2>> ComposeSceneTextureBindings(
    const SceneTextureRecipe &recipe, const std::array<Binding, 2> &inputs, Valid valid) {
  std::array<Binding, 2> result{};
  for (uint32_t i = 0; i < result.size(); ++i) {
    if (!recipe.Uses(SceneTextureRole(i)))
      continue;
    if (!valid(inputs[i]))
      return {};
    result[i] = inputs[i];
  }
  return result;
}
} // namespace bd::gpu::scene
