/**
 * @file    gpu/vertex_pull.h
 * @brief   Vertex pulling: the per-record stream table, the per-declaration
 *          attribute table and the block buffer heap that the recompiled
 *          vertex shaders read under SPEC_CONSTANT_PULLED (shader_common.h),
 *          so one indirect draw can cover meshes bound at different offsets.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <plume_render_interface.h>
#include <rex/types.h>

namespace bd::gpu {

struct VideoState;
struct GuestVertexDeclaration;

// Mirrors BDPullInfo in shader_common.h: per instance record, each stream's
// slot in the block buffer heap, its base byte offset and its stride, and
// the declaration's id into the attribute table.
struct VertexPullInfo {
  u32 streams[16][4];
  u32 decl;
  u32 pad[3];
};
static_assert(sizeof(VertexPullInfo) == 272);

// The heap: binding 3 of the texture set, after the three texture heaps.
constexpr u32 kVertexBufferHeapCount = 256;
// Entries per declaration in the attribute table, indexed by location.
constexpr u32 kPullTableEntries = 16;
constexpr u32 kPullDeclCount = 8192;

// Creates and binds the two tables; false leaves pulling off (every draw
// then goes through the input assembler as before).
bool VertexPullInit(plume::RenderDevice *device);
bool VertexPullReady();

// The attribute table entry for an input element the declaration builder
// decided on: code << 24 | slot << 16 | byte offset, zero for a format the
// pulled path cannot decode.
u32 VertexPullEntry(plume::RenderFormat format, u32 slot, u32 offset);

// The declaration's id in the table, written on first use.
u32 VertexPullDeclId(GuestVertexDeclaration *decl);

// Stages the pull info for the instance record just staged at record_index,
// from the bound streams and declaration. Streams bound from a buffer the
// heap does not hold get a slot on first sight.
bool VertexPullStage(u32 record_index, const VideoState &s);
// Commits the staged infos of the records CommitInstanceRecords committed,
// at the same GPU indices.
void VertexPullCommit(const u32 *staged, u32 n, u32 first);
void VertexPullFrameReset(u32 slot);
void VertexPullNoteTwinMissing();

// The pulled pipeline's input layout: every attribute location on slot 15
// at offset 0, stride 0, bound to a 64-byte zero buffer - the assembler
// then reads the same in-bounds bytes for every vertex and the shader never
// looks at them. One declaration for the process.
GuestVertexDeclaration *VertexPullDummyDeclaration();
const plume::RenderVertexBufferView *VertexPullDummyView();
const plume::RenderInputSlot *VertexPullDummySlot();

// Indirect draws (bd_draw_indirect): a ring of VkDrawIndexedIndirectCommand
// slots per frame, rewound with the records. Alloc returns the mapped
// commands and their byte offset in the buffer, or null when the frame's
// region is full or the device cannot multi-draw.
struct IndirectCommand {
  u32 index_count;
  u32 instance_count;
  u32 first_index;
  i32 vertex_offset;
  u32 first_instance;
};
static_assert(sizeof(IndirectCommand) == 20);
bool VertexPullIndirectOK();
IndirectCommand *VertexPullAllocIndirect(u32 count, u64 &byte_offset);
plume::RenderBuffer *VertexPullIndirectBuffer();

// A buffer that is about to be retired: its heap slot is reusable after the
// frames in flight have drained.
void VertexPullForgetBuffer(const plume::RenderBuffer *buffer);

} // namespace bd::gpu
