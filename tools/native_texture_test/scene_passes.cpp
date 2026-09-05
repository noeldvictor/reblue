/**
 * @file    scene_passes.cpp
 * @brief   Full-size native scene policy, overflow refusal and authored exposure.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/scene/native_scene_pass.h"
#include <stdexcept>
using namespace bd::gpu::scene;
namespace {
void Require(bool value) {
  if (!value) throw std::runtime_error("native scene policy check failed");
}
}
int main() {
  Require(ScaleSceneExtent({1920, 1080}, 1, 100) == SceneExtent{1920, 1080});
  Require(ScaleSceneExtent({1440, 1584}, 1, 100) == SceneExtent{1440, 1584});
  Require(ScaleSceneExtent({1440, 1440}, 1, 100) == SceneExtent{1440, 1440});
  Require(ScaleSceneExtent({1920, 1080}, 2, 100) == SceneExtent{3840, 2160});
  Require(ScaleSceneExtent({1920, 1080}, 2, 50) == SceneExtent{1920, 1080});
  Require(ScaleSceneExtent({1, 1}, 0, 1) == SceneExtent{1, 1});
  Require(ScaleSceneExtent({1920, 1080}, -1, 101) == SceneExtent{1920, 1080});
  Require(!ScaleSceneExtent({0, 1584}, 1, 100));
  Require(!ScaleSceneExtent({1440, 0}, 1, 100));
  Require(!ScaleSceneExtent({1440, 1584}, 1, 0));
  Require(!ScaleSceneExtent({1440, 1584}, 1, -1));
  Require(!ScaleSceneExtent({UINT32_MAX, 1}, 2, 100));
  Require(!ScaleSceneExtent({1, UINT32_MAX}, 2, 100));
  Require(!ScaleSceneExtent({UINT32_MAX, 1}, 1, 50));
  Require(ScaleSceneExtent({UINT32_MAX, 1}, 1, 100)->width == UINT32_MAX);
  Require(SceneClearColor(0x12345678, true) == 0xFF345678);
  Require(SceneClearColor(0x12345678, false) == 0x12345678);
  Require(SceneColorWriteMask(true) == 7 && SceneColorWriteMask(false) == 15);
  Require(SceneOutputExposure(true) == 0.25f && SceneOutputExposure(false) == 1.0f);
  return 0;
}
