/**
 * @file    gpu/draw_queue.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/draw_queue.h"

#include <algorithm>
#include <vector>

#include <rex/cvar.h>

#include "core/logging.h"
#include "gpu/device.h"

REXCVAR_DECLARE(bool, bd_draw_defer);
REXCVAR_DECLARE(bool, bd_draw_sort);

namespace bd::gpu {

namespace {

// One pass's worth. Reserved once and reused for the life of the process: a
// field frame records a few hundred of these and reallocating inside the
// submission path would be a new per-draw cost in the middle of removing one.
std::vector<QueuedDraw> g_queue;
u32 g_sequence = 0;

// Only what actually has to be re-emitted. Sorting by pipeline means runs of
// draws share one, and re-binding it per draw would spend back exactly what the
// sort saves.
struct EmitState {
  plume::RenderPipeline *pipeline = nullptr;
  u32 constant_offsets[3] = {~0u, ~0u, ~0u};
  const plume::RenderBuffer *index_buffer = nullptr;
  bool any = false;
};

void EmitOne(plume::RenderCommandList *cmd, const QueuedDraw &d,
             EmitState &st) {
  // A queued draw with no pipeline means the capture missed one - the bind is
  // dirty-gated, so a draw reusing the previous pipeline records nothing unless
  // the whole binding is captured. setPipeline(nullptr) is an access violation,
  // which is how that bug announced itself the first time.
  if (!d.pipeline) {
    static u32 told = 0;
    if (told++ < 8)
      BD_ERROR("[draw-queue] queued draw with no pipeline, skipped");
    return;
  }

  if (d.pipeline != st.pipeline) {
    cmd->setPipeline(d.pipeline);
    st.pipeline = d.pipeline;
  }

  if (!st.any || d.constant_offsets[0] != st.constant_offsets[0] ||
      d.constant_offsets[1] != st.constant_offsets[1] ||
      d.constant_offsets[2] != st.constant_offsets[2]) {
    // Space 0 only. Spaces 1 and 2 are the same physical set and carry no
    // constants the shader reads; frame_ring binds those once with zeroes.
    if (auto *set = Video::TextureDescriptorSet())
      cmd->setGraphicsDescriptorSetDynamic(set, 0, d.constant_offsets, 3);
    st.constant_offsets[0] = d.constant_offsets[0];
    st.constant_offsets[1] = d.constant_offsets[1];
    st.constant_offsets[2] = d.constant_offsets[2];
  }

  // In runs of slots that actually have a buffer.
  //
  // The recorded range is the union of everything bound since the command list
  // began, and the guest does not fill it densely - a gap is a slot it never
  // bound. plume dereferences every view it is handed, so passing one with a
  // null buffer is a null dereference inside the backend, which is exactly how
  // this announced itself: ACCESS_VIOLATION reading address 0x10, three times.
  u32 i = d.vertex_first;
  const u32 end = d.vertex_first + d.vertex_count;
  while (i < end && i < 16u) {
    if (d.vertex_views[i].buffer.ref == nullptr) {
      ++i;
      continue;
    }
    u32 run = i;
    while (run < end && run < 16u && d.vertex_views[run].buffer.ref != nullptr)
      ++run;
    cmd->setVertexBuffers(i, d.vertex_views + i, run - i, d.input_slots + i);
    i = run;
  }
  if (d.has_index_buffer) {
    const plume::RenderBuffer *ib = d.index_view.buffer.ref;
    if (ib != st.index_buffer) {
      cmd->setIndexBuffer(&d.index_view);
      st.index_buffer = ib;
    }
  }

  st.any = true;

  if (d.indexed)
    cmd->drawIndexedInstanced(d.count, 1, d.start_index, d.base_vertex, 0);
  else
    cmd->drawInstanced(d.count, 1, d.start_vertex, 0);
}

} // namespace

bool DrawQueueEnabled() { return REXCVAR_GET(bd_draw_defer); }

void DrawQueuePush(const QueuedDraw &draw) {
  if (g_queue.capacity() == 0)
    g_queue.reserve(4096);
  g_queue.push_back(draw);
  g_queue.back().sequence = g_sequence++;
}

u32 DrawQueueDepth() { return static_cast<u32>(g_queue.size()); }

void DrawQueueDiscardStragglers() {
  if (g_queue.empty())
    return;
  static u32 told = 0;
  if (told++ < 8)
    BD_ERROR("[draw-queue] {} draws still queued at present - a render pass "
             "ended somewhere that does not flush",
             g_queue.size());
  g_queue.clear();
}

void DrawQueueFlush(plume::RenderCommandList *cmd) {
  if (g_queue.empty())
    return;
  if (!cmd) {
    // Nothing to emit into. Dropping the draws is wrong but silently keeping
    // them across a pass boundary is worse - they would be emitted against a
    // framebuffer they were not recorded for.
    BD_ERROR("[draw-queue] flush with no command list, {} draws dropped",
             g_queue.size());
    g_queue.clear();
    return;
  }

  if (REXCVAR_GET(bd_draw_sort)) {
    // Opaque first, grouped by pipeline, near to far inside each group.
    //
    // Grouping collapses pipeline switches. Near-first lets Adreno's LRZ reject
    // a hidden fragment before shading it, which is the whole reason this is
    // worth doing on a tiler.
    //
    // Blended draws keep submission order and follow the opaque set. Their
    // result depends on what is already in the framebuffer, so reordering them
    // against each other changes the image - this is the one constraint in the
    // whole rewrite that cannot be relaxed.
    std::stable_sort(g_queue.begin(), g_queue.end(),
                     [](const QueuedDraw &a, const QueuedDraw &b) {
                       if (a.blended != b.blended)
                         return !a.blended;
                       if (a.blended)
                         return a.sequence < b.sequence;
                       if (a.pipeline != b.pipeline)
                         return a.pipeline < b.pipeline;
                       return a.depth < b.depth;
                     });
  }

  EmitState st;
  for (const QueuedDraw &d : g_queue)
    EmitOne(cmd, d, st);
  g_queue.clear();
}

} // namespace bd::gpu
