/**
 * @file    draw_intent.cpp
 * @brief   Native draw packets cannot consume unrelated engine bind history.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "gpu/draw_intent.h"
#include <cassert>
#include <iostream>

struct Pipeline {
  int vertexShader = 0, pixelShader = 0, vertexDeclaration = 0;
  int blend = 0, depth = 0, alpha = 0, topology = 0, target = 0;
  bool operator==(const Pipeline &) const = default;
};
struct State {
  Pipeline pipelineState;
  const Pipeline *native_draw_pipeline = nullptr;
  int vertex_shader = 1, pixel_shader = 2, vertex_declaration = 3;
  struct {
    bool pipelineState = false;
  } dirtyStates;
};
int main() {
  using namespace bd::gpu;
  State state;
  int imports = 0;
  auto publish = [&] {
    ++imports;
    state.pipelineState.blend = 4;
    state.pipelineState.depth = 5;
    state.pipelineState.alpha = 6;
  };
  ApplyEngineDrawIntent(state, publish);
  assert(imports == 1 && state.dirtyStates.pipelineState);
  assert(state.pipelineState == (Pipeline{1, 2, 3, 4, 5, 6, 0, 0}));
  assert(DrawPixelShader(state) == 2 && DrawVertexDeclaration(state) == 3);
  const Pipeline engine = state.pipelineState;
  const Pipeline packet{10, 20, 30, 40, 50, 60, 70, 80};
  state.pipelineState = packet;
  state.native_draw_pipeline = &packet;
  // Current target/topology composition is not undone during flush either.
  state.pipelineState.topology = 71;
  state.pipelineState.target = 81;
  const auto composed = state.pipelineState;
  state.dirtyStates.pipelineState = false;
  ApplyEngineDrawIntent(state, publish);
  assert(imports == 1 && !state.dirtyStates.pipelineState);
  assert(state.pipelineState == composed);
  assert(DrawPixelShader(state) == 20 && DrawVertexDeclaration(state) == 30);
  assert(state.pixel_shader == 2 && state.vertex_declaration == 3);
  const Pipeline missing{};
  state.native_draw_pipeline = &missing;
  assert(DrawPixelShader(state) == 0 && DrawVertexDeclaration(state) == 0);
  // Returning to the engine restores its own history, not the native packet.
  state.native_draw_pipeline = nullptr;
  state.pipelineState = engine;
  ApplyEngineDrawIntent(state, publish);
  assert(imports == 2 && state.pipelineState == engine);
  assert(DrawPixelShader(state) == 2 && DrawVertexDeclaration(state) == 3);
  std::cout << "Native draw intent ownership passed\n";
}
