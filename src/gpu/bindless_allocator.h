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
