/**
 * @file    gpu/hooks/draw.cpp
 * @brief   Guest draw calls, and the EDRAM resolve / tiling calls that bracket
 *          them.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>

#include <rex/graphics/xenos.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/runtime.h>
#include <rex/types.h>

#include <plume_render_interface.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/profiling.h"
#include "gpu/constant_buffers.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/format.h"
#include "gpu/frame_stats.h"
#include "gpu/gpu_timing.h"
#include "gpu/host_resource_heap.h"
#include "gpu/output.h"
#include "gpu/shaders/shader_cache.h"

REXCVAR_DECLARE(i32, bd_debug_max_draws);
REXCVAR_DECLARE(bool, bd_stereo);
REXCVAR_DECLARE(bool, bd_stereo_multiview);
REXCVAR_DECLARE(i32, bd_render_scale);
REXCVAR_DECLARE(f64, bd_stereo_separation);
REXCVAR_DECLARE(f64, bd_stereo_convergence);

REXCVAR_DECLARE(bool, bd_draw_phase_timing);

namespace {

namespace xe = rex::graphics::xenos;

struct DrawArgs {
  bool indexed = false;
  bool is_up = false;
  u32 vertexOrIndexCount = 0;
  i32 baseVertexIndex = 0;
  u32 startIndex = 0;
  u32 startVertex = 0;
};

plume::RenderPrimitiveTopology MapPrimitiveType(u32 prim) {
  switch (static_cast<xe::PrimitiveType>(prim)) {
  case xe::PrimitiveType::kPointList:
    return plume::RenderPrimitiveTopology::POINT_LIST;
  case xe::PrimitiveType::kLineList:
    return plume::RenderPrimitiveTopology::LINE_LIST;
  case xe::PrimitiveType::kLineStrip:
    return plume::RenderPrimitiveTopology::LINE_STRIP;
  case xe::PrimitiveType::kTriangleList:
    return plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  case xe::PrimitiveType::kTriangleFan:
    return plume::RenderPrimitiveTopology::TRIANGLE_FAN;
  case xe::PrimitiveType::kTriangleStrip:
    return plume::RenderPrimitiveTopology::TRIANGLE_STRIP;
  case xe::PrimitiveType::kQuadList:
    return plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  default: {
    // Unmapped X360 primitive type (e.g. kRectangleList). Drawing as a
    // triangle list is almost certainly wrong, so warn rather than fail
    // silently.
    static std::atomic<u32> s_warn{0};
    if (s_warn.fetch_add(1, std::memory_order_relaxed) < 4) {
      BD_WARN("MapPrimitiveType: unmapped X360 primitive type {} drawn as "
              "TRIANGLE_LIST - rect/unknown topology not implemented",
              prim);
    }
    return plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  }
  }
}

void DispatchDraw(u32 device_guest, u32 primitive_type, const char *name,
                  const DrawArgs &args = {}) {
#if defined(REXGLUE_ENABLE_PROFILING)
  // Per-draw pass/shader attribution, only formatted while a profiler is
  // connected (TRACY_ON_DEMAND).
  char zone_name[64] = "Draw";
  if (BD_PROFILER_CONNECTED()) {
    const auto *ps = bd::gpu::state().pixel_shader;
    const u64 ps_hash =
        (ps && ps->shaderCacheEntry) ? ps->shaderCacheEntry->hash : 0;
    std::snprintf(zone_name, sizeof(zone_name), "Draw pass=%u ps=%016llX",
                  bd::gpu::CurrentRenderPassId(),
                  static_cast<unsigned long long>(ps_hash));
  }
  // CPU zone only: per-draw GPU timestamps serialize the GPU and poison every
  // GPU number in the capture. Coarse GPU cost comes from the per-frame zones.
  BD_CPU_ZONE_DYN(zone_name);
#endif
  bd::gpu::NoteDraw();
  bd::gpu::NoteDrawVertices(args.vertexOrIndexCount);

  // Diagnostic, off by default. A field scene submits ~2925 draws and spends
  // ~110ms on the GPU fence, and that cost does not move when the render
  // resolution is halved - so it is not fill, and the suspicion is the tiler's
  // binning pass, which scales with draw calls and vertex count rather than
  // pixels.
  //
  // Capping the draw count answers that directly: if the fence falls roughly in
  // proportion, the frame is draw-bound and culling is the lever. If it does
  // not, the binning theory is wrong and the search moves elsewhere. The frame
  // renders incorrectly while this is set - that is the point, it is a
  // measurement and not a setting.
  if (const i32 cap = REXCVAR_GET(bd_debug_max_draws); cap > 0) {
    if (bd::gpu::DrawsThisFrame() > static_cast<u32>(cap))
      return;
  }

  // A field scene costs ~25us of CPU per draw across ~2850 draws, and guessing
  // which phase owns it has been wrong twice. These four counters attribute it
  // directly: clock_gettime is ~25ns, so ~100us a frame to measure 70ms, and
  // they are summed per frame rather than logged per draw.
  //
  // Kept permanently rather than deleted after use - this is the number that
  // decides every renderer optimisation, and it was expensive to not have.
  //
  // Gated, because the estimate above was wrong by a factor of 25. It reasoned
  // clock_gettime at ~25ns and ~100us a frame; the first real profile of the
  // process put NoteDrawPhases at 3.4% of all CPU samples, because it is four
  // clock reads and three atomics on every one of ~1200 draws. The capability
  // is worth keeping and the default cost is not.
  const bool phase_timing = REXCVAR_GET(bd_draw_phase_timing);
  const auto t_enter = phase_timing ? bd::gpu::DrawPhaseNow() : 0;

  // One lock across the whole recording sequence: loader threads record texture
  // uploads and Present records under the same mutex, and the per-frame command
  // list they all write is single-producer.
  auto &s = bd::gpu::state();
  std::unique_lock<std::mutex> lock(s.mutex);
  const auto t_locked = phase_timing ? bd::gpu::DrawPhaseNow() : 0;
  bd::gpu::Video::OpenCommandListLocked();

  // PSO key includes topology, so set it before any flush.
  bd::gpu::Video::SetDirtyValue<plume::RenderPrimitiveTopology>(
      s.dirtyStates.pipelineState, s.pipelineState.primitiveTopology,
      MapPrimitiveType(primitive_type));

  if (!bd::gpu::Video::BindDrawFramebufferLocked()) {
    return;
  }
  // Whether this draw is scene geometry, decided before the flush because the
  // shared constants are uploaded there and the multiview skew reads them.
  //
  // Both stereo paths need this and only one had it. The host patch below is
  // gated on scene_pass already; the shader skew was not, so under multiview it
  // ran in *every* vertex shader - including the full-screen quads of the post
  // chain, which are drawn at w = 1, where a constant added to clip.x is a
  // constant slide of the finished image rather than parallax. Measured as a
  // uniform +38px of disparity at every depth, which is 2*separation at w = 1.
  const u32 stereo_pct = u32(REXCVAR_GET(bd_render_scale));
  const bool scene_pass =
      s.render_target != nullptr &&
      s.render_target->width >=
          u32(bd::gpu::kDesignCanvasWidth) * stereo_pct / 100u &&
      s.render_target->height >=
          u32(bd::gpu::kDesignCanvasHeight) * stereo_pct / 100u &&
      args.vertexOrIndexCount > 6;
  s.stereoEligible = scene_pass;

  // Flatten any two-layer surface this draw is about to sample.
  //
  // **Follow sourceSurface.** A bound slot holds a *texture*, and the render
  // surface behind it hangs off `sourceSurface` - UploadSharedConstants does
  // exactly this hop when it publishes descriptor indices. Checking the slot
  // itself matches nothing, which is what made an earlier version of this
  // resolve zero times and read as "the guest never tells us".
  //
  // This is the honest "about to be read" point: the writes are provably
  // finished, because something is sampling it. The render-target change is
  // too early - the scene surface is bound and unbound several times a frame,
  // so resolving there fires on a half-drawn array.
  if (s.command_list_open) {
    for (bd::gpu::GuestTexture *bound : s.textures) {
      if (!bound)
        continue;
      bd::gpu::GuestTexture *surf =
          (bound->sourceSurface && bound->sourceSurface->texture)
              ? bound->sourceSurface
              : bound;
      if (surf->layers > 1 && surf->multiviewDirty &&
          surf != s.render_target && surf != s.depth_stencil) {
        bd::gpu::ResolveMultiviewSurfaceLocked(s, surf);
      }
    }
  }

  const auto t_fb = phase_timing ? bd::gpu::DrawPhaseNow() : 0;
  if (!bd::gpu::Video::FlushRenderStateLocked(device_guest)) {
    return; // FlushRenderState logs its own reason
  }
  if (phase_timing) {
    const auto t_state = bd::gpu::DrawPhaseNow();
    bd::gpu::NoteDrawPhases(t_enter, t_locked, t_fb, t_state);
  }

  // Which target is this draw hitting, and how big is it? The GPU counters say
  // ~167M fragments a frame across ~2822 draws - about 59,000 fragments per
  // draw from 131 vertices - and that total does not move when the scene
  // resolution is halved. So the pixels are going somewhere whose size ignores
  // bd_max_render_height, and this says where rather than inferring it.
  if (s.render_target) {
    bd::gpu::NoteDrawTarget(s.render_target, s.render_target->width,
                            s.render_target->height, s.render_target->layers);
    // So the resolve only fires on a surface something actually drew into.
    if (s.render_target->layers > 1)
      s.render_target->multiviewDirty = true;
    // Does a draw landing on a two-layer target actually get a multiview
    // pipeline? A framebuffer with viewMask 3 and a pipeline with viewMask 0
    // is a render-pass incompatibility, which is undefined rather than an
    // error - and would leave the array empty while the draw count looks fine.
    {
      static std::atomic<u32> layered{0}, layered_mv{0};
      if (s.render_target->layers > 1) {
        const u32 n = layered.fetch_add(1, std::memory_order_relaxed);
        if (s.pipelineState.multiview)
          layered_mv.fetch_add(1, std::memory_order_relaxed);
        if (n == 4000)
          BD_INFO("[mv] of 4000 draws on two-layer targets, {} had a multiview "
                  "pipeline", layered_mv.load(std::memory_order_relaxed));
      }
    }
  }

  auto *cmd_list = s.command_list;
  if (!cmd_list)
    return;
  bd::gpu::MarkDraw(cmd_list);

  // Stereo, renderer side. Re-entering the guest to render a second view does
  // not work - it yields +21% draws rather than a second scene, because the
  // render list is built once per frame above every seam worth hooking (see
  // research/20260829_0600_stereo-groundwork.md). So the second view is
  // produced here instead: one guest frame, one render list, the same recorded
  // draw submitted once per eye.
  //
  // This first step gives the two eyes different viewports and nothing else, so
  // the result is the same image twice, side by side. That is deliberate - it
  // separates "can the renderer emit every draw twice" from "are the per-eye
  // matrices right", and the matrices are the part that already has unit tests.
  const auto emit = [&]() {
  if (primitive_type == 13) {
    u32 quads = args.vertexOrIndexCount / 4;
    const u32 max_quads = bd::gpu::Video::QuadlistMaxQuads();
    if (quads > max_quads)
      quads = max_quads;
    const auto *ib = bd::gpu::Video::QuadlistExpansionIBView();
    if (quads && ib) {
      const i32 base = args.indexed ? args.baseVertexIndex
                                    : static_cast<i32>(args.startVertex);
      cmd_list->setIndexBuffer(ib);
      cmd_list->drawIndexedInstanced(quads * 6, 1, 0, base, 0);
      // The expansion IB replaced the tracked guest IB on the command list, and
      // without re-dirtying the next indexed draw reads quad pattern indices.
      s.dirtyStates.indices = true;
    }
  } else if (args.indexed) {
    // No IB bound on an indexed draw: FlushRenderState skipped setIndexBuffer
    // and D3D12 fires EXECUTION WARNING #211, every index reads as 0,
    // geometry collapses, scene goes black.
    if (s.index_view.buffer.ref == nullptr) {
      static std::atomic<u32> s_no_ib{0};
      const u32 k = s_no_ib.fetch_add(1, std::memory_order_relaxed);
      if (k < 16) {
        BD_WARN("[draw] indexed draw with no IB bound: #{} {} count={} "
                "startI={} baseV={}",
                k, name, args.vertexOrIndexCount, args.startIndex,
                args.baseVertexIndex);
      }
    }
    cmd_list->drawIndexedInstanced(args.vertexOrIndexCount, 1, args.startIndex,
                                   args.baseVertexIndex, 0);
  } else {
    cmd_list->drawInstanced(args.vertexOrIndexCount, 1, args.startVertex, 0);
  }
  };

  // Scene geometry only. Doubling *every* draw compounds through the
  // post-process chain: each full-screen pass reads a target that is already
  // two half-width copies and writes two more, so the frame recursively
  // subdivides into vertical stripes. Verified by looking at it.
  //
  // The scene target is the one at or above the design canvas; the bloom chain
  // (640x360 down to 80x45) and the 2D/UI passes are all below it and must be
  // left alone, composited once over the already-stereo scene.
  // Target size alone is not enough: the full-resolution post passes render to
  // the same surface, and each one samples an already-doubled image and doubles
  // it again, so the frame subdivides once per pass - about six passes gave
  // roughly sixty vertical stripes. A post pass is a full-screen quad, three or
  // four vertices; scene geometry is not. That is the discriminator.
  //
  // The threshold has to follow bd_render_scale. Against a fixed design canvas,
  // bd_render_scale=50 shrinks the scene target to 960x540, which falls under
  // 1280x720, and stereo then silently does nothing - the two features were
  // mutually exclusive, and they are precisely the pair that belong together
  // because the render scale is what pays for stereo's doubled fill. Caught by
  // screenshotting the combination, not by either feature's own test.
  const u32 min_w = u32(bd::gpu::kDesignCanvasWidth) * stereo_pct / 100u;
  const u32 min_h = u32(bd::gpu::kDesignCanvasHeight) * stereo_pct / 100u;
  // A 2D overlay drawn once spans the whole target, which in a side-by-side
  // frame means it straddles the join and each eye sees half of it - which is
  // what "Microsoft Game Studios is not on both eyes" was. It gets the two half
  // viewports like scene geometry, but **no eye offset**: an overlay belongs at
  // the same place in both eyes, where it fuses at screen depth. Parallax here
  // would push the HUD into the world.
  const bool overlay_2d = s.overlay2D || s.overlay2DScope;
  s.overlay2D = false;
  // bd_stereo and bd_stereo_multiview are two implementations of one thing, and
  // running both composes wrongly rather than doing nothing: the eye loop below
  // submits each draw into a half-width viewport, multiview then replicates
  // *that* into both array layers, and the resolve finally squeezes a layer
  // which already holds a complete side-by-side pair into one half of the
  // companion. Both layers therefore carry the same two eyes and differ only by
  // the shader skew, which is exactly the "multiview renders identical layers"
  // symptom - and it also means every scene triangle is rasterised four times,
  // which is why multiview once measured *slower* than the path it replaces.
  //
  // Proven from a RenderDoc capture: the scene passes had viewMask=3 with
  // viewports alternating 960x1080@0 and 960x1080@960. Multiview wins, because
  // it is the one that submits each draw once.
  const bool multiview_stereo = REXCVAR_GET(bd_stereo_multiview);
  if (multiview_stereo || !REXCVAR_GET(bd_stereo) ||
      (!scene_pass && !overlay_2d)) {
    // Counted, because "stereo does nothing" has three different causes and
    // they are indistinguishable from the image: the cvar off, the scene-pass
    // gate rejecting every draw, or the per-eye constants not landing. This
    // separates the first two from the third.
    if (REXCVAR_GET(bd_stereo) && !multiview_stereo) {
      static std::atomic<u32> rejected{0};
      const u32 n = rejected.fetch_add(1, std::memory_order_relaxed);
      if (n == 2000)
        BD_INFO("[stereo] scene_pass rejected 2000 draws; rt={}x{} min={}x{}",
                s.render_target ? s.render_target->width : 0u,
                s.render_target ? s.render_target->height : 0u, min_w, min_h);
    }
    emit();
    return;
  }
  {
    static std::atomic<u32> accepted{0};
    const u32 n = accepted.fetch_add(1, std::memory_order_relaxed);
    if (n == 0 || n == 2000)
      BD_INFO("[stereo] per-eye path taken {} times, rt={}x{}", n + 1,
              s.render_target ? s.render_target->width : 0u,
              s.render_target ? s.render_target->height : 0u);
  }

  // Half-width viewports, left eye then right. The scissor follows the viewport
  // rather than the full target, so neither eye can bleed into the other.
  const plume::RenderViewport vp = s.viewport;
  for (int eye = 0; eye < 2; ++eye) {
    plume::RenderViewport half = vp;
    half.width = vp.width * 0.5f;
    half.x = vp.x + (eye ? half.width : 0.0f);
    if (half.minDepth > half.maxDepth)
      std::swap(half.minDepth, half.maxDepth);
    const plume::RenderRect rc{
        static_cast<i32>(half.x), static_cast<i32>(half.y),
        static_cast<i32>(half.x + half.width),
        static_cast<i32>(half.y + half.height)};
    cmd_list->setViewports(&half, 1);
    cmd_list->setScissors(&rc, 1);
    // The left eye's camera sits to the left, so the world appears shifted
    // *right* in its image - left eye positive. Getting this backwards is not a
    // subtle error: it renders the scene pseudoscopic, near geometry reading as
    // far and the whole world turned inside out, which fuses badly and is
    // exactly the kind of sign mistake a symmetric test pose cannot see.
    //
    // Checkable from a capture: for a convergence plane at infinity every point
    // must have crossed (negative) disparity, i.e. appear further left in the
    // right eye, by more the nearer it is.
    const float sep =
        scene_pass ? float(REXCVAR_GET(bd_stereo_separation)) : 0.0f;
    const float conv =
        scene_pass ? float(REXCVAR_GET(bd_stereo_convergence)) : 0.0f;
    if (sep != 0.0f || conv != 0.0f)
      bd::gpu::Video::BindEyeVertexConstants(device_guest, eye ? -sep : sep,
                                             eye ? conv : -conv);
    emit();
  }
  // The last eye left a skewed block bound and FlushRenderState believes the
  // constants are clean, so without this the next draw inherits an eye.
  s.dirtyStates.vertexShaderConstants = true;
  // FlushViewport owns s.viewport and believes it is still set; the next draw
  // must reprogram it rather than inherit an eye's half.
  s.dirtyStates.viewport = true;
  s.dirtyStates.scissorRect = true;
}

// bdBuildQuadVertices assembles every glyph into this one buffer, and unlike
// bdPrim it divides each position by the 2D basis on the way in, so text
// arrives in the unit square while sprites arrive in canvas pixels.
constexpr u32 kTextQuadBatchEA = 0x82DBECF0;

// Which edges of a quad's UV rect the inset is allowed to move.
enum class UVEdges {
  All,      // every edge, for a batch whose cells are known to abut
  CellSeam, // only edges sitting on an interior texel boundary
};

void FitDesignCanvasVertices(u8 *verts, u32 vertexCount, u32 vertexStride,
                             bool normalized);
void InsetQuadUVs(u8 *verts, u32 vertexCount, u32 vertexStride, UVEdges edges);
bool IsScreenSpriteQuad(u32 primitiveType, u32 vertexCount, u32 vertexStride);

bool UploadAndBindUpVertices(u32 primitiveType, u32 pVertexData,
                             u32 vertexCount, u32 vertexStride) {
  if (!pVertexData || !vertexCount || !vertexStride)
    return false;
  const u32 totalSize = vertexCount * vertexStride;
  auto alloc = bd::gpu::UploadGuestBytesByteSwap32(pVertexData, totalSize,
                                                   /*alignment=*/4);
  if (!alloc.memory)
    return false;
  const bool text_batch = pVertexData == kTextQuadBatchEA;
  // Remember whether this is genuinely 2D overlay content - the glyph batch, or
  // a screen sprite - so the stereo path can put it in *both* eyes.
  //
  // These two tests and nothing looser. An earlier attempt keyed off "came
  // through the user-pointer path" and quadrupled the frame, because the
  // full-screen post blits arrive that way too and doubling one squashes the
  // whole source into half the target.
  // The glyph batch's fixed address, and nothing else.
  //
  // IsScreenSpriteQuad is not a usable discriminator here: a full-screen post
  // blit has the same shape - a four-vertex triangle strip at the sprite stride
  // - so including it quadrupled the frame, because doubling a blit squashes
  // the whole source into half the target. The text batch is a specific buffer
  // at a known EA and cannot be confused with one.
  bd::gpu::state().overlay2D = text_batch;
  FitDesignCanvasVertices(alloc.memory, vertexCount, vertexStride, text_batch);
  if (text_batch)
    InsetQuadUVs(alloc.memory, vertexCount, vertexStride, UVEdges::All);
  else if (IsScreenSpriteQuad(primitiveType, vertexCount, vertexStride))
    InsetQuadUVs(alloc.memory, vertexCount, vertexStride, UVEdges::CellSeam);
  bd::gpu::Video::SetVertexStream(0, alloc.ref, alloc.size, vertexStride);
  return true;
}

// Zooming an event sprite about an off-canvas pivot retreats the authored art
// inside the frame and lets the power-of-two padding take the edge, which X360
// TV overscan hid. Pull the sampled UV back until the art edge meets the
// frame edge. Sibling of bdMotionBlurQuadInsetHook in gpu/hooks/tweaks.cpp.
constexpr u32 kQuadVertices = 4;

// An overshoot an artist could hide had to fit inside title-safe overscan. Past
// that, the art fills its canvas and the UV means what it says.
constexpr float kOverscanFraction = 0.05f;

// bdPrimPushVertex2D's layout. bdDrawRectPrimitive shares the stride but puts
// the color where u sits here, and submits nothing but QUADLIST or LINESTRIP,
// so the primitive type is what tells the two apart.
struct ScreenSpriteVertex {
  be_f32 x;
  be_f32 y;
  be_f32 u;
  be_f32 v;
  be_u32 color;
};
static_assert(sizeof(ScreenSpriteVertex) == 0x14);

// Stride tells the prim system's three vertex layouts apart. The two 2D ones
// lead with a canvas position, the 3D one with a world position.
constexpr u32 kPrimVertex2DDualTexStride = 0x1C;

bool Is2DPrimStride(u32 stride) {
  return stride == sizeof(ScreenSpriteVertex) ||
         stride == kPrimVertex2DDualTexStride;
}

// bdPrimPushVertex2D writes x,y,u,v,color and always submits a strip, while
// bdDrawRectPrimitive shares the stride but puts the color where u sits and
// submits QUADLIST or LINESTRIP. Matching all three tells the two apart.

bool IsScreenSpriteQuad(u32 primitiveType, u32 vertexCount, u32 vertexStride) {
  return static_cast<xe::PrimitiveType>(primitiveType) ==
             xe::PrimitiveType::kTriangleStrip &&
         vertexCount == kQuadVertices &&
         vertexStride == sizeof(ScreenSpriteVertex);
}

// uv runs linearly from uv_lo at pos_lo to uv_hi at pos_hi. Yields the uv_hi
// that puts art_uv on the frame edge, or nothing when the quad already keeps
// the padding off screen.
std::optional<float> InsetSampledSpan(float pos_lo, float pos_hi, float uv_lo,
                                      float uv_hi, float frame_extent,
                                      float art_uv) {
  const float span = pos_hi - pos_lo;
  const float reach = frame_extent - pos_lo;
  if (span <= 0.0f || uv_hi <= uv_lo || uv_lo >= art_uv)
    return std::nullopt;
  // Only a quad covering the whole frame can put padding on screen.
  if (pos_lo > 0.0f || pos_hi < frame_extent)
    return std::nullopt;
  const float at_edge = uv_lo + reach * (uv_hi - uv_lo) / span;
  if (at_edge <= art_uv || at_edge > art_uv * (1.0f + kOverscanFraction))
    return std::nullopt;
  return uv_lo + (art_uv - uv_lo) * span / reach;
}

void InsetOverscanScreenSprite(u32 pVertexData, u32 primitiveType,
                               u32 vertexCount, u32 vertexStride) {
  if (!IsScreenSpriteQuad(primitiveType, vertexCount, vertexStride))
    return;

  const auto *tex = bd::gpu::state().textures[0];
  if (!tex)
    return;
  const auto tex_w = static_cast<float>(tex->width);
  const auto tex_h = static_cast<float>(tex->height);
  if (tex_w <= bd::gpu::kDesignCanvasWidth ||
      tex_h <= bd::gpu::kDesignCanvasHeight)
    return;

  auto *v = bd::mem::at<ScreenSpriteVertex>(pVertexData);
  if (!v)
    return;

  // Strip order is top-left, bottom-left, top-right, bottom-right, so u is
  // shared down each column and v across each row.
  if (auto inset_u = InsetSampledSpan(v[0].x, v[2].x, v[0].u, v[2].u,
                                      bd::gpu::kDesignCanvasWidth,
                                      bd::gpu::kDesignCanvasWidth / tex_w)) {
    v[2].u = *inset_u;
    v[3].u = *inset_u;
  }
  if (auto inset_v = InsetSampledSpan(v[0].y, v[1].y, v[0].v, v[1].v,
                                      bd::gpu::kDesignCanvasHeight,
                                      bd::gpu::kDesignCanvasHeight / tex_h)) {
    v[1].v = *inset_v;
    v[3].v = *inset_v;
  }
}

// Guest UV rects land on exact texel boundaries, so a fraction of a texel of
// slack is float noise rather than an authored position.
constexpr float kTexelBoundaryTolerance = 0.01f;

// An atlas packs its cells flush against each other, so a UV edge on a texel
// boundary with texture on both sides is a cell seam. An edge at 0 or 1 has no
// neighbor to reach across, and CLAMP already returns the border texel there.
bool OnCellSeam(float uv, float extent) {
  if (uv <= 0.0f || uv >= 1.0f)
    return false;
  const float texel = uv * extent;
  return std::abs(texel - std::round(texel)) < kTexelBoundaryTolerance;
}

// At any scale but 1:1 a bilinear tap at a quad edge reaches past it, so an
// edge sitting on a cell seam returns the neighbor cell: the next glyph's ink,
// the orange copy of a gauge behind the gray one, the far half of a nine slice.
// Pull the edge half a source texel inward, which keeps every tap inside its
// own cell at any scale and still puts the 1:1 case on texel centers.
//
// UVEdges::All moves both edges whether or not they measure onto a boundary,
// for the glyph batch, whose cells are known to abut. CellSeam moves only the
// edges that do. That is what leaves a quad spanning a whole texture, the frame
// composite above all, sampling its own texel grid untouched.
//
// Applied to the uploaded copy, after the byte swap, so guest memory is
// untouched.
void InsetQuadUVs(u8 *verts, u32 vertexCount, u32 vertexStride, UVEdges edges) {
  if (vertexStride != sizeof(ScreenSpriteVertex))
    return;
  const auto *tex = bd::gpu::state().textures[0];
  if (!tex || !tex->width || !tex->height)
    return;
  const float extent[2] = {static_cast<float>(tex->width),
                           static_cast<float>(tex->height)};
  // Depends only on the texture, so it is two divisions per draw rather than
  // two per quad.
  const float insets[2] = {0.5f / extent[0], 0.5f / extent[1]};

  for (u32 base = 0; base + kQuadVertices <= vertexCount;
       base += kQuadVertices) {
    // Resolved once per quad rather than on each of the sixteen accesses
    // below: the lambda this replaced recomputed base + i times the stride
    // every time it was called, and this function came out as the single
    // hottest entry in the first profile of the process.
    float *uvp[kQuadVertices];
    for (u32 i = 0; i < kQuadVertices; ++i)
      uvp[i] = reinterpret_cast<float *>(verts +
                                         (base + i) * size_t{vertexStride}) + 2;
    auto uv = [&uvp](u32 i) { return uvp[i]; };

    for (u32 axis = 0; axis < 2; ++axis) {
      const float inset = insets[axis];
      float lo = uv(0)[axis], hi = lo;
      for (u32 i = 1; i < kQuadVertices; ++i) {
        lo = std::min(lo, uv(i)[axis]);
        hi = std::max(hi, uv(i)[axis]);
      }
      if (hi - lo <= 2.0f * inset)
        continue;
      const bool move_lo =
          edges == UVEdges::All || OnCellSeam(lo, extent[axis]);
      const bool move_hi =
          edges == UVEdges::All || OnCellSeam(hi, extent[axis]);
      // bdBuildQuadVertices copies the rect's two u and two v values verbatim
      // into the corners, so comparing against the extremes recovers which
      // corner each vertex is regardless of winding or mirroring.
      for (u32 i = 0; i < kQuadVertices; ++i) {
        float &c = uv(i)[axis];
        if (move_lo && c == lo)
          c = lo + inset;
        else if (move_hi && c == hi)
          c = hi - inset;
      }
    }
  }
}

// Authored extents this far apart still count as the same canvas edge.
constexpr float kCanvasEdgeTolerance = 8.0f;

// Scales one 2D draw about the canvas center. It has to be the geometry rather
// than a shrunk viewport: the rasterizer clips to the NDC box before the
// viewport transform, so a backdrop reaching for the surface edge would be cut
// off at exactly the rect it was trying to escape.
//
// Applied to the uploaded copy, so guest memory is untouched. 'normalized' says
// the draw arrived already divided by the 2D basis, as the text batch is.
void FitDesignCanvasVertices(u8 *verts, u32 vertexCount, u32 vertexStride,
                             bool normalized) {
  if (!verts || !bd::gpu::Video::DesignCanvasDrain() ||
      !Is2DPrimStride(vertexStride) || !vertexCount)
    return;
  const float kx = bd::gpu::Output::DesignScaleX();
  const float ky = bd::gpu::Output::DesignScaleY();
  if (kx == 1.0f && ky == 1.0f)
    return;

  const float canvas_x = normalized ? 1.0f : bd::gpu::kDesignCanvasWidth;
  const float canvas_y = normalized ? 1.0f : bd::gpu::kDesignCanvasHeight;
  const float tol_x = kCanvasEdgeTolerance * canvas_x / bd::gpu::kDesignCanvasWidth;
  const float tol_y =
      kCanvasEdgeTolerance * canvas_y / bd::gpu::kDesignCanvasHeight;

  // Uploaded already byte-swapped, so position is two host floats at the front
  // of each vertex.
  auto pos = [verts, vertexStride](u32 i) {
    return reinterpret_cast<float *>(verts + i * vertexStride);
  };

  float min_x = 999999.0f, min_y = 999999.0f;
  float max_x = -999999.0f, max_y = -999999.0f;
  for (u32 i = 0; i < vertexCount; ++i) {
    const float *p = pos(i);
    min_x = std::min(min_x, p[0]);
    min_y = std::min(min_y, p[1]);
    max_x = std::max(max_x, p[0]);
    max_y = std::max(max_y, p[1]);
  }

  // Covering the canvas, not matching it: a menu's ground is often authored
  // well past the edges, and scaling one of those inward opens a gap at the
  // very edge it was oversized to reach.
  const bool one_quad = vertexCount <= 4;
  const bool spans_x =
      one_quad && min_x <= tol_x && max_x >= canvas_x - tol_x;
  const bool spans_y =
      one_quad && min_y <= tol_y && max_y >= canvas_y - tol_y;
  const float scale_x = spans_x ? 1.0f : kx;
  const float scale_y = spans_y ? 1.0f : ky;
  if (scale_x == 1.0f && scale_y == 1.0f)
    return;

  const float mid_x = canvas_x * 0.5f;
  const float mid_y = canvas_y * 0.5f;
  for (u32 i = 0; i < vertexCount; ++i) {
    float *p = pos(i);
    p[0] = mid_x + (p[0] - mid_x) * scale_x;
    p[1] = mid_y + (p[1] - mid_y) * scale_y;
  }
}

u32 D3DDevice_DrawVerticesUP_hook(u32 device_guest, u32 primitiveType,
                                  u32 vertexCount, u32 pVertexData,
                                  u32 vertexStride) {
  InsetOverscanScreenSprite(pVertexData, primitiveType, vertexCount,
                            vertexStride);
  const bool ok =
      UploadAndBindUpVertices(primitiveType, pVertexData, vertexCount,
                              vertexStride);
  DrawArgs args{};
  args.is_up = ok;
  args.vertexOrIndexCount = vertexCount;
  args.startVertex = 0;
  DispatchDraw(device_guest, primitiveType, "DrawVerticesUP", args);
  return 0;
}

// The streaming draw-up pair. Their recompiled bodies drive the PM4 ring buffer
// reblue stubs out, so an unhooked BeginVertices spins forever in
// D3DDevice_RingBufferFlush. EndVertices copies synchronously, so the grow-only
// scratch is free again for the next BeginVertices.
struct {
  u32 va = 0;       // guest VA of the scratch buffer, 0 until first use
  u32 capacity = 0; // bytes currently allocated
} g_begin_vertices_scratch;

struct {
  u32 device = 0;
  u32 primitive_type = 0;
  u32 vertex_count = 0;
  u32 stride = 0;
  u32 data_va = 0; // 0 when nothing is pending to draw
} g_begin_vertices_pending;

u32 D3DDevice_BeginVertices_hook(u32 device_guest, u32 primitiveType,
                                 u32 vertexCount, u32 vertexStride) {
  g_begin_vertices_pending = {};
  const u64 size =
      static_cast<u64>(vertexCount) * static_cast<u64>(vertexStride);
  // Nothing to draw: return null so the caller skips its memcpy + EndVertices
  // (Visual__DrawVerticesUP gates both on a non-null BeginVertices result).
  if (size == 0 || size > 0xFFFFFFFFull)
    return 0;

  auto *memory = REX_KERNEL_MEMORY();
  if (g_begin_vertices_scratch.capacity < size) {
    // Safe to free the old block here: EndVertices copies the bytes out
    // synchronously, so nothing references the previous scratch by now.
    const u32 new_capacity = static_cast<u32>(size);
    const u32 va = memory->SystemHeapAlloc(new_capacity, 0x20);
    if (!va)
      return 0;
    if (g_begin_vertices_scratch.va)
      memory->SystemHeapFree(g_begin_vertices_scratch.va);
    g_begin_vertices_scratch.va = va;
    g_begin_vertices_scratch.capacity = new_capacity;
  }

  g_begin_vertices_pending.device = device_guest;
  g_begin_vertices_pending.primitive_type = primitiveType;
  g_begin_vertices_pending.vertex_count = vertexCount;
  g_begin_vertices_pending.stride = vertexStride;
  g_begin_vertices_pending.data_va = g_begin_vertices_scratch.va;
  return g_begin_vertices_scratch.va;
}

u32 D3DDevice_EndVertices_hook(u32 /*device_guest*/) {
  const auto p = g_begin_vertices_pending;
  g_begin_vertices_pending = {}; // consume: a stray EndVertices must not redraw
  if (!p.data_va || !p.vertex_count)
    return 0;

  const bool ok = UploadAndBindUpVertices(p.primitive_type, p.data_va,
                                         p.vertex_count, p.stride);
  DrawArgs args{};
  args.is_up = ok;
  args.vertexOrIndexCount = p.vertex_count;
  args.startVertex = 0;
  DispatchDraw(p.device, p.primitive_type, "BeginVertices", args);
  return 0;
}

// All five params must be marshaled: a 4-arg signature drops the real
// IndexCount in r7 and submits zero count draws.
u32 D3DDevice_DrawIndexedVertices_hook(u32 device_guest, u32 primitiveType,
                                       u32 baseVertexIndex, u32 startIndex,
                                       u32 indexCount) {
  DrawArgs args{};
  args.indexed = true;
  args.vertexOrIndexCount = indexCount;
  args.baseVertexIndex = static_cast<i32>(baseVertexIndex);
  args.startIndex = startIndex;
  DispatchDraw(device_guest, primitiveType, "DrawIndexedVertices", args);
  return 0;
}

u32 D3DDevice_Resolve_hook(u32 /*device_guest*/, u32 Flags, u32 /*pSourceRect*/,
                           u32 pDestTexture, u32 /*pDestPoint*/, u32 DestLevel,
                           u32 DestSliceOrFace, u32 /*pClearColor*/,
                           u32 /*ClearZHi*/, u32 /*ClearZLo*/,
                           u32 /*ClearStencil*/, u32 /*pParameters*/) {
  auto *dst =
      bd::gpu::HostResourceHeap::FromGuest<bd::gpu::GuestTexture>(pDestTexture);
  if (!dst) {
    static std::atomic<u32> s_miss{0};
    const u32 n = s_miss.fetch_add(1, std::memory_order_relaxed);
    if (n < 8) {
      BD_WARN("D3DDevice_Resolve: destination guest VA 0x{:08X} not a "
              "host texture",
              pDestTexture);
    }
    return 0;
  }
  // DestSliceOrFace selects the cube face (D3DCUBEMAP_FACES) for a cube
  // destination, and DestLevel the mip. Both are 0 for the common 2D resolve.
  bd::gpu::Video::TrackResolveSource(Flags, dst, DestLevel, DestSliceOrFace);
  bd::gpu::Video::ResolveRtToTexture(dst);
  return 0;
}

// Per the X360 contract this clears the bound EDRAM tile, so draws record over
// a known state rather than the host RT's stale contents.
//
// ClearZ arrives in fpr1 and marshals as f64, and the Xenon ABI float slot skip
// reserves r8, so ClearStencil is in r9 and needs the placeholder to line up.
u32 D3DDevice_BeginTiling_hook(u32 /*device_guest*/, u32 /*Flags*/,
                               u32 /*Count*/, u32 /*pTileRects*/,
                               mapped_f32 pClearColor, f64 ClearZ,
                               u32 /*z_gpr_slot*/, u32 ClearStencil) {
  u32 color_argb = 0;
  if (const be_f32 *color_vec = pClearColor) {
    const auto pack = [](float v) -> u32 {
      return static_cast<u32>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    color_argb = (pack(color_vec[3]) << 24)   // A
                 | (pack(color_vec[0]) << 16) // R
                 | (pack(color_vec[1]) << 8)  // G
                 | (pack(color_vec[2]) << 0); // B
  }
  // X360 D3DCLEAR bits: TARGET 0x1 | ZBUFFER 0x10 | STENCIL 0x20 = color+depth+
  // stencil. The pending clear drains onto the bound RT/DS at the next draw,
  // matching the BeginTiling -> draws -> EndTiling flow.
  bd::gpu::Video::RequestClear(0x31u, color_argb, float(ClearZ), ClearStencil);
  return 0;
}

// Copies the resolved EDRAM tile into pDestTexture, the Resolve equivalent.
// Here ClearZ marshals as a single u32 slot, not the wider double slot
// Resolve's earlier ClearZ uses.
u32 D3DDevice_EndTiling_hook(u32 /*device_guest*/, u32 ResolveFlags,
                             u32 /*pResolveRects*/, u32 pDestTexture,
                             u32 /*pClearColor*/, u32 /*ClearZ*/,
                             u32 /*ClearStencil*/, u32 /*pParameters*/) {
  if (!pDestTexture) {
    return 0;
  }
  auto *dst =
      bd::gpu::HostResourceHeap::FromGuest<bd::gpu::GuestTexture>(pDestTexture);
  if (!dst) {
    static std::atomic<u32> s_miss{0};
    const u32 n = s_miss.fetch_add(1, std::memory_order_relaxed);
    if (n < 8) {
      BD_WARN("D3DDevice_EndTiling: destination guest VA 0x{:08X} not a "
              "host texture",
              pDestTexture);
    }
    return 0;
  }
  bd::gpu::Video::TrackResolveSource(ResolveFlags, dst);
  bd::gpu::Video::ResolveRtToTexture(dst);
  return 0;
}

u32 D3DDevice_DrawVertices_hook(u32 device_guest, u32 primitiveType,
                                u32 startVertex, u32 vertexCount) {
  DrawArgs args{};
  args.vertexOrIndexCount = vertexCount;
  args.startVertex = startVertex;
  DispatchDraw(device_guest, primitiveType, "DrawVertices", args);
  return 0;
}

} // namespace

REX_HOOK(D3DDevice_DrawVertices, D3DDevice_DrawVertices_hook);
REX_HOOK(D3DDevice_DrawVerticesUP, D3DDevice_DrawVerticesUP_hook);
REX_HOOK(D3DDevice_DrawIndexedVertices, D3DDevice_DrawIndexedVertices_hook);
REX_HOOK(D3DDevice_BeginVertices, D3DDevice_BeginVertices_hook);
REX_HOOK(D3DDevice_EndVertices, D3DDevice_EndVertices_hook);
REX_HOOK(D3DDevice_Resolve, D3DDevice_Resolve_hook);
REX_HOOK(D3DDevice_BeginTiling, D3DDevice_BeginTiling_hook);
REX_HOOK(D3DDevice_EndTiling, D3DDevice_EndTiling_hook);

