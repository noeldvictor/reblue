/**
 * @file    gpu/bindless.cpp
 * @brief   The shared bindless texture descriptor set: slot allocation, SRV
 *          binding, and the fence-deferred retire of a released slot.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/device.h"

#include <mutex>

#include <plume_render_interface.h>

#include "core/logging.h"
#include "gpu/bindless_allocator.h"

namespace bd::gpu {

u32 AllocateSlot(VideoState &s) {
  return BindlessAllocateSlot(s.descriptor_slot_used,
                              kNullTextureDescriptorCount,
                              kInvalidDescriptorIndex);
}

// Park a bindless slot for fence-deferred null+free. Descriptors are
// dereferenced at GPU execution time: the other in-flight command list can
// hold draws whose constants index this slot, so rewriting it now would serve
// them the null sentinel. DrainDescriptorSlotsLocked pays the rewrite
// once the slot's next fence proves every such list retired. The old texture
// object outlives the descriptor via texture_graveyard / SurfacePool, which
// share the same boundary. Caller holds s.mutex.
void ParkDescriptorSlotLocked(VideoState &s, u32 slot, u32 null_index) {
  if (slot < kNullTextureDescriptorCount ||
      slot >= s.descriptor_slot_used.size()) {
    return;
  }
  s.descriptor_graveyard[Video::RetireSlot("descriptor slot")].push_back(
      {slot, null_index});
}

void DrainDescriptorSlotsLocked(VideoState &s, u32 slot) {
  for (const auto &d : s.descriptor_graveyard[slot]) {
    if (s.texture_descriptor_set) {
      s.texture_descriptor_set->setTexture(
          TextureDescriptor(d.slot), s.null_textures[d.null_index].get(),
          plume::RenderTextureLayout::SHADER_READ,
          s.null_texture_views[d.null_index].get());
    }
    BindlessFreeSlot(s.descriptor_slot_used, d.slot,
                     kNullTextureDescriptorCount);
  }
  s.descriptor_graveyard[slot].clear();
}

// Retire a GuestTexture's bindless slot. The slot keeps its live SRV until the
// fence-deferred drain rewrites it to the dimension-matched null sentinel, and
// descriptorIndex is invalidated immediately so no new reference is recorded.
// Caller holds s.mutex.
void ReleaseTextureSRVLocked(VideoState &s, GuestTexture *tex) {
  if (!tex || tex->descriptorIndex == kInvalidDescriptorIndex)
    return;
  const u32 slot = tex->descriptorIndex;
  tex->descriptorIndex = kInvalidDescriptorIndex;
  u32 null_index = kNullTexture2DDescriptorIndex;
  switch (tex->viewDimension) {
  case plume::RenderTextureViewDimension::TEXTURE_3D:
    null_index = kNullTexture3DDescriptorIndex;
    break;
  case plume::RenderTextureViewDimension::TEXTURE_CUBE:
    null_index = kNullTextureCubeDescriptorIndex;
    break;
  default:
    break;
  }
  ParkDescriptorSlotLocked(s, slot, null_index);
}

u32 BindTextureSRVLocked(VideoState &s, GuestTexture *tex) {
  if (!tex || !tex->texture || !s.texture_descriptor_set) {
    return kInvalidDescriptorIndex;
  }
  if (tex->descriptorIndex != kInvalidDescriptorIndex) {
    return tex->descriptorIndex;
  }
  if (!tex->textureView && tex->format != plume::RenderFormat::UNKNOWN) {
    plume::RenderTextureViewDesc view_desc;
    // D3D12 forbids a typed-depth SRV format, so view D32_FLOAT as R32_FLOAT
    // for BD's depth shader-resolves (fog / soft particles / SSAO inputs).
    // D32_FLOAT_S8_UINT is left as-is: plume's toDXGITextureView already
    // specializes it to a depth-only view.
    view_desc.format = (tex->format == plume::RenderFormat::D32_FLOAT)
                           ? plume::RenderFormat::R32_FLOAT
                           : tex->format;
    // An ARRAY view, always. The bindless 2D heap is declared
    // Texture2DArray so a multiview target can be sampled per eye without
    // being flattened first, and a Texture2DArray sampler requires an array
    // view for every descriptor it might read - a one-layer 2D view bound
    // there is a type mismatch. arraySize stays 1 for ordinary textures;
    // Vulkan clamps the layer coordinate, so they read layer 0.
    view_desc.dimension =
        (tex->viewDimension != plume::RenderTextureViewDimension::UNKNOWN &&
         tex->viewDimension != plume::RenderTextureViewDimension::TEXTURE_2D)
            ? tex->viewDimension
            : plume::RenderTextureViewDimension::TEXTURE_2D_ARRAY;
    view_desc.mipLevels = tex->mipLevels ? tex->mipLevels : 1;
    // One layer, explicitly. arraySize defaults to UINT32_MAX, which plume
    // expands to the image's full layer count - so on a two-layer multiview
    // target this builds a 2-layer view with VK_IMAGE_VIEW_TYPE_2D, which
    // Vulkan forbids (that view type requires layerCount == 1), and nothing can
    // sample the surface through it.
    //
    // surface_pool sets this at creation, but that is not enough: ResetPooled
    // re-binds every recycled surface through here, and a pooled surface whose
    // view was dropped rebuilds it on this path - every frame.
    view_desc.arraySize = 1;
    view_desc.arrayIndex = 0;
    tex->textureView = tex->texture->createTextureView(view_desc);
  }
  if (!tex->textureView) {
    return kInvalidDescriptorIndex;
  }
  const u32 slot = AllocateSlot(s);
  if (slot == kInvalidDescriptorIndex) {
    BD_ERROR("Bindless texture heap full at {} slots, SRV bind dropped",
             kBindlessTextureCount);
    return kInvalidDescriptorIndex;
  }
  s.texture_descriptor_set->setTexture(TextureDescriptor(slot), tex->texture,
                                       plume::RenderTextureLayout::SHADER_READ,
                                       tex->textureView.get());
  tex->descriptorIndex = slot;
  return slot;
}

// Points an already-allocated slot at an arbitrary view of a texture.
//
// BindTextureSRVLocked only knows how to bind a surface's own primary view, and
// the multiview resolve needs two more - one per array slice - registered
// against the same image. Splitting that out is cheaper than teaching the
// primary path about layers it otherwise never sees.
void SetBindlessTextureLocked(VideoState &s, u32 slot,
                              plume::RenderTexture *texture,
                              plume::RenderTextureView *view) {
  if (!s.texture_descriptor_set || slot == kInvalidDescriptorIndex || !texture)
    return;
  s.texture_descriptor_set->setTexture(
      TextureDescriptor(slot), texture, plume::RenderTextureLayout::SHADER_READ, view);
}

void Video::SetBindlessTexture(u32 slot, plume::RenderTexture *texture,
                               plume::RenderTextureView *view) {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (!s.texture_descriptor_set || slot == kInvalidDescriptorIndex || !texture)
    return;
  s.texture_descriptor_set->setTexture(
      TextureDescriptor(slot), texture, plume::RenderTextureLayout::SHADER_READ, view);
}

// Registers a multiview surface's *resolved* companion as its sampled image, so
// every downstream read gets both eyes rather than one array slice.
u32 Video::BindResolvedSRV(GuestTexture *tex) {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (!tex || !tex->resolvedTexture || !tex->textureView ||
      !s.texture_descriptor_set)
    return kInvalidDescriptorIndex;
  if (tex->descriptorIndex != kInvalidDescriptorIndex)
    return tex->descriptorIndex;
  const u32 slot = AllocateSlot(s);
  if (slot == kInvalidDescriptorIndex) {
    BD_ERROR("Bindless heap full, multiview resolve SRV dropped");
    return kInvalidDescriptorIndex;
  }
  s.texture_descriptor_set->setTexture(TextureDescriptor(slot), tex->resolvedTexture,
                                       plume::RenderTextureLayout::SHADER_READ,
                                       tex->textureView.get());
  tex->descriptorIndex = slot;
  return slot;
}

u32 Video::AllocateBindlessTextureSlot() {
  // AllocateSlot's kInvalidDescriptorIndex is the sentinel callers expect, so
  // no remap is needed.
  return AllocateSlot(state());
}

void Video::FreeBindlessTextureSlot(u32 slot) {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  ParkDescriptorSlotLocked(s, slot, kNullTexture2DDescriptorIndex);
}

u32 Video::BindTextureSRV(GuestTexture *tex) {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  return BindTextureSRVLocked(s, tex);
}

} // namespace bd::gpu
