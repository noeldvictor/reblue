/**
 * @file    gpu/draw_queue.h
 * @brief   Deferred draw submission: collect a render pass's draws, then emit
 *          them sorted by pipeline and front-to-back.
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 * @license     BSD 3-Clause - see LICENSE
 */
#pragma once

#include <vector>

#include <plume_render_interface.h>
#include <rex/types.h>

namespace bd::gpu {

// Blue Dragon submits about a thousand individually placed scene nodes a frame,
// in whatever order the guest's traversal produced, because on a Xenon the
// command processor made a draw call nearly free. Nothing in this renderer
// sorted them: ~114 pipeline switches against ~553 draws, and no depth order at
// all, so Adreno's low-resolution Z has no chance to reject a hidden fragment
// before shading it.
//
// This is the seam that fixes both. Every guest draw funnels through
// DispatchDraw, and after the constant rewrite a fully resolved draw is small:
// a pipeline, three dynamic uniform buffer offsets - the shared block carries
// the texture and sampler descriptor indices, so the material rides along - a
// vertex and index binding, and the draw parameters. That is cheap to record
// and cheap to replay.
//
// Deliberately not a render graph. It collects within one render pass and
// flushes when that pass ends, which is where the barriers already are.
struct QueuedDraw {
  plume::RenderPipeline *pipeline = nullptr;

  // Base offsets for the vertex, pixel and shared guest constant blocks. These
  // are the whole per-draw material: transform, material parameters, and the
  // descriptor indices for every texture and sampler the draw reads.
  u32 constant_offsets[3]{};

  // The vertex stream binding as FlushRenderState resolved it. Copied by value
  // because the guest overwrites its own views between draws.
  plume::RenderVertexBufferView vertex_views[16]{};
  // By value, like the views. Held as a pointer into VideoState first time
  // round, which meant a replayed draw read whatever the guest had left there
  // rather than what it was recorded with.
  plume::RenderInputSlot input_slots[16]{};
  u32 vertex_first = 0;
  u32 vertex_count = 0;

  plume::RenderIndexBufferView index_view{plume::RenderBufferReference{}, 0,
                                          plume::RenderFormat::R16_UINT};
  bool has_index_buffer = false;

  bool indexed = false;
  u32 count = 0;
  u32 start_index = 0;
  i32 base_vertex = 0;
  u32 start_vertex = 0;

  // Sort keys. `depth` is a view-space distance for front-to-back ordering;
  // `sequence` is submission order and is what alpha-blended draws are kept in,
  // because reordering those changes the image.
  float depth = 0.0f;
  u32 sequence = 0;
  bool blended = false;
};

// Recording. Returns false when deferral is off, in which case the caller
// submits immediately as before.
bool DrawQueueEnabled();

// Record a resolved draw instead of emitting it.
void DrawQueuePush(const QueuedDraw &draw);

// Emit everything recorded, sorted, and clear. Called where a render pass ends:
// a framebuffer change, a barrier, or present. Safe to call when empty.
void DrawQueueFlush(plume::RenderCommandList *cmd);

// Draws currently held, for the per-frame counters.
u32 DrawQueueDepth();

// Drop anything still queued at end of frame, loudly. Reaching present with a
// non-empty queue means a pass ended somewhere that does not flush; emitting
// the draws there is not a repair, because the framebuffer and pipeline layout
// they were recorded against are gone.
void DrawQueueDiscardStragglers();

} // namespace bd::gpu
