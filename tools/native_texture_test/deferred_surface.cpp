/**
 * @file    deferred_surface.cpp
 * @brief   Deferred multi-pass policies without a GPU or game memory.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/scene/deferred_surface.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <limits>

using namespace bd::gpu::scene;
int main() {
  for (int shells = -128; shells <= 127; ++shells)
    for (bool stencil : {false, true}) {
      const auto plan = PlanDeferredSurface(shells, stencil);
      assert(plan.draws ==
             uint32_t(shells ? (shells > 0 ? shells : 0) : (stencil ? 2 : 1)));
      assert(plan.kind == (shells ? DeferredSurfaceKind::FurShells
                                  : (stencil ? DeferredSurfaceKind::StencilPair
                                             : DeferredSurfaceKind::Regular)));
    }
  assert(DeferredFaces(false, 0) == DeferredCullFace::Back);
  assert(DeferredFaces(false, 1) == DeferredCullFace::Front);
  assert(DeferredFaces(true, 0) == DeferredCullFace::Front);
  assert(DeferredFaces(true, 1) == DeferredCullFace::Back);
  for (uint32_t side = 2; side < 256; ++side)
    for (bool reverse : {false, true})
      assert(DeferredFaces(reverse, side) == DeferredCullFace::None);
  for (uint32_t count = 1; count <= 127; ++count) {
    float previous = 0;
    for (uint32_t shell = 1; shell <= count; ++shell) {
      const auto slice = ComposeDeferredFurSlice(shell, count, 3);
      assert(slice && slice->fraction > previous && slice->fraction <= 1);
      assert(slice->extrusion == slice->fraction * 3);
      previous = slice->fraction;
    }
    assert(previous == 1);
    assert(ComposeDeferredFurSlice(count, count, -2)->extrusion == -2);
  }
  assert(!ComposeDeferredFurSlice(0, 3, 1));
  assert(!ComposeDeferredFurSlice(1, 0, 1));
  assert(!ComposeDeferredFurSlice(4, 3, 1));
  assert(
      !ComposeDeferredFurSlice(1, 3, std::numeric_limits<float>::quiet_NaN()));
  assert(
      !ComposeDeferredFurSlice(1, 3, std::numeric_limits<float>::infinity()));
}
