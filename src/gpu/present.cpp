/**
 * @file    gpu/present.cpp
 * @brief   End of frame: the deferred clear, the swapchain
 * acquire/blit/present, and the pre-Runtime overlay-only present the installer
 * uses.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/draw_queue.h"
#include "gpu/frame.h"
#include "gpu/host_targets.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <mutex>
#include <thread>

#include <plume_render_interface.h>
#if defined(REBLUE_OPENXR)
#include <plume_vulkan.h>

#include "xr/xr_session.h"
#include "xr/xr_game_camera.h"
#include "xr/xr_settings.h"
#endif

#include "core/app_root.h"
#include "gpu/gpu_profiling.h"
#include "gpu/renderdoc_capture.h"

#include "core/logging.h"
#include "core/ab_experiment.h"
#include "core/sampling_profiler.h"
#include "engine/engine.h"
#include "gpu/backend.h"
#include "gpu/constant_buffers.h"
#include "gpu/frame_stats.h"
#include "gpu/gpu_timing.h"
#include "gpu/frag_census.h"
#include "gpu/occlusion_cull.h"
#include "gpu/output.h"
#include "gpu/screenshot.h"
#include "gpu/settings.h"

REXCVAR_DECLARE(bool, bd_cel_shading);
REXCVAR_DECLARE(bool, bd_mv_capture_array);
REXCVAR_DECLARE(bool, bd_mv_capture_resolved);
REXCVAR_DECLARE(double, bd_capture_after_s);
REXCVAR_DECLARE(i32, bd_capture_min_draws);
REXCVAR_DECLARE(i32, bd_capture_frames);
REXCVAR_DECLARE(bool, bd_debug_present_clear);
REXCVAR_DECLARE(bool, bd_mv_debug_layer_diff);
REXCVAR_DECLARE(bool, bd_xr_mirror);
REXCVAR_DECLARE(f64, bd_xr_present_scale);
REXCVAR_DECLARE(bool, bd_xr_direct_present);
REXCVAR_DECLARE(bool, bd_xr_layered_swapchain);
REXCVAR_DECLARE(bool, bd_xr_eye_sized);
REXCVAR_DECLARE(double, bd_xr_render_scale);
REXCVAR_DECLARE(bool, bd_stereo_multiview);

namespace bd::gpu {

namespace {

// Defined with the XR target wrappers far below; the present pass reads it
// to know its back buffer is the runtime's layered swapchain image.
extern i32 g_xr_direct_index;

// Defined further down, next to the rest of the capture machinery. Declared
// here because the only point at which the composited frame - game plus
// overlay - actually exists is inside RecordPresentPass, and the capture has
// to be recorded there.
// Matches the back-buffer format chosen in Present below.
#if defined(__ANDROID__)
constexpr auto kCaptureFmt = plume::RenderFormat::R8G8B8A8_UNORM;
constexpr bool kCaptureIsBgra = false;
#else
constexpr auto kCaptureFmt = plume::RenderFormat::B8G8R8A8_UNORM;
constexpr bool kCaptureIsBgra = true;
#endif

bool CaptureDue();
bool RecordCapture(VideoState &s, plume::RenderTexture *back, u32 width,
                   u32 height, plume::RenderFormat format, u32 layers,
                   plume::RenderTextureLayout current);
void ResolveCapture(bool bgra);
// Set when the capture was taken inside the present pass, so the later
// swapchain-sourced site does not fire as well.
bool g_captured_in_pass = false;

// setVsyncEnabled only assigns the bool the present path reads, so applying it
// every frame is free and stops a resize (which rebuilds the swapchain with
// vsync on) leaving a stale value.
// True while the OpenXR compositor owns frame pacing.
//
// In VR there must be exactly one thing deciding when a frame goes out, and it
// has to be xrWaitFrame - the compositor knows when the next scanout is and
// nothing else does. Everything below that would also like to pace gets turned
// off while this is true, because two pacers do not average out, they beat
// against each other and the result reads as judder.
bool XrCompositorPacing() {
#if defined(REBLUE_OPENXR)
  return bd::xr::Session::Get().Running();
#else
  return false;
#endif
}

void ApplyVsync(VideoState &s) {
  if (s.swap_chain) {
    // The flat present still happens in VR - plume drives the swapchain
    // acquire/release cycle off it - but on a headset nobody ever sees that
    // surface, and blocking on its vsync just adds a second clock competing
    // with the compositor's. Present immediately and let xrWaitFrame pace.
    s.swap_chain->setVsyncEnabled(!XrCompositorPacing() &&
                                  Settings::Get().Vsync());
  }
}

// Sleeps (no busy-wait) so consecutive presents sit at least 1000/bd_fps_limit
// ms apart, and 0 disables. Pacing the render thread back-pressures the guest
// main thread through the DrawEnd event. Runs even under vsync, which paces to
// the monitor instead, and a 120Hz panel ran the loop at 120 under a 60 cap.
void PaceFrame() {
  using Clock = std::chrono::steady_clock;
  static Clock::time_point next{};
  // Third clock, same problem. bd_fps_limit defaults to 0 so this is usually
  // inert, but a cap set for the flat game would fight the compositor here.
  if (XrCompositorPacing())
    return;
  i32 fps = bd::engine::Settings::Get().FPSLimit();
  // The Sofdec movie clock advances from the per-frame delta inside BD's
  // 30Hz-gated logic, so it only runs at 1.0x when the engine ticks at 30Hz.
  if (bd::engine::SofdecMoviePlaying())
    fps = 30;
  // Event cutscenes are coupled the same way (see EventScenePlaying), but never
  // raise a user cap already at or below 30.
  if ((fps == 0 || fps > 30) && bd::engine::EventScenePlaying())
    fps = 30;
  if (fps <= 0) {
    next = {};
    return;
  }
  const auto period = std::chrono::duration_cast<Clock::duration>(
      std::chrono::duration<double, std::milli>(1000.0 / fps));
  const auto now = Clock::now();
  if (next.time_since_epoch().count() != 0 && now < next) {
    std::this_thread::sleep_until(next);
    next += period;
  } else {
    next = now + period;
  }
}

// A recorded list must still keep the ring's per-slot fence discipline. A frame
// that opened none deliberately leaves the ring unrotated, so destroys stamped
// this frame carry to this slot's next reuse. Releases 'lock' when it drains.
void AbandonFrame(VideoState &s, std::unique_lock<std::mutex> &lock) {
  s.frame_present_committed = true;
  if (!s.command_list_open)
    return;
  SubmitOpenListLocked(s);
  AdvanceAndWaitReused(s);
  const u32 reclaimed = s.frame.load(std::memory_order_relaxed);
  lock.unlock();
  DrainSlot(s, reclaimed);
}

// Rebuild the swapchain and everything keyed to its back buffers.
void RebuildSwapChain(VideoState &s) {
  // Execute the open list, do not just end it: plume's CPU-side state tracker
  // updates on barriers() while the GPU transitions fire on execute, so
  // abandoning a list leaves the tracker claiming states the GPU never reached.
  SubmitOpenListLocked(s);
  // Every slot's back buffer / framebuffer reference must retire before
  // resize() destroys the old back buffer COMs, so wait every submitted slot's
  // fence.
  for (u32 i = 0; i < kNumFrames; ++i) {
    if (s.command_list_submitted[i]) {
      s.queue->waitForCommandFence(s.fences[i].get());
      s.command_list_submitted[i] = false;
    }
  }
  s.framebuffers.clear();
  if (!s.swap_chain->resize() || !BuildFramebuffers(s) ||
      !BuildPresentSemaphores(s)) {
    if (s.swap_chain->getWidth() && s.swap_chain->getHeight())
      BD_ERROR("Swap chain resize failed"); // minimized 0x0 is benign
  }
  // Engine-facing dims are latched at boot, so a resize moves only the blit
  // dest.
}

// BD renders its whole frame with RT[0] implicit, so the finished image lives
// in the back_buffer_surface placeholder and the frontBuffer handed to Swap is
// an empty guest surface that must not outrank it. 'chosen' reports the pick
// before the deferred resolve redirect, which the late-clear guard needs.
GuestTexture *SelectPresentSource(VideoState &s, GuestTexture *frontBuffer,
                                  GuestTexture *&chosen) {
  GuestTexture *last_rt = s.last_drawn_rt[s.recording_slot()];
  GuestTexture *rt = (s.back_buffer_surface && last_rt == s.back_buffer_surface)
                         ? s.back_buffer_surface
                     : (frontBuffer && frontBuffer->texture) ? frontBuffer
                     : last_rt                               ? last_rt
                     : s.last_resolved_dst ? s.last_resolved_dst
                                           : s.back_buffer_surface;
  chosen = rt;
  // A deferred resolve dst still has its content in the source surface. Not for
  // an MSAA source: the gamma blit samples a Texture2D SRV while the descriptor
  // is a Texture2DMS view.
  if (rt && rt->sourceSurface && rt->sourceSurface != rt &&
      rt->sourceSurface->texture &&
      rt->sourceSurface->sampleCount == plume::RenderSampleCount::COUNT_1 &&
      rt->sourceSurface->descriptorIndex != kInvalidDescriptorIndex) {
    rt = rt->sourceSurface;
  }
  return rt;
}

// Per-frame order: RT[0] -> COLOR_WRITE (+ late clear if no draw bound it)
// -> SHADER_READ, then back buffer -> COLOR_WRITE, fullscreen tri sampling
// RT[0]
// -> PRESENT.
void RecordPresentPass(VideoState &s, GuestTexture *rt, GuestTexture *chosen,
                       plume::RenderTexture *back,
                       plume::RenderFramebuffer *back_fb,
                       plume::RenderTextureLayout final_layout =
                           plume::RenderTextureLayout::PRESENT) {
  BD_GPU_ZONE("RecordPresentPass");

  // Flatten a still-dirty multiview target, because nothing else will.
  //
  // ResolveMultiviewSurfaceLocked otherwise runs only when the render target
  // changes *away* from a dirty layered surface, and the scene target is still
  // bound when the frame ends - so it never takes that path. Measured: the
  // resolve fired 501 times on a 120x67 bloom target and never once on the
  // 1920x1080 surface the scene draws into, while present reads
  // rt->resolvedTexture and found a companion nothing had written. That is the
  // black frame under bd_stereo_multiview.
  //
  // Here rather than in SelectPresentSource, which is where this was tried
  // first: that routine only picks the surface and runs outside the window in
  // which the command list accepts commands, so recording a resolve from it
  // faults. This function is already issuing barriers and discards, so the list
  // is provably open.
  // No multiviewDirty check here, deliberately. That flag means "a draw landed
  // on this surface since the last resolve", which is the right guard mid-frame
  // - it stops a resolve firing on a surface nothing touched. It is the WRONG
  // guard at present: the scene alternates between two pooled 1920x1080
  // surfaces (the census shows both at 0.50 binds/frame), so the one being
  // presented is routinely not the one that was resolved this frame, and it
  // arrives here with dirty=false and a companion nothing has written.
  //
  // Measured: the resolve is entered for 1920x1080 with dirty=true, while
  // present's own rt reports dirty=false in the same run. Resolving whatever is
  // about to be displayed is always correct - it is by definition the last
  // thing drawn - and a redundant resolve is idempotent.
  if (rt && rt->layers > 1 && rt->resolvedTexture)
    ResolveMultiviewSurfaceLocked(s, rt);
  // Which of those four is false when the frame comes back black? The scene
  // target is layered and present samples it, but the resolve log never names
  // 1920x1080 - so one of these is stopping it and guessing has already cost
  // two sessions.
  if (rt && rt->layers > 1) {
    static u32 told = 0;
    if (told < 3) {
      ++told;
      BD_INFO("[mv] present resolve gate: {}x{} layers={} dirty={} companion={} "
              "fb={} desc={}",
              rt->width, rt->height, rt->layers, rt->multiviewDirty,
              rt->resolvedTexture != nullptr,
              rt->resolvedFramebuffer != nullptr, rt->descriptorIndex);
    }
  }
  {
    // Which surface does present actually hand the resolve, and is it the one
    // the scene drew into? Sampling it through its own known-good descriptor
    // still gives black, so the suspicion is that these are different objects.
    static std::atomic<u32> n{0};
    const u32 i = n.fetch_add(1, std::memory_order_relaxed);
    if (i == 300 || i == 600) {
      auto *drawn = s.last_drawn_rt[s.recording_slot()];
      BD_INFO("[mv] present rt={:012X} {}x{} layers={} desc={} | last_drawn={:012X} "
              "layers={} | back={:012X}",
              u64(uintptr_t(rt)), rt ? rt->width : 0u, rt ? rt->height : 0u,
              rt ? rt->layers : 0u, rt ? rt->descriptorIndex : 0u,
              u64(uintptr_t(drawn)), drawn ? drawn->layers : 0u,
              u64(uintptr_t(s.back_buffer_surface)));
    }
  }

  if (rt->layout != plume::RenderTextureLayout::COLOR_WRITE &&
      rt->layout != plume::RenderTextureLayout::SHADER_READ) {
    const bool needs_discard =
        (rt->layout == plume::RenderTextureLayout::UNKNOWN);
    s.command_list->barriers(
        plume::RenderBarrierStage::GRAPHICS,
        plume::RenderTextureBarrier(rt->texture,
                                    plume::RenderTextureLayout::COLOR_WRITE));
    rt->layout = plume::RenderTextureLayout::COLOR_WRITE;
    // discardTexture requires RT/DS state, which the barrier just set. Same as
    // BindDrawFramebuffer.
    if (needs_discard)
      s.command_list->discardTexture(rt->texture);
  }
  // Late clear only for the placeholder back buffer (no draws this frame).
  // Clearing last_resolved_dst/last_drawn_rt would erase what is being blitted.
  const bool clear_target_is_engine_content =
      (chosen == s.last_resolved_dst) ||
      (chosen == s.last_drawn_rt[s.recording_slot()]) || (rt != chosen);
  if (s.clear_pending && !clear_target_is_engine_content &&
      rt->layout == plume::RenderTextureLayout::COLOR_WRITE) {
    plume::RenderFramebuffer *rt_fb = GetFramebuffer(s, rt, nullptr);
    if (rt_fb) {
      s.command_list->setFramebuffer(rt_fb);
      s.command_list->clearColor(0, ArgbToRenderColor(s.clear_color_argb));
      s.command_list->setFramebuffer(nullptr);
  s.plume_framebuffer_bound = false;
    }
  }

  s.clear_pending = false;
  // Plume flushes any clear still held when the list ends; the marker is per
  // frame.
  s.held_clear_rt = nullptr;

  s.command_list->barriers(
      plume::RenderBarrierStage::GRAPHICS,
      plume::RenderTextureBarrier(rt->texture,
                                  plume::RenderTextureLayout::SHADER_READ));
  rt->layout = plume::RenderTextureLayout::SHADER_READ;

  // The target's own extent, not the window's: under the headset the target
  // is the offscreen frame sized by bd_xr_present_scale, and a window-sized
  // viewport into it showed the top-left 40% of the game, magnified
  // (capture, 2026-09-02).
  const auto *back_tex = static_cast<const plume::VulkanTexture *>(back);
  const u32 swap_w = back_tex->desc.width ? back_tex->desc.width
                                          : s.swap_chain->getWidth();
  const u32 swap_h = back_tex->desc.height ? back_tex->desc.height
                                           : s.swap_chain->getHeight();
  {
    static bool logged = false;
    if (!logged) {
      logged = true;
      BD_INFO("[present] composing {}x{} game frame into a {}x{} target "
              "(texture desc {}x{}, window {}x{})",
              rt ? rt->width : 0u, rt ? rt->height : 0u, swap_w, swap_h,
              back_tex->desc.width, back_tex->desc.height,
              s.swap_chain->getWidth(), s.swap_chain->getHeight());
    }
  }
  // Under multiview the surface is a two-layer array, and rt->descriptorIndex
  // may be bound to that array rather than to the flattened side-by-side
  // companion - sampling it through a 2D view yields black. The resolve owns a
  // slot that is always the companion, so prefer it when there is one.
  // What is present actually sampling? The scene array holds both layers, the
  // blit reads layer 1 as black, and four fixes aimed upstream all missed - so
  // this reports the sample site itself rather than another theory: the image
  // the view was built against, how many layers it exposes, and whether the
  // companion path is being taken at all.
  {
    static u32 told = 0;
    if (told < 3 && rt->layers > 1) {
      ++told;
      BD_INFO("[mv] present sample site: image={:012X} viewOf={:012X} "
              "rt.layers={} view.layers={} desc={} resolvedDesc={}",
              u64(uintptr_t(rt->texture)), u64(uintptr_t(rt->textureViewOf)),
              rt->layers, rt->textureViewLayers, rt->descriptorIndex,
              rt->resolvedDescriptorIndex);
    }
  }

  const bool use_companion = rt->layers > 1 &&
                             rt->resolvedDescriptorIndex !=
                                 bd::gpu::kInvalidDescriptorIndex;
  const u32 gamma_src_desc =
      use_companion ? rt->resolvedDescriptorIndex : rt->descriptorIndex;

  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS,
                           plume::RenderTextureBarrier(
                               back, plume::RenderTextureLayout::COLOR_WRITE));
  s.command_list->setFramebuffer(back_fb);

  // Splits "present is broken" from "the blit produces nothing", which reading
  // the code has not settled. Magenta on screen means the swapchain acquire,
  // present and layout transitions all work and the gamma blit draws nothing;
  // still black means the frame never reaches the display at all.
  if (REXCVAR_GET(bd_debug_present_clear))
    s.command_list->clearColor(0, plume::RenderColor(1.0f, 0.0f, 1.0f, 1.0f));

  // Fits what BD actually rendered rather than what the cvar asks for, so a
  // live aspect change or a resize cannot stretch the image: neither moves the
  // latched render rect. A Sofdec movie is prerendered 16:9 and BD stretches it
  // across that rect, so fitting the present to the design ratio squeezes it
  // back out. Stretch mode asked for the distortion and keeps it.
  const double present_aspect =
      (bd::engine::SofdecMoviePlaying() && !Output::StretchToFill())
          ? kDesignCanvasAspect
          : Output::RenderAspect();
  const bool cel = REXCVAR_GET(bd_cel_shading) && s.cel_pipeline;
  // The layered path: the back buffer is the runtime's two-layer image and
  // the pass has two views, so one draw writes both eyes and neither layer is
  // flattened. Cel keeps the mono pipeline (it has no layered twin yet).
  const bool layered_present = g_xr_direct_index >= 0 &&
                               bd::xr::Session::Get().SwapchainArraySize() > 1 &&
                               !cel &&
                               s.gamma_correction_pipeline_layered != nullptr &&
                               rt->layers > 1;

  u32 fit_w = swap_w, fit_h = swap_h;
  i32 off_x = 0, off_y = 0;
  // The fit stays on the layered path too, and the reason is worth writing
  // down because removing it looked right and was not (2026-09-04). Under
  // bd_mv_half_width the latched render width is halved while RenderAspect
  // keeps the *full* width (output.cpp), so the guest draws content for 1.78
  // into a 0.89-shaped layer - anamorphically squeezed 2:1 by design, the same
  // squeeze side-by-side's half-width viewports carry. This fit is what undoes
  // it: 1.78 into 960x1080 lands on 960x540. Mapping the layer 1:1 filled the
  // eye and left every frame stretched 2x vertically.
  //
  // So the black bars are not this pass's to remove. They go when the guest
  // renders at the eye's own aspect in the first place, which is
  // Output::LatchedFit taking the runtime's per-eye size - and that has to
  // carry the 2D layout with it.
  Output::ComputeFit(swap_w, swap_h, present_aspect, fit_w, fit_h, off_x,
                     off_y);
  {
    // What the present actually samples from and into. Written down because
    // the source's size and the rect it lands in are the two numbers that say
    // whether any rendered pixel is discarded here (2026-09-04).
    static bool told = false;
    static bool told_layered = false;
    if (!told || (layered_present && !told_layered)) {
      told = true;
      told_layered = told_layered || layered_present;
      BD_INFO("[present] source {}x{} layers {} -> back {}x{}, rect {}x{}+{},{}"
              " (aspect {:.3f}); {:.2f} source pixels a destination pixel",
              rt->width, rt->height, rt->layers, swap_w, swap_h, fit_w, fit_h,
              off_x, off_y, present_aspect,
              (fit_w && fit_h)
                  ? double(rt->width) * rt->height / (double(fit_w) * fit_h)
                  : 0.0);
    }
  }
  if (fit_w != swap_w || fit_h != swap_h) {
    // Clear the whole back buffer so the uncovered edges show as black bars.
    s.command_list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
  }
  s.command_list->setViewports(plume::RenderViewport(
      static_cast<float>(off_x), static_cast<float>(off_y),
      static_cast<float>(fit_w), static_cast<float>(fit_h)));
  s.command_list->setScissors(
      plume::RenderRect(off_x, off_y, off_x + static_cast<i32>(fit_w),
                        off_y + static_cast<i32>(fit_h)));
  // pow(color, guest scanout ramp exponent * kPresentGamma), then the guest TV
  // display correction curve at kPresentDisplayCorrection strength. Pipeline
  // layout + bindless sets were bound once at BeginCommandList.
  constexpr float kPresentGamma = 1.0f;             // guest ramp unscaled
  constexpr float kPresentDisplayCorrection = 1.0f; // full X360 scanout curve
  // Cel shading replaces the gamma pass rather than following it: the cel
  // shader ends with the same gamma maths, so this is a swap and the push
  // constants below are unchanged either way.
  {
    // Once, and once more the first time it turns true: the layered path
    // needs four things to line up and this line says which one is missing.
    // bd_xr_mirror is on by default off Android and gates direct present, so
    // a desktop XR run never took this path until it was turned off.
    static u32 told = 0;
    static bool told_layered = false;
    if (told < 1 || (layered_present && !told_layered)) {
      ++told;
      told_layered = told_layered || layered_present;
      BD_INFO("[xr] present pass: layered={} (direct index {}, swapchain "
              "layers {}, source layers {}, layered pipeline {})",
              layered_present, g_xr_direct_index,
              bd::xr::Session::Get().SwapchainArraySize(), rt->layers,
              s.gamma_correction_pipeline_layered ? "yes" : "no");
    }
  }
  s.command_list->setPipeline(
      cel ? s.cel_pipeline.get()
          : (layered_present ? s.gamma_correction_pipeline_layered.get()
                             : s.gamma_correction_pipeline.get()));
  struct PresentPushConstants {
    u32 descriptor_index;
    u32 descriptor_index_2;
    float gamma;
    float display_correction;
    // 1 tells the gamma shader the source is a two-layer multiview target, so
    // it emits a side-by-side pair by reading layer 0 into the left half and
    // layer 1 into the right. That is the whole of the multiview flatten now -
    // the five full-resolution resolve passes it replaces are gone.
  } pc{gamma_src_desc,
       REXCVAR_GET(bd_mv_debug_layer_diff) ? 2u
       : layered_present                   ? 3u
       : (rt->layers > 1                   ? 1u : 0u),
       s.guest_gamma * kPresentGamma, kPresentDisplayCorrection};
  s.command_list->setGraphicsPushConstants(kCopyPushConstantRangeIndex, &pc,
                                           kCopyPushConstantByteOffset,
                                           sizeof(pc));
  s.command_list->drawInstanced(3, 1, 0, 0);

  // One-shot screenshot of the post-gamma game frame, before the overlay.
  ServiceOnPresent(s, back, back_fb, swap_w, swap_h);

  // Overlays (the F3 menu) cover the whole window, not the letterboxed rect.
  s.command_list->setViewports(plume::RenderViewport(
      0.0f, 0.0f, static_cast<float>(swap_w), static_cast<float>(swap_h)));
  s.command_list->setScissors(plume::RenderRect(0, 0, static_cast<i32>(swap_w),
                                                static_cast<i32>(swap_h)));

  // Onto the back buffer while it is still bound + COLOR_WRITE. The hook
  // marshals to the UI thread (ImGui is not thread-safe) and records into this
  // list while this guest thread is parked, so it runs under the s.mutex we
  // already hold.
  if (g_overlay_draw_hook) {
    g_overlay_draw_hook(s.command_list, back_fb, swap_w, swap_h);
  }

  s.command_list->setFramebuffer(nullptr);
  s.plume_framebuffer_bound = false;

  // bd_capture_after_s, recorded HERE and not after the present.
  //
  // The old site sourced the swapchain image late in Present and came back
  // all-zero on every path - desktop and device, both stereo modes, max 0
  // across every pixel - which made the one instrument for "look at the frame"
  // useless, and silently: a black capture reads exactly like a rendering bug.
  // Here `back` provably holds the composited frame, because the gamma blit and
  // the overlay hook both just wrote into it, and it is still in COLOR_WRITE.
  //
  // This is also the only site that can see the overlay at all. The screenshot
  // service above deliberately runs before it.
  // Stand aside when a specific guest surface was asked for. CaptureDue()
  // latches on the first caller, so without this the composited grab silently
  // consumes the one-shot and bd_mv_capture_array photographs nothing - which
  // looks exactly like the scene target being black.
  const bool wants_guest_surface = REXCVAR_GET(bd_mv_capture_array) ||
                                   REXCVAR_GET(bd_mv_capture_resolved);
  if (!wants_guest_surface && !g_captured_in_pass && CaptureDue()) {
    // Both layers of a layered swapchain image, stacked: the presented pair
    // is the only place the two eyes the compositor receives can be seen.
    g_captured_in_pass = RecordCapture(
        s, back, swap_w, swap_h, kCaptureFmt, layered_present ? 2u : 1u,
        plume::RenderTextureLayout::COLOR_WRITE);
  }

  // PRESENT for a swapchain image of our own; the runtime's image goes back
  // to COLOR_WRITE, the layout OpenXR requires a released image to be in.
  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS,
                           plume::RenderTextureBarrier(back, final_layout));
}

} // namespace

void Video::RequestClear(u32 flags, u32 color_argb, float depth, u32 stencil) {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  s.clear_pending = true;
  s.clear_flags = flags;
  s.clear_color_argb = color_argb;
  s.clear_depth = depth;
  s.clear_stencil = stencil;
  // BD's frame model is Clear -> draws -> Swap, so the first Clear is the frame
  // boundary. Opening the list twice is safe, so the paired second Clear is a
  // no-op. Without it draws record into a closed list and vanish.
  s.frame_present_committed = false;
  BeginCommandList(s);

  // X360 Clear hits the bound RT immediately, while reblue defers so the next
  // draw folds it into a load op. A color clear whose RT then receives no draw
  // would float to an unrelated RT, so between passes with a real color target
  // clear now. The deferred path still covers depth, stencil and mid-pass.
  GuestTexture *rt = s.render_target;
  // A host-owned target keeps its own clear until its pass binds
  // (HostTargetApplyClears), so no framebuffer is touched here: the colour
  // path below switched plume's framebuffer to clear the scene, which pushed
  // the shadow map's deferred clear out of plume's hold and made it a
  // zero-draw pass of its own (a framebuffer trace, 2026-09-03). The links
  // out of the target are dropped, not copied: every reader of its previous
  // content has run by the time the guest clears it again.
  if (s.command_list_open && !s.draw_framebuffer_bound) {
    if ((flags & 0x30u) != 0) {
      if (GuestTexture *ds = s.depth_stencil; ds && ds->hostOwned) {
        HostTargetDropLinks(s, ds);
        HostTargetRequestClear(ds, flags & 0x30u, color_argb, depth, stencil);
        s.clear_flags &= ~0x30u;
      }
    }
    if ((flags & 0x1u) != 0 && rt && rt->hostOwned) {
      HostTargetDropLinks(s, rt);
      HostTargetRequestClear(rt, 0x1u, color_argb, depth, stencil);
      s.clear_flags &= ~0x1u;
    }
    if (s.clear_flags == 0)
      s.clear_pending = false;
    flags = s.clear_flags;
  }
  // The depth clear is deferred to the target's next pass (plume holds it as
  // that pass's load op). A resolve still pending out of the depth surface
  // has to read the surface before the clear, not after: left to the next
  // bind, MaterializeOutbound's read barrier made plume run the held clear
  // first, as a zero-draw pass, and the shadow map then LOADed (2026-09-02).
  if (s.command_list_open && (flags & 0x30u) != 0 && !s.draw_framebuffer_bound) {
    if (GuestTexture *ds = s.depth_stencil; ds && ds->texture) {
      MaterializeOutboundLocked(s, ds);
      DetachSourceSurfaceLocked(s, ds);
    }
  }
  if (s.command_list_open && (flags & 0x1u) != 0 && rt && rt->texture &&
      !s.draw_framebuffer_bound) {
    // The clear wipes rt, so deferred resolves out of it must copy first. A
    // pending resolve into it is fully replaced, so that link just drops.
    MaterializeOutboundLocked(s, rt);
    DetachSourceSurfaceLocked(s, rt);
    const bool fresh = rt->layout == plume::RenderTextureLayout::UNKNOWN;
    if (rt->layout != plume::RenderTextureLayout::COLOR_WRITE) {
      plume::RenderTextureBarrier b(rt->texture,
                                    plume::RenderTextureLayout::COLOR_WRITE);
      s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, &b, 1);
      rt->layout = plume::RenderTextureLayout::COLOR_WRITE;
    }
    if (fresh)
      s.command_list->discardTexture(rt->texture);
    if (plume::RenderFramebuffer *fb = GetFramebuffer(s, rt, nullptr)) {
      // Plume defers this clear and, since 2026-09-02, holds it across the
      // unbind for the pass that next draws into rt (the scene, after the
      // shadow and reflection passes), where it becomes the load op. Marked
      // so nothing flips rt to a read layout in between.
      s.command_list->setFramebuffer(fb);
      s.command_list->clearColor(0, ArgbToRenderColor(color_argb));
      s.command_list->setFramebuffer(nullptr);
      s.held_clear_rt = rt;
  s.plume_framebuffer_bound = false;
      s.clear_flags &= ~0x1u;
      if ((s.clear_flags & 0x30u) == 0)
        s.clear_pending = false;
    }
  }
}

void Video::RequestResize() {
  state().resize_requested.store(true, std::memory_order_release);
}


namespace {

// Deliberately outside the REBLUE_OPENXR guard. This reaches no OpenXR header -
// it is plume and nothing else - and its use site in Present is gated at run
// time on XrCompositorPacing(), which is a compile-time false without OpenXR
// rather than an #if. Guarding the definition and not the use meant the file
// only built with OpenXR on, which the Android target always has.
//
// The game's finished frame, copied into the runtime's swapchain image and
// submitted as a world-locked quad - a screen floating in front of the player.
// Stereo projection comes later; this is the mode most likely to be comfortable
// for a fixed-camera JRPG, and it needs no per-eye rendering at all.
//
// The runtime's images are plain VkImages, and plume can wrap one, so the copy
// goes into the command list the present is already recording rather than
// needing a submission of its own.
// The frame buffer the game draws into while the headset owns the output.
//
// Presenting to the Android surface in VR costs 124ms of every 150ms frame. On
// a Quest the compositor shows OpenXR layers, not the application's own 2D
// surface, so that surface is refreshed at something like 8Hz - and a FIFO
// present against an 8Hz consumer throttles the entire engine to 8Hz. The GPU
// was measured idle at 0.1ms on the fence while this was happening, so none of
// it was graphics cost.
//
// So in VR the swapchain is not acquired, not rendered to, and not presented.
// The present pass targets this texture instead, and the only consumer is the
// copy into the runtime's swapchain image. Owning the final target is also
// what stereo will need, since that wants one per eye and a swapchain has no
// concept of them.
std::unique_ptr<plume::RenderTexture> g_offscreen;
std::unique_ptr<plume::RenderFramebuffer> g_offscreen_fb;
u32 g_offscreen_w = 0;
u32 g_offscreen_h = 0;

// The size the headset frame is composed at - see bd_xr_present_scale. Locked
// to the XR swapchain once that exists: the copy into the runtime's image
// needs both sides equal, and the swapchain is created once.
// True when the frame should be handed to the runtime as a two-layer array,
// one layer an eye: multiview only, since that is the only path that renders
// the eyes as array layers in the first place.
// The layer size the layered swapchain will take, latched from the first
// real front buffer. Zero until one exists.
u32 g_xr_layered_size_w = 0;
u32 g_xr_layered_size_h = 0;

bool XrWantsLayeredSwapchain() {
  return REXCVAR_GET(bd_xr_layered_swapchain) &&
         REXCVAR_GET(bd_stereo_multiview);
}

void XrPresentSize(VideoState &s, const GuestTexture *front, u32 &w, u32 &h) {
  auto &session = bd::xr::Session::Get();
  if (session.SwapchainWidth() && session.SwapchainHeight()) {
    w = session.SwapchainWidth();
    h = session.SwapchainHeight();
    return;
  }
  // The layered swapchain is one eye. Under bd_xr_eye_sized the layer is the
  // runtime's own per-eye rect and the guest's 16:9 content is fitted into it
  // at present, 1:1; otherwise it is whatever the frame's layer happens to be.
  if (XrWantsLayeredSwapchain() && REXCVAR_GET(bd_xr_eye_sized) &&
      session.RecommendedWidth() && session.RecommendedHeight()) {
    const double scale = std::clamp(REXCVAR_GET(bd_xr_render_scale), 0.05, 2.0);
    w = std::max<u32>(64, static_cast<u32>(session.RecommendedWidth() * scale + 0.5));
    h = std::max<u32>(64, static_cast<u32>(session.RecommendedHeight() * scale + 0.5));
    if (!g_xr_layered_size_w) {
      g_xr_layered_size_w = w;
      g_xr_layered_size_h = h;
      BD_INFO("[xr] layered swapchain at the runtime's per-eye rect: {}x{} x2",
              w, h);
    }
    return;
  }
  if (XrWantsLayeredSwapchain() && front && front->width && front->height) {
    w = front->width;
    h = front->height;
    if (!g_xr_layered_size_w) {
      g_xr_layered_size_w = w;
      g_xr_layered_size_h = h;
      BD_INFO("[xr] layered swapchain sized to the frame's layer: {}x{} x2", w,
              h);
    }
    return;
  }
  // A fraction of the window, aspect preserved: the first present comes
  // before any game frame exists, and the swapchain is created on it.
  w = s.swap_chain->getWidth();
  h = s.swap_chain->getHeight();
  const double scale = std::clamp(REXCVAR_GET(bd_xr_present_scale), 0.0, 1.0);
  if (scale > 0.0 && scale < 1.0) {
    w = std::max<u32>(64, static_cast<u32>(w * scale + 0.5));
    h = std::max<u32>(64, static_cast<u32>(h * scale + 0.5));
  }
  static bool logged = false;
  if (!logged) {
    logged = true;
    BD_INFO("[xr] headset frame composed at {}x{} (window {}x{} x {:.3f}, game "
            "frame {}x{})",
            w, h, s.swap_chain->getWidth(), s.swap_chain->getHeight(), scale,
            front ? front->width : 0u, front ? front->height : 0u);
  }
}

bool EnsureOffscreen(VideoState &s, u32 width, u32 height,
                     plume::RenderFormat format) {
  if (g_offscreen && g_offscreen_w == width && g_offscreen_h == height)
    return true;

  g_offscreen_fb.reset();
  g_offscreen.reset();
  g_offscreen = s.device->createTexture(
      plume::RenderTextureDesc::ColorTarget(width, height, format));
  if (!g_offscreen) {
    BD_ERROR("[xr] offscreen target {}x{} failed", width, height);
    return false;
  }
  const plume::RenderTexture *attachments[1] = {g_offscreen.get()};
  g_offscreen_fb =
      s.device->createFramebuffer(plume::RenderFramebufferDesc(attachments, 1));
  if (!g_offscreen_fb) {
    BD_ERROR("[xr] offscreen framebuffer failed");
    g_offscreen.reset();
    return false;
  }
  g_offscreen_w = width;
  g_offscreen_h = height;
  BD_INFO("[xr] rendering to an owned {}x{} target; the Android surface is "
          "left alone", width, height);
  return true;
}

// --- one-shot frame capture -------------------------------------------------
//
// Records a copy of the finished frame into a readback buffer, then stalls on
// the fence and writes it out. The stall is deliberate: a capture is a debug
// one-shot, and threading the readback across frames to avoid a hitch would
// buy nothing and be much easier to get subtly wrong.


// True exactly once, on the first frame at or after bd_capture_after_s.
// bd_capture_frames consecutive frames from the first one the time and
// draw-count gates admit: an artefact that lasts a few frames shows up as a
// jump between neighbours (tools/capture_seq.py).
u32 g_capture_seq = 0;

bool CaptureDue() {
  static u32 fired = 0;
  const u32 frames = std::max<u32>(1u, u32(REXCVAR_GET(bd_capture_frames)));
  if (fired >= frames)
    return false;
  if (fired > 0) {
    ++fired;
    return true;
  }
  const double after = REXCVAR_GET(bd_capture_after_s);
  if (after <= 0.0)
    return false;

  using Clock = std::chrono::steady_clock;
  static const Clock::time_point start = Clock::now();
  if (std::chrono::duration<double>(Clock::now() - start).count() < after)
    return false;

  // Wait for a frame that is actually the thing you wanted to photograph.
  //
  // bd_xr_autoplay does not land in the same place twice - menus, loading and
  // field scenes all turn up at a given elapsed time across runs - so a purely
  // time-gated capture is close to a coin toss. Six consecutive black grabs
  // were read here as "the renderer is broken" when they were menus and
  // loading screens; the frames either side were field scenes at 2187 draws.
  //
  // A field scene is >= 1500 draws on the desktop and >= 300 on a Quest; a menu
  // is 20-800. Default 0 keeps the old behaviour.
  const u32 min_draws = u32(REXCVAR_GET(bd_capture_min_draws));
  if (min_draws) {
    const u32 draws = DrawsThisFrame();
    if (draws < min_draws) {
      static u32 waited = 0;
      if ((waited++ % 600) == 0)
        BD_INFO("[capture] holding for a frame with >= {} draws (this one has "
                "{})", min_draws, draws);
      return false;
    }
  }

  fired = 1;
  return true;
}

std::unique_ptr<plume::RenderBuffer> g_capture_buffer;
u32 g_capture_w = 0;
u32 g_capture_h = 0;
u32 g_capture_row_texels = 0;
u32 g_capture_texel = 4;
plume::RenderFormat g_capture_format = plume::RenderFormat::R8G8B8A8_UNORM;

// D3D12 wants 256-byte aligned rows in a placed footprint and Vulkan does not
// care, so align always and let the writer skip the padding.
constexpr u32 kCaptureRowAlign = 64; // texels, i.e. 256 bytes at RGBA8

// Records the copy. Returns false if nothing was set up, in which case the
// resolve step must not run.
//
// `layers` > 1 captures both array slices of a multiview target, stacked
// vertically into one file. That is the only way to compare the two eyes
// honestly: bd_stereo_debug_layer is read when the surface is created, so a run
// can only ever sample one slice, and comparing two runs compares two different
// scenes - autoplay is not frame-identical across restarts. Both eyes have to
// come out of the same frame or the difference means nothing.
bool RecordCapture(VideoState &s, plume::RenderTexture *back, u32 width,
                   u32 height, plume::RenderFormat format, u32 layers = 1,
                   plume::RenderTextureLayout restore_layout =
                       plume::RenderTextureLayout::PRESENT) {
  if (width == 0 || height == 0) {
    BD_WARN("[capture] nothing recorded: target reports {}x{}", width, height);
    return false;
  }

  const u32 slices = layers > 1 ? 2u : 1u;
  const u32 row_texels =
      ((width + kCaptureRowAlign - 1) / kCaptureRowAlign) * kCaptureRowAlign;
  // NOT 4. The swapchain is RGBA8, but the guest scene target is
  // R16G16B16A16_FLOAT - eight bytes a texel - and copyTextureRegion is handed
  // the real format, so sizing this at four both overran the readback buffer
  // and stacked the second array slice half a slice too early. Every reading
  // ever taken from a multiview array capture went through that.
  const u32 texel = plume::RenderFormatSize(format);
  const u64 slice_bytes = u64(row_texels) * height * texel;
  const u64 size = slice_bytes * slices;

  if (!g_capture_buffer || g_capture_w != width ||
      g_capture_h != height * slices || g_capture_texel != texel) {
    g_capture_buffer.reset();
    g_capture_buffer =
        s.device->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(size));
    if (!g_capture_buffer) {
      BD_ERROR("[capture] readback buffer {} bytes failed", size);
      return false;
    }
    g_capture_w = width;
    g_capture_h = height * slices;
    g_capture_row_texels = row_texels;
  }
  g_capture_texel = texel;
  g_capture_format = format;

  BD_INFO("[capture] recording {}x{} layers {} fmt {} texel {} row_texels {} "
          "buffer {} bytes",
          width, height, layers, u32(format), texel, row_texels, size);

  const plume::RenderTextureBarrier to_copy(
      back, plume::RenderTextureLayout::COPY_SOURCE);
  s.command_list->barriers(plume::RenderBarrierStage::COPY, nullptr, 0,
                           &to_copy, 1);
  for (u32 slice = 0; slice < slices; ++slice) {
    s.command_list->copyTextureRegion(
        plume::RenderTextureCopyLocation::PlacedFootprint(
            g_capture_buffer.get(), format, width, height, 1, row_texels,
            u64(slice) * slice_bytes),
        plume::RenderTextureCopyLocation::Subresource(back, 0, slice));
  }

  // Back to the layout the rest of the frame expects, exactly as the XR copy
  // above does - leaving it in COPY_SOURCE shows as a black layer on Quest
  // rather than as an error.
  const plume::RenderTextureBarrier restore(back, restore_layout);
  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, nullptr, 0,
                           &restore, 1);
  return true;
}

// Maps the buffer and writes it. Must be called only after the fence for the
// frame that recorded the copy has been waited.
void ResolveCapture(bool bgra) {
  if (!g_capture_buffer)
    return;

  const void *mapped = g_capture_buffer->map();
  if (!mapped) {
    BD_ERROR("[capture] map failed");
    return;
  }

  // Which half of "the capture is black" is true: the readback never got
  // written, or it was written and decoded wrong. Every guess so far has been
  // wrong, so measure it.
  {
    const u64 bytes =
        u64(g_capture_row_texels) * g_capture_h * g_capture_texel;
    const auto *p8 = static_cast<const u8 *>(mapped);
    u64 first_nz = bytes;
    u64 nz_count = 0;
    for (u64 i = 0; i < bytes; ++i) {
      if (p8[i]) {
        if (first_nz == bytes)
          first_nz = i;
        ++nz_count;
      }
    }
    BD_INFO("[capture] readback {} bytes, {} non-zero, first at {} | "
            "{}x{} row_texels {} texel {} fmt {}",
            bytes, nz_count, first_nz == bytes ? -1LL : i64(first_nz),
            g_capture_w, g_capture_h, g_capture_row_texels, g_capture_texel,
            u32(g_capture_format));
  }

  std::error_code ec;
  const auto dir = bd::AppRootFolder() / "logs" / "capture";
  std::filesystem::create_directories(dir, ec);
  const auto path =
      dir / ("frame_" + std::to_string(std::chrono::duration_cast<
                                       std::chrono::seconds>(
                                       std::chrono::system_clock::now()
                                           .time_since_epoch())
                                           .count()) +
             "_" + std::to_string(g_capture_seq++) + ".raw");

  std::ofstream out(path, std::ios::binary);
  if (out) {
    // One text line, then tightly packed RGBA. The row padding is dropped
    // here rather than on the host so the file needs no alignment rule to
    // read - it is just width*height*4 after the newline.
    // The tag names the texel format rather than assuming RGBA8. A scene
    // target is R16G16B16A16_FLOAT, and a reader that guesses gets noise
    // that looks like a rendering bug - which is how this was found.
    const char *tag =
        g_capture_format == plume::RenderFormat::R16G16B16A16_FLOAT
            ? "RGBA16F"
            : "RGBA";
    out << tag << " " << g_capture_w << " " << g_capture_h << " "
        << (bgra ? "bgra" : "rgba") << "\n";
    const auto *src = static_cast<const u8 *>(mapped);
    for (u32 y = 0; y < g_capture_h; ++y)
      out.write(reinterpret_cast<const char *>(src + u64(y) *
                                               g_capture_row_texels *
                                               g_capture_texel),
                std::streamsize(u64(g_capture_w) * g_capture_texel));
  }
  g_capture_buffer->unmap();

  if (out)
    BD_INFO("[capture] wrote {}x{} to {} (seq {}, frame {})", g_capture_w,
            g_capture_h, path.string(), g_capture_seq - 1, FrameStatFrameCount());
  else
    BD_ERROR("[capture] could not write {}", path.string());
}

} // namespace

#if defined(REBLUE_OPENXR)
namespace {

// Frame accounting. A dropped quad layer and a slow frame look identical from
// inside the headset - both read as "flickery and choppy" - so they are counted
// separately here rather than guessed at.
struct XrStats {
  u32 begun = 0;
  u32 submitted = 0;   // frames that actually carried a quad layer
  u32 noRender = 0;    // runtime said shouldRender = false
  u32 noImage = 0;     // swapchain image unavailable
  double waitMs = 0.0; // time inside xrWaitFrame, which blocks by design
  double waitMax = 0.0;
  double frameMs = 0.0; // wall clock between consecutive EndFrames
  // Where Present actually blocks. A steady frame time with everything here
  // near zero means the cost is upstream of Present entirely - guest
  // simulation or command recording - which is a completely different fix
  // from a slow GPU.
  double acquireMs = 0.0;
  double submitMs = 0.0;
  double presentMs = 0.0;
  double fenceMs = 0.0;
  double drainMs = 0.0;
  u32 samples = 0;
  std::chrono::steady_clock::time_point lastEnd{};
  std::chrono::steady_clock::time_point lastReport{};
};

XrStats g_xr_stats;

// Called at the end of Present with that frame's breakdown.
void NoteXrBreakdown(const PresentBreakdown &b) {
  g_xr_stats.acquireMs += b.acquire_ms;
  g_xr_stats.submitMs += b.submit_ms;
  g_xr_stats.presentMs += b.present_ms;
  g_xr_stats.fenceMs += b.fence_ms;
  g_xr_stats.drainMs += b.drain_ms;
  ++g_xr_stats.samples;
}

// The frame this present is inside, kept between BeginFrame and EndFrame.
bd::xr::FrameState g_xr_frame;
bool g_xr_frame_open = false;

void BeginXrFrame() {
  auto &session = bd::xr::Session::Get();
  if (!session.SessionCreated())
    return;
  // Events have to be pumped every frame even before the session is running,
  // or it never transitions to READY and no frame ever arrives.
  session.PollEvents();
  const auto waitStart = std::chrono::steady_clock::now();
  g_xr_frame_open = session.BeginFrame(g_xr_frame);
  const double waited =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - waitStart)
          .count();
  ++g_xr_stats.begun;
  g_xr_stats.waitMs += waited;
  g_xr_stats.waitMax = std::max(g_xr_stats.waitMax, waited);
  if (g_xr_frame_open && !g_xr_frame.shouldRender)
    ++g_xr_stats.noRender;
  if (!g_xr_frame_open || !g_xr_frame.shouldRender || g_xr_frame.viewCount == 0) {
    bd::xr::ClearEye();
    return;
  }

  // Latch the eye the guest will render from. This is one frame of latency by
  // construction: BeginXrFrame runs inside Present, which is the end of the
  // guest's frame, so the pose taken here drives the frame after this one. The
  // fix is to move the xrWaitFrame/xrBeginFrame pair to the top of the guest
  // frame instead, which is a frame-pacing change and wants doing on its own.
  //
  // FrameState carries poses already converted to game space, and ComposeEye's
  // contract is that it is handed OpenXR ones. The mirror is its own inverse,
  // so this converts back - it is not a no-op that happens to compile.
  const bd::xr::EyeView &eye = g_xr_frame.views[0];
  bd::xr::SubmitEye(bd::xr::FromOpenXRPose(eye.pose), eye.fov);
}

void EndXrFrame() {
  if (!g_xr_frame_open)
    return;
  bd::xr::Session::Get().EndFrame(g_xr_frame);
  g_xr_frame_open = false;

  using Clock = std::chrono::steady_clock;
  const auto now = Clock::now();
  if (g_xr_stats.lastEnd.time_since_epoch().count() != 0) {
    g_xr_stats.frameMs +=
        std::chrono::duration<double, std::milli>(now - g_xr_stats.lastEnd)
            .count();
  }
  g_xr_stats.lastEnd = now;

  if (g_xr_stats.lastReport.time_since_epoch().count() == 0)
    g_xr_stats.lastReport = now;
  const double sinceReport =
      std::chrono::duration<double>(now - g_xr_stats.lastReport).count();
  if (sinceReport < 5.0)
    return;

  const u32 begun = std::max(g_xr_stats.begun, 1u);
  const u32 samples = std::max(g_xr_stats.samples, 1u);
  const double frameMs = g_xr_stats.frameMs / begun;
  const double accounted =
      (g_xr_stats.acquireMs + g_xr_stats.submitMs + g_xr_stats.presentMs +
       g_xr_stats.fenceMs + g_xr_stats.drainMs) /
      samples;
  BD_INFO("[xr] {:.1f} fps | {} begun, {} layered, {} no-render, {} no-image "
          "| frame {:.1f}ms = xrWait {:.1f} + acquire {:.1f} + submit {:.1f} "
          "+ present {:.1f} + fence {:.1f} + drain {:.1f} + elsewhere {:.1f}",
          g_xr_stats.submitted / sinceReport, g_xr_stats.begun,
          g_xr_stats.submitted, g_xr_stats.noRender, g_xr_stats.noImage,
          frameMs, g_xr_stats.waitMs / begun,
          g_xr_stats.acquireMs / samples, g_xr_stats.submitMs / samples,
          g_xr_stats.presentMs / samples, g_xr_stats.fenceMs / samples,
          g_xr_stats.drainMs / samples,
          frameMs - accounted - g_xr_stats.waitMs / begun);

  g_xr_stats = XrStats{};
  g_xr_stats.lastEnd = now;
  g_xr_stats.lastReport = now;
}

// The runtime's swapchain images, wrapped for plume: as copy destinations
// (the offscreen path) and as render targets (the direct path), with a
// framebuffer each.
std::vector<std::unique_ptr<plume::VulkanTexture>> g_xr_textures;
std::vector<std::unique_ptr<plume::RenderFramebuffer>> g_xr_fbs;
bool g_xr_targets_ready = false;
// The image acquired for direct present this frame, -1 when the offscreen
// path is in use. Set before RecordPresentPass, consumed by RecordXrQuad.
// Forward-declared above, where the present pass reads it.
i32 g_xr_direct_index = -1;

bool EnsureXrTargets(VideoState &s, u32 sw, u32 sh) {
  if (g_xr_targets_ready)
    return true;
  auto &session = bd::xr::Session::Get();
  if (!sw || !sh)
    XrPresentSize(s, nullptr, sw, sh);
  const u32 layers = XrWantsLayeredSwapchain() ? 2u : 1u;
  // The swapchain's size is locked for the session (the runtime hands out its
  // images once), and the first present happens before any game frame exists -
  // which is how the panel-sized swapchain of 2026-09-02 got locked in. Under
  // the layered path the size has to be the frame's own layer, so nothing is
  // created until a frame has been seen (XrLayeredFrameSize).
  if (layers > 1 && !g_xr_layered_size_w) {
    static u32 waited = 0;
    if (waited++ == 0)
      BD_INFO("[xr] layered swapchain waiting for the first game frame");
    return false;
  }
  if (layers > 1) {
    sw = g_xr_layered_size_w;
    sh = g_xr_layered_size_h;
  }
  if (!session.CreateSwapchain(sw, sh, layers))
    return false;
  auto *vk_device = static_cast<plume::VulkanDevice *>(s.device.get());
  for (u32 i = 0; i < session.SwapchainImageCount(); ++i) {
    auto image = reinterpret_cast<VkImage>(session.SwapchainImage(i));
    auto tex = std::make_unique<plume::VulkanTexture>(vk_device, image);
    // The VkImage constructor sets only the handle and the device - desc,
    // imageFormat and the subresource range are all left zeroed, because
    // plume normally fills them in from the swapchain that owns the image.
    // Without them copyTexture computes a 0x0 region and copies nothing,
    // which is a black layer rather than any kind of error.
    tex->desc.width = session.SwapchainWidth();
    tex->desc.height = session.SwapchainHeight();
    tex->desc.depth = 1;
    tex->desc.mipLevels = 1;
    // arraySize > 1 makes plume's own view a 2D array, which is what the
    // layered pass writes through and what the runtime reads per eye.
    tex->desc.arraySize = session.SwapchainArraySize();
    tex->desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
    tex->desc.format = kPresentBackFormat;
    tex->desc.flags = plume::RenderTextureFlag::RENDER_TARGET;
    tex->imageFormat = static_cast<VkFormat>(session.SwapchainFormat());
    tex->imageSubresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
                                  session.SwapchainArraySize()};
    // A view, so the image can be a framebuffer attachment.
    tex->createImageView(tex->imageFormat);
    const plume::RenderTexture *attachments[1] = {tex.get()};
    plume::RenderFramebufferDesc fb_desc(attachments, 1);
    // Must match the pipeline's mask, or the render pass is rejected.
    fb_desc.viewMask = session.SwapchainArraySize() > 1 ? 0x3u : 0u;
    auto fb = s.device->createFramebuffer(fb_desc);
    if (!fb) {
      BD_ERROR("[xr] framebuffer over swapchain image {} failed", i);
      return false;
    }
    g_xr_textures.push_back(std::move(tex));
    g_xr_fbs.push_back(std::move(fb));
  }
  g_xr_targets_ready = true;
  BD_INFO("[xr] swapchain ready, {} images wrapped as render targets, {} "
          "layer(s) each",
          g_xr_textures.size(), session.SwapchainArraySize());
  return true;
}

// Acquire the runtime's image as this frame's present target. False leaves
// the offscreen path to run; true has the image acquired, and RecordXrQuad
// releases it.
bool AcquireXrTargetDirect(VideoState &s, u32 w, u32 h,
                           plume::RenderTexture **back,
                           plume::RenderFramebuffer **back_fb) {
  auto &session = bd::xr::Session::Get();
  if (!session.Running())
    return false;
  if (!EnsureXrTargets(s, w, h))
    return false;
  if (session.SwapchainWidth() != w || session.SwapchainHeight() != h) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      BD_INFO("[xr] direct present off: frame {}x{}, swapchain {}x{}", w, h,
              session.SwapchainWidth(), session.SwapchainHeight());
    }
    return false;
  }
  const i32 index = session.AcquireSwapchainImage();
  if (index < 0 || static_cast<size_t>(index) >= g_xr_textures.size()) {
    ++g_xr_stats.noImage;
    return false;
  }
  g_xr_direct_index = index;
  *back = g_xr_textures[index].get();
  *back_fb = g_xr_fbs[index].get();
  static bool told = false;
  if (!told) {
    told = true;
    BD_INFO("[xr] direct present: the gamma pass renders into the runtime's "
            "{}x{} swapchain image", w, h);
  }
  return true;
}

void RecordXrQuad(VideoState &s, plume::RenderTexture *back) {
  auto &session = bd::xr::Session::Get();
  const i32 direct = g_xr_direct_index;
  g_xr_direct_index = -1;
  if (!session.Running() || !g_xr_frame_open || !g_xr_frame.shouldRender) {
    // An image acquired for a frame that is not rendered still has to be
    // handed back, or the next acquire fails.
    if (direct >= 0)
      session.ReleaseSwapchainImage();
    return;
  }

  if (direct < 0) {
    if (!EnsureXrTargets(s, g_offscreen_w, g_offscreen_h))
      return;
    const i32 index = session.AcquireSwapchainImage();
    if (index < 0 || static_cast<size_t>(index) >= g_xr_textures.size()) {
      ++g_xr_stats.noImage;
      return;
    }

    plume::RenderTexture *dst = g_xr_textures[index].get();
    {
      const auto *src = static_cast<const plume::VulkanTexture *>(back);
      if (src->desc.width != session.SwapchainWidth() ||
          src->desc.height != session.SwapchainHeight()) {
        static bool logged = false;
        if (!logged) {
          logged = true;
          BD_INFO("[xr] frame {}x{} skipped: swapchain is {}x{}",
                  src->desc.width, src->desc.height, session.SwapchainWidth(),
                  session.SwapchainHeight());
        }
        session.ReleaseSwapchainImage();
        ++g_xr_stats.noImage;
        return;
      }
    }
    const plume::RenderTextureBarrier to_copy[] = {
        plume::RenderTextureBarrier(dst, plume::RenderTextureLayout::COPY_DEST),
        plume::RenderTextureBarrier(back,
                                    plume::RenderTextureLayout::COPY_SOURCE),
    };
    s.command_list->barriers(plume::RenderBarrierStage::COPY, nullptr, 0,
                             to_copy, 2);
    s.command_list->copyTexture(dst, back);

    // Both images have to be handed back in the layout their owner expects.
    // OpenXR requires a released swapchain image to be in the layout implied
    // by its usage flags - COLOR_ATTACHMENT_OPTIMAL for a colour attachment -
    // and leaving it in COPY_DEST is undefined, which on Quest shows as a
    // black layer rather than an error. Ours goes back to whatever its owner
    // expects next: PRESENT for a real swapchain image, and COLOR_WRITE for
    // the offscreen target, which is never presented and would be left in an
    // undefined layout for next frame's render pass otherwise.
    const plume::RenderTextureBarrier restore[] = {
        plume::RenderTextureBarrier(dst,
                                    plume::RenderTextureLayout::COLOR_WRITE),
        plume::RenderTextureBarrier(back,
                                    XrCompositorPacing()
                                        ? plume::RenderTextureLayout::COLOR_WRITE
                                        : plume::RenderTextureLayout::PRESENT),
    };
    s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, nullptr, 0,
                             restore, 2);
  }
  // The direct path rendered into the image and left it in COLOR_WRITE (the
  // present pass's final layout for it), which is what the release needs.

  session.ReleaseSwapchainImage();

  // Cinema is a screen hanging in the world. Every other mode is the world
  // itself, which is a projection layer: the image becomes a window the
  // compositor reprojects as the head moves, rather than a rectangle pinned in
  // space. That difference is what "being in the world" actually is.
  if (bd::xr::Settings::Get().Mode() == bd::xr::CameraMode::Cinema) {
    // 2 m wide at 2 m distance is roughly a 60-inch screen - big enough to
    // read the HUD, close enough not to need head turning. Submit before
    // anchoring: the anchor is placed at quadDistance_ along the view, and
    // this is what sets it.
    session.SubmitQuadLayer(2.0f, 2.0f * float(session.SwapchainHeight()) /
                                      float(session.SwapchainWidth()), 2.0f);
    session.AnchorQuad(g_xr_frame);
  } else {
    session.SubmitProjectionLayer();
  }
  ++g_xr_stats.submitted;
}

} // namespace
#endif

void Video::Present(GuestTexture *frontBuffer) {
  auto &s = state();
  // Before the lock: shutdown runs on the UI thread, so a Present that reached
  // the overlay hook would marshal into a thread no longer pumping, holding
  // s.mutex while it waits.
  if (s.shutting_down.load(std::memory_order_acquire))
    return;
  std::unique_lock lock(s.mutex);
  if (!s.ready || s.shutting_down.load(std::memory_order_acquire)) {
    return;
  }
  // One back buffer present per engine frame, and RequestClear reopens the
  // gate.
  if (s.frame_present_committed) {
    return;
  }

  const bool resize_requested =
      s.resize_requested.exchange(false, std::memory_order_acq_rel);
  if (s.swap_chain->needsResize() || resize_requested) {
    RebuildSwapChain(s);
  }

  // Empty after a failed or skipped resize (minimized), and indexing it is a
  // UAF.
  if (s.framebuffers.empty()) {
    AbandonFrame(s, lock);
    return;
  }

  using Clock = std::chrono::steady_clock;
  const auto ms_since = [](Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  };
  PresentBreakdown pb;

  // In VR nothing acquires, renders to, or presents the Android surface. See
  // EnsureOffscreen for why - it is worth 124ms a frame.
  const bool headset_owns_output = XrCompositorPacing();

  u32 texture_index = 0;
  bool mirroring = false;
  plume::RenderTexture *back = nullptr;
  plume::RenderFramebuffer *back_fb = nullptr;

  if (headset_owns_output) {
    // Matches the swapchain format picked in device.cpp. plume's RenderTexture
    // does not expose its desc, and the copy into the runtime's image needs
    // both sides to agree, so the choice is repeated rather than queried.
    u32 present_w = 0;
    u32 present_h = 0;
    XrPresentSize(s, frontBuffer, present_w, present_h);
    // Straight into the runtime's swapchain image when it can be: the
    // offscreen frame and the copy into the image were a full-frame blit and
    // a preemption slot every frame (render-stage trace, 2026-09-02). The
    // mirror keeps the offscreen path, whose image the window can copy.
    if (!(REXCVAR_GET(bd_xr_direct_present) && !REXCVAR_GET(bd_xr_mirror) &&
          AcquireXrTargetDirect(s, present_w, present_h, &back, &back_fb))) {
      if (!EnsureOffscreen(s, present_w, present_h, kPresentBackFormat)) {
        AbandonFrame(s, lock);
        return;
      }
      back = g_offscreen.get();
      back_fb = g_offscreen_fb.get();
      // Acquire the flat swapchain too when mirroring, so the offscreen
      // image can be copied into it below. Failing to acquire is not fatal -
      // the headset still gets its frame, the window just stays stale.
      if (REXCVAR_GET(bd_xr_mirror)) {
        mirroring = s.swap_chain->acquireTexture(
            s.acquire_semaphores[s.frame.load(std::memory_order_relaxed)].get(),
            &texture_index);
      }
    }
  } else {
    BD_CPU_ZONE("AcquireTexture");
    const auto t0 = Clock::now();
    if (!s.swap_chain->acquireTexture(
            s.acquire_semaphores[s.frame.load(std::memory_order_relaxed)].get(),
            &texture_index)) {
      return;
    }
    pb.acquire_ms = ms_since(t0);
    back = s.swap_chain->getTexture(texture_index);
    back_fb = s.framebuffers[texture_index].get();
  }

  GuestTexture *chosen = nullptr;
  GuestTexture *rt = SelectPresentSource(s, frontBuffer, chosen);

  // An MSAA rt must never reach the gamma blit: its descriptor is a Texture2DMS
  // view, the blit shader samples a Texture2D. A correct frame ends on a
  // resolved single-sample surface, so an MSAA one here is already wrong
  // upstream, so skip the blit rather than crash the present.
  const bool have_rt_blit =
      rt && rt->texture && rt->descriptorIndex != kInvalidDescriptorIndex &&
      rt->sampleCount == plume::RenderSampleCount::COUNT_1;
  if (!have_rt_blit) {
    static std::atomic<u32> s_log{0};
    const u32 n = s_log.fetch_add(1, std::memory_order_relaxed);
    if (n < 5) {
      BD_ERROR("Present #{} skipped: no drawable RT", n);
    }
    AbandonFrame(s, lock);
    return;
  }

  // By here the queue must already be empty: every render pass ends at a
  // framebuffer change or a barrier, and both flush. Emitting here instead
  // would be wrong rather than late - there is no bound framebuffer to emit
  // against, and no pipeline layout, which is an access violation and was one.
  bd::gpu::DrawQueueDiscardStragglers();

  // Reopens a closed list, so end() below always has one.
  BeginCommandList(s);
  RecordPresentPass(s, rt, chosen, back, back_fb,
                    g_xr_direct_index >= 0
                        ? plume::RenderTextureLayout::COLOR_WRITE
                        : plume::RenderTextureLayout::PRESENT);
#if defined(REBLUE_OPENXR)
  BeginXrFrame();
  RecordXrQuad(s, back);
#endif

  // Mirror into the flat swapchain. After the XR copy, so the window shows
  // exactly what the headset was handed.
  // The copy needs equal sizes; with bd_xr_present_scale the offscreen frame is
  // smaller than the window, and the mirror is skipped rather than clipped.
  if (mirroring && (g_offscreen_w != s.swap_chain->getWidth() ||
                    g_offscreen_h != s.swap_chain->getHeight())) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      BD_INFO("[xr] mirror skipped: headset frame {}x{} is not the window's "
              "{}x{}",
              g_offscreen_w, g_offscreen_h, s.swap_chain->getWidth(),
              s.swap_chain->getHeight());
    }
    mirroring = false;
  }
  if (mirroring) {
    plume::RenderTexture *front = s.swap_chain->getTexture(texture_index);
    if (front) {
      const plume::RenderTextureBarrier to_copy[] = {
          plume::RenderTextureBarrier(front,
                                      plume::RenderTextureLayout::COPY_DEST),
          plume::RenderTextureBarrier(back,
                                      plume::RenderTextureLayout::COPY_SOURCE),
      };
      s.command_list->barriers(plume::RenderBarrierStage::COPY, nullptr, 0,
                               to_copy, 2);
      s.command_list->copyTexture(front, back);
      const plume::RenderTextureBarrier restore[] = {
          plume::RenderTextureBarrier(front,
                                      plume::RenderTextureLayout::PRESENT),
          plume::RenderTextureBarrier(back,
                                      plume::RenderTextureLayout::COLOR_WRITE),
      };
      s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, nullptr, 0,
                               restore, 2);
    } else {
      mirroring = false;
    }
  }

  // Recorded after the XR copy, so what lands in the file is exactly the image
  // the compositor was handed, not an earlier stage of it.
  // A two-layer scene target carries both eyes, so capture that rather than the
  // composited back buffer, which is one eye by the time the mono post chain
  // has finished with it. Anything else captures `back` as before.
  // Prefer the resolved companion: under multiview that is the image the post
  // chain and the compositor actually see, and the array behind it is an
  // intermediate. Capturing the array instead photographs the wrong texture and
  // reads as a black frame even when the output is fine.
  // bd_mv_capture_array photographs the layered array itself, both slices,
  // instead of the companion the resolve writes. The whole multiview chain has
  // now been verified correct end to end and the frame is still black, so the
  // remaining question is whether the array has anything in it - and that is
  // answered by looking at it rather than by inferring it from what leaves the
  // resolve.
  const bool force_array = REXCVAR_GET(bd_mv_capture_array);
  // Photograph the *scene's* layered target, not whatever bound last. They are
  // different surfaces - the post chain binds after the scene - and capturing
  // the latter is what made the array look empty for a whole investigation.
  if (force_array) {
    if (GuestTexture *scene = s.last_scene_rt[s.recording_slot()])
      rt = scene;
  }
  // The scene's *companion* - what the resolve writes and the post chain reads.
  // With the array capture above this splits the remaining question in one run:
  // the array is known good (two distinct views), so if the companion is good
  // too the loss is in the post chain or present, and if it is black the resolve
  // is at fault.
  const bool capture_resolved = REXCVAR_GET(bd_mv_capture_resolved);
  if (capture_resolved) {
    if (GuestTexture *scene = s.last_scene_rt[s.recording_slot()])
      rt = scene;
  }
  // Say what is about to be photographed. A capture that silently grabbed the
  // wrong surface is indistinguishable from a rendering bug, and on device that
  // cost a whole investigation.
  // NOT CaptureDue() here - it latches, returns true exactly once, and calling
  // it from a diagnostic would consume the latch so the real capture never
  // fires. Its own static is the right gate.
  static bool told_capture_pick = false;
  if (force_array && !told_capture_pick) {
    told_capture_pick = true;
    BD_INFO("[mv] capture_array picking {}x{} layers={} (scene rt {})",
            rt ? rt->width : 0u, rt ? rt->height : 0u, rt ? rt->layers : 0u,
            static_cast<const void *>(rt));
  }
  if (force_array && rt && rt->layers > 1) {
    static std::atomic<u32> cn{0};
    if (cn.fetch_add(1, std::memory_order_relaxed) % 600 == 0)
      BD_INFO("[mv] capture   from guest {:012X} plume tex {:012X}",
              u64(uintptr_t(rt)), u64(uintptr_t(rt->texture)));
  }
  const bool resolved_rt =
      rt && rt->resolvedTexture && rt->layers > 1 && !force_array;
  const bool multiview_rt = rt && rt->texture && rt->layers > 1 && !resolved_rt;
  // CaptureDue() latches, so it is asked only when this site can record;
  // asked first, a mono frame consumed the one shot with nothing written and
  // the composite site never fired (2026-09-02).
  const bool can_capture_here =
      resolved_rt || multiview_rt || (force_array && rt && rt->texture);
  const bool capturing =
      // A multi-frame sequence can admit both capture sites in one frame.
      // Once the present pass recorded a copy, keep that readback alive and
      // do not replace it with a guest surface before the GPU executes it.
      !g_captured_in_pass && can_capture_here && CaptureDue() &&
      (resolved_rt
           ? RecordCapture(s, rt->resolvedTexture, rt->width, rt->height,
                           rt->format, 1,
                           plume::RenderTextureLayout::SHADER_READ)
       : multiview_rt
           ? RecordCapture(s, rt->texture, rt->width, rt->height, rt->format,
                           rt->layers, plume::RenderTextureLayout::SHADER_READ)
       // A single-layer scene target, which is what bd_mv_capture_array finds
       // whenever multiview is off - on the flat renderer, and on any device
       // without a headset. Without this the array branch cannot fire, the
       // in-pass capture stands aside because a guest surface was asked for,
       // and the run produces no capture at all while looking healthy.
       //
       // This is the only capture that works on the flat path: it photographs a
       // guest texture, which always supports being copied from, where the
       // swapchain may not advertise TRANSFER_SRC at all.
       : (force_array && rt && rt->texture)
           ? RecordCapture(s, rt->texture, rt->width, rt->height, rt->format, 1,
                           plume::RenderTextureLayout::SHADER_READ)
           // The flat composited frame is captured inside RecordPresentPass
           // instead, which is the only site that can see the ImGui overlay.
           : false);

  const u32 cur = s.frame.load(std::memory_order_relaxed);
  FrameEnd(s.command_list);
  s.command_lists[cur]->end();
  s.command_list_open = false;

  const plume::RenderCommandList *lists[] = {s.command_lists[cur].get()};
  plume::RenderCommandSemaphore *waits[] = {s.acquire_semaphores[cur].get()};
  // Indexed by the acquired swapchain image, not the frame-in-flight slot:
  // see the render_semaphores declaration on VideoState.
  plume::RenderCommandSemaphore *signals[] = {
      s.render_semaphores[texture_index].get()};
  // Nothing was acquired in VR, so the acquire semaphore will never signal and
  // waiting on it would deadlock on the first frame. Nothing is presented
  // either, so there is no one to signal.
  // Mirroring acquires and presents, so it needs the same semaphore pairing a
  // flat frame does - without it the present races the copy.
  const bool uses_swapchain = !headset_owns_output || mirroring;
  const u32 wait_count = uses_swapchain ? 1u : 0u;
  const u32 signal_count = uses_swapchain ? 1u : 0u;
  // NOT the frame just submitted: AdvanceAndWaitReused waits the slot about to
  // be reused, one frame old. That gap is the CPU/GPU overlap.
  {
    BD_CPU_ZONE("Submit");
    const auto t0 = Clock::now();
    s.queue->executeCommandLists(lists, 1, waits, wait_count, signals,
                                 signal_count, s.fences[cur].get());
    pb.submit_ms = ms_since(t0);
  }

  if (capturing || g_captured_in_pass) {
    // The one place this stalls. CaptureDue() has already latched, so a
    // failed capture cannot retry every frame for the rest of the session.
    s.queue->waitForCommandFence(s.fences[cur].get());
    ResolveCapture(kCaptureIsBgra);
    g_captured_in_pass = false;
    // That wait consumed the fence (plume resets it), so the slot must not be
    // waited again when the ring comes back round to it: marked submitted, it
    // hung the render thread in AdvanceAndWaitReused on every capture run of
    // 2026-09-02 (the log stops at "[capture] wrote", the profile never dumps).
    // Its GPU work is complete, which is what "not submitted" means here.
    s.command_list_submitted[cur] = false;
    CollectGPUTimings(cur);
    FragCensusCollect(cur);
    OcclusionCullCollect(cur);
  } else {
    s.command_list_submitted[cur] = true;
  }
  ApplyVsync(s);
  if (uses_swapchain) {
    BD_CPU_ZONE("PresentSwap");
    const auto t0 = Clock::now();
    // A removed device fails Present first, and the fence wait below still
    // returns (removal signals every fence), so without this the next D3D12
    // call is the one that reports the loss.
    if (!s.swap_chain->present(texture_index, signals, 1)) {
      CheckDeviceRemoved("swapchain present");
    }
    pb.present_ms = ms_since(t0);
  }
#if defined(REBLUE_OPENXR)
  // After the flat present: the copy into the runtime's image was recorded in
  // the same command list, so by here it has been submitted.
  EndXrFrame();
#endif
  // After the present, so the capture covers the whole of the next frame
  // rather than the tail of this one. RenderDoc delimits frames by present.
  bd::gpu::renderdoc::TriggerIfDue();
  const auto wait_t0 = Clock::now();
  {
    BD_CPU_ZONE("WaitFence");
    AdvanceAndWaitReused(s);
  }
  pb.fence_ms = ms_since(wait_t0);
  RecordGPUWait(pb.fence_ms);
  // Also ticked here, not only from the XR frame loop: a flat build has no
  // xrBeginFrame, and the guest simulation this samples is identical either
  // way - which is what makes the desktop the fast loop for guest CPU work.
  bd::SamplingProfilerTick();
  bd::gpu::NoteABArm(bd::ABExperimentTick());
  const u32 reclaimed = s.frame.load(std::memory_order_relaxed);
  s.frame_present_committed = true;
  BD_FRAME_MARK();
  UpdateFrameStats();
  // The reused slot's fence has signalled, so resources released kNumFrames ago
  // can no longer be referenced by in-flight work. Drop the lock first, since
  // DrainSlot re-acquires it per entry.
  lock.unlock();
  {
    BD_CPU_ZONE("DrainSlot");
    const auto t0 = Clock::now();
    DrainSlot(s, reclaimed);
    pb.drain_ms = ms_since(t0);
  }
  {
    BD_CPU_ZONE("PaceFrame");
    const auto t0 = Clock::now();
    PaceFrame();
    pb.pace_ms = ms_since(t0);
  }
#if defined(REBLUE_OPENXR)
  NoteXrBreakdown(pb);
#endif
  RecordFrameSample(pb);
}

void Video::PresentOverlayFrame() {
  auto &s = state();
  if (s.shutting_down.load(std::memory_order_acquire))
    return;
  std::unique_lock lock(s.mutex);
  if (!s.swap_chain || s.ready)
    return;

  const bool resize_requested =
      s.resize_requested.exchange(false, std::memory_order_acq_rel);
  if (s.swap_chain->needsResize() || resize_requested) {
    for (u32 i = 0; i < kNumFrames; ++i) {
      if (s.command_list_submitted[i]) {
        s.queue->waitForCommandFence(s.fences[i].get());
        s.command_list_submitted[i] = false;
      }
    }
    s.framebuffers.clear();
    if (!s.swap_chain->resize() || !BuildFramebuffers(s) ||
        !BuildPresentSemaphores(s)) {
      if (s.swap_chain->getWidth() && s.swap_chain->getHeight())
        BD_ERROR("Swap chain resize failed");
    }
  }
  if (s.framebuffers.empty())
    return;

  const u32 cur = s.frame.load(std::memory_order_relaxed);
  u32 texture_index = 0;
  if (!s.swap_chain->acquireTexture(s.acquire_semaphores[cur].get(),
                                    &texture_index)) {
    return;
  }
  plume::RenderTexture *back = s.swap_chain->getTexture(texture_index);
  plume::RenderFramebuffer *back_fb = s.framebuffers[texture_index].get();
  const u32 swap_w = s.swap_chain->getWidth();
  const u32 swap_h = s.swap_chain->getHeight();

  BeginCommandList(s);
  if (!s.command_list_open)
    return;

  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS,
                           plume::RenderTextureBarrier(
                               back, plume::RenderTextureLayout::COLOR_WRITE));
  s.command_list->setFramebuffer(back_fb);
  s.command_list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
  s.command_list->setViewports(plume::RenderViewport(
      0.0f, 0.0f, static_cast<float>(swap_w), static_cast<float>(swap_h)));
  s.command_list->setScissors(plume::RenderRect(0, 0, static_cast<i32>(swap_w),
                                                static_cast<i32>(swap_h)));
  if (g_overlay_draw_hook) {
    g_overlay_draw_hook(s.command_list, back_fb, swap_w, swap_h);
  }
  s.command_list->setFramebuffer(nullptr);
  s.plume_framebuffer_bound = false;
  s.command_list->barriers(
      plume::RenderBarrierStage::GRAPHICS,
      plume::RenderTextureBarrier(back, plume::RenderTextureLayout::PRESENT));

  FrameEnd(s.command_list);
  s.command_lists[cur]->end();
  s.command_list_open = false;
  const plume::RenderCommandList *lists[] = {s.command_lists[cur].get()};
  plume::RenderCommandSemaphore *waits[] = {s.acquire_semaphores[cur].get()};
  // Indexed by the acquired swapchain image, not the frame-in-flight slot:
  // see the render_semaphores declaration on VideoState.
  plume::RenderCommandSemaphore *signals[] = {
      s.render_semaphores[texture_index].get()};
  s.queue->executeCommandLists(lists, 1, waits, 1, signals, 1,
                               s.fences[cur].get());
  s.command_list_submitted[cur] = true;
  ApplyVsync(s);
  if (!s.swap_chain->present(texture_index, signals, 1)) {
    CheckDeviceRemoved("swapchain present (overlay)");
  }
  AdvanceAndWaitReused(s);
  const u32 reclaimed = s.frame.load(std::memory_order_relaxed);
  lock.unlock();
  DrainSlot(s, reclaimed);
  RecordBlankFrameSample();
  // No PaceFrame: the installer tick scheduler paces, and sleeping here would
  // only delay SDL event handling on the UI thread.
}

} // namespace bd::gpu
