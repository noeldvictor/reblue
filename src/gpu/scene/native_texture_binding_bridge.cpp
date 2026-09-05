/**
 * @file    native_texture_binding_bridge.cpp
 * @brief   Explicit immutable-image boundary shared by native input producers.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/scene/native_texture_binding_bridge.h"
#include "gpu/resources.h"
#include <rex/cvar.h>
REXCVAR_DECLARE(bool, bd_native_texture_bindings);
namespace bd::gpu::scene {
NativeTextureBinding CaptureNativeTexture(const GuestTexture *t) {
  if (!REXCVAR_GET(bd_native_texture_bindings) || !t ||
      (t->type != ResourceType::Texture && t->type != ResourceType::VolumeTexture) ||
      !t->nativeGpu || t->nativeGpu->descriptor == ~0u ||
      t->sourceSurface || t->aliasOf || t->resolvedTexture ||
      t->descriptorIndex != t->nativeGpu->descriptor ||
      (t->companion2D && !t->companion2D->nativeGpu) ||
      (t->companionCube && !t->companionCube->nativeGpu))
    return {};
  return {t->nativeGpu, t->companion2D ? t->companion2D->nativeGpu : nullptr,
          t->companionCube ? t->companionCube->nativeGpu : nullptr};
}
} // namespace bd::gpu::scene
