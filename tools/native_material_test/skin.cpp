/**
 * @file    skin.cpp
 * @brief   Joint identity must survive equal poses and per-draw binding
 * changes.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/scene/native_material_data.h"
#include <stdexcept>

using namespace bd::gpu::scene;
namespace {
void Require(bool value) {
  if (!value)
    throw std::runtime_error("native skin binding test failed");
}
} // namespace
void TestNativeSkinBindings() {
  const uint16_t commands[]{0x1000, 1,      0,      0x0202, 2,      1, 0x1000,
                            1,      0,      0x0201, 0,      0x1000, 1, 0,
                            0x0200, 0x1000, 1,      0,      0xff};
  std::vector<NativeMaterialRange> draws;
  Require(DecodeMeshMaterials(commands, draws) && draws.size() == 4);
  Require(!draws[0].skin && draws[1].skin->count == 2);
  Require(draws[1].skin->joints[0] == 2 && draws[1].skin->joints[1] == 1);
  Require(draws[2].skin->count == 1 && draws[2].skin->joints[0] == 0);
  Require(draws[3].skin && draws[3].skin->count == 0);
  using Matrix = std::array<float, 16>;
  std::array<Matrix, 3> pose{}; // every matrix equal, identities still differ
  std::array<Matrix, 2> output{};
  auto load = [&](uint16_t joint, Matrix &matrix) {
    if (joint >= pose.size())
      return false;
    matrix = pose[joint];
    return true;
  };
  Require(GatherNativeSkinPalette(*draws[1].skin, load, std::span(output)));
  pose[0][12] = 100;
  pose[1][12] = 200;
  pose[2][12] = 300;
  Require(GatherNativeSkinPalette(*draws[1].skin, load, std::span(output)));
  Require(output[0][12] == 300 && output[1][12] == 200);
  Require(GatherNativeSkinPalette(*draws[2].skin, load, std::span(output)));
  Require(output[0][12] == 100 && output[1][12] == 200);
  auto invalid = *draws[1].skin;
  invalid.joints[1] = 99;
  const auto before = output;
  Require(!GatherNativeSkinPalette(invalid, load, std::span(output)));
  Require(output == before);
  Require(!GatherNativeSkinPalette(*draws[1].skin, load,
                                   std::span(output).first(1)));
  Require(output == before);
  invalid.count = 50;
  Require(!GatherNativeSkinPalette(invalid, load, std::span(output)));
  Require(output == before);
  const std::array<uint16_t, 50> too_many{};
  Require(!DecodeNativeSkinBinding(too_many));
  const std::array<uint16_t, 49> full{};
  Require(DecodeNativeSkinBinding(full)->count == 49);
  for (size_t n = 0; n < std::size(commands); ++n) {
    Require(!DecodeMeshMaterials(std::span(commands).first(n), draws));
    Require(draws.size() == 4 && draws[1].skin->joints[0] == 2);
  }
}
