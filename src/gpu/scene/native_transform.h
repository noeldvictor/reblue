/**
 * @file    native_transform.h
 * @brief   Native object/pass transforms, independent of engine memory and
 * shaders.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include <array>
#include <cmath>
#include <optional>

namespace bd::gpu::scene {
using RenderMatrix = std::array<float, 16>; // row-major, row-vector convention
struct RenderTransformInputs {
  RenderMatrix world{}, view{}, projection{};
};
struct RenderTransforms {
  RenderTransformInputs inputs;
  RenderMatrix view_projection{};
};

inline RenderMatrix TransposeRenderMatrix(const RenderMatrix &matrix) {
  RenderMatrix result;
  for (size_t row = 0; row < 4; ++row)
    for (size_t column = 0; column < 4; ++column)
      result[row * 4 + column] = matrix[column * 4 + row];
  return result;
}

// Arithmetic layer also preserves IEEE exceptional values from transitional
// engine imports. Native assets should use the checked entry point below.
inline RenderMatrix MultiplyRenderMatrices(const RenderMatrix &left,
                                          const RenderMatrix &right) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
  RenderMatrix result;
  // Explicit pairwise sums preserve the established CPU transform convention.
  // The native output is not a register block; packing belongs to a backend.
  for (size_t row = 0; row < 4; ++row)
    for (size_t column = 0; column < 4; ++column) {
      const auto *v = left.data() + row * 4;
      const auto *p = right.data() + column;
      const float value =
          (v[0] * p[0] + v[1] * p[4]) + (v[2] * p[8] + v[3] * p[12]);
      result[row * 4 + column] = value;
    }
  return result;
}

inline RenderTransforms
ComposeRenderTransformValues(const RenderTransformInputs &inputs) {
  return {inputs, MultiplyRenderMatrices(inputs.view, inputs.projection)};
}

inline std::optional<RenderTransforms>
ComposeRenderTransforms(const RenderTransformInputs &inputs) {
  for (const auto *matrix : {&inputs.world, &inputs.view, &inputs.projection})
    for (float value : *matrix)
      if (!std::isfinite(value))
        return {};
  auto result = ComposeRenderTransformValues(inputs);
  for (float value : result.view_projection)
    if (!std::isfinite(value))
      return {};
  return result;
}
} // namespace bd::gpu::scene
