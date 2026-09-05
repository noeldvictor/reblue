/**
 * @file    native_scene_texture_bridge.h
 * @brief   Host scene-image selection with explicit live resource adapters.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include "gpu/scene/native_texture_binding_bridge.h"
#include "gpu/scene/scene_texture_import.h"
namespace bd::gpu::scene {
struct SceneTextureInput {
  NativeTextureBinding native;
  GuestTexture *bridge = nullptr;
  uint32_t source_address = 0; // transient import/diagnostic only
  SceneTextureSelection selection; // transient source stamp, not recipe identity
};
using SceneTextureInputs = std::array<SceneTextureInput, 2>;
// Checked logical snapshot only; no registry lookup and no retained association.
std::optional<SceneTextureSelections> ReadNativeSceneTextureSources();
// Must run outside the video mutex: resource lookup can wait for an IO upload.
std::optional<SceneTextureInputs> PrepareNativeSceneTextures();
} // namespace bd::gpu::scene
