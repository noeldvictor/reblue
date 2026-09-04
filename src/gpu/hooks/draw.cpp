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
#include <fstream>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>

#include <rex/graphics/xenos.h>
#include <rex/cvar.h>
#include <rex/hash.h>
#include <rex/hook.h>
#include <rex/runtime.h>
#include <rex/types.h>

#include <plume_render_interface.h>

#include "core/app_root.h"
#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/profiling.h"
#include "engine/guest_census.h"
#include "gpu/draw_queue.h"
#include "gpu/constant_buffers.h"
#include <xxhash.h>
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/format.h"
#include "gpu/frag_census.h"
#include "gpu/frame_stats.h"
#include "gpu/gpu_timing.h"
#include "gpu/host_resource_heap.h"
#include "gpu/output.h"
#include "gpu/vertex_pull.h"
#include "gpu/occlusion_cull.h"
#include "gpu/hooks/draw_dispatch.h"
#include "gpu/frame.h"
#include "gpu/post_chain.h"
#include "gpu/scene/host_draw.h"
#include "gpu/scene/node_tag.h"
#include "gpu/scene/scene_recorder.h"
#include "gpu/shaders/shader_cache.h"
#include "gpu/shaders/shader_constants.h"

REXCVAR_DECLARE(bool, bd_cel_characters);
REXCVAR_DECLARE(bool, bd_debug_skip_list_draws);
REXCVAR_DECLARE(bool, bd_debug_skip_blended);
REXCVAR_DECLARE(i32, bd_debug_max_draws);
REXCVAR_DECLARE(bool, bd_stereo);
REXCVAR_DECLARE(bool, bd_stereo_multiview);
REXCVAR_DECLARE(bool, bd_mv_half_width);
REXCVAR_DECLARE(i32, bd_dump_post_draws);
REXCVAR_DECLARE(bool, bd_draw_defer_each);
REXCVAR_DECLARE(bool, bd_draw_ledger);
REXCVAR_DECLARE(bool, bd_debug_blend_off);
REXCVAR_DECLARE(bool, bd_xr_eye_sized);
REXCVAR_DECLARE(bool, bd_draw_instancing_reorder_blended);
REXCVAR_DECLARE(i32, bd_node_diag_mesh);
REXCVAR_DECLARE(bool, bd_tail_identity_skip);
REXCVAR_DECLARE(i32, bd_render_scale);
REXCVAR_DECLARE(f64, bd_stereo_separation);
REXCVAR_DECLARE(f64, bd_stereo_convergence);

REXCVAR_DECLARE(bool, bd_draw_phase_timing);

namespace {
// Defined beside UploadAndBindUpVertices below.
void LedgerNote(const bd::gpu::scene::NodeTag &tag, const bd::gpu::QueuedDraw &q,
                const char *path);

namespace xe = rex::graphics::xenos;

struct DrawArgs {
  bool indexed = false;
  bool is_up = false;
  // A full-frame bd_simple2d quad whose texture is the image it draws into
  // (the tail's tile copy): an identity, skipped (bd_tail_identity_skip).
  bool identity_copy = false;
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

// Whether a draw's vertex data is still valid at the end of the render pass.
//
// DrawVerticesUP and BeginVertices upload the guest's vertices into a scratch
// buffer on the spot, and a later draw in the same pass reuses that scratch. A
// deferred replay therefore reads whatever overwrote it - which on the Thor
// produced a scene target full of NaN and a tenth of the GPU work, because the
// geometry came out degenerate. Only draws that read a real guest vertex buffer
// can wait.
bool VertexDataOutlivesTheDraw(const char *name) {
  // By name, explicitly. A character test is too clever here: "DrawVertices"
  // and "DrawVerticesUP" agree on every letter up to the suffix that decides
  // it, so any prefix check silently defers exactly the draws that must not be.
  return std::strcmp(name, "DrawIndexedVertices") == 0 ||
         std::strcmp(name, "DrawVertices") == 0;
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
  bd::gpu::scene::NoteGuestDeviceVa(device_guest);
  bd::gpu::NoteDraw();
  bd::gpu::NoteDrawVertices(args.vertexOrIndexCount);
  bd::gpu::state().current_draw_count = args.vertexOrIndexCount;
  // The fragment census's path census: this draw's pixel shader and the
  // boolean constant words that steer it (bd_frag_census).
  if (const auto *dev = bd::mem::at<const bd::gpu::D3DDevice>(device_guest)) {
    const auto *ps = bd::gpu::state().pixel_shader;
    const u32 bools[4] = {
        u32(dev->psBoolConstants[0]), u32(dev->psBoolConstants[1]),
        u32(dev->psBoolConstants[2]), u32(dev->psBoolConstants[3])};
    bd::gpu::FragCensusNoteDraw(
        (ps && ps->shaderCacheEntry) ? ps->shaderCacheEntry->hash : 0ull,
        bools);
  }
  // bd_dump_post_draws: name every post-effect draw of the next N frames by
  // its pixel shader, with the target it writes, the textures it samples and
  // the two parameter registers the bd_pe_* shaders read (c26 g_vCount, c27
  // g_vParam). The filters are vtable-dispatched and the function map does not
  // name them, so this log is the only record of the chain's per-frame order
  // (2026-09-02, for the host-owned post chain).
  if (const i32 dump_frames = REXCVAR_GET(bd_dump_post_draws); dump_frames > 0) {
    auto &vs = bd::gpu::state();
    const auto *ps = vs.pixel_shader;
    const u64 ps_hash =
        (ps && ps->shaderCacheEntry) ? ps->shaderCacheEntry->hash : 0;
    static const struct {
      u64 hash;
      const char *name;
    } kPostShaders[] = {
        {0xFFDBD782126EB6E8ull, "brightpass"}, {0xD386EA2FABF16CE9ull, "ms_bright"},
        {0x57119886DF6D73C0ull, "ms_ave"},     {0x1E2676BD7DBBE4F7ull, "ms_weight"},
        {0x620B403BCBBF1B98ull, "ms_tex"},     {0x77344D98A7F5B956ull, "quoter"},
        {0xF6FF1BED057E0FC4ull, "dof"},        {0x51DE4E8BB53154BEull, "dof_quoter"},
        {0x66FC81994AC2E6F0ull, "dof_glare"},  {0x540B8B07B2908FABull, "lenzflare"},
        {0xE91190BD6F0C677Dull, "heatshimmer"}, {0xD8F0F8BA9A0F14A6ull, "colcoord"},
        {0xF9ADCBCF92CA2258ull, "discolor"},   {0x92B9BD677B2A3B82ull, "reverse"},
        {0x5B476664F31C6F18ull, "ntsc"},       {0xA78FF7850A4C372Eull, "noise"},
        {0x500521BAC3F60D5Full, "fisheye"},    {0x5C45045200D30CF0ull, "packed"},
        {0xD94E164866C3B9BCull, "blur"},
    };
    const char *post_name = nullptr;
    for (const auto &e : kPostShaders)
      if (e.hash == ps_hash)
        post_name = e.name;
    // Any other quad-sized draw into a full-screen target is a composite,
    // overlay or copy the guest makes after the chain; name it by hash.
    char other_name[24];
    if (!post_name && args.vertexOrIndexCount <= 6 && vs.render_target &&
        vs.render_target->width >= 640 && vs.depth_stencil == nullptr) {
      std::snprintf(other_name, sizeof(other_name), "quad_%08llX",
                    static_cast<unsigned long long>(ps_hash & 0xFFFFFFFFull));
      post_name = other_name;
    }
    // vs.frame is the ring slot, not a counter: frames are counted here by
    // slot changes, and the window opens after enough draws to be deep in a
    // field scene (a menu frame is ~20 draws, a field frame ~550).
    static u32 draws_total = 0;
    static u32 last_slot = ~0u;
    static u32 frame = 0;
    static u32 first_frame = 0;
    static u32 frames_seen = 0;
    ++draws_total;
    const u32 slot = vs.frame.load(std::memory_order_relaxed);
    if (slot != last_slot) {
      last_slot = slot;
      ++frame;
    }
    if (post_name && draws_total > 200000u) {
      if (first_frame == 0)
        first_frame = frame;
      if (frame - first_frame < u32(dump_frames)) {
        auto dims = [](const bd::gpu::GuestTexture *t) {
          return t ? fmt::format("{}x{}L{}", t->width, t->height, t->layers)
                   : std::string("-");
        };
        const auto *device_p =
            bd::mem::at<const bd::gpu::D3DDevice>(device_guest);
        float c26[4] = {0, 0, 0, 0};
        float c27[4] = {0, 0, 0, 0};
        if (device_p) {
          const auto *psc = reinterpret_cast<const u32 *>(
              reinterpret_cast<const u8 *>(device_p) +
              offsetof(bd::gpu::D3DDevice, psFloatConstants));
          for (int i = 0; i < 4; ++i) {
            const u32 a = __builtin_bswap32(psc[26 * 4 + i]);
            const u32 b = __builtin_bswap32(psc[27 * 4 + i]);
            std::memcpy(&c26[i], &a, 4);
            std::memcpy(&c27[i], &b, 4);
          }
        }
        BD_INFO("[post] f{} {:<11} rt={} ds={} verts={} tex0={} tex1={} "
                "tex2={} tex3={} c26=({:.3g},{:.3g},{:.3g},{:.3g}) "
                "c27=({:.3g},{:.3g},{:.3g},{:.3g}) blend={}",
                frame, post_name, dims(vs.render_target),
                dims(vs.depth_stencil), args.vertexOrIndexCount,
                dims(vs.textures[0]), dims(vs.textures[1]),
                dims(vs.textures[2]), dims(vs.textures[3]), c26[0], c26[1],
                c26[2], c26[3], c27[0], c27[1], c27[2], c27[3],
                vs.pipelineState.alphaBlendEnable ? 1 : 0);
        ++frames_seen;
      }
    }
  }

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
  // The lighting-model slot: skinned draws (the characters) get the cel
  // variant of their pixel shader when asked. Part of the PSO key.
  {
    const bool cel = REXCVAR_GET(bd_cel_characters) &&
                     bd::gpu::scene::PipelineReadsBones(s.pipelineState);
    const u32 want = cel ? (s.pipelineState.specConstants | bd::gpu::kSpecConstantCel)
                         : (s.pipelineState.specConstants & ~bd::gpu::kSpecConstantCel);
    bd::gpu::Video::SetDirtyValue<u32>(s.dirtyStates.pipelineState,
                                       s.pipelineState.specConstants, want);
  }

  const u64 ps_hash = (s.pixel_shader && s.pixel_shader->shaderCacheEntry)
                          ? s.pixel_shader->shaderCacheEntry->hash
                          : 0;
  // A guest producer draw of the post chain is dropped before its target is
  // bound: the bind seeds a fresh target from its predecessor, and those
  // copies were ten of the frame's fourteen (2026-09-02).
  if (ps_hash && bd::gpu::HostPostProducerSkip(s, ps_hash))
    return;
  // A scaled alias (the HDR scene resolved at x0.25, aliased while the host
  // post chain runs) holds the unscaled surface. A guest draw that will
  // sample it - not one the host chain takes - gets the scaled copy first.
  // Materialising at SetTexture instead copied twice a frame for the dof
  // and ms_tex draws the chain drops (Quest, rs_materialize 2, 2026-09-02).
  if (args.identity_copy) {
    // The tail's tile copy onto its own image (IsIdentityCopyQuad).
    static u32 skipped = 0;
    if (skipped++ < 3)
      BD_INFO("[tail] identity copy quad skipped ({}x{})",
              s.render_target ? s.render_target->width : 0u,
              s.render_target ? s.render_target->height : 0u);
    bd::gpu::NoteTailIdentitySkip();
    return;
  }
  if (!(ps_hash && bd::gpu::HostPostWillIntercept(ps_hash))) {
    bd::gpu::GuestTexture *rt_now = s.render_target;
    for (u32 i = 0; i < 16; ++i) {
      bd::gpu::GuestTexture *tex = s.textures[i];
      if (!tex || !tex->sourceSurface || tex->sourceSurface == tex)
        continue;
      if (tex->resolveScale != 1.0f) {
        bd::gpu::MaterializeInboundLocked(s, tex);
        continue;
      }
      // A deferred link into the image this draw renders into, sampled by
      // a draw that is not the identity copy: it needs the copy after all.
      if (tex->selfReadDeferred && rt_now && rt_now->texture &&
          tex->sourceSurface->texture == rt_now->texture) {
        static u32 told = 0;
        if (told++ < 6)
          BD_INFO("[tail] deferred self-read copied for a {}-vertex draw, ps "
                  "{:016X}", args.vertexOrIndexCount, ps_hash);
        tex->selfReadDeferred = false;
        bd::gpu::MaterializeInboundLocked(s, tex);
      }
    }
  }
  s.bind_overwrites = ps_hash && bd::gpu::HostPostOverwritesTarget(s, ps_hash);
  const bool bound = bd::gpu::Video::BindDrawFramebufferLocked();
  s.bind_overwrites = false;
  if (!bound) {
    return;
  }
  {
    // The host-owned post chain takes the guest's dof and ms_tex draws here,
    // before any state is flushed for them, and fills the textures the
    // guest's composites sample. See gpu/post_chain.cpp.
    if (ps_hash && bd::gpu::HostPostIntercept(s, ps_hash, device_guest))
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
  // Under bd_mv_half_width the scene target is half the design width, so the
  // width test compares the layer's width doubled.
  u32 width_factor =
      (REXCVAR_GET(bd_stereo_multiview) && REXCVAR_GET(bd_mv_half_width)) ? 2u
                                                                          : 1u;
  u32 want_w = u32(bd::gpu::kDesignCanvasWidth) * stereo_pct / 100u;
  u32 want_h = u32(bd::gpu::kDesignCanvasHeight) * stereo_pct / 100u;
  // Under bd_xr_eye_sized the frame is whatever fraction of the headset's own
  // per-eye rect the budget asks for, which has nothing to do with the design
  // canvas and is not squeezed. Measuring against the canvas there declared a
  // 720x400 eye frame "not the scene pass", switched the per-eye skew off and
  // presented two identical layers - stereo silently gone (2026-09-04). The
  // latched frame is the right yardstick: the scene target is the frame, and
  // the post chain's targets are fractions of it.
  if (REXCVAR_GET(bd_xr_eye_sized)) {
    u32 lw = 0, lh = 0;
    if (bd::gpu::Output::LatchedFit(lw, lh) && lw && lh) {
      width_factor = 1u;
      want_w = lw * 3u / 4u;
      want_h = lh * 3u / 4u;
    }
  }
  const bool scene_pass = s.render_target != nullptr &&
                          s.render_target->width * width_factor >= want_w &&
                          s.render_target->height >= want_h &&
                          args.vertexOrIndexCount > 6;
  s.stereoEligible = scene_pass;

  // Fragment census probes, not settings (2026-09-03): the scene pass is
  // bound by fragments x texture fetches (the Quest's texture pipes at 72%,
  // unchanged with every fetch on a tiny mip level), and a within-run A/B
  // that drops one class of scene draw reads that class's GPU share. The
  // render list carries the guest's sorted and translucent materials; the
  // blended class is everything drawn with alpha blending on.
  if (scene_pass) {
    if (REXCVAR_GET(bd_debug_skip_list_draws) &&
        bd::gpu::scene::CurrentNodeTag().from_list)
      return;
    if (REXCVAR_GET(bd_debug_skip_blended) && s.pipelineState.alphaBlendEnable)
      return;
  }

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
        // The resolve takes the command list over - its own framebuffer,
        // viewport, pipeline and draws - so anything queued has to come out
        // first. Left queued, those draws would be replayed after the resolve
        // against its framebuffer and its viewport rather than their own,
        // which is why the deferred path rendered a black scene target on the
        // Quest while the same code was correct with a flush after every draw.
        if (s.plume_framebuffer_bound)
          bd::gpu::DrawQueueFlushAt(s.command_list, BD_FLUSH_SITE);
        bd::gpu::ResolveMultiviewSurfaceLocked(s, surf);
      }
    }
  }

  // Arm deferral before the flush, because the flush is what resolves a draw
  // into the handful of things worth recording.
  //
  // The side-by-side stereo path defers too, as of 2026-09-02: a queued draw
  // carries its own viewport, scissor and constant offsets now, so the per-eye
  // loop below records one queued draw per eye. Until then the shipping stereo
  // path bypassed the queue entirely - and with it the sort, the prepass and
  // every other technique that attaches there.
  // Not on the quad-list path, which rewrites the index binding itself.
  s.deferring_draw = bd::gpu::DrawQueueEnabled() && primitive_type != 13 &&
                     VertexDataOutlivesTheDraw(name);

  // A draw that cannot wait forces out everything that was waiting, so the
  // order the guest submitted in is preserved exactly. Without this, an
  // immediate draw would land before draws the guest issued before it.
  if (!s.deferring_draw && s.plume_framebuffer_bound)
    bd::gpu::DrawQueueFlushAt(s.command_list, BD_FLUSH_SITE);
  if (s.deferring_draw)
    s.pending = bd::gpu::QueuedDraw{};

  const auto t_fb = phase_timing ? bd::gpu::DrawPhaseNow() : 0;
  if (!bd::gpu::Video::FlushRenderStateLocked(device_guest)) {
    s.deferring_draw = false;
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
                            s.render_target->height, s.render_target->layers,
                            s.depth_stencil != nullptr);
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

  // Blended draws that also write depth. Qualcomm's and Mesa's documentation
  // both say writing depth with blend enabled forces low-resolution Z to be
  // invalidated - so one such draw early in a pass costs early rejection for
  // every draw after it. The scene carries ~2x overdraw (measured: forcing
  // depth ALWAYS doubles desktop GPU time), so that would be expensive.
  //
  // Counted rather than assumed, because two theories for why LRZ is off have
  // already been tested and disproved.
  {
    const bool heuristic =
        !(s.pipelineState.srcBlend == plume::RenderBlend::ONE &&
          s.pipelineState.destBlend == plume::RenderBlend::ZERO);
    const bool writes_depth =
        s.pipelineState.zWriteEnable && s.pipelineState.zEnable;
    bd::gpu::NoteBlendedDepthWrite(s.pipelineState.alphaBlendEnable, heuristic,
                                   writes_depth);
    if (s.pipelineState.alphaBlendEnable && writes_depth)
      bd::gpu::NoteBlendedDepthWriteAlphaTest(
          (s.pipelineState.specConstants & bd::gpu::kSpecConstantAlphaTest) != 0);
    if (s.pipelineState.alphaBlendEnable && writes_depth)
      bd::gpu::NoteBlendDepthMode(u32(s.pipelineState.srcBlend),
                                  u32(s.pipelineState.destBlend));
  }

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
  // Record instead of emit. With an eye viewport the queued draw takes that
  // viewport and whatever vertex-constant offset the eye skew just uploaded;
  // without one it keeps what FlushViewport and FlushRenderState recorded.
  const auto push_queued = [&](const plume::RenderViewport *eye_vp,
                               const plume::RenderRect *eye_rc) {
    bd::gpu::QueuedDraw q = s.pending;
    q.indexed = args.indexed;
    q.count = args.vertexOrIndexCount;
    q.start_index = args.startIndex;
    q.base_vertex = args.baseVertexIndex;
    q.start_vertex = args.startVertex;
    // D3D9's "no blend" is ONE/ZERO. Anything else depends on what is already
    // in the framebuffer, so it keeps submission order when the queue sorts.
    q.blended = !(s.pipelineState.srcBlend == plume::RenderBlend::ONE &&
                  s.pipelineState.destBlend == plume::RenderBlend::ZERO);
    // The blend-off probe (pipeline_cache.cpp) made this draw's pipeline
    // opaque; the queue must see it as opaque too, or nothing is sorted.
    if (REXCVAR_GET(bd_debug_blend_off) && s.pipelineState.alphaBlendEnable &&
        s.pipelineState.zWriteEnable && s.pipelineState.zEnable &&
        s.pipelineState.srcBlend == plume::RenderBlend::SRC_ALPHA &&
        s.pipelineState.destBlend == plume::RenderBlend::INV_SRC_ALPHA)
      q.blended = false;
    // Order-independent: depth-tested LESS/LEQUAL, no stencil, and either
    // opaque or (bd_draw_instancing_reorder_blended) blended with depth
    // writes on - which in this scene is nearly every draw, because the
    // guest leaves blending enabled on opaque and cut-out materials, and a
    // real transparency does not write depth. Two such draws commute up to
    // coplanar ties and to blended overlap between two depth-writers, both
    // accepted (approximate visuals, owner 2026-09-02). Anything else - a
    // depth test off or ALWAYS, stencil, blending without a depth write -
    // depends on what came before and keeps its place; the queue reorders
    // only inside runs of reorderable draws, so a run never crosses one.
    const bool depth_tested =
        s.pipelineState.zEnable && !s.pipelineState.stencilEnable &&
        (s.pipelineState.zFunc == plume::RenderComparisonFunction::LESS ||
         s.pipelineState.zFunc == plume::RenderComparisonFunction::LESS_EQUAL);
    q.reorderable =
        depth_tested &&
        (!q.blended || (s.pipelineState.zWriteEnable &&
                        REXCVAR_GET(bd_draw_instancing_reorder_blended)));
    // Front-to-back key: the view-space distance of the scene node the guest's
    // cull traverse last visited, which is the node these draws belong to. The
    // guest computes it anyway for its own culling, so this costs a load.
    q.depth = static_cast<float>(bd::engine::LastNodeViewDistanceSq());
    {
      const auto &tag = bd::gpu::scene::CurrentNodeTag();
      q.visual_va = tag.valid ? tag.visual_va : 0u;
      q.render_view = tag.valid ? tag.render_view : 0xFFu;
      q.zwrite = s.pipelineState.zWriteEnable;
      if (!bd::gpu::scene::LastNodeSphere(q.sphere))
        q.sphere[3] = 0.0f;
      q.vs_reg_mask = (s.pipelineState.vertexShader &&
                       s.pipelineState.vertexShader->shaderCacheEntry)
                          ? s.pipelineState.vertexShader->shaderCacheEntry
                                ->constantRegisterMask
                          : nullptr;
      bool any = false, opaque = true, others_opaque = true;
      for (u32 k = 0; k < 16; ++k) {
        const bd::gpu::GuestTexture *t = s.textures[k];
        if (!t || t->type != bd::gpu::ResourceType::Texture)
          continue;
        any = true;
        if (t->alphaOpaque != 1) {
          opaque = false;
          if (k != 0)
            others_opaque = false;
        }
      }
      q.tex_opaque = any && opaque;
      // Slot 0 (the colour texture) the only partial-alpha one bound: a
      // per-triangle footprint test against that texture could promote the
      // draw (the census counts these as the reachable candidates).
      q.tex_slot0_only = any && !opaque && others_opaque;
    }
    q.recorded_rt = s.render_target;
    q.framebuffer = s.pending_framebuffer;
    if (eye_vp) {
      q.viewport = *eye_vp;
      q.scissor = *eye_rc;
      q.has_viewport = true;
      q.constant_offsets[0] = s.constant_dyn_offsets[0];
      // The eye skew rewrote the vertex block; a record staged before it
      // holds the unskewed one, so this eye takes a record of its own. Out
      // of records, the plain pipeline reads the skewed window the eye bind
      // just uploaded, which is exact.
      if (q.record_index != ~0u) {
        q.record_index = bd::gpu::StageInstanceRecord();
        if (!bd::gpu::VertexPullStage(q.record_index, s))
          q.pulled_pipeline = nullptr;
        if (q.record_index == ~0u)
          q.instanced_pipeline = nullptr;
      }
    }
    // The scene recorder sees the draw whole - pipeline, streams, offsets,
    // textures - plus the node tag the DrawSingle hook set. Only while its
    // window is open.
    if (bd::gpu::scene::RecordingArmed())
      bd::gpu::scene::OnQueuedDraw(s, q, device_guest);
    // The host-issued draw's template: what the interpreter produced for
    // this node, kept so the next frames skip the interpreter.
    if (!eye_vp && bd::gpu::scene::HostDrawEnabled())
      bd::gpu::scene::HostDrawCapture(s, q, device_guest, primitive_type);
    if (const u32 diag = static_cast<u32>(REXCVAR_GET(bd_node_diag_mesh));
        diag && (diag < 4096 ? bd::gpu::scene::CurrentNodeTag().node_index == diag
                             : bd::gpu::scene::CurrentNodeTag().mesh_va == diag)) {
      const auto &tag = bd::gpu::scene::CurrentNodeTag();
      // Frames in which this node's scene-pass draw never came: the
      // intermittently vanishing rock, if the walk is what drops it.
      {
        static u32 last_frame = 0, seen = 0, frames = 0, missing = 0;
        const u32 frame = bd::gpu::FrameStatFrameCount();
        if (frame != last_frame) {
          if (last_frame && seen == 0 && ++missing <= 8)
            BD_INFO("[node-diag] frame {}: node {} had no view-3 draw",
                    last_frame, diag);
          if (++frames == 600)
            BD_INFO("[node-diag] over {} frames node {} missed its view-3 draw "
                    "in {}", frames, diag, missing);
          last_frame = frame;
          seen = 0;
        }
        if (tag.render_view == 3)
          ++seen;
      }
      std::string tex;
      for (u32 k = 0; k < 6; ++k)
        tex += fmt::format(" {}:{:x}", k,
                           s.textures[k] ? (s.textures[k]->contentHash & 0xFFFF)
                                         : 0);
      BD_INFO("[node-diag] {} node {} view {} pass {} fb {} rt {} pso {} inst {} "
              "count {} start {} base {} co {}/{}/{} ib {}+{} vb0 {}+{} tex{} "
              "alpha {:.3f} blended {} depth {:.0f}",
              bd::gpu::scene::HostDrawReplaying() ? "REPLAY" : "interp",
              tag.node_index, tag.render_view, bd::gpu::CurrentRenderPassId(),
              static_cast<const void *>(q.framebuffer),
              static_cast<const void *>(s.render_target),
              static_cast<const void *>(q.pipeline),
              static_cast<const void *>(q.instanced_pipeline), q.count,
              q.start_index, q.base_vertex, q.constant_offsets[0],
              q.constant_offsets[1], q.constant_offsets[2],
              static_cast<const void *>(q.index_view.buffer.ref),
              q.index_view.buffer.offset,
              static_cast<const void *>(q.vertex_views[0].buffer.ref),
              q.vertex_views[0].buffer.offset, tex,
              bd::gpu::Video::AlphaThreshold(), q.blended ? 1 : 0, q.depth);
      // The registers and slots that decide what the draw looks like, so a
      // replayed draw can be diffed against the interpreted one.
      const float *vs = bd::gpu::StagedVertexBlock();
      const float *ps = bd::gpu::StagedPixelBlock();
      std::string regs;
      for (u32 r : {20u, 21u, 22u, 23u, 57u})
        regs += fmt::format(" c{}=({:.3f} {:.3f} {:.3f} {:.3f})", r, vs[r * 4],
                            vs[r * 4 + 1], vs[r * 4 + 2], vs[r * 4 + 3]);
      std::string pregs;
      for (u32 r = 0; r < 8; ++r)
        pregs += fmt::format(" p{}=({:.3f} {:.3f} {:.3f} {:.3f})", r, ps[r * 4],
                             ps[r * 4 + 1], ps[r * 4 + 2], ps[r * 4 + 3]);
      std::string slots;
      for (u32 k = 0; k < 8; ++k) {
        const auto *t = s.textures[k];
        if (!t)
          continue;
        slots += fmt::format(" {}:va{:08X}/{}x{}/t{}{}", k, t->selfVa, t->width,
                             t->height, u32(t->type),
                             t->sourceSurface ? "/link" : "");
      }
      BD_INFO("[node-diag]   vs{} | ps{} | tex{}", regs, pregs, slots);
    }
    // Occlusion culling (gpu/occlusion_cull.h): a scene node whose proxy
    // passed no sample two frames running is dropped here, after every
    // state effect of its dispatch has happened.
    {
      const auto &tag = bd::gpu::scene::CurrentNodeTag();
      // Depth-tested draws only: an occluded one contributes nothing; a
      // draw with the depth test off is an overlay and keeps its place.
      const bool depth_tested =
          s.pipelineState.zEnable &&
          (s.pipelineState.zFunc == plume::RenderComparisonFunction::LESS ||
           s.pipelineState.zFunc == plume::RenderComparisonFunction::LESS_EQUAL);
      if (tag.valid && tag.render_view == 3 && depth_tested) {
        const u64 key = (u64(tag.matrix_va) << 32) | u64(tag.mesh_va);
        static u32 told = 0;
        if (told++ < 3)
          BD_INFO("[occ] dispatch key {:016X} (matrix {:08X} mesh {:08X} list {})",
                  key, tag.matrix_va, tag.mesh_va, tag.from_list ? 1 : 0);
        if (bd::gpu::OcclusionCullOccluded(key)) {
          LedgerNote(tag, q, "dropped");
          return;
        }
      }
    }
    LedgerNote(bd::gpu::scene::CurrentNodeTag(), q,
               bd::gpu::scene::HostDrawReplaying() ? "replay" : "interp");
    bd::gpu::DrawQueuePush(q);
  };
  const auto finish_deferred = [&]() {
    s.deferring_draw = false;
    // Flush every draw: functionally identical to immediate submission, but it
    // still goes through the whole record-and-replay path. If this renders
    // correctly the state capture is right and the fault is in batching or
    // ordering; if it is still black the capture itself is wrong. There is no
    // way to tell those apart from the batched result alone.
    if (REXCVAR_GET(bd_draw_defer_each))
      bd::gpu::DrawQueueFlushAt(s.command_list, BD_FLUSH_SITE);
  };

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
    if (s.deferring_draw) {
      push_queued(nullptr, nullptr);
      finish_deferred();
    } else {
      emit();
    }
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
    if (s.deferring_draw)
      push_queued(&half, &rc);
    else
      emit();
  }
  if (s.deferring_draw)
    finish_deferred();
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

// The draw ledger (bd_draw_ledger): one line per queued scene draw, with
// the frame, the node key and the path it came by, so a frame with a hole
// in a capture sequence can be diffed against its neighbour by draw.
void LedgerNote(const bd::gpu::scene::NodeTag &tag, const bd::gpu::QueuedDraw &q,
                const char *path) {
  if (!REXCVAR_GET(bd_draw_ledger))
    return;
  static std::ofstream out;
  static bool opened = false;
  if (!opened) {
    opened = true;
    out.open((bd::AppRootFolder() / "logs" / "draw_ledger.txt").string(),
             std::ios::trunc);
  }
  if (!out)
    return;
  // Fingerprints, so two frames can be diffed draw by draw: the pixel
  // block, the vertex block without the camera rows (c32-c39, which move
  // every frame), the texture slots and the pipeline (2026-09-03).
  const float *ps = bd::gpu::StagedPixelBlock();
  const float *vs = bd::gpu::StagedVertexBlock();
  const u64 ps_h = XXH3_64bits(ps, 256 * 16);
  // c0-c4 (the camera block) and c32-c39 (view-projection, the fitted
  // light matrix) move every frame; the rest is the draw's own.
  u64 vs_h = XXH3_64bits(vs + 5 * 4, (32 - 5) * 16);
  vs_h ^= XXH3_64bits(vs + 40 * 4, (256 - 40) * 16);
  const auto &st = bd::gpu::state();
  struct {
    const void *tex[16];
    const void *pipeline;
  } tp{};
  for (u32 k = 0; k < 16; ++k)
    tp.tex[k] = st.textures[k];
  tp.pipeline = q.pipeline;
  const u64 tex_h = XXH3_64bits(&tp, sizeof(tp));
  const u64 ps_hash = (st.pixel_shader && st.pixel_shader->shaderCacheEntry)
                          ? st.pixel_shader->shaderCacheEntry->hash
                          : 0ull;
  out << bd::gpu::FrameStatFrameCount() << ' ' << std::hex << tag.matrix_va
      << ' ' << tag.mesh_va << ' ' << tag.visual_va << std::dec << ' '
      << tag.render_view << ' ' << (tag.from_list ? 1 : 0) << ' ' << path
      << ' ' << q.count << ' ' << (q.blended ? 1 : 0) << ' ' << std::hex
      << (ps_h & 0xFFFFFFFFu) << ' ' << (vs_h & 0xFFFFFFFFu) << ' '
      << (tex_h & 0xFFFFFFFFu) << ' ' << ps_hash << std::dec << ' '
      << q.depth << ' ' << u32(st.pipelineState.primitiveTopology) << '\n';
}

bool UploadAndBindUpVertices(u32 primitiveType, u32 pVertexData,
                             u32 vertexCount, u32 vertexStride) {
  if (!pVertexData || !vertexCount || !vertexStride)
    return false;
  const u32 totalSize = vertexCount * vertexStride;
  const bool text_batch = pVertexData == kTextQuadBatchEA;
  // The glyph batch is the same bytes frame after frame, and its processing
  // (the swap, the canvas fit, the half-texel UV inset over every quad) was
  // the single hottest host function in the 2026-09-03 desktop profile, 6% of
  // the Draw Thread's samples. Processed once per content change: the raw
  // bytes are hashed, and a hit uploads the processed copy instead.
  struct TextBatchCache {
    u64 hash = 0;
    u32 size = 0;
    u32 stride = 0;
    u32 tex_w = 0;
    u32 tex_h = 0;
    float kx = 0.0f;
    float ky = 0.0f;
    bool drain = false;
    std::vector<u8> bytes;
  };
  static TextBatchCache text_cache; // guest thread only
  // Where the inset's time goes: calls and vertices per path, every 300 frames.
  struct UpDiag {
    u32 frame = 0;
    u32 text_calls = 0, text_hits = 0, text_verts = 0, text_max = 0;
    u32 sprite_calls = 0, sprite_verts = 0, other_calls = 0, other_verts = 0;
  };
  static UpDiag diag;
  {
    const u32 f = bd::gpu::FrameStatFrameCount();
    if (f - diag.frame >= 300) {
      if (diag.frame)
        BD_INFO("[up] per frame: text {:.1f} calls ({:.1f} hits) {:.0f} verts "
                "max {} | sprite {:.1f} calls {:.0f} verts | other {:.1f} calls "
                "{:.0f} verts",
                diag.text_calls / 300.0, diag.text_hits / 300.0,
                diag.text_verts / 300.0, diag.text_max,
                diag.sprite_calls / 300.0, diag.sprite_verts / 300.0,
                diag.other_calls / 300.0, diag.other_verts / 300.0);
      diag = UpDiag{};
      diag.frame = f;
    }
    if (text_batch) {
      ++diag.text_calls;
      diag.text_verts += vertexCount;
      diag.text_max = std::max(diag.text_max, vertexCount);
    } else if (IsScreenSpriteQuad(primitiveType, vertexCount, vertexStride)) {
      ++diag.sprite_calls;
      diag.sprite_verts += vertexCount;
    } else {
      ++diag.other_calls;
      diag.other_verts += vertexCount;
    }
  }
  u64 text_hash = 0;
  const u8 *text_raw = nullptr;
  u32 tex_w = 0;
  u32 tex_h = 0;
  const float kx = bd::gpu::Output::DesignScaleX();
  const float ky = bd::gpu::Output::DesignScaleY();
  const bool drain = bd::gpu::Video::DesignCanvasDrain();
  if (text_batch) {
    const auto *tex = bd::gpu::state().textures[0];
    tex_w = tex ? tex->width : 0u;
    tex_h = tex ? tex->height : 0u;
    text_raw = bd::mem::at<const u8>(pVertexData);
    if (text_raw) {
      text_hash = XXH3_64bits(text_raw, totalSize);
      if (text_cache.hash == text_hash && text_cache.size == totalSize &&
          text_cache.stride == vertexStride && text_cache.tex_w == tex_w &&
          text_cache.tex_h == tex_h && text_cache.kx == kx &&
          text_cache.ky == ky && text_cache.drain == drain) {
        auto hit = bd::gpu::UploadHostBytes(text_cache.bytes.data(),
                                            totalSize, /*alignment=*/4);
        if (!hit.memory)
          return false;
        ++diag.text_hits;
        bd::gpu::state().overlay2D = true;
        bd::gpu::Video::SetVertexStream(0, hit.ref, hit.size, vertexStride);
        return true;
      }
    }
  }
  // Swapped into a host scratch, fixed up there, uploaded once. The fix-ups
  // below used to run on the upload ring's mapped memory, which is
  // host-visible GPU memory: write-combined BAR on a desktop, uncached on the
  // Quest. Reading it back made two four-vertex quads a frame the hottest
  // host function in the 2026-09-03 desktop profile (8% of the Draw Thread).
  static std::vector<u8> scratch; // guest thread only
  scratch.resize(totalSize);
  const u8 *raw = bd::mem::at<const u8>(pVertexData);
  if (!raw)
    return false;
  bd::gpu::ByteSwap32ToHost(scratch.data(), raw, totalSize);
  struct HostAlloc {
    u8 *memory;
  } alloc{scratch.data()};
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
  if (text_batch && text_raw) {
    text_cache.hash = text_hash;
    text_cache.size = totalSize;
    text_cache.stride = vertexStride;
    text_cache.tex_w = tex_w;
    text_cache.tex_h = tex_h;
    text_cache.kx = kx;
    text_cache.ky = ky;
    text_cache.drain = drain;
    text_cache.bytes.assign(alloc.memory, alloc.memory + totalSize);
  }
  auto up = bd::gpu::UploadHostBytes(scratch.data(), totalSize, /*alignment=*/4);
  if (!up.memory)
    return false;
  bd::gpu::Video::SetVertexStream(0, up.ref, up.size, vertexStride);
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

// The tail's tile copy: a full-canvas bd_simple2d strip, white, uv 0..1,
// sampling a texture whose image is the bound target's own (a deferred link
// out of the composite's tile). Drawing it would copy the image onto itself.
bool IsIdentityCopyQuad(u32 primitiveType, u32 vertexCount, u32 pVertexData,
                        u32 vertexStride) {
  if (!REXCVAR_GET(bd_tail_identity_skip))
    return false;
  if (!IsScreenSpriteQuad(primitiveType, vertexCount, vertexStride))
    return false;
  auto &s = bd::gpu::state();
  const auto *ps = s.pixel_shader;
  constexpr u64 kSimple2DPS = 0xFF2C108217046270ull;
  if (!ps || !ps->shaderCacheEntry || ps->shaderCacheEntry->hash != kSimple2DPS)
    return false;
  bd::gpu::GuestTexture *rt = s.render_target;
  bd::gpu::GuestTexture *tex = s.textures[0];
  if (!rt || !rt->texture || !tex || !tex->selfReadDeferred ||
      !tex->sourceSurface || tex->sourceSurface == tex)
    return false;
  if (tex->sourceSurface->texture != rt->texture)
    return false;
  const auto *v = bd::mem::try_at<const ScreenSpriteVertex>(pVertexData);
  if (!v)
    return false;
  float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
  float minu = 1e9f, minv = 1e9f, maxu = -1e9f, maxv = -1e9f;
  for (u32 i = 0; i < kQuadVertices; ++i) {
    if (static_cast<u32>(v[i].color) != 0xFFFFFFFFu)
      return false;
    const float x = v[i].x, y = v[i].y, u = v[i].u, w = v[i].v;
    minx = std::min(minx, x); maxx = std::max(maxx, x);
    miny = std::min(miny, y); maxy = std::max(maxy, y);
    minu = std::min(minu, u); maxu = std::max(maxu, u);
    minv = std::min(minv, w); maxv = std::max(maxv, w);
  }
  const float cw = bd::gpu::kDesignCanvasWidth, ch = bd::gpu::kDesignCanvasHeight;
  if (minx > 1.0f || miny > 1.0f || maxx < cw - 1.0f || maxy < ch - 1.0f)
    return false;
  if (minu > 0.01f || minv > 0.01f || maxu < 0.99f || maxv < 0.99f)
    return false;
  return true;
}

u32 D3DDevice_DrawVerticesUP_hook(u32 device_guest, u32 primitiveType,
                                  u32 vertexCount, u32 pVertexData,
                                  u32 vertexStride) {
  InsetOverscanScreenSprite(pVertexData, primitiveType, vertexCount,
                            vertexStride);
  const bool identity =
      IsIdentityCopyQuad(primitiveType, vertexCount, pVertexData, vertexStride);
  const bool ok =
      !identity && UploadAndBindUpVertices(primitiveType, pVertexData,
                                           vertexCount, vertexStride);
  DrawArgs args{};
  args.is_up = ok;
  args.identity_copy = identity;
  args.vertexOrIndexCount = vertexCount;
  args.startVertex = 0;
  bd::gpu::NoteDrawKind(2);
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
  bd::gpu::NoteDrawKind(3);
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
  bd::gpu::NoteDrawKind(0);
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
  bd::gpu::NoteDrawKind(1);
  DispatchDraw(device_guest, primitiveType, "DrawVertices", args);
  return 0;
}

} // namespace

namespace bd::gpu::hooks {
void DispatchHostNodeDraw(u32 device_guest, u32 primitive_type, bool indexed,
                          u32 count, u32 start_index, i32 base_vertex,
                          u32 start_vertex) {
  DrawArgs args{};
  args.indexed = indexed;
  args.vertexOrIndexCount = count;
  args.startIndex = start_index;
  args.baseVertexIndex = base_vertex;
  args.startVertex = start_vertex;
  bd::gpu::NoteDrawKind(indexed ? 0 : 1);
  DispatchDraw(device_guest, primitive_type,
               indexed ? "DrawIndexedVertices" : "DrawVertices", args);
}
} // namespace bd::gpu::hooks

REX_HOOK(D3DDevice_DrawVertices, D3DDevice_DrawVertices_hook);
REX_HOOK(D3DDevice_DrawVerticesUP, D3DDevice_DrawVerticesUP_hook);
REX_HOOK(D3DDevice_DrawIndexedVertices, D3DDevice_DrawIndexedVertices_hook);
REX_HOOK(D3DDevice_BeginVertices, D3DDevice_BeginVertices_hook);
REX_HOOK(D3DDevice_EndVertices, D3DDevice_EndVertices_hook);
REX_HOOK(D3DDevice_Resolve, D3DDevice_Resolve_hook);
REX_HOOK(D3DDevice_BeginTiling, D3DDevice_BeginTiling_hook);
REX_HOOK(D3DDevice_EndTiling, D3DDevice_EndTiling_hook);

