/**
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause License
 */
#include "gpu/scene/native_texture_binding.h"
#include "gpu/scene/fenced_asset_cache.h"
#include "gpu/scene/scene_recipe_residency.h"
#include "gpu/sampler_key.h"
#include <cstdlib>
#include <iostream>

using namespace bd::gpu;
using namespace bd::gpu::scene;
using D = plume::RenderTextureViewDimension;

static void Check(bool ok, const char *why) {
  if (!ok) {
    std::cerr << why << '\n';
    std::exit(1);
  }
}
static NativeTextureGpuHandle Image(uint64_t id, uint32_t slot, D dim) {
  auto asset = std::make_shared<NativeTextureAsset>();
  asset->id = id;
  auto gpu = std::make_shared<NativeTextureGpu>();
  gpu->asset = asset;
  gpu->descriptor = slot;
  gpu->dimension = dim;
  return gpu;
}

int main() {
  const NativeTextureIndices nulls{1, 2, 3};
  auto two = Image(10, 100, D::TEXTURE_2D_ARRAY);
  auto cube = Image(20, 200, D::TEXTURE_CUBE);
  auto volume = Image(30, 300, D::TEXTURE_3D);
  NativeTextureBinding atlas{two, {}, cube};
  auto slots = TextureIndices(atlas, nulls);
  Check(slots.image_2d == 100 && slots.image_3d == 2 && slots.image_cube == 200,
        "native atlas and explicit cube companion");
  slots = TextureIndices({volume, two, {}}, nulls);
  Check(slots.image_2d == 100 && slots.image_3d == 300 && slots.image_cube == 3,
        "native volume and explicit slice companion");
  slots = TextureIndices({cube, {}, {}}, nulls);
  Check(slots.image_2d == 1 && slots.image_3d == 2 && slots.image_cube == 200,
        "cube must not populate other dimensions");
  slots = TextureIndices({}, nulls);
  Check(slots.image_2d == 1 && slots.image_3d == 2 && slots.image_cube == 3,
        "unbound slot resets every dimension");
  auto copied = atlas;
  two.reset();
  cube.reset();
  Check(copied == atlas && copied.primary->asset->id == 10 &&
            copied.cube->asset->id == 20,
        "material owns both assets after importer owners disappear");
  Check(copied != NativeTextureBinding{atlas.primary, {}, {}},
        "companion changes invalidate binding identity");

  FencedAssetCache<NativeTextureGpu> residency(64, 2);
  auto imported = residency.Acquire(42, 32, [] { return Image(42, 400, D::TEXTURE_2D_ARRAY); });
  NativeTextureBinding material{imported, {}, {}};
  imported.reset();
  unsigned retired = 0;
  auto retire = [&](const NativeTextureGpu &gpu) {
    Check(gpu.descriptor == 400, "retire the material's exact descriptor");
    ++retired;
  };
  residency.MarkUnused(0);
  residency.AfterFence(0, retire);
  Check(!retired && material.primary->asset->id == 42,
        "material pins image after the importer is gone");
  material = {}; // template invalidation/pruning drops the native owner
  residency.AfterFence(0, retire);
  Check(!retired, "dropping a material does not retire an unmarked image");
  residency.MarkUnused(1);
  residency.AfterFence(0, retire);
  Check(!retired, "material release cannot use an unrelated fence");
  residency.AfterFence(1, retire);
  Check(retired == 1 && residency.Stats().resident == 0,
        "material release retires exactly once after its marked fence");

  struct Recipe { uint32_t used_frame; NativeTextureBinding binding; };
  Check(HasDirectRecipe(false, true) && HasDirectRecipe(true, false) &&
            !HasDirectRecipe(false, false),
        "a volatile direct recipe cannot be classified as list-only");
  Check(SceneImportEpoch{1, 2} == SceneImportEpoch{1, 2} &&
            SceneImportEpoch{1, 2} != SceneImportEpoch{2, 2} &&
            SceneImportEpoch{1, 2} != SceneImportEpoch{1, 3},
        "texture or geometry changes expire the imported recipe");
  std::unordered_map<uint64_t, Recipe> draws;
  std::unordered_map<uint64_t, int> lists;
  draws.emplace(1, Recipe{100, atlas});
  draws.emplace(2, Recipe{400, atlas});
  lists.emplace(1, 1);
  lists.emplace(2, 1);
  lists.emplace(3, 1); // genuinely list-only node
  uint64_t retired_key = 0;
  auto forget = [&](uint64_t id) { retired_key = id; };
  Check(PruneNodeRecipes(draws, lists, 400, 300, forget) == 0,
        "recipe is retained at the age boundary");
  Check(PruneNodeRecipes(draws, lists, 401, 300, forget) == 1 && retired_key == 1 &&
            !draws.contains(1) && !lists.contains(1) && draws.contains(2) &&
            lists.contains(2) && lists.contains(3),
        "retiring draws must also retire their deferred list, not unrelated lists");
  draws.at(2).used_frame = 700; // lookup of even an empty/volatile recipe touches it
  Check(PruneNodeRecipes(draws, lists, 701, 300, forget) == 0,
        "a visited recipe does not lose its native owners");
  draws.at(2).used_frame = UINT32_MAX - 10;
  Check(PruneNodeRecipes(draws, lists, 10, 30, forget) == 0 &&
            PruneNodeRecipes(draws, lists, 30, 30, forget) == 1,
        "frame-age pruning handles unsigned frame wrap");

  const plume::RenderSamplerDesc base;
  const SamplerKey key(base);
  auto distinct = [&](const plume::RenderSamplerDesc &d) {
    Check(SamplerKey(d) != key, "sampler state omitted from identity");
  };
  auto changed = base; changed.mipLODBias = 1; distinct(changed);
  changed = base; changed.minLOD = 1; distinct(changed);
  changed = base; changed.maxLOD = 1; distinct(changed);
  changed = base; changed.anisotropyEnabled = true; distinct(changed);
  changed = base; changed.maxAnisotropy = 1; distinct(changed);
  changed = base; changed.comparisonEnabled = true; distinct(changed);
  changed = base; changed.comparisonFunc = plume::RenderComparisonFunction::LESS; distinct(changed);
  changed = base; changed.addressU = plume::RenderTextureAddressMode::CLAMP; distinct(changed);
  changed = base; changed.addressV = plume::RenderTextureAddressMode::CLAMP; distinct(changed);
  changed = base; changed.addressW = plume::RenderTextureAddressMode::CLAMP; distinct(changed);
  changed = base; changed.minFilter = plume::RenderFilter::NEAREST; distinct(changed);
  changed = base; changed.magFilter = plume::RenderFilter::NEAREST; distinct(changed);
  changed = base; changed.mipmapMode = plume::RenderMipmapMode::NEAREST; distinct(changed);
  changed = base; changed.borderColor = plume::RenderBorderColor::OPAQUE_WHITE; distinct(changed);
  changed = base; changed.shaderVisibility = plume::RenderShaderVisibility::PIXEL; distinct(changed);
  changed = base; changed.mipLODBias = -0.0f;
  Check(SamplerKey(changed) == key && SamplerKeyHash{}(SamplerKey(changed)) == SamplerKeyHash{}(key),
        "signed zero uses the same sampler");
  std::cout << "native texture bindings and complete sampler identity passed\n";
}
