/**
 * @file    reflection.cpp
 * @brief   Reflection selection is not a retained image or an enable bit.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/scene/native_material_data.h"
#include "gpu/scene/reflection_texture_import.h"
#include <stdexcept>
#include <unordered_map>

using namespace bd::gpu::scene;
namespace {
void Require(bool value) {
  if (!value)
    throw std::runtime_error("native reflection recipe check failed");
}
}
void TestNativeReflectionRecipes() {
  Require(!ModelReflectionCallbackSupported(0));
  Require(!ModelReflectionCallbackSupported(0x8221E618));
  Require(!ModelReflectionCallbackSupported(0x82454C08));
  Require(ModelReflectionCallbackSupported(0x820EFA50));
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

  std::unordered_map<uint64_t, uint32_t> memory{
      {kReflectionPassDefault, 11}, {kReflectionTableState + 4, 1000},
      {kReflectionTableState, 2}, {kReflectionTableState + 32, 22},
      {1000, 4}, {1004, 2000}, {2000 + 3 * 28 + 24, 33}};
  auto read = [&](uint64_t address) -> std::optional<uint32_t> {
    const auto it = memory.find(address);
    return it == memory.end() ? std::nullopt : std::optional(it->second);
  };
  auto inputs = ReadReflectionTextureImport(read);
  Require(bool(inputs));
  auto select = [&](NativeReflectionRecipe recipe) {
    return SelectReflectionTextureImport(*inputs, recipe, read);
  };
  Require(select({Source::PassDefault, 0, false}) == 11);
  Require(select({Source::Table, 1, true}) == 33);
  Require(select({Source::Table, 1, false}) == 33); // disabled still retains selection
  // Capture fixes the selector at draw time; registry validation occurs after
  // the draw lock is released and must not re-read a subsequent table value.
  const auto captured_selection = select({Source::Table, 1, false});
  memory[2000 + 3 * 28 + 24] = 66;
  Require(captured_selection == 33);
  Require(select({Source::Table, 1, false}) == 66);
  memory[2000 + 3 * 28 + 24] = 33;
  Require(select({Source::Table, 2, true}) == 22); // out of bounds is fallback
  Require(!select({Source::Unknown, 0, true}));
  Require(!select({Source::Table, 0, true})); // unreadable row is not a null texture
  memory[kReflectionPassDefault] = 44;
  auto next_inputs = ReadReflectionTextureImport(read);
  Require(next_inputs && next_inputs->pass_default == 44);
  Require(select({Source::PassDefault, 0, true}) == 11); // prior packet is unchanged
  memory[kReflectionTableState + 4] = 0;
  inputs = ReadReflectionTextureImport(read);
  Require(inputs && select({Source::Table, 1, true}) == 0);
  memory.erase(kReflectionPassDefault);
  Require(!ReadReflectionTextureImport(read));
  inputs = ReflectionTextureImport{11, UINT32_MAX, 4, 2000, 22, true};
  memory[2024] = 55;
  Require(select({Source::Table, 1, true}) == 55); // unsigned index wrap matches source
  inputs->table_entries = UINT32_MAX - 4;
  Require(!select({Source::Table, 1, true})); // address overflow refuses before reading
}
