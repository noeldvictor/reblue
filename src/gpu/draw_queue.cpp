/**
 * @file    gpu/draw_queue.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/draw_queue.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include <rex/cvar.h>

#include "core/logging.h"
#include "gpu/backend.h"
#include "gpu/device.h"

REXCVAR_DECLARE(bool, bd_draw_defer);
REXCVAR_DECLARE(bool, bd_draw_sort);
REXCVAR_DECLARE(bool, bd_draw_eye_major);
REXCVAR_DECLARE(i32, bd_pass_split_draws);

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
  plume::RenderIndexBufferView index_view{plume::RenderBufferReference{}, 0,
                                          plume::RenderFormat::R16_UINT};
  plume::RenderViewport viewport{};
  plume::RenderRect scissor{};
  plume::RenderFramebuffer *framebuffer = nullptr;
  bool any = false;
};

void EmitOne(plume::RenderCommandList *cmd, const QueuedDraw &d,
             EmitState &st) {
  // Its own framebuffer, always. Whatever is bound at flush time is not
  // necessarily what this draw was recorded against, and may be nothing at all.
  if (!d.framebuffer) {
    static u32 told = 0;
    if (told++ < 8)
      BD_ERROR("[draw-queue] queued draw with no framebuffer, skipped");
    return;
  }
  if (d.framebuffer != st.framebuffer) {
    cmd->setFramebuffer(d.framebuffer);
    st.framebuffer = d.framebuffer;
  }
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

  if (d.has_viewport &&
      (!st.any || std::memcmp(&d.viewport, &st.viewport, sizeof(d.viewport)) ||
       std::memcmp(&d.scissor, &st.scissor, sizeof(d.scissor)))) {
    cmd->setViewports(&d.viewport, 1);
    cmd->setScissors(&d.scissor, 1);
    st.viewport = d.viewport;
    st.scissor = d.scissor;
  }

  if (d.pipeline != st.pipeline) {
    cmd->setPipeline(d.pipeline);
    st.pipeline = d.pipeline;
  }

  if (!st.any || d.constant_offsets[0] != st.constant_offsets[0] ||
      d.constant_offsets[1] != st.constant_offsets[1] ||
      d.constant_offsets[2] != st.constant_offsets[2]) {
    // The constant ranges are a set of their own; the texture and sampler
    // heaps are bound once by frame_ring and never rebound.
    if (auto *set = Video::ConstantDescriptorSet())
      cmd->setGraphicsDescriptorSetDynamic(set, kConstantDescriptorSetIndex,
                                           d.constant_offsets, 3);
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
    // The whole view, not just the buffer pointer. Two draws can share a buffer
    // at different offsets, sizes or index formats, and deduplicating on the
    // pointer alone would silently keep the previous draw's format.
    if (!st.any || d.index_view.buffer.ref != st.index_view.buffer.ref ||
        d.index_view.buffer.offset != st.index_view.buffer.offset ||
        d.index_view.size != st.index_view.size ||
        d.index_view.format != st.index_view.format) {
      cmd->setIndexBuffer(&d.index_view);
      st.index_view = d.index_view;
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
                       // Depth first, pipeline only to break ties.
                       //
                       // Pipeline-major was the first attempt and it is worse
                       // than useless: it scatters near and far geometry across
                       // pipeline groups, so the tiler's low-resolution Z never
                       // sees a near occluder before the far fragments it would
                       // reject. And it buys nothing here - the guest already
                       // submits pipeline-coherently, 14 binds for 166 opaque
                       // draws, so sorting on it is a no-op that measured
                       // byte-identical to not sorting at all.
                       if (a.depth != b.depth)
                         return a.depth < b.depth;
                       return a.pipeline < b.pipeline;
                     });
  }

  // Eye-major order for the side-by-side path: every left-eye draw, then every
  // right-eye draw, instead of alternating viewports on every draw. The queued
  // draws carry their viewport, so this is a stable sort on viewport.x and
  // changes nothing in the image - each eye's draws keep their submission
  // order. It removes ~1000 viewport and scissor changes from the scene pass,
  // which is one of the things that pass does that the passes the tiler does
  // bin do not.
  if (REXCVAR_GET(bd_draw_eye_major)) {
    std::stable_sort(g_queue.begin(), g_queue.end(),
                     [](const QueuedDraw &a, const QueuedDraw &b) {
                       const float ax = a.has_viewport ? a.viewport.x : -1.0f;
                       const float bx = b.has_viewport ? b.viewport.x : -1.0f;
                       return ax < bx;
                     });
  }

  // Recorded against emitted, once. A frame that records 800 and emits 800 has
  // a placement problem; one that emits fewer has a dropping problem, and the
  // two need completely different fixes.
  static u32 told = 0;
  static u32 emitted_total = 0;
  static u32 flushes = 0;
  ++flushes;
  emitted_total += static_cast<u32>(g_queue.size());
  if (told < 6 && flushes % 64 == 0) {
    ++told;
    BD_INFO("[draw-queue] flush #{}: {} draws now, {} emitted so far",
            flushes, g_queue.size(), emitted_total);
  }

  // Are these draws being emitted against the target they were recorded for?
  // "The draws execute and do no GPU work" is equally consistent with landing
  // on the wrong framebuffer and with being clipped away, and those need
  // opposite fixes.
  {
    static u32 told = 0;
    const void *live = Video::CurrentRenderTargetForDiag();
    u32 mismatched = 0;
    for (const QueuedDraw &d : g_queue)
      if (d.recorded_rt != live)
        ++mismatched;
    if (told < 6 && mismatched) {
      ++told;
      BD_INFO("[draw-queue] flushing {} draws, {} recorded against a DIFFERENT "
              "render target than the live one ({} vs {})",
              g_queue.size(), mismatched, g_queue.front().recorded_rt, live);
    }
  }

  // Did the sort actually change anything? pso_switches cannot answer this - it
  // is counted in FlushRenderState at record time and is identical however the
  // draws are later ordered. Count the binds that really happen, and the spread
  // of the depth keys, because a sort over a constant key is a no-op however
  // correct the comparator is.
  static u32 sort_told = 0;
  const bool report = sort_told < 4 && g_queue.size() > 100;

  EmitState st;
  u32 pipeline_binds = 0;
  const plume::RenderPipeline *prev = nullptr;
  float dmin = 1e30f, dmax = -1e30f;
  u32 opaque = 0;

  // Depth prepass: every draw that qualified is emitted first with colour
  // writes off, near to far (a depth-only draw has no ordering constraint, and
  // near-first is cheapest for the fine depth test too). The colour pass then
  // runs in submission order with depth writes off and a LEQUAL test, so a
  // fragment behind the nearest depth is rejected before it is shaded. The
  // scene pass measured ~7 ms per eye at 1376x720 on a Quest 2 with ~2x
  // overdraw shaded in full, because 64% of its draws blend and write depth
  // and that disables the tiler's low-resolution Z for the rest of the pass.
  u32 prepassed = 0;
  for (const QueuedDraw &d : g_queue)
    if (d.prepass_pipeline && d.color_pipeline)
      ++prepassed;
  if (prepassed) {
    static std::vector<const QueuedDraw *> order;
    order.clear();
    order.reserve(prepassed);
    for (const QueuedDraw &d : g_queue)
      if (d.prepass_pipeline && d.color_pipeline)
        order.push_back(&d);
    std::stable_sort(order.begin(), order.end(),
                     [](const QueuedDraw *a, const QueuedDraw *b) {
                       if (a->depth != b->depth)
                         return a->depth < b->depth;
                       return a->prepass_pipeline < b->prepass_pipeline;
                     });
    for (const QueuedDraw *p : order) {
      QueuedDraw d = *p;
      d.pipeline = d.prepass_pipeline;
      if (d.pipeline != prev) { ++pipeline_binds; prev = d.pipeline; }
      EmitOne(cmd, d, st);
    }
    static u32 told = 0;
    if (told < 3 && g_queue.size() > 100) {
      ++told;
      BD_INFO("[draw-queue] depth prepass: {} of {} draws", prepassed,
              g_queue.size());
    }
  }

  // Probe: end and reopen the render pass every N draws. Adreno runs the
  // 500-draw scene pass in direct (non-tiled) mode while a small pass on the
  // same kind of surface bins; if the trigger is the size of the pass, the
  // chunks will bin, at a tile load and store per split (~1 ms at 1376x720).
  const i32 split_every = REXCVAR_GET(bd_pass_split_draws);
  u32 since_split = 0;
  for (const QueuedDraw &q : g_queue) {
    const bool two_pass = q.prepass_pipeline && q.color_pipeline;
    QueuedDraw d = q;
    if (two_pass)
      d.pipeline = q.color_pipeline;
    if (split_every > 0 && since_split >= static_cast<u32>(split_every)) {
      cmd->setFramebuffer(nullptr);
      st.framebuffer = nullptr;
      since_split = 0;
    }
    ++since_split;
    if (d.pipeline != prev) { ++pipeline_binds; prev = d.pipeline; }
    if (!d.blended) {
      ++opaque;
      if (d.depth < dmin) dmin = d.depth;
      if (d.depth > dmax) dmax = d.depth;
    }
    EmitOne(cmd, d, st);
  }
  if (report) {
    ++sort_told;
    BD_INFO("[draw-queue] {} draws, {} opaque, {} pipeline binds, depth {:.0f}"
            "..{:.0f}", g_queue.size(), opaque, pipeline_binds,
            opaque ? dmin : 0.0f, opaque ? dmax : 0.0f);
  }
  g_queue.clear();
}

} // namespace bd::gpu
