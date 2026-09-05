/**
 * @file    native_raster.h
 * @brief   Native raster/depth/stencil intent, independent of engine storage.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include <cstdint>
#include <plume_render_interface.h>

namespace bd::gpu::scene {
struct RasterState {
  bool zEnable = true, zWriteEnable = true;
  plume::RenderComparisonFunction zFunc =
      plume::RenderComparisonFunction::LESS_EQUAL;
  plume::RenderCullMode cullMode = plume::RenderCullMode::NONE;
  plume::RenderFillMode fillMode = plume::RenderFillMode::SOLID;
  uint32_t colorWriteEnable = 15;
  bool stencilEnable = false, stencilTwoSided = false;
  plume::RenderComparisonFunction stencilFunc =
      plume::RenderComparisonFunction::ALWAYS;
  plume::RenderStencilOp stencilFail = plume::RenderStencilOp::KEEP;
  plume::RenderStencilOp stencilZFail = plume::RenderStencilOp::KEEP;
  plume::RenderStencilOp stencilPass = plume::RenderStencilOp::KEEP;
  uint8_t stencilRef = 0, stencilMask = 255, stencilWriteMask = 255;
  bool operator==(const RasterState &) const = default;
};

// Named-field copy keeps unrelated shader, blend, target and multiview fields
// intact. The source is live intent, never the last bound/replayed pipeline.
template <class Pipeline>
void ApplyRasterState(const RasterState &source, Pipeline &target,
                      bool &dirty) {
  auto set = [&](auto &dst, const auto &value) {
    if (dst != value) {
      dst = value;
      dirty = true;
    }
  };
  set(target.zEnable, source.zEnable);
  set(target.zWriteEnable, source.zWriteEnable);
  set(target.zFunc, source.zFunc);
  set(target.cullMode, source.cullMode);
  set(target.fillMode, source.fillMode);
  set(target.colorWriteEnable, source.colorWriteEnable);
  set(target.stencilEnable, source.stencilEnable);
  set(target.stencilTwoSided, source.stencilTwoSided);
  set(target.stencilFunc, source.stencilFunc);
  set(target.stencilFail, source.stencilFail);
  set(target.stencilZFail, source.stencilZFail);
  set(target.stencilPass, source.stencilPass);
  set(target.stencilRef, source.stencilRef);
  set(target.stencilMask, source.stencilMask);
  set(target.stencilWriteMask, source.stencilWriteMask);
}
} // namespace bd::gpu::scene
