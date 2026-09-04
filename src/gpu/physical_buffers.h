/**
 * @file    gpu/physical_buffers.h
 * @brief   Registry bridging engine-owned D3DVertexBuffer/D3DIndexBuffer struct
 *          VAs (asset-loaded geometry that bypasses CreateVertex/IndexBuffer)
 * to host GuestBuffers. Owns its own mutex, separate from VideoState.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/types.h>

namespace plume {
struct RenderBuffer;
} // namespace plume

namespace bd::gpu {

struct GuestBuffer;
enum class ResourceType : u32;

// For an engine-owned D3DIndexBuffer / D3DVertexBuffer struct VA that missed
// HostResourceHeap::FromGuest. Bootstrap rebuilds and registers a host
// GuestBuffer from a fully initialized X360 struct, for buffers no
// creation-time registration ever saw (asset-loaded meshes patched by an
// unhooked loader).
GuestBuffer *FindPhysicalBufferByStruct(u32 struct_va);

// The model block a host geometry buffer mirrors, for the scene recorder:
// the block is the cook unit and its pristine guest bytes hash stably
// (guest addresses do not). The hash is computed on first request and
// cached with the block.
struct PhysicalBlockInfo {
  u32 base = 0;
  u32 size = 0;
  u64 content_hash = 0;
};
bool PhysicalBlockOfBuffer(const plume::RenderBuffer *buffer,
                           PhysicalBlockInfo &out);
GuestBuffer *AdoptPhysicalBuffer(u32 struct_va,
                                               ResourceType rtype);

// FindPhysicalBufferByStruct, then AdoptPhysicalBuffer. Used by
// every draw/lock hook that binds a physical VB/IB by its engine-owned VA.
GuestBuffer *ResolveGuestBufferVa(u32 va, ResourceType rtype);
// Bumped whenever a physical buffer is evicted or refreshed: a GuestBuffer
// pointer resolved at one generation is still that buffer while it holds.
u64 PhysicalBufferGeneration();

// Free the plume buffers retired by the registry's stale-reuse refresh. Runs
// from the per-slot post-fence drain, so no in-flight list references them.
void DrainBufferGraveyard(u32 slot);

} // namespace bd::gpu
