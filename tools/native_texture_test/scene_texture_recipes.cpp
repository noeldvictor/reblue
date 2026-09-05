/**
 * @file    scene_texture_recipes.cpp
 * @brief   Semantic scene input ownership, overwrite order and live composition.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/scene/scene_texture_import.h"
#include <stdexcept>
using namespace bd::gpu::scene;
namespace {
void Require(bool value) {
  if (!value)
    throw std::runtime_error("scene texture recipe check failed");
}
}
int main() {
  constexpr auto current = SceneTextureRole::Current;
  constexpr auto next = SceneTextureRole::Next;
  constexpr auto images = SceneTextureProducer::Images;
  SceneTextureRecipe recipe;
  Require(recipe.roles == 0 && recipe.SlotMask() == 0);
  recipe.Publish(current, images, false);
  Require(recipe == SceneTextureRecipe{}); // null never invents ownership
  recipe.Publish(current, images, true);
  const auto first_draw = recipe;
  Require(recipe.Uses(current) && !recipe.Uses(next));
  Require(recipe.UsesSlot(5) && !recipe.UsesSlot(10) && !recipe.UsesSlot(32));
  recipe.Publish(next, images, true);
  Require(recipe.SlotMask() == ((1u << 5) | (1u << 10)));
  recipe.Publish(current, images, false);
  recipe.OverrideSlot(4);
  recipe.OverrideSlot(31);
  Require(recipe.roles == 3); // null and unrelated writes leave both unchanged
  recipe.Publish(current, images, true);
  const auto both = recipe;
  recipe.OverrideSlot(5); // same image via an ordinary write still erases the role
  Require(!recipe.Uses(current) && recipe.Uses(next));
  Require(recipe.producer == images);
  recipe.OverrideSlot(10);
  Require(recipe == SceneTextureRecipe{});
  Require(first_draw.Uses(current) && !first_draw.Uses(next));
  Require(both.roles == 3); // later sub-draw overrides do not mutate earlier recipes
  SceneTextureRecipe another_node;
  Require(another_node.roles == 0); // no preceding-node inference

  struct Binding {
    uint32_t image = 0;
    bool dynamic = false;
    bool operator==(const Binding &) const = default;
  };
  auto valid = [](const Binding &b) { return b.image != 0; };
  std::array<Binding, 2> inputs{{{10, false}, {20, true}}};
  const auto first_frame = ComposeSceneTextureBindings(both, inputs, valid);
  Require(first_frame && *first_frame == inputs);
  inputs = {{{30, true}, {40, false}}};
  const auto second_frame = ComposeSceneTextureBindings(both, inputs, valid);
  Require(second_frame && *second_frame == inputs);
  Require((*first_frame)[0].image == 10 && (*first_frame)[1].image == 20);
  inputs[1] = {};
  Require(!ComposeSceneTextureBindings(both, inputs, valid)); // atomic refusal
  const auto current_only = ComposeSceneTextureBindings(first_draw, inputs, valid);
  Require(current_only && (*current_only)[0].image == 30 && (*current_only)[1].image == 0);
  inputs[0] = {};
  Require(!ComposeSceneTextureBindings(first_draw, inputs, valid)); // no null inheritance
  Require(ComposeSceneTextureBindings(SceneTextureRecipe{}, inputs, valid).has_value());

  Require(ImportSceneTextureProducer(0x8221E618) == images);
  Require(ImportSceneTextureProducer(0x82454C08) == SceneTextureProducer::ImagesAndBlend);
  Require(ImportSceneTextureProducer(0) == SceneTextureProducer::None);
  Require(ImportSceneTextureProducer(0x8221E61C) == SceneTextureProducer::None);
}
