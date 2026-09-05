/**
 * @file    draw_intent.h
 * @brief   Explicit native draw ownership, separate from engine bind history.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once

namespace bd::gpu {
// Native packets are authoritative even when a shader/declaration is null.
// Never fill a missing native binding from an unrelated engine draw.
template <class State> auto DrawPixelShader(const State &state) {
  return state.native_draw_pipeline ? state.native_draw_pipeline->pixelShader
                                    : state.pixel_shader;
}
template <class State> auto DrawVertexDeclaration(const State &state) {
  return state.native_draw_pipeline
             ? state.native_draw_pipeline->vertexDeclaration
             : state.vertex_declaration;
}

// The packet's complete pipeline has already been bound by its producer.
// Target formats/topology/strides are composed separately for this dispatch.
// Only engine-origin draws consume the compatibility binding/state history.
template <class State, class PublishState>
void ApplyEngineDrawIntent(State &state, PublishState publish_state) {
  if (state.native_draw_pipeline)
    return;
  auto assign = [&](auto &destination, const auto &source) {
    if (destination != source) {
      destination = source;
      state.dirtyStates.pipelineState = true;
    }
  };
  assign(state.pipelineState.vertexShader, state.vertex_shader);
  assign(state.pipelineState.pixelShader, state.pixel_shader);
  assign(state.pipelineState.vertexDeclaration, state.vertex_declaration);
  publish_state();
}
} // namespace bd::gpu
