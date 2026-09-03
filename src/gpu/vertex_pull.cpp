/**
 * @file    gpu/vertex_pull.cpp
 * @brief   Vertex pulling tables and the block buffer heap.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/vertex_pull.h"

#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "core/logging.h"
#include "gpu/bindless_allocator.h"
#include "gpu/constant_buffers.h"
#include "gpu/device.h"
#include "gpu/frame_stats.h"
#include "gpu/host_resource_heap.h"
#include "gpu/resources.h"
#include "gpu/vertex_declaration.h"

namespace bd::gpu {
namespace {

// The pull-info region per frame slot matches the instance record region:
// the same index addresses both (constant_buffers.cpp commits them together).
constexpr u32 kInfosPerSlot = 2 * kInstanceRecordsPerFrame;
// Indirect commands a frame slot can hold: one per issued draw at most.
constexpr u32 kIndirectPerSlot = 16384;

struct HeapSlot {
  const plume::RenderBuffer *buffer = nullptr;
  u32 freed_frame = 0; // when the buffer was forgotten; 0 = in use
};

struct PullState {
  bool tried = false;
  bool ready = false;
  std::unique_ptr<plume::RenderBuffer> infos;
  u8 *infos_mapped = nullptr;
  std::unique_ptr<plume::RenderBuffer> decls;
  u8 *decls_mapped = nullptr;
  u32 next_decl = 1; // 0 stays the "no declaration" id
  std::vector<VertexPullInfo> staged;
  std::unique_ptr<plume::RenderBuffer> dummy;
  plume::RenderVertexBufferView dummy_view{};
  plume::RenderInputSlot dummy_slot{};
  GuestVertexDeclaration *dummy_decl = nullptr;
  std::once_flag dummy_once;
  // The indirect command ring: kNumFrames regions, one rewound per reset.
  std::unique_ptr<plume::RenderBuffer> indirect;
  u8 *indirect_mapped = nullptr;
  bool multi_draw = false;
  u32 indirect_slot = 0;
  u32 indirect_used = 0; // commands used in the current region
  u32 indirect_full_told = 0;
  // The heap: plume buffer -> slot, and the slots' state.
  std::mutex heap_mutex;
  std::unordered_map<const plume::RenderBuffer *, u32> slot_of;
  HeapSlot slots[kVertexBufferHeapCount];
  u32 next_slot = 0;
  u32 heap_full_told = 0;
  // Why a record's pull info did not stage, per 300 frames.
  u32 n_staged = 0, n_ok = 0, n_no_decl = 0, n_unbound = 0, n_no_slot = 0;
  u32 n_twin_missing = 0; // draw.cpp: pull staged but the twin not compiled
  u32 diag_frame = 0;
};

PullState &pull() {
  static PullState p;
  return p;
}

// Binding 3 of the texture set sits after the three texture heaps in plume's
// flat descriptor index space (bindless_allocator.h).
constexpr u32 kHeapBase = kTextureHeapDims * kBindlessTextureCount;

u32 HeapSlotFor(PullState &p, const plume::RenderBuffer *buffer, u64 size) {
  std::lock_guard lock(p.heap_mutex);
  if (auto it = p.slot_of.find(buffer); it != p.slot_of.end())
    return it->second;
  // A slot never used, else one whose buffer was forgotten long enough ago
  // that no frame in flight can still name it.
  const u32 now = FrameStatFrameCount();
  u32 slot = ~0u;
  if (p.next_slot < kVertexBufferHeapCount) {
    slot = p.next_slot++;
  } else {
    for (u32 i = 0; i < kVertexBufferHeapCount; ++i) {
      const HeapSlot &h = p.slots[i];
      if (h.buffer == nullptr && h.freed_frame != 0 &&
          now - h.freed_frame > kNumFrames + 1) {
        slot = i;
        break;
      }
    }
  }
  if (slot == ~0u) {
    if (p.heap_full_told++ < 3)
      BD_WARN("[pull] block buffer heap full ({} slots); this stream is not "
              "pullable",
              kVertexBufferHeapCount);
    return ~0u;
  }
  auto *set = state().texture_descriptor_set.get();
  if (!set)
    return ~0u;
  const plume::RenderBufferStructuredView view(4, 0);
  set->setBuffer(kHeapBase + slot, buffer, size, &view);
  p.slots[slot] = HeapSlot{buffer, 0};
  p.slot_of.emplace(buffer, slot);
  return slot;
}

} // namespace

bool VertexPullInit(plume::RenderDevice *device) {
  auto &p = pull();
  if (p.tried)
    return p.ready;
  p.tried = true;
#if defined(REBLUE_D3D12)
  (void)device;
  return false;
#else
  auto *set = state().constant_descriptor_set.get();
  if (!device || !set)
    return false;
  {
    const u64 bytes = u64(kNumFrames) * kInfosPerSlot * sizeof(VertexPullInfo);
    auto desc = plume::RenderBufferDesc::UploadBuffer(
        bytes, plume::RenderBufferFlag::STORAGE);
    p.infos = CreateHostBuffer(device, desc, "vertex-pull-infos");
    if (!p.infos)
      return false;
    p.infos_mapped = reinterpret_cast<u8 *>(p.infos->map());
    if (!p.infos_mapped)
      return false;
    const plume::RenderBufferStructuredView view(sizeof(VertexPullInfo), 0);
    set->setBuffer(4, p.infos.get(), bytes, &view);
  }
  {
    const u64 bytes = u64(kPullDeclCount) * kPullTableEntries * sizeof(u32);
    auto desc = plume::RenderBufferDesc::UploadBuffer(
        bytes, plume::RenderBufferFlag::STORAGE);
    p.decls = CreateHostBuffer(device, desc, "vertex-pull-decls");
    if (!p.decls)
      return false;
    p.decls_mapped = reinterpret_cast<u8 *>(p.decls->map());
    if (!p.decls_mapped)
      return false;
    std::memset(p.decls_mapped, 0, kPullTableEntries * sizeof(u32));
    const plume::RenderBufferStructuredView view(sizeof(u32), 0);
    set->setBuffer(5, p.decls.get(), bytes, &view);
  }
  {
    auto desc = plume::RenderBufferDesc::UploadBuffer(
        64, plume::RenderBufferFlag::VERTEX);
    p.dummy = CreateHostBuffer(device, desc, "vertex-pull-dummy");
    if (!p.dummy)
      return false;
    if (auto *m = p.dummy->map()) {
      std::memset(m, 0, 64);
      p.dummy->unmap();
    }
    p.dummy_view = plume::RenderVertexBufferView(
        plume::RenderBufferReference(p.dummy.get(), 0), 64);
    p.dummy_slot = plume::RenderInputSlot(
        15, 0, plume::RenderInputSlotClassification::PER_VERTEX_DATA);
  }
  {
    const u64 bytes = u64(kNumFrames) * kIndirectPerSlot * sizeof(IndirectCommand);
    auto desc = plume::RenderBufferDesc::UploadBuffer(
        bytes, plume::RenderBufferFlag::INDIRECT);
    p.indirect = CreateHostBuffer(device, desc, "vertex-pull-indirect");
    if (p.indirect)
      p.indirect_mapped = reinterpret_cast<u8 *>(p.indirect->map());
    p.multi_draw = device->getCapabilities().multiDrawIndirect;
    if (!p.indirect_mapped)
      BD_WARN("[pull] no indirect command ring; indirect draws are off");
    else if (!p.multi_draw)
      BD_WARN("[pull] the device has no multiDrawIndirect; indirect draws are off");
  }
  p.staged.reserve(kInstanceRecordsPerFrame);
  p.ready = true;
  BD_INFO("[pull] vertex pulling tables bound: {} infos a slot, {} "
          "declarations, {} heap slots",
          kInfosPerSlot, kPullDeclCount, kVertexBufferHeapCount);
  return true;
#endif
}

bool VertexPullReady() { return pull().ready; }

u32 VertexPullEntry(plume::RenderFormat format, u32 slot, u32 offset) {
  using F = plume::RenderFormat;
  // The codes of shader_common.h's BD_PULL_* table.
  u32 code = 0;
  switch (format) {
  case F::R32_FLOAT: code = 1; break;
  case F::R32G32_FLOAT: code = 2; break;
  case F::R32G32B32_FLOAT: code = 3; break;
  case F::R32G32B32A32_FLOAT: code = 4; break;
  case F::B8G8R8A8_UNORM: code = 5; break;
  case F::R8G8B8A8_UINT: code = 6; break;
  case F::R8G8B8A8_UNORM: code = 7; break;
  case F::R16G16_SINT: code = 8; break;
  case F::R16G16_SNORM: code = 9; break;
  case F::R16G16B16A16_SNORM: code = 10; break;
  case F::R16G16_UNORM: code = 11; break;
  case F::R16G16B16A16_UNORM: code = 12; break;
  case F::R16G16_FLOAT: code = 13; break;
  case F::R16G16B16A16_FLOAT: code = 14; break;
  case F::R32_UINT: code = 15; break;
  case F::R16G16B16A16_SINT: code = 16; break;
  default: return 0;
  }
  if (slot >= 16 || offset >= 0x10000)
    return 0;
  return (code << 24) | (slot << 16) | offset;
}

u32 VertexPullDeclId(GuestVertexDeclaration *decl) {
  auto &p = pull();
  if (!decl || !p.ready)
    return 0;
  if (decl->pullId)
    return decl->pullId;
  if (!decl->pullable)
    return 0;
  if (p.next_decl >= kPullDeclCount) {
    static u32 told = 0;
    if (told++ < 3)
      BD_WARN("[pull] declaration table full ({}); this declaration is not "
              "pullable",
              kPullDeclCount);
    decl->pullable = false;
    return 0;
  }
  const u32 id = p.next_decl++;
  std::memcpy(p.decls_mapped + u64(id) * kPullTableEntries * sizeof(u32),
              decl->pullTable, sizeof(decl->pullTable));
  decl->pullId = id;
  return id;
}

bool VertexPullStage(u32 record_index, const VideoState &s) {
  auto &p = pull();
  if (!p.ready || record_index == ~0u)
    return false;
  if (p.staged.size() <= record_index)
    p.staged.resize(record_index + 1);
  VertexPullInfo &info = p.staged[record_index];
  std::memset(&info, 0, sizeof(info));
  ++p.n_staged;
  info.decl = VertexPullDeclId(s.pipelineState.vertexDeclaration);
  if (!info.decl) {
    ++p.n_no_decl;
    return false;
  }
  const GuestVertexDeclaration *decl = s.pipelineState.vertexDeclaration;
  for (u32 i = 0; i < 16; ++i) {
    if (!decl->vertexStreams[i])
      continue;
    const auto &view = s.vertex_views[i];
    if (!view.buffer.ref) {
      info.decl = 0; // a declared stream with nothing bound: not pullable
      ++p.n_unbound;
      return false;
    }
    const u32 slot = HeapSlotFor(p, view.buffer.ref, 0);
    if (slot == ~0u) {
      info.decl = 0;
      ++p.n_no_slot;
      return false;
    }
    info.streams[i][0] = slot;
    info.streams[i][1] = static_cast<u32>(view.buffer.offset);
    info.streams[i][2] = s.input_slots[i].stride;
  }
  ++p.n_ok;
  return true;
}

void VertexPullNoteTwinMissing() { ++pull().n_twin_missing; }

GuestVertexDeclaration *VertexPullDummyDeclaration() {
  auto &p = pull();
  std::call_once(p.dummy_once, [&p] {
    // A guest element list with only its terminator: every table semantic
    // then comes from the builder's synthetic elements on slot 15.
    GuestVertexElement terminator{};
    terminator.stream = 0xFF;
    p.dummy_decl = CreateVertexDeclaration(&terminator);
    if (p.dummy_decl) {
      p.dummy_decl->pullable = false;
      BD_INFO("[pull] dummy declaration: {} elements", p.dummy_decl->inputElementCount);
    }
  });
  return p.dummy_decl;
}

const plume::RenderVertexBufferView *VertexPullDummyView() {
  auto &p = pull();
  return p.ready ? &p.dummy_view : nullptr;
}

const plume::RenderInputSlot *VertexPullDummySlot() {
  auto &p = pull();
  return p.ready ? &p.dummy_slot : nullptr;
}

void VertexPullCommit(const u32 *staged, u32 n, u32 first) {
  auto &p = pull();
  if (!p.ready || !p.infos_mapped || first == ~0u)
    return;
  auto *dst = reinterpret_cast<VertexPullInfo *>(p.infos_mapped) + first;
  for (u32 i = 0; i < n; ++i) {
    const u32 idx = staged[i];
    if (idx < p.staged.size())
      dst[i] = p.staged[idx];
    else
      std::memset(&dst[i], 0, sizeof(VertexPullInfo));
  }
}

bool VertexPullIndirectOK() {
  auto &p = pull();
  return p.ready && p.indirect_mapped && p.multi_draw;
}

IndirectCommand *VertexPullAllocIndirect(u32 count, u64 &byte_offset) {
  auto &p = pull();
  if (!VertexPullIndirectOK() || count == 0)
    return nullptr;
  if (p.indirect_used + count > kIndirectPerSlot) {
    if (p.indirect_full_told++ < 3)
      BD_WARN("[pull] indirect ring full ({} commands a slot)", kIndirectPerSlot);
    return nullptr;
  }
  const u32 first = p.indirect_slot * kIndirectPerSlot + p.indirect_used;
  p.indirect_used += count;
  byte_offset = u64(first) * sizeof(IndirectCommand);
  return reinterpret_cast<IndirectCommand *>(p.indirect_mapped) + first;
}

plume::RenderBuffer *VertexPullIndirectBuffer() { return pull().indirect.get(); }

void VertexPullFrameReset(u32 slot) {
  auto &p = pull();
  p.staged.clear();
  p.indirect_slot = slot % kNumFrames;
  p.indirect_used = 0;
  const u32 f = FrameStatFrameCount();
  if (f - p.diag_frame >= 300) {
    if (p.diag_frame)
      BD_INFO("[pull] per frame: {:.1f} records staged, {:.1f} pullable; "
              "refused: {:.1f} no declaration, {:.1f} stream unbound, {:.1f} no "
              "heap slot; {:.1f} with the twin not compiled",
              p.n_staged / 300.0, p.n_ok / 300.0, p.n_no_decl / 300.0,
              p.n_unbound / 300.0, p.n_no_slot / 300.0,
              p.n_twin_missing / 300.0);
    p.n_staged = p.n_ok = p.n_no_decl = p.n_unbound = p.n_no_slot = 0;
    p.n_twin_missing = 0;
    p.diag_frame = f;
  }
}

void VertexPullForgetBuffer(const plume::RenderBuffer *buffer) {
  auto &p = pull();
  std::lock_guard lock(p.heap_mutex);
  auto it = p.slot_of.find(buffer);
  if (it == p.slot_of.end())
    return;
  HeapSlot &h = p.slots[it->second];
  h.buffer = nullptr;
  h.freed_frame = FrameStatFrameCount() | 1u;
  p.slot_of.erase(it);
}

} // namespace bd::gpu
