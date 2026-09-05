/**
 * @file    reflection.cpp
 * @brief   Reflection selection is not a retained image or an enable bit.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/scene/native_material_data.h"
#include <stdexcept>

using namespace bd::gpu::scene;
namespace {
void Require(bool value) {
  if (!value)
    throw std::runtime_error("native reflection recipe check failed");
}
}
void TestNativeReflectionRecipes() {
  std::vector<uint16_t> words;
  std::vector<NativeReflectionRecipe> expected;
  auto draw = [&](NativeReflectionRecipe recipe) {
    words.insert(words.end(), {0x1000, 1, uint16_t(expected.size() * 3)});
    expected.push_back(recipe);
  };
  using Source = ReflectionTextureSource;
  draw({Source::PassDefault, 0, false});
  words.push_back(0x06ff);
  draw({Source::PassDefault, 0, false}); // disable is not an unbind
  words.push_back(0x0603);
  draw({Source::Table, 3, true});
  words.push_back(0x06ff);
  draw({Source::Table, 3, false});
  words.push_back(0x06fe);
  draw({Source::PassDefault, 0, true});
  words.push_back(0x6509);
  draw({Source::Unknown, 0, true});
  words.push_back(0x06fe);
  draw({Source::Unknown, 0, true}); // repeated selection does not rebind
  words.push_back(0x06ff);
  draw({Source::Unknown, 0, false});
  words.push_back(0x0600);
  draw({Source::Table, 0, true}); // zero is a real table index
  words.push_back(0x6409);
  draw({Source::Table, 0, true}); // another slot does not affect reflection
  words.push_back(0x06fd);
  draw({Source::Table, 253, true});
  words.push_back(0xff);
  std::vector<NativeMaterialRange> ranges;
  Require(DecodeMeshMaterials(words, ranges));
  Require(ranges.size() == expected.size());
  for (size_t i = 0; i < ranges.size(); ++i)
    Require(ranges[i].reflection == expected[i]);
  for (size_t n = 0; n < words.size(); ++n) {
    Require(!DecodeMeshMaterials(std::span(words).first(n), ranges));
    Require(ranges.size() == expected.size());
    for (size_t i = 0; i < ranges.size(); ++i)
      Require(ranges[i].reflection == expected[i]);
  }
  const uint16_t next_model[]{0x1000, 1, 0, 0xff};
  Require(DecodeMeshMaterials(next_model, ranges));
  Require(ranges.size() == 1 &&
          ranges[0].reflection == NativeReflectionRecipe{});
}
