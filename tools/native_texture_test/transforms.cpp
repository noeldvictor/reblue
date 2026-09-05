/**
 * @file    transforms.cpp
 * @brief   Native transform composition without engine memory, SDK or a GPU.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/scene/native_transform.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <bit>
#include <cassert>
#include <cstdint>
#include <limits>

using namespace bd::gpu::scene;
constexpr RenderMatrix identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
int main() {
  RenderTransformInputs inputs{identity, identity, identity};
  assert(ComposeRenderTransforms(inputs)->view_projection == identity);
  for (size_t i = 0; i < 16; ++i)
    inputs.world[i] = float(i);
  const auto transposed = TransposeRenderMatrix(inputs.world);
  assert(TransposeRenderMatrix(transposed) == inputs.world);
  for (size_t row = 0; row < 4; ++row)
    for (size_t column = 0; column < 4; ++column)
      assert(transposed[row * 4 + column] == float(column * 4 + row));
  // World must not accidentally enter the pass view-projection product.
  assert(ComposeRenderTransforms(inputs)->view_projection == identity);
  inputs.view[12] = -7;
  inputs.view[13] = 3;
  inputs.view[14] = -2;
  inputs.projection[0] = 2;
  inputs.projection[5] = 4;
  inputs.projection[10] = 5;
  auto result = ComposeRenderTransforms(inputs);
  assert(result && result->inputs.world == inputs.world);
  assert(result->view_projection[12] == -14);
  assert(result->view_projection[13] == 12);
  assert(result->view_projection[14] == -10);
  // Asymmetric perspective terms and arbitrary affine/projective matrices.
  uint32_t random = 0x24519682;
  auto sample = [&] {
    random = random * 1664525u + 1013904223u;
    return float(int32_t(random >> 16) - 32768) / 4096.0f;
  };
  for (unsigned trial = 0; trial < 2000; ++trial) {
    for (auto *matrix : {&inputs.world, &inputs.view, &inputs.projection})
      for (auto &value : *matrix)
        value = sample();
    result = ComposeRenderTransforms(inputs);
    assert(result && result->inputs.view == inputs.view &&
           result->inputs.projection == inputs.projection);
    for (size_t row = 0; row < 4; ++row)
      for (size_t column = 0; column < 4; ++column) {
        double reference = 0;
        for (size_t k = 0; k < 4; ++k)
          reference += double(inputs.view[row * 4 + k]) *
                       inputs.projection[k * 4 + column];
        assert(std::abs(result->view_projection[row * 4 + column] - reference) <
               0.00002 * (1 + std::abs(reference)));
      }
  }
  inputs = {identity, identity, identity};
  for (auto *matrix : {&inputs.world, &inputs.view, &inputs.projection})
    for (size_t i = 0; i < 16; ++i) {
      const float saved = (*matrix)[i];
      for (float invalid : {std::numeric_limits<float>::infinity(),
                            -std::numeric_limits<float>::infinity(),
                            std::numeric_limits<float>::quiet_NaN()}) {
        (*matrix)[i] = invalid;
        assert(!ComposeRenderTransforms(inputs));
      }
      (*matrix)[i] = saved;
    }
  inputs.view[0] = std::numeric_limits<float>::max();
  inputs.projection[0] = 2;
  assert(!ComposeRenderTransforms(inputs));
  inputs = {};
  assert(ComposeRenderTransforms(inputs)); // zero is not a singularity error
  inputs.world[1] = std::bit_cast<float>(0x7fc12345u);
  inputs.world[4] = -0.0f;
  const auto exceptional = ComposeRenderTransformValues(inputs);
  const auto exceptional_transpose =
      TransposeRenderMatrix(exceptional.inputs.world);
  assert(std::bit_cast<uint32_t>(exceptional_transpose[4]) == 0x7fc12345u);
  assert(std::signbit(exceptional_transpose[1]));
  assert(!ComposeRenderTransforms(inputs));
  inputs = {identity, identity, identity};
  inputs.view[0] = std::numeric_limits<float>::infinity();
  const auto infinite = ComposeRenderTransformValues(inputs);
  assert(std::isinf(infinite.view_projection[0]));
  assert(std::isnan(infinite.view_projection[1])); // infinity * zero
  assert(!ComposeRenderTransforms(inputs));
}
