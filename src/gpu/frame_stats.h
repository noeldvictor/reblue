/**
 * @file    gpu/frame_stats.h
 * @brief   Per-present render thread frame statistics for the F3 debug menu.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/types.h>

namespace bd::gpu {

struct FrameStats {
  f64 frame_time_ms = 0.0;
  f64 fps = 0.0;
  f64 logic_tps = 0.0;
  u64 frame_count = 0;
  f64 gpu_wait_ms = 0.0; // CPU time blocked on the GPU fence each frame
  u32 draws = 0;
};

// EMA of the per-frame CPU block on the GPU fence (AdvanceAndWaitReused). Large
// => GPU-bound, near-zero => CPU/record-bound.
void RecordGPUWait(f64 ms);

// Wall time Present() spent blocked in each phase. What is left over (guest sim
// and command recording) becomes PerfSample::other_ms.
struct PresentBreakdown {
  f64 acquire_ms = 0.0; // swapchain acquireTexture
  f64 submit_ms = 0.0;  // executeCommandLists
  f64 present_ms = 0.0; // swapchain present (blocks under FIFO/vsync)
  f64 fence_ms = 0.0;   // wait on the reused slot's fence (GPU depth)
  f64 drain_ms = 0.0;   // deferred destroys
  f64 pace_ms = 0.0;    // bd_fps_limit sleep
};

// Once per presented frame, after PaceFrame.
void RecordFrameSample(const PresentBreakdown &b);

// The pre-guest overlay path, so a capture also covers installer and boot
// frames instead of starting with a gap.
void RecordBlankFrameSample();

// Once per presented frame, before RecordFrameSample.
void UpdateFrameStats();

// Per draw call.
void NoteDraw();

// Vertices or indices a draw submits. Separates vertex-processing cost from
// per-draw overhead, which is the open question on a draw-bound frame.
void NoteDrawVertices(u32 count);

// Per-draw CPU attribution. DrawPhaseNow is a monotonic nanosecond stamp;
// NoteDrawPhases accumulates the three phases of DispatchDraw - waiting on the
// renderer mutex, binding the framebuffer, and flushing render state - into
// per-frame totals reported alongside the draw count.
// Frames counted so far, for diagnostics that need to reset per frame.
u32 FrameStatFrameCount();

// Records the bound render target's size for a draw, so the per-frame report
// can say which surfaces the draws are actually going to.
void NoteDrawTarget(u32 width, u32 height);

u64 DrawPhaseNow();
void NoteDrawPhases(u64 enter, u64 locked, u64 fb, u64 state);

// Draws recorded so far this frame. Reset with the rest of the counters at
// frame end; used by the bd_debug_max_draws diagnostic.
u32 DrawsThisFrame();

// Per frame, like NoteDraw. On Vulkan every barriers() call ends the active
// render pass and every framebuffer bind starts one, so these measure
// render pass churn.
enum class BarrierSite : u8 { DrawFb, TexUpload, Resolve, Occlusion };
void NoteBarrierCall(u32 barrier_count, BarrierSite site);
void NoteFbBind();
void NotePSOSwitch();

enum class ResolveOp : u8 {
  EagerCopy,   // non-aliasable resolve, copied at Resolve time
  LazyLink,    // aliasable resolve, deferred as a sourceSurface link
  Materialize, // a deferred link forced to copy
  DeadElide,   // a deferred link superseded by a re-resolve, never paid for
  Seed,        // composite-chain tile seed copy
};
void NoteResolveOp(ResolveOp op);

// GPU-side time from timestamp queries (gpu_timing), one frame late: the total
// command list span, then the draw and resolve portions of it. The remainder is
// barriers and pass re-begins.
void RecordGPUTime(f64 total_ms, f64 draw_ms, f64 resolve_ms);

FrameStats GetFrameStats();

} // namespace bd::gpu
