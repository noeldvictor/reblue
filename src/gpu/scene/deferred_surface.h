/**
 * @file    deferred_surface.h
 * @brief   Native deferred surface and fur-shell submission policies.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

namespace bd::gpu::scene {
enum class DeferredCullFace { None, Front, Back };
enum class DeferredSurfaceKind { Regular, StencilPair, FurShells };

struct DeferredSurfacePlan {
  DeferredSurfaceKind kind = DeferredSurfaceKind::Regular;
  uint32_t draws = 1;
};

// A negative shell selector selects the fur setup/cleanup but submits no
// shells. The importer preserves that explicit policy, not a stale draw count.
inline DeferredSurfacePlan PlanDeferredSurface(int32_t shells, bool stencil) {
  if (shells != 0)
    return {DeferredSurfaceKind::FurShells, uint32_t(shells > 0 ? shells : 0)};
  return {stencil ? DeferredSurfaceKind::StencilPair
                  : DeferredSurfaceKind::Regular,
          stencil ? 2u : 1u};
}

// 'reverse' is object winding policy; sidedness 0/1 chooses the corresponding
// face and all other values are two-sided. Import numeric engine selectors
// separately; the native policy knows no render-state IDs or bit encodings.
inline DeferredCullFace DeferredFaces(bool reverse, uint32_t sidedness) {
  if (sidedness > 1)
    return DeferredCullFace::None;
  return (bool(sidedness) != reverse) ? DeferredCullFace::Front
                                      : DeferredCullFace::Back;
}

struct DeferredFurSlice {
  float fraction = 0;
  float extrusion = 0;
};

inline std::optional<DeferredFurSlice>
ComposeDeferredFurSlice(uint32_t shell, uint32_t count, float length) {
  if (!shell || !count || shell > count || !std::isfinite(length))
    return {};
  const float fraction = float(shell) / float(count);
  const float extrusion = fraction * length;
  if (!std::isfinite(extrusion))
    return {};
  return DeferredFurSlice{fraction, extrusion};
}

struct DeferredFoliageInputs {
  std::array<float, 4> displacement{};
  bool collision = false;
  bool stencil = false;
};
} // namespace bd::gpu::scene
