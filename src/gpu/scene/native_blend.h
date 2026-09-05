/**
 * @file    native_blend.h
 * @brief   Host blend intent, independent of engine storage and draw history.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include <plume_render_interface.h>

namespace bd::gpu::scene {
struct BlendState {
  bool alphaBlendEnable = false;
  plume::RenderBlend srcBlend = plume::RenderBlend::ONE;
  plume::RenderBlend destBlend = plume::RenderBlend::ZERO;
  plume::RenderBlendOperation blendOp = plume::RenderBlendOperation::ADD;
  plume::RenderBlend srcBlendAlpha = plume::RenderBlend::ONE;
  plume::RenderBlend destBlendAlpha = plume::RenderBlend::ZERO;
  plume::RenderBlendOperation blendOpAlpha = plume::RenderBlendOperation::ADD;
  bool operator==(const BlendState &) const = default;
};

// Copy live intent, not a retained/replayed pipeline. Raster, shaders, targets
// and multiview fields belong to their own producers and remain untouched.
template <class Pipeline>
void ApplyBlendState(const BlendState &source, Pipeline &target, bool &dirty) {
  auto set = [&](auto &dst, const auto &value) {
    if (dst != value) {
      dst = value;
      dirty = true;
    }
  };
  set(target.alphaBlendEnable, source.alphaBlendEnable);
  set(target.srcBlend, source.srcBlend);
  set(target.destBlend, source.destBlend);
  set(target.blendOp, source.blendOp);
  set(target.srcBlendAlpha, source.srcBlendAlpha);
  set(target.destBlendAlpha, source.destBlendAlpha);
  set(target.blendOpAlpha, source.blendOpAlpha);
}
} // namespace bd::gpu::scene
