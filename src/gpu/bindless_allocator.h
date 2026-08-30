/**
 * @file    gpu/bindless_allocator.h
 * @brief   Linear free slot scan shared by the texture and sampler bindless
 *          descriptor heaps.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

// kConstantChunkDescriptors below is chosen by REBLUE_D3D12, which makes this
// header backend-conditional - and it had no guard, so it compiled happily into
// the shared reblue_common library and resolved for whichever backend got there
// first. gpu/bindless.cpp did exactly that: it wrote every texture descriptor at
// TextureDescriptor(slot) using the D3D12 value of 0 while reblue_vk's pipeline
// layout put the texture array at 3, so every texture in the Vulkan build was
// sampled three slots away from where it was written and the whole scene
// rendered black with a working overlay on top.
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
// so its live texture set is nowhere near even this. Sizing it from
// maxDescriptorSetUpdateAfterBindSampledImages at runtime would be better and
// is the proper fix; this is the number that gets a frame on screen.
constexpr u32 kBindlessTextureCount = 4096;
constexpr u32 kBindlessSamplerCount = 256;
#else
constexpr u32 kBindlessTextureCount = 65536;
constexpr u32 kBindlessSamplerCount = 1024;
#endif

// Guest constant chunks, bound as ByteAddressBuffers ahead of the texture array
// in the same descriptor set.
//
// They have to live in an existing set: the pipeline layout already binds four
// (spaces 0/1/2 all point at the texture set, space 3 at the samplers) and
// Adreno's maxBoundDescriptorSets is exactly 4. They have to come *before* the
// texture array, because that array is the boundless range and plume requires
// the boundless range to be last.
//
// Which means every texture descriptor index in the physical set shifts up by
// this much. The shader is unaffected - it indexes within its own binding - so
// this is purely a host-side offset, applied in TextureDescriptor() and nowhere
// else. constant_buffers.cpp never allocates more chunks than this.
//
// Vulkan only. D3D12 reaches guest constants through a real constant buffer
// already (the packoffset path in shader_common.h), so it needs no chunk
// descriptors and must keep its texture indices where they are.
#if defined(REBLUE_D3D12)
constexpr u32 kConstantChunkDescriptors = 0;
#else
constexpr u32 kConstantChunkDescriptors = 3;
#endif

// Where texture slot `slot` actually lives in the physical descriptor set.
// Every setTexture on the texture set goes through this; the bindless slot
// allocator above is unchanged and still hands out 0-based texture slots.
inline u32 TextureDescriptor(u32 slot) {
  return kConstantChunkDescriptors + slot;
}

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
