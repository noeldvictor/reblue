/**
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause License
 */
#pragma once
#include <cstdint>
#include <plume_render_interface.h>

namespace bd::gpu {
// Host resource staging, deliberately not a shader-register allocation.
// No uniform dynamic offset, guest address, or emulated resource is involved.
struct HostUploadAllocation {
  uint8_t *memory = nullptr;
  plume::RenderBufferReference ref{};
  uint32_t size = 0;
};
// Caller holds the renderer lock and has an open command list. Storage stays
// valid until the matching recording slot is reused after its completed fence.
// Allocations are bounded to 64 MiB each and 256 MiB across all in-flight
// slots.
HostUploadAllocation AllocateHostUpload(uint32_t size, uint32_t alignment = 4);
HostUploadAllocation UploadHostData(const void *data, uint32_t size,
                                    uint32_t alignment = 4);
void ResetHostUploadsAfterFence(uint32_t slot);
// Frame-local streams cannot be frozen into a cross-frame geometry recipe.
// Caller holds the renderer lock, as for allocation/reset.
bool IsHostUploadBuffer(const plume::RenderBuffer *buffer);
} // namespace bd::gpu
