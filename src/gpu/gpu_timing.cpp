/**
 * @file    gpu/gpu_timing.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/gpu_timing.h"

#include <memory>
#include <vector>

#include <plume_render_interface.h>
#if defined(REBLUE_D3D12)
#include <plume_d3d12.h>
#endif

#include "core/logging.h"
#include "gpu/backend.h"
#include "gpu/device.h"
#include "gpu/frame_stats.h"

namespace bd::gpu {

namespace {

constexpr u32 kQueryCount = 512;

enum class Cat : u8 { Draw, Inter, Resolve };

struct SlotTiming {
  std::unique_ptr<plume::RenderQueryPool> pool;
  // journal[i] = category of the segment query i CLOSES ([0] = frame start,
  // unused). Queries >= journal.size() are padding.
  std::vector<Cat> journal;
  u32 used = 0;
  bool pending = false;
  // The last query index is reserved for the frame-end timestamp so the total
  // stays honest once the per-segment journal saturates. Without it a frame
  // with more transitions than the pool holds reports only the time up to the
  // point it filled - which read as a 2ms GPU on a frame the CPU waited 74ms
  // for, and sent two sessions looking for the cost on the wrong side.
  bool end_written = false;
  bool saturated = false;
};

SlotTiming g_slots[kNumFrames];
u32 g_active_slot = 0;
Cat g_cat = Cat::Inter;
// MoltenVK reports queryPools support but rejects the timestamp pools, so it
// never starts supported rather than being switched off on the first frame.
bool g_supported = !g_mvk;
bool g_open = false;

void WriteMark(SlotTiming &st, plume::RenderCommandList *cmd, Cat closing) {
  if (st.used >= kQueryCount - 1) {
    st.saturated = true;
    return; // tail lumps into the last segment; the total is still exact
  }
  cmd->writeTimestamp(st.pool.get(), st.used++);
  st.journal.push_back(closing);
}

void SetSegment(plume::RenderCommandList *cmd, Cat cat) {
  if (!g_supported || !g_open || cat == g_cat)
    return;
  WriteMark(g_slots[g_active_slot], cmd, g_cat);
  g_cat = cat;
}

// D3D12QueryPool::queryResults memcpy's the whole readback buffer from map()
// without checking it, so a failed ID3D12Resource::Map (device removed, or an
// allocation failure) turns into a memcpy from null inside vendored plume.
// Probe the same buffer here first. VulkanQueryPool::queryResults checks its
// own vkGetQueryPoolResults result, so only D3D12 needs this.
bool ReadbackIsMappable(plume::RenderQueryPool *pool) {
#if defined(REBLUE_D3D12)
  auto *d3d_pool = static_cast<plume::D3D12QueryPool *>(pool);
  auto *readback = d3d_pool->readbackBuffer.get();
  if (!readback)
    return false;
  if (!readback->map())
    return false;
  readback->unmap();
#else
  (void)pool;
#endif
  return true;
}

} // namespace

void FrameBegin(plume::RenderDevice *device, plume::RenderCommandList *cmd,
                u32 slot) {
  if (!g_supported || !device || !cmd)
    return;
  if (!device->getCapabilities().queryPools) {
    g_supported = false;
    return;
  }
  auto &st = g_slots[slot];
  if (!st.pool) {
    st.pool = device->createQueryPool(kQueryCount);
    if (!st.pool) {
      g_supported = false;
      return;
    }
  }
  st.pending = false;
  st.used = 0;
  st.end_written = false;
  st.saturated = false;
  st.journal.clear();
  st.journal.push_back(Cat::Inter); // slot 0 = frame start marker
  cmd->resetQueryPool(st.pool.get(), 0, kQueryCount);
  cmd->writeTimestamp(st.pool.get(), st.used++);
  g_active_slot = slot;
  g_cat = Cat::Inter;
  g_open = true;
}

void MarkDraw(plume::RenderCommandList *cmd) { SetSegment(cmd, Cat::Draw); }

void MarkInter(plume::RenderCommandList *cmd) { SetSegment(cmd, Cat::Inter); }

void MarkResolve(plume::RenderCommandList *cmd) {
  SetSegment(cmd, Cat::Resolve);
}

void FrameEnd(plume::RenderCommandList *cmd) {
  if (!g_supported || !g_open)
    return;
  auto &st = g_slots[g_active_slot];
  WriteMark(st, cmd, g_cat);
  // Always close the frame, even when the journal filled.
  cmd->writeTimestamp(st.pool.get(), kQueryCount - 1);
  st.end_written = true;
  if (st.saturated) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      BD_INFO("[perf] gpu timing journal saturated at {} queries; per-category "
              "split is partial but gpu_total_ms spans the whole frame",
              kQueryCount);
    }
  }
  // No tail padding: writeTimestamp resolves ONE query per call, so padding to
  // kQueryCount cost 512 EndQuery + ResolveQueryData every frame to fill slots
  // nothing reads. queryResults still copies and rescales the whole pool, so
  // the unwritten tail carries stale values that get rescaled again each frame
  // and saturate. Harmless, since CollectGPUTimings only reads below
  // journal.size().
  st.pending = st.journal.size() > 1;
  g_open = false;
}

void CollectGPUTimings(u32 slot) {
  if (!g_supported)
    return;
  auto &st = g_slots[slot];
  if (!st.pending || !st.pool)
    return;
  st.pending = false;
  if (!ReadbackIsMappable(st.pool.get())) {
    BD_ERROR("gpu timing: query readback map() null (slot {}), timing disabled",
             slot);
    CheckDeviceRemoved("gpu timing readback");
    g_supported = false;
    return;
  }
  st.pool->queryResults();
  const u64 *r = st.pool->getResults(); // nanoseconds
  const size_t n = st.journal.size();
  if (n < 2 || r[n - 1] <= r[0])
    return;
  u64 draw_ns = 0, resolve_ns = 0;
  for (size_t i = 1; i < n; ++i) {
    if (r[i] <= r[i - 1])
      continue;
    if (st.journal[i] == Cat::Draw) {
      draw_ns += r[i] - r[i - 1];
    } else if (st.journal[i] == Cat::Resolve) {
      resolve_ns += r[i] - r[i - 1];
    }
  }
  const u64 end_ns = st.end_written ? r[kQueryCount - 1] : r[n - 1];
  const f64 total_ms = (end_ns > r[0]) ? (end_ns - r[0]) * 1e-6
                                       : (r[n - 1] - r[0]) * 1e-6;
  RecordGPUTime(total_ms, draw_ns * 1e-6, resolve_ns * 1e-6);
}

} // namespace bd::gpu
