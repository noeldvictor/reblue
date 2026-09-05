/**
 * @file    deferred_depth.h
 * @brief   Native deferred depth policy from object bounds and live transforms.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>

namespace bd::gpu::scene {
using DeferredMatrix = std::array<float, 16>; // row-major, translation in row 3

struct DeferredDepthRecipe {
  enum class Kind { BoundsFarExtent, Fixed };
  Kind kind = Kind::BoundsFarExtent;
  std::array<float, 3> centre{};
  float radius = 0;
  float fixed_depth = 0;
};

// Object bounds are native values. Neither this recipe nor the calculation
// contains a guest address, register number, previous-frame depth or GPU ABI.
inline std::optional<float>
EvaluateDeferredDepth(const DeferredDepthRecipe &recipe,
                      const DeferredMatrix &world, const DeferredMatrix &view) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
  if (recipe.kind == DeferredDepthRecipe::Kind::Fixed)
    return std::isfinite(recipe.fixed_depth) ? std::optional(recipe.fixed_depth)
                                             : std::nullopt;
  if (recipe.kind != DeferredDepthRecipe::Kind::BoundsFarExtent ||
      !std::isfinite(recipe.radius))
    return {};
  for (float value : recipe.centre)
    if (!std::isfinite(value))
      return {};
  for (std::size_t i = 0; i < 16; ++i)
    if (!std::isfinite(world[i]) || !std::isfinite(view[i]))
      return {};
  // Only the Z column of world*view is needed. Paired dot sums and the
  // Z,Y,X point composition preserve the original producer's FP ordering.
  std::array<float, 4> z;
  for (std::size_t row = 0; row < 4; ++row) {
    const auto *w = world.data() + row * 4;
    z[row] =
        (w[0] * view[2] + w[1] * view[6]) + (w[2] * view[10] + w[3] * view[14]);
  }
  float point_z = recipe.centre[2] * z[2] + z[3];
  point_z = recipe.centre[1] * z[1] + point_z;
  point_z = recipe.centre[0] * z[0] + point_z;
  const float depth = -(recipe.radius + point_z);
  return std::isfinite(depth) ? std::optional(depth) : std::nullopt;
}
} // namespace bd::gpu::scene
