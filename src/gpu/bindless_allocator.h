/**
 * @file    gpu/bindless_allocator.h
 * @brief   Linear free slot scan shared by the texture and sampler bindless
 *          descriptor heaps, and the physical layout of the texture heap.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

// TextureDescriptor() below is chosen by REBLUE_D3D12, which makes this header
// backend-conditional - and it had no guard, so it compiled happily into the
// shared reblue_common library and resolved for whichever backend got there
// first. gpu/bindless.cpp did exactly that: it wrote every texture descriptor
// at the D3D12 index while reblue_vk's pipeline layout put it elsewhere, and
// the whole scene rendered black with a working overlay on top.
#if defined(REBLUE_COMMON_TU)
#error "backend-conditional: move this TU to reblue_backend_only in src/CMakeLists.txt"
#endif

#include <rex/types.h>
#include <vector>

namespace bd::gpu {

// Heap capacities. Every CreateTexture registers a texture slot, so that heap
// must be large.
#if defined(__ANDROID__)
// 65536 is a desktop number. It assumes VK_EXT_descriptor_indexing with
// update-after-bind, which is core in Vulkan 1.2 - and the Quest 2's Adreno 650
// is a Vulkan 1.1 device where that extension is optional and per-set sampled
// image limits are orders of magnitude lower. Asking for 65536 there does not
// fail cleanly; it takes the driver somewhere that jumps through a pointer
// holding a small integer.
//
// This is still generous for the game: Blue Dragon is a 512 MB console title,
// so its live texture set is nowhere near even this. BuildPipelineLayout logs
// the device's update-after-bind limit against three times this number.
constexpr u32 kBindlessTextureCount = 4096;
constexpr u32 kBindlessSamplerCount = 256;
#else
constexpr u32 kBindlessTextureCount = 65536;
constexpr u32 kBindlessSamplerCount = 1024;
#endif

// The Vulkan pipeline layout, since 2026-09-02 - four real sets, which is
// every set Adreno allows (maxBoundDescriptorSets = 4):
//
//   set 0  texture heap: binding 0 Texture2DArray[], 1 Texture3D[], 2
//          TextureCube[], each kBindlessTextureCount entries, all
//          update-after-bind and partially bound; bound once per command list
//   set 1  sampler heap: binding 0 SamplerState[kBindlessSamplerCount];
//          bound once per command list
//   set 2  guest constants: bindings 0/1/2 the vertex, pixel and shared
//          blocks as dynamic uniform buffers; the set re-based per draw
//   set 3  the sun-occlusion counter UAV, bound only while counting
//
// Why the constants have a set of their own: the Adreno driver copies a
// descriptor set's contents on every bind that carries dynamic offsets. With
// the three constant ranges in the texture set, re-basing them per draw copied
// the whole 4096-entry texture array - 56% of the render thread's samples in
// one driver memcpy on a Quest 2 (2026-09-01). Moving them into the 256-entry
// sampler set took that to 1.3%; a set holding only the three ranges copies
// 48 bytes. It also ends the spec violation the layer reported as
// VUID-VkDescriptorSetLayoutCreateInfo-descriptorType-03001 (a dynamic uniform
// buffer in a set with an update-after-bind binding), and frees the fourth
// set for the occlusion counter, which Android had to drop.
//
// The three HLSL heaps used to be three register spaces bound to one physical
// set; they are now three bindings of one set. plume addresses a set's
// descriptors by a flat index that runs across its ranges in order, so the
// 3D heap starts at kBindlessTextureCount and the cube heap at twice that.
constexpr u32 kTextureHeapDims = 3;
constexpr u32 kTextureHeap2D = 0;
constexpr u32 kTextureHeap3D = 1;
constexpr u32 kTextureHeapCube = 2;

// Where texture slot `slot` lives in the physical texture set for the heap of
// dimension `dim`. The shader indexes within its own heap, so `slot` is what
// the shared constants carry; this is purely the host-side placement.
//
// D3D12 keeps three register spaces over one descriptor heap, so every
// dimension reads the same descriptor there.
inline u32 TextureDescriptor(u32 slot, u32 dim) {
#if defined(REBLUE_D3D12)
  (void)dim;
  return slot;
#else
  return dim * kBindlessTextureCount + slot;
#endif
}

// Where sampler slot `slot` lives in the physical sampler set. Identity since
// the constant ranges left the sampler set; kept as the single place a shift
// would go.
inline u32 SamplerDescriptor(u32 slot) { return slot; }

// First free slot in used[start_index..end), marked used, returned. on_full
// when none free. Caller holds the heap's mutex.
inline u32 BindlessAllocateSlot(std::vector<bool> &used, u32 start_index,
                                u32 on_full) {
  for (size_t i = start_index; i < used.size(); ++i) {
    if (!used[i]) {
      used[i] = true;
      return static_cast<u32>(i);
    }
  }
  return on_full;
}

// Marks used[slot] free. No-op if slot < start_index or out of range.
inline void BindlessFreeSlot(std::vector<bool> &used, u32 slot,
                             u32 start_index) {
  if (slot < start_index || slot >= static_cast<u32>(used.size()))
    return;
  used[slot] = false;
}

} // namespace bd::gpu
