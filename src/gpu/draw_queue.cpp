/**
 * @file    gpu/draw_queue.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/draw_queue.h"
#include "gpu/frame_stats.h"
#include "gpu/frag_census.h"
#include "gpu/vertex_pull.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <xxhash.h>

#include "core/logging.h"
#include "gpu/backend.h"
#include "gpu/constant_buffers.h"
#include "gpu/device.h"
#include "gpu/scene/host_draw.h"

REXCVAR_DECLARE(bool, bd_draw_defer);
REXCVAR_DECLARE(bool, bd_draw_sort);
REXCVAR_DECLARE(bool, bd_draw_eye_major);
REXCVAR_DECLARE(i32, bd_pass_split_draws);
REXCVAR_DECLARE(bool, bd_draw_instancing);
REXCVAR_DECLARE(bool, bd_draw_pull);
REXCVAR_DECLARE(bool, bd_draw_indirect);
REXCVAR_DECLARE(bool, bd_record_mask);
REXCVAR_DECLARE(i32, bd_debug_bisect_windows);
REXCVAR_DECLARE(i32, bd_debug_bisect_frames);
REXCVAR_DECLARE(i32, bd_debug_bisect_span);
REXCVAR_DECLARE(i32, bd_record_mask_mode);
REXCVAR_DECLARE(bool, bd_draw_instancing_reorder);
REXCVAR_DECLARE(bool, bd_draw_instancing_singles_plain);

namespace bd::gpu {

namespace {

// One pass's worth. Reserved once and reused for the life of the process: a
// field frame records a few hundred of these and reallocating inside the
// submission path would be a new per-draw cost in the middle of removing one.
std::vector<QueuedDraw> g_queue;
u32 g_sequence = 0;
u32 g_pulled_draws = 0; // per flush, reported with the instancing line
u32 g_indirect_calls = 0, g_indirect_draws = 0;

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
  bool dummy_bound = false; // the pulled path's slot-15 binding
};

// instance_count / first_instance: the instanced group this draw stands for
// (1 and 0 for a plain draw). d.pipeline is whichever variant the caller chose.
// The state a draw needs bound, without the draw: false when the draw
// cannot be issued at all.
bool EmitBindings(plume::RenderCommandList *cmd, const QueuedDraw &d,
                  EmitState &st) {
  // Its own framebuffer, always. Whatever is bound at flush time is not
  // necessarily what this draw was recorded against, and may be nothing at all.
  if (!d.framebuffer) {
    static u32 told = 0;
    if (told++ < 8)
      BD_ERROR("[draw-queue] queued draw with no framebuffer, skipped");
    return false;
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
    return false;
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
  // A pulled draw binds no streams: its pipeline's input layout reads slot
  // 15 alone, from the 64-byte dummy, and the shader pulls the real
  // attributes from the record's streams.
  const bool pulled = d.pulled_pipeline && d.pipeline == d.pulled_pipeline;
  if (pulled) {
    if (!st.dummy_bound) {
      cmd->setVertexBuffers(15, VertexPullDummyView(), 1, VertexPullDummySlot());
      st.dummy_bound = true;
    }
  } else {
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
      // A stream bound over slot 15 invalidates the dummy binding.
      if (run > 15u)
        st.dummy_bound = false;
      i = run;
    }
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
  return true;
}

void EmitOne(plume::RenderCommandList *cmd, const QueuedDraw &d,
             EmitState &st, u32 instance_count = 1, u32 first_instance = 0) {
  if (!EmitBindings(cmd, d, st))
    return;

  // Fragment census: every draw's fragment shader invocations, folded per
  // pixel shader at readback (bd_frag_census).
  const bool counted = FragCensusBegin(
      cmd, d.ps_hash, d.visual_va, d.render_view,
      FragCensusFlags(d.blended, d.tex_opaque, d.zwrite, d.tex_slot0_only));
  if (d.indexed)
    cmd->drawIndexedInstanced(d.count, instance_count, d.start_index,
                              d.base_vertex, first_instance);
  else
    cmd->drawInstanced(d.count, instance_count, d.start_vertex,
                       first_instance);
  if (counted)
    FragCensusEnd(cmd);
}

// Everything two draws must share to be one instanced draw. The vertex
// constant offset is not in it: an instanced draw reads its whole vertex
// block from its record. The pixel and shared offsets are, and equal offsets
// mean equal content (constant_buffers.cpp keys every upload by content).
u64 GroupKey(const QueuedDraw &d) {
  struct Blob {
    const void *pipeline;
    const void *framebuffer;
    float viewport[6];
    i32 scissor[4];
    const void *index_ref;
    u64 index_offset;
    u32 index_size;
    u32 index_format;
    u32 constant_offsets[3];
    u32 indexed, count, start_index, start_vertex;
    i32 base_vertex;
    u32 vertex_first, vertex_count;
    struct {
      const void *ref;
      u64 offset;
      u32 size;
      u32 stride;
    } streams[16];
  } b;
  std::memset(&b, 0, sizeof(b));
  b.pipeline = d.instanced_pipeline;
  b.framebuffer = d.framebuffer;
  if (d.has_viewport) {
    b.viewport[0] = d.viewport.x;
    b.viewport[1] = d.viewport.y;
    b.viewport[2] = d.viewport.width;
    b.viewport[3] = d.viewport.height;
    b.viewport[4] = d.viewport.minDepth;
    b.viewport[5] = d.viewport.maxDepth;
    b.scissor[0] = d.scissor.left;
    b.scissor[1] = d.scissor.top;
    b.scissor[2] = d.scissor.right;
    b.scissor[3] = d.scissor.bottom;
  }
  if (d.has_index_buffer) {
    b.index_ref = d.index_view.buffer.ref;
    b.index_offset = d.index_view.buffer.offset;
    b.index_size = d.index_view.size;
    b.index_format = static_cast<u32>(d.index_view.format);
  }
  b.constant_offsets[0] = 0;
  b.constant_offsets[1] = d.constant_offsets[1];
  b.constant_offsets[2] = d.constant_offsets[2];
  b.indexed = d.indexed;
  b.count = d.count;
  b.start_index = d.start_index;
  b.start_vertex = d.start_vertex;
  b.base_vertex = d.base_vertex;
  b.vertex_first = d.vertex_first;
  b.vertex_count = d.vertex_count;
  const u32 end = std::min<u32>(d.vertex_first + d.vertex_count, 16u);
  for (u32 i = d.vertex_first; i < end; ++i) {
    b.streams[i].ref = d.vertex_views[i].buffer.ref;
    b.streams[i].offset = d.vertex_views[i].buffer.offset;
    b.streams[i].size = d.vertex_views[i].size;
    b.streams[i].stride = d.input_slots[i].stride;
  }
  return XXH3_64bits(&b, sizeof(b));
}

} // namespace

bool DrawQueueEnabled() { return REXCVAR_GET(bd_draw_defer); }

// bd_debug_bisect_windows: the draw bisector. Each frame's queued draws are
// numbered in push order; the frame's draws in window w of N (each
// bd_debug_bisect_span / N draws wide) are dropped, and w advances every
// bd_debug_bisect_frames frames. A capture sequence over the run shows which
// window removes an artefact, and the draw ledger names the draws in it
// (2026-09-03, for the cyan polygon RenderDoc never captures).
static bool BisectDrops(u32 &index_in_frame) {
  const i32 windows = REXCVAR_GET(bd_debug_bisect_windows);
  static u32 last_frame = ~0u, count = 0, last_window = ~0u;
  const u32 frame = FrameStatFrameCount();
  if (frame != last_frame) {
    last_frame = frame;
    count = 0;
  }
  index_in_frame = count++;
  if (windows <= 0)
    return false;
  const u32 frames = static_cast<u32>(std::max(1, REXCVAR_GET(bd_debug_bisect_frames)));
  const u32 span = static_cast<u32>(std::max(1, REXCVAR_GET(bd_debug_bisect_span)));
  const u32 w = (frame / frames) % static_cast<u32>(windows);
  const u32 size = std::max(1u, span / static_cast<u32>(windows));
  const u32 lo = w * size, hi = lo + size;
  if (w != last_window) {
    last_window = w;
    BD_INFO("[bisect] frame {}: window {} of {} drops draws [{}, {})", frame, w,
            windows, lo, hi);
  }
  return index_in_frame >= lo && index_in_frame < hi;
}

void DrawQueuePush(const QueuedDraw &draw) {
  u32 index_in_frame = 0;
  if (BisectDrops(index_in_frame))
    return;
  if (g_queue.capacity() == 0)
    g_queue.reserve(4096);
  g_queue.push_back(draw);
  QueuedDraw &q = g_queue.back();
  q.sequence = g_sequence++;
  // Computed here, after the caller filled in the draw parameters and the
  // eye viewport, and once rather than per comparison in the flush.
  q.group_key = (q.instanced_pipeline && q.record_index != ~0u) ? GroupKey(q)
                                                                : 0;
  q.batch_key = 0;
  if (q.pulled_pipeline && q.record_index != ~0u && q.indexed &&
      q.has_index_buffer) {
    struct B {
      const void *pipeline, *ib, *fb;
      u32 ps, shared, ib_fmt;
      plume::RenderViewport vp;
      plume::RenderRect sc;
      u8 has_vp;
    } b;
    std::memset(&b, 0, sizeof(b));
    b.pipeline = q.pulled_pipeline;
    b.ib = q.index_view.buffer.ref;
    b.fb = q.framebuffer;
    b.ps = q.constant_offsets[1];
    b.shared = q.constant_offsets[2];
    b.ib_fmt = u32(q.index_view.format);
    b.vp = q.viewport;
    b.sc = q.scissor;
    b.has_vp = q.has_viewport ? 1 : 0;
    q.batch_key = XXH3_64bits(&b, sizeof(b)) | 1ull;
  }
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

void DrawQueueFlushAt(plume::RenderCommandList *cmd, const char *site) {
  if (!g_queue.empty() && bd::gpu::scene::HostDrawReplaying()) {
    static u32 told = 0;
    if (told++ < 6)
      BD_INFO("[draw-queue] flush of {} draws during a host-issued node draw, from {}",
              g_queue.size(), site);
  }
  DrawQueueFlush(cmd);
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

  // Instancing: inside every run of consecutive order-independent draws,
  // bring the draws that can share an instanced draw together - by
  // pipeline, then group key, then submission order. A draw that has to keep
  // its place (blended, depth test off, stencil) bounds the runs, so nothing
  // is ever moved across it. The merge below only joins *consecutive* equal
  // keys, so this is what turns "same mesh, ten nodes apart" into one draw.
  const bool instancing = REXCVAR_GET(bd_draw_instancing);
  if (instancing && REXCVAR_GET(bd_draw_instancing_reorder)) {
    size_t i = 0;
    while (i < g_queue.size()) {
      if (!g_queue[i].reorderable) {
        ++i;
        continue;
      }
      size_t j = i + 1;
      while (j < g_queue.size() && g_queue[j].reorderable)
        ++j;
      if (j - i > 1) {
        std::stable_sort(
            g_queue.begin() + i, g_queue.begin() + j,
            [](const QueuedDraw &a, const QueuedDraw &b) {
              const void *pa = a.instanced_pipeline ? a.instanced_pipeline
                                                    : a.pipeline;
              const void *pb = b.instanced_pipeline ? b.instanced_pipeline
                                                    : b.pipeline;
              if (pa != pb)
                return pa < pb;
              if (a.batch_key != b.batch_key)
                return a.batch_key < b.batch_key;
              if (a.group_key != b.group_key)
                return a.group_key < b.group_key;
              return a.sequence < b.sequence;
            });
      }
      i = j;
    }
  }

  // Probe: end and reopen the render pass every N draws. Adreno runs the
  // 500-draw scene pass in direct (non-tiled) mode while a small pass on the
  // same kind of surface bins; if the trigger is the size of the pass, the
  // chunks will bin, at a tile load and store per split (~1 ms at 1376x720).
  const i32 split_every = REXCVAR_GET(bd_pass_split_draws);
  u32 since_split = 0;
  u32 groups = 0, grouped_draws = 0, emitted = 0;
  static std::vector<u32> records;
  for (size_t i = 0; i < g_queue.size();) {
    const QueuedDraw &q = g_queue[i];
    if (split_every > 0 && since_split >= static_cast<u32>(split_every)) {
      cmd->setFramebuffer(nullptr);
      st.framebuffer = nullptr;
      since_split = 0;
    }
    ++since_split;

    // A run of consecutive draws sharing this one's group key becomes one
    // instanced draw; its records are committed to the GPU contiguously, in
    // this order, so firstInstance + SV_InstanceID walks them.
    // Indirect: the run of draws sharing this one's batch key becomes one
    // drawIndexedIndirect - a command per instancing group inside it, its
    // records committed contiguously so firstInstance walks them, the index
    // buffer bound once at offset zero with each command's firstIndex
    // carrying the mesh's own offset.
    if (instancing && REXCVAR_GET(bd_draw_indirect) && q.batch_key &&
        q.pulled_pipeline && VertexPullIndirectOK()) {
      size_t j = i + 1;
      // Mode 5 (diagnostic): a batch is one group.
      const bool one_group = REXCVAR_GET(bd_record_mask_mode) == 5;
      while (j < g_queue.size() && g_queue[j].batch_key == q.batch_key &&
             g_queue[j].pulled_pipeline == q.pulled_pipeline &&
             g_queue[j].record_index != ~0u &&
             (!one_group || g_queue[j].group_key == q.group_key))
        ++j;
      // The groups inside the batch, each one command.
      static std::vector<std::pair<size_t, size_t>> spans;
      spans.clear();
      records.clear();
      for (size_t k = i; k < j;) {
        size_t e = k + 1;
        while (e < j && g_queue[e].group_key == g_queue[k].group_key)
          ++e;
        spans.emplace_back(k, e);
        for (size_t r = k; r < e; ++r)
          records.push_back(g_queue[r].record_index);
        k = e;
      }
      u64 byte_offset = 0;
      IndirectCommand *cmds =
          VertexPullAllocIndirect(static_cast<u32>(spans.size()), byte_offset);
      // Mode 4: the batch's records carry their whole block (diagnostic).
      const u32 first =
          cmds ? CommitInstanceRecords(records.data(),
                                       static_cast<u32>(records.size()),
                                       REXCVAR_GET(bd_record_mask_mode) != 4,
                                       REXCVAR_GET(bd_record_mask_mode) != 9)
               : ~0u;
      if (cmds && first != ~0u) {
        const u32 index_bytes =
            q.index_view.format == plume::RenderFormat::R32_UINT ? 4u : 2u;
        u32 running = 0;
        for (size_t c = 0; c < spans.size(); ++c) {
          const QueuedDraw &g = g_queue[spans[c].first];
          const u32 n = static_cast<u32>(spans[c].second - spans[c].first);
          cmds[c].index_count = g.count;
          cmds[c].instance_count = n;
          cmds[c].first_index =
              static_cast<u32>(g.index_view.buffer.offset / index_bytes) +
              g.start_index;
          cmds[c].vertex_offset = g.base_vertex;
          cmds[c].first_instance = first + running;
          running += n;
          if (!g.blended) {
            opaque += n;
            if (g.depth < dmin) dmin = g.depth;
            if (g.depth > dmax) dmax = g.depth;
          }
        }
        QueuedDraw d = q;
        d.pipeline = q.pulled_pipeline;
        d.index_view.buffer.offset = 0;
        d.index_view.size = ~0u;
        if (REXCVAR_GET(bd_record_mask) && REXCVAR_GET(bd_record_mask_mode) != 3) {
          const u32 base_off = UploadVertexBlockFromStaged(records[0]);
          if (base_off != ~0u)
            d.constant_offsets[0] = base_off;
        }
        if (d.pipeline != prev) { ++pipeline_binds; prev = d.pipeline; }
        if (EmitBindings(cmd, d, st)) {
          // The census counts the batch under its first draw's visual; the
          // batch shares the pixel shader, the view and the blend state.
          const bool counted = FragCensusBegin(
              cmd, q.ps_hash, q.visual_va, q.render_view,
              FragCensusFlags(q.blended, q.tex_opaque, q.zwrite, q.tex_slot0_only));
          cmd->drawIndexedIndirect(VertexPullIndirectBuffer(), byte_offset,
                                   static_cast<u32>(spans.size()),
                                   sizeof(IndirectCommand));
          if (counted)
            FragCensusEnd(cmd);
          ++g_indirect_calls;
          g_indirect_draws += static_cast<u32>(j - i);
          g_pulled_draws += static_cast<u32>(spans.size());
          emitted += static_cast<u32>(spans.size());
          for (const auto &sp : spans)
            if (sp.second - sp.first > 1) {
              ++groups;
              grouped_draws += static_cast<u32>(sp.second - sp.first);
            }
        }
        i = j;
        continue;
      }
    }
    if (instancing && q.instanced_pipeline && q.record_index != ~0u) {
      size_t j = i + 1;
      while (j < g_queue.size() && g_queue[j].instanced_pipeline &&
             g_queue[j].record_index != ~0u &&
             g_queue[j].group_key == q.group_key)
        ++j;
      const u32 n = static_cast<u32>(j - i);
      // A group of one goes through the plain pipeline with its record
      // uploaded as an ordinary window: on Adreno the instanced variant's
      // storage-buffer constant reads cost more than the window re-base they
      // save (Quest 2, 2026-09-02: 45 ms GPU against 37.5 with every scene
      // draw instanced). Only a real group pays for the record path.
      if (n == 1 && REXCVAR_GET(bd_draw_instancing_singles_plain)) {
        const u32 off = UploadVertexBlockFromStaged(q.record_index);
        if (off != ~0u) {
          QueuedDraw d = q;
          d.constant_offsets[0] = off;
          if (d.pipeline != prev) { ++pipeline_binds; prev = d.pipeline; }
          if (!d.blended) {
            ++opaque;
            if (d.depth < dmin) dmin = d.depth;
            if (d.depth > dmax) dmax = d.depth;
          }
          EmitOne(cmd, d, st);
          ++emitted;
          i = j;
          continue;
        }
      }
      records.clear();
      for (size_t k = i; k < j; ++k)
        records.push_back(g_queue[k].record_index);
      // One-shot: a group whose members share a record index or whose
      // records carry the same world rows draws one node twice and another
      // not at all (the vanishing rock, 2026-09-02). Name the first few.
      if (n > 1) {
        static u32 told = 0;
        bool dup_index = false, dup_world = false;
        for (u32 a = 0; a < n && !dup_index; ++a)
          for (u32 b = a + 1; b < n; ++b)
            if (records[a] == records[b]) { dup_index = true; break; }
        for (u32 a = 0; a < n && !dup_world; ++a) {
          const InstanceRecord *ra = StagedInstanceRecord(records[a]);
          for (u32 b = a + 1; ra && b < n; ++b) {
            const InstanceRecord *rb = StagedInstanceRecord(records[b]);
            if (rb && std::memcmp(ra->regs + 20 * 4, rb->regs + 20 * 4, 64) == 0) {
              dup_world = true;
              break;
            }
          }
        }
        if ((dup_index || dup_world) && told++ < 6) {
          std::string idx;
          for (u32 k = 0; k < n; ++k)
            idx += fmt::format(" {}", records[k]);
          BD_INFO("[draw-queue] group of {} with {}{}: records{} (seq {}..{})",
                  n, dup_index ? "a shared record index" : "",
                  dup_world ? " identical world rows" : "", idx,
                  g_queue[i].sequence, g_queue[j - 1].sequence);
        }
      }
      const u32 first = CommitInstanceRecords(records.data(), n);
      if (first != ~0u) {
        QueuedDraw d = q;
        d.pipeline = q.instanced_pipeline;
        // The group's uniform window is its first record's block: the
        // records' masks were computed against it (bd_record_mask).
        if (REXCVAR_GET(bd_record_mask) && REXCVAR_GET(bd_record_mask_mode) != 3) {
          const u32 base_off = UploadVertexBlockFromStaged(records[0]);
          if (base_off != ~0u)
            d.constant_offsets[0] = base_off;
        }
        // Pulled when every draw of the group staged its pull info (the
        // group key already fixes the pipeline, so one check per group).
        if (REXCVAR_GET(bd_draw_pull) && q.pulled_pipeline) {
          bool all_pulled = true;
          for (size_t k = i; k < j && all_pulled; ++k)
            all_pulled = g_queue[k].pulled_pipeline == q.pulled_pipeline;
          if (all_pulled) {
            d.pipeline = q.pulled_pipeline;
            ++g_pulled_draws;
          }
        }
        if (d.pipeline != prev) { ++pipeline_binds; prev = d.pipeline; }
        if (!d.blended) {
          opaque += n;
          if (d.depth < dmin) dmin = d.depth;
          if (d.depth > dmax) dmax = d.depth;
        }
        EmitOne(cmd, d, st, n, first);
        ++emitted;
        if (n > 1) {
          ++groups;
          grouped_draws += n;
        }
        i = j;
        continue;
      }
      // Out of GPU records (CommitInstanceRecords said so): the plain
      // pipeline below reads whatever the uniform block holds, which for a
      // draw that expected the record may be another node's transform.
    }

    const bool two_pass = q.prepass_pipeline && q.color_pipeline;
    QueuedDraw d = q;
    if (two_pass)
      d.pipeline = q.color_pipeline;
    if (d.pipeline != prev) { ++pipeline_binds; prev = d.pipeline; }
    if (!d.blended) {
      ++opaque;
      if (d.depth < dmin) dmin = d.depth;
      if (d.depth > dmax) dmax = d.depth;
    }
    EmitOne(cmd, d, st);
    ++emitted;
    ++i;
  }
  if (report) {
    ++sort_told;
    BD_INFO("[draw-queue] {} draws, {} opaque, {} pipeline binds, depth {:.0f}"
            "..{:.0f}", g_queue.size(), opaque, pipeline_binds,
            opaque ? dmin : 0.0f, opaque ? dmax : 0.0f);
  }
  // Which key component keeps draws apart. Once, on a scene-sized flush:
  // distinct values of the mesh alone, then with the pipeline, the material
  // offsets, the vertex offset and the viewport added in turn. The first
  // count that jumps is the component that is not shared between nodes.
  {
    static u32 told = 0, since = 400;
    ++since;
    if (instancing && told < 3 && since >= 400 && g_queue.size() > 200) {
      ++told;
      since = 0;
      auto mesh_key = [](const QueuedDraw &d) {
        struct M { const void *ib; u64 ib_off; u32 ib_size; u32 count, start; i32 base;
                   const void *vb[16]; u64 vb_off[16]; u32 vb_size[16]; } m;
        std::memset(&m, 0, sizeof(m));
        m.ib = d.index_view.buffer.ref; m.ib_off = d.index_view.buffer.offset;
        m.ib_size = d.index_view.size; m.count = d.count; m.start = d.start_index;
        m.base = d.base_vertex;
        const u32 end = std::min<u32>(d.vertex_first + d.vertex_count, 16u);
        for (u32 i = d.vertex_first; i < end; ++i) {
          m.vb[i] = d.vertex_views[i].buffer.ref; m.vb_off[i] = d.vertex_views[i].buffer.offset;
          m.vb_size[i] = d.vertex_views[i].size;
        }
        return XXH3_64bits(&m, sizeof(m));
      };
      std::vector<u64> k0, k1, k2, k3, k4, k5;
      u32 eligible = 0, reorder = 0;
      for (const QueuedDraw &d : g_queue) {
        if (d.reorderable) ++reorder;
        if (!(d.instanced_pipeline && d.record_index != ~0u)) continue;
        ++eligible;
        const u64 m = mesh_key(d);
        k0.push_back(m);
        k1.push_back(m ^ (u64(uintptr_t(d.instanced_pipeline)) * 0x9E3779B97F4A7C15ull));
        k2.push_back(k1.back() ^ (u64(d.constant_offsets[2]) * 0xC2B2AE3D27D4EB4Full));
        k3.push_back(k2.back() ^ (u64(d.constant_offsets[1]) * 0x165667B19E3779F9ull));
        k4.push_back(k3.back() ^ (u64(d.constant_offsets[0]) * 0x27D4EB2F165667C5ull));
        k5.push_back(d.group_key);
      }
      auto distinct = [](std::vector<u64> v) {
        std::sort(v.begin(), v.end());
        return static_cast<u32>(std::unique(v.begin(), v.end()) - v.begin());
      };
      // Indirect draws (2026-09-03): how many draws of this flush one
      // drawIndexedIndirect could cover. A group shares the instanced
      // pipeline, the material constants (PS, shared), the index buffer and
      // format, every stream's buffer and stride, and the pass geometry; the
      // commands differ in first index, index count and a vertex offset. A
      // draw is "commandable" when every one of its streams sits at the
      // group's base offset plus the same whole number of strides.
      {
        struct Coarse {
          const void *pipeline; u32 ps, shared; const void *ib; u32 ib_fmt;
          const void *vb[16]; u32 stride[16]; u32 first, count; float vp[6]; i32 sc[4];
        };
        struct Group { u64 key; u64 base_off[16]; u32 draws = 0, ok = 0; };
        std::vector<std::pair<u64, const QueuedDraw *>> keyed;
        for (const QueuedDraw &d : g_queue) {
          if (!(d.instanced_pipeline && d.record_index != ~0u && d.indexed)) continue;
          Coarse c{}; std::memset(&c, 0, sizeof(c));
          c.pipeline = d.instanced_pipeline; c.ps = d.constant_offsets[1];
          c.shared = d.constant_offsets[2]; c.ib = d.index_view.buffer.ref;
          c.ib_fmt = u32(d.index_view.format); c.first = d.vertex_first; c.count = d.vertex_count;
          const u32 end = std::min<u32>(d.vertex_first + d.vertex_count, 16u);
          for (u32 i = d.vertex_first; i < end; ++i) {
            c.vb[i] = d.vertex_views[i].buffer.ref; c.stride[i] = d.input_slots[i].stride;
          }
          c.vp[0] = d.viewport.x; c.vp[1] = d.viewport.y; c.vp[2] = d.viewport.width;
          c.vp[3] = d.viewport.height; c.vp[4] = d.viewport.minDepth; c.vp[5] = d.viewport.maxDepth;
          c.sc[0] = d.scissor.left; c.sc[1] = d.scissor.top; c.sc[2] = d.scissor.right; c.sc[3] = d.scissor.bottom;
          keyed.emplace_back(XXH3_64bits(&c, sizeof(c)), &d);
        }
        std::sort(keyed.begin(), keyed.end(),
                  [](const auto &a, const auto &b) { return a.first < b.first; });
        u32 groups = 0, commandable = 0, multi_groups = 0, multi_draws = 0;
        size_t i = 0;
        while (i < keyed.size()) {
          size_t j = i;
          while (j < keyed.size() && keyed[j].first == keyed[i].first) ++j;
          ++groups;
          if (j - i > 1) { ++multi_groups; multi_draws += u32(j - i); }
          // The group's base: the smallest offset per stream.
          u64 base[16]; for (auto &b : base) b = ~0ull;
          for (size_t k = i; k < j; ++k) {
            const QueuedDraw &d = *keyed[k].second;
            const u32 end = std::min<u32>(d.vertex_first + d.vertex_count, 16u);
            for (u32 st = d.vertex_first; st < end; ++st)
              base[st] = std::min<u64>(base[st], d.vertex_views[st].buffer.offset);
          }
          for (size_t k = i; k < j; ++k) {
            const QueuedDraw &d = *keyed[k].second;
            const u32 end = std::min<u32>(d.vertex_first + d.vertex_count, 16u);
            bool ok = true; i64 quot = -1;
            for (u32 st = d.vertex_first; st < end && ok; ++st) {
              const u32 stride = d.input_slots[st].stride;
              const u64 rel = d.vertex_views[st].buffer.offset - base[st];
              if (!stride || rel % stride) { ok = false; break; }
              const i64 q = i64(rel / stride);
              if (quot < 0) quot = q; else if (q != quot) ok = false;
            }
            if (ok) ++commandable;
          }
          i = j;
        }
        BD_INFO("[draw-queue] indirect diag: {} indexed instanced draws in {} "
                "coarse groups ({} groups of >1 covering {} draws); {} draws "
                "commandable by vertexOffset",
                keyed.size(), groups, multi_groups, multi_draws, commandable);
      }
      BD_INFO("[draw-queue] instancing diag: {} draws, {} eligible, {} "
              "reorderable; distinct mesh {}, +pipeline {}, +shared {}, +ps {}, "
              "+vs {}, full key {}",
              g_queue.size(), eligible, reorder, distinct(k0), distinct(k1),
              distinct(k2), distinct(k3), distinct(k4), distinct(k5));
      // Which vertex registers keep same-mesh, same-material draws apart:
      // for every pair that shares everything but the vertex offset, count
      // the registers whose 16 bytes differ between the two blocks.
      {
        std::vector<std::pair<u64, const QueuedDraw *>> by_mat;
        for (const QueuedDraw &d : g_queue) {
          if (!(d.instanced_pipeline && d.record_index != ~0u)) continue;
          u64 k = mesh_key(d) ^ (u64(uintptr_t(d.instanced_pipeline)) * 0x9E3779B97F4A7C15ull);
          k ^= u64(d.constant_offsets[2]) * 0xC2B2AE3D27D4EB4Full;
          k ^= u64(d.constant_offsets[1]) * 0x165667B19E3779F9ull;
          by_mat.emplace_back(k, &d);
        }
        std::sort(by_mat.begin(), by_mat.end(),
                  [](const auto &a, const auto &b) { return a.first < b.first; });
        u32 hist[256] = {};
        u32 pairs = 0;
        for (size_t a = 0; a + 1 < by_mat.size(); ++a) {
          if (by_mat[a].first != by_mat[a + 1].first) continue;
          const QueuedDraw *x = by_mat[a].second, *y = by_mat[a + 1].second;
          if (x->constant_offsets[0] == y->constant_offsets[0]) continue;
          const u8 *bx = ConstantBlockBytes(x->constant_offsets[0]);
          const u8 *by = ConstantBlockBytes(y->constant_offsets[0]);
          if (!bx || !by) continue;
          ++pairs;
          for (u32 r = 0; r < 256; ++r)
            if (std::memcmp(bx + r * 16, by + r * 16, 16) != 0) ++hist[r];
        }
        std::string regs;
        for (u32 r = 0; r < 256; ++r)
          if (hist[r]) regs += " c" + std::to_string(r) + ":" + std::to_string(hist[r]);
        BD_INFO("[draw-queue] instancing diag: {} same-mesh-and-material pairs "
                "with different vertex blocks; differing registers:{}",
                pairs, regs.empty() ? " none" : regs);
      }
    }
  }
  // The instancing tally, averaged over ~5 s of scene-sized flushes: how
  // many draws the queue took in, how many it issued, and how many of those
  // issues carried more than one node.
  if (instancing && g_queue.size() > 100) {
    static u32 n_flush = 0, acc_in = 0, acc_out = 0, acc_groups = 0,
               acc_grouped = 0;
    ++n_flush;
    acc_in += static_cast<u32>(g_queue.size());
    acc_out += emitted;
    acc_groups += groups;
    acc_grouped += grouped_draws;
    if (n_flush == 300) {
      BD_INFO("[draw-queue] instancing: {} draws in -> {} issued per flush; "
              "{} groups of >1 covering {} draws; {} issued pulled; indirect: "
              "{} draws in {} calls",
              acc_in / n_flush, acc_out / n_flush, acc_groups / n_flush,
              acc_grouped / n_flush, g_pulled_draws / n_flush,
              g_indirect_draws / n_flush, g_indirect_calls / n_flush);
      n_flush = acc_in = acc_out = acc_groups = acc_grouped = 0;
      g_pulled_draws = g_indirect_calls = g_indirect_draws = 0;
    }
  }
  g_queue.clear();
}

} // namespace bd::gpu
