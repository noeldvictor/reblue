/**
 * @file    pass_bindings.cpp
 * @brief   Native attachment-driven pipeline and viewport state.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/pass_bindings.h"
#include "gpu/device.h"
#include "gpu/foveation.h"

namespace bd::gpu {
void BindColorAttachment(GuestTexture *surface) {
  auto &s = state();
  if (s.render_target != surface)
    s.draw_framebuffer_bound = false;
  // Do not flush here: this boundary also runs without an open command list.
  // BindDrawFramebuffer emits the queue before replacing the outgoing FB.
  Video::SetDirtyValue(s.dirtyStates.renderTargetAndDepthStencil,
                      s.render_target, surface);
  Video::SetDirtyValue(s.dirtyStates.pipelineState,
                      s.pipelineState.renderTargetFormat,
                      surface ? surface->format : plume::RenderFormat::UNKNOWN);
  Video::SetDirtyValue(s.dirtyStates.pipelineState, s.pipelineState.sampleCount,
                      surface ? surface->sampleCount
                              : plume::RenderSampleCounts(plume::RenderSampleCount::COUNT_1));
  Video::SetDirtyValue(s.dirtyStates.pipelineState, s.pipelineState.multiview,
                      surface ? surface->layers > 1
                              : (s.depth_stencil && s.depth_stencil->layers > 1));
  Video::SetDirtyValue(s.dirtyStates.pipelineState,
                      s.pipelineState.fragmentDensityMap,
                      surface && FoveationWanted(surface->width, surface->height,
                                                surface->layers));
  Video::SetDefaultViewport(nullptr, surface);
}

void BindDepthAttachment(GuestTexture *surface) {
  auto &s = state();
  if (s.depth_stencil != surface)
    s.draw_framebuffer_bound = false;
  Video::SetDirtyValue(s.dirtyStates.renderTargetAndDepthStencil,
                      s.depth_stencil, surface);
  if (!s.render_target)
    Video::SetDirtyValue(s.dirtyStates.pipelineState, s.pipelineState.multiview,
                        surface && surface->layers > 1);
  Video::SetDirtyValue(s.dirtyStates.pipelineState,
                      s.pipelineState.depthStencilFormat,
                      surface ? surface->format : plume::RenderFormat::UNKNOWN);
  if (surface)
    s.scene_depth = surface; // current depth input to the remaining DOF adapter
  Video::SetDefaultViewport(nullptr, surface);
}
} // namespace bd::gpu
