/**
 * @file    gpu/resolve.cpp
 * @brief   EDRAM resolve emulation: surface-to-texture copy, the lazy resolve
 *          alias, and the fullscreen composite chain seeding.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/frame.h"
#include "gpu/draw_queue.h"

#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <vector>

#include <plume_render_interface.h>
#include <rex/cvar.h>

#include "core/profiling.h"

#include "core/logging.h"
#include "gpu/backend.h"
#include "gpu/format.h"
#include "gpu/frame_stats.h"
#include "gpu/gpu_timing.h"
#include "gpu/post_chain.h"
#include "gpu/settings.h"

namespace bd::gpu {

namespace {

// Sentinel fb cache key so a depth entry never collides with the
// color-only entry under key=nullptr.
inline const plume::RenderTexture *const kDepthFbKey =
    reinterpret_cast<const plume::RenderTexture *>(uintptr_t{1});

// X360 Resolve reads the EDRAM region of the surface bound at call time, so a
// surface drawn into is that region's content. A bound surface with no draws
// since the pool issued it still holds its predecessor's content, tracked by
// last_drawn_*.
GuestTexture *ResolveSourceForFlagsLocked(VideoState &s, u32 flags,
                                          bool &used_fallback) {
  const u32 slot = s.recording_slot();
  used_fallback = true;
  if (flags & 0x4u) {
    if (s.depth_stencil && s.depth_stencil->texture &&
        s.depth_stencil->surfaceDrawn) {
      used_fallback = false;
      return s.depth_stencil;
    }
    // A host-owned target (gpu/host_targets.h) holds the previous frame's
    // image until its pass draws, which is what the guest's frame-start
    // depth grab is after: a link, not a guessed source and a copy. The
    // shadow map's square-and-undrawn clear-to-far is decided by the caller
    // before this matters.
    if (s.depth_stencil && s.depth_stencil->texture &&
        s.depth_stencil->hostOwned) {
      used_fallback = false;
      return s.depth_stencil;
    }
    if (s.last_drawn_ds[slot] && s.last_drawn_ds[slot]->texture) {
      u32 n;
      // Expected per-frame behavior (frame start history grab), so verbosity 1
      // stays quiet.
      if (Settings::Get().DiagVerbosity() >= 2 &&
          DiagShouldLog(2, s.depth_stencil, &n)) {
        BD_WARN("[resolve-diag] #{} bound DS {}x{} fmt={} never drawn -> "
                "falling back to last_drawn {}x{}",
                n, s.depth_stencil ? s.depth_stencil->width : 0,
                s.depth_stencil ? s.depth_stencil->height : 0,
                s.depth_stencil ? u32(s.depth_stencil->format) : 0,
                s.last_drawn_ds[slot]->width, s.last_drawn_ds[slot]->height);
      }
      return s.last_drawn_ds[slot];
    }
    return s.depth_stencil;
  }
  if (s.render_target && s.render_target->texture &&
      (s.render_target->surfaceDrawn || s.render_target->hostOwned)) {
    used_fallback = false;
    return s.render_target;
  }
  if (s.last_drawn_rt[slot] && s.last_drawn_rt[slot]->texture) {
    u32 n;
    if (Settings::Get().DiagVerbosity() >= 2 &&
        DiagShouldLog(1, s.render_target, &n)) {
      BD_WARN("[resolve-diag] #{} bound RT {}x{} fmt={} never drawn -> "
              "falling back to last_drawn {}x{}",
              n, s.render_target ? s.render_target->width : 0,
              s.render_target ? s.render_target->height : 0,
              s.render_target ? u32(s.render_target->format) : 0,
              s.last_drawn_rt[slot]->width, s.last_drawn_rt[slot]->height);
    }
    return s.last_drawn_rt[slot];
  }
  return s.render_target;
}

// Exact-content aliasing only: identical dims and format, no exp bias scale, 2D
// non-cube single-mip dst, source SRV live so SharedConstants can substitute
// it. Anything else takes the eager copy.
bool CanAliasResolveLocked(const GuestTexture *src, const GuestTexture *dst) {
  // An MSAA source can never alias: substitution would bind a Texture2DMS
  // descriptor into a Texture2D slot.
  // A scaled resolve (the HDR scene at x0.25) aliases only while the host
  // post chain runs: its passes multiply the surface by dst->resolveScale
  // themselves, and a guest draw that samples the texture materialises the
  // scaled copy first (SetTexture). That copy was a full-res pass a frame.
  const bool scale_ok = dst->resolveScale == 1.0f || HostPostActive();
  // The tail's 16-to-8-bit front conversion (the composite's fp16 surface
  // resolved into the RGBA8 front texture) is a full-res pass that converts
  // nothing a reader needs: every reader samples the texture as float4, so
  // the link can point at the fp16 surface and the conversion never runs.
  // Colour to colour only, and only while the host owns the post chain
  // (2026-09-03). A read the substitution cannot reach still materialises
  // through the shader resolve, which converts.
  const bool format_ok =
      src && dst &&
      (src->format == dst->format ||
       (HostPostActive() && !IsDepthFormat(src->format) &&
        !IsDepthFormat(dst->format) && FullscreenChainClassLocked(state(), dst)));
  return src && dst && src != dst && src->texture && dst->texture &&
         src->sampleCount == plume::RenderSampleCount::COUNT_1 &&
         src->width == dst->width && src->height == dst->height &&
         format_ok && scale_ok &&
         dst->viewDimension !=
             plume::RenderTextureViewDimension::TEXTURE_CUBE &&
         dst->mipLevels <= 1 && src->descriptorIndex != kInvalidDescriptorIndex;
}

} // namespace

void DetachSourceSurfaceLocked(VideoState &s, GuestTexture *texture) {
  if (!texture || !texture->sourceSurface)
    return;
  GuestTexture *source = texture->sourceSurface;
  source->destinationTextures.erase(texture);
  texture->sourceSurface = nullptr;
}

void Video::ScrubPooledSurfaceLinks(GuestTexture *pooled) {
  if (!pooled)
    return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (!pooled->sourceSurface && pooled->destinationTextures.empty())
    return;
  // NotifyTextureDestroyed materializes and detaches before parking, so a
  // linked parked surface means that broke.
  BD_WARN("SurfacePool: acquired surface had live resolve links");
  for (auto *d : pooled->destinationTextures) {
    if (d && d->sourceSurface == pooled)
      d->sourceSurface = nullptr;
  }
}

bool CopySurfaceToTextureLocked(VideoState &s, GuestTexture *src,
                                GuestTexture *dst, const char *reason) {
  if (!dst || !dst->texture || !src || !src->texture || src == dst) {
    return false;
  }
  if (!s.ready)
    return false;
  // Every path below writes a single-sample destination, and a resolve goes
  // MSAA to single, never to MSAA. Rejecting here beats an invalid
  // CopyResource or a sample-mismatched attachment draw.
  if (dst->sampleCount != plume::RenderSampleCount::COUNT_1) {
    static std::atomic<u32> s_warn{0};
    if (s_warn.fetch_add(1, std::memory_order_relaxed) < 8) {
      BD_WARN("{}: MSAA destination {}x{} rejected (resolve targets must be "
              "single-sample)",
              reason, dst->width, dst->height);
    }
    return false;
  }

  BeginCommandList(s);
  if (!s.command_list_open)
    return false;
  // The draws still queued were recorded before this copy was asked for and
  // may render into its source: they go out first. Without this the guest's
  // end-of-pass Resolve copied the scene before its last queued draws landed
  // - a whole frame of clear colour when nothing had flushed during the pass,
  // a darker ground without its last layers otherwise (2026-09-03).
  bd::gpu::DrawQueueFlushAt(s.command_list, BD_FLUSH_SITE);

  // Layouts move below, so BindDrawFramebuffer must re-bind after the copy.
  s.draw_framebuffer_bound = false;

  const bool dims_match =
      src->width == dst->width && src->height == dst->height;
  bool format_match = src->format == dst->format;
  // A non-unity exp bias scale (BD's HDR scene at x0.25) needs the
  // shader resolve path (copy_color_ps * Param0), not a 1:1 copyTexture.
  const float resolve_scale = dst->resolveScale;
  const bool needs_scale = (resolve_scale != 1.0f);
  // A cube dst takes ONE face from a 2D source, and carries no per-face RTV,
  // so it needs a strict 1:1 copyTextureRegion rather than either other path.
  const bool dst_is_cube =
      dst->viewDimension == plume::RenderTextureViewDimension::TEXTURE_CUBE;

  // A full-screen chain texture of another colour format (the RGBA8 front
  // buffer taking the composite's fp16 image) is re-backed in the source's
  // format the first time it is copied into, while the host owns the post
  // chain: the guest never reads the host format back, every reader samples
  // float4, and from then on the copy is a blit and the lazy link needs no
  // conversion. The declared format stays in guestFormat (2026-09-03).
  if (!format_match && dims_match && !needs_scale && !dst_is_cube &&
      dst->type == ResourceType::Texture && dst->mipLevels <= 1 &&
      dst->layers == src->layers && !IsDepthFormat(src->format) &&
      !IsDepthFormat(dst->format) && IsRenderTargetCapable(src->format) &&
      src->sampleCount == plume::RenderSampleCount::COUNT_1 &&
      HostPostActive() && FullscreenChainClassLocked(s, dst) &&
      dst->textureHolder) {
    plume::RenderTextureDesc desc;
    desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
    desc.width = dst->width;
    desc.height = dst->height;
    desc.depth = 1;
    desc.mipLevels = 1;
    desc.arraySize = dst->layers ? dst->layers : 1;
    desc.format = src->format;
    desc.flags = plume::RenderTextureFlag::RENDER_TARGET;
    desc.committed = true;
    auto retyped = CreateHostTexture(s.device.get(), desc, "chain-retyped");
    if (retyped) {
      // Parked by hand: the locked Park helpers take s.mutex, which the copy
      // already holds.
      s.texture_graveyard[Video::RetireSlot("texture")].push_back(
          std::move(dst->textureHolder));
      if (dst->textureView)
        s.texture_view_graveyard[Video::RetireSlot("texture view")].push_back(
            std::move(dst->textureView));
      dst->framebuffers.clear();
      dst->textureHolder = std::move(retyped);
      dst->texture = dst->textureHolder.get();
      dst->format = src->format;
      dst->layout = plume::RenderTextureLayout::UNKNOWN;
      BindTextureSRVLocked(s, dst); // the same slot, a view of the new image
      format_match = true;
      static u32 told = 0;
      if (told++ < 4)
        BD_INFO("[resolve] {}x{} chain texture re-backed in the source's "
                "format (guest 0x{:X} -> {}); its copies are blits now",
                dst->width, dst->height, dst->guestFormat, u32(src->format));
    }
  }

  if (dims_match && format_match && !needs_scale &&
      src->sampleCount == plume::RenderSampleCount::COUNT_1) {
    plume::RenderTextureBarrier pre[2] = {
        plume::RenderTextureBarrier(src->texture,
                                    plume::RenderTextureLayout::COPY_SOURCE),
        plume::RenderTextureBarrier(dst->texture,
                                    plume::RenderTextureLayout::COPY_DEST),
    };
    s.command_list->barriers(plume::RenderBarrierStage::COPY, pre,
                             std::size(pre));
    NoteBarrierCall(2, BarrierSite::Resolve);
    MarkResolve(s.command_list);
    src->layout = plume::RenderTextureLayout::COPY_SOURCE;
    dst->layout = plume::RenderTextureLayout::COPY_DEST;
    if (dst_is_cube) {
      s.command_list->copyTextureRegion(
          plume::RenderTextureCopyLocation::Subresource(
              dst->texture, dst->resolveLevel, dst->resolveFace),
          plume::RenderTextureCopyLocation::Subresource(src->texture, 0, 0));
    } else {
      s.command_list->copyTexture(dst->texture, src->texture);
    }
  } else if (dst_is_cube) {
    // No usable RTV for the shader path, and no real BD cube resolve gets here.
    static std::atomic<u32> s_warn{0};
    if (s_warn.fetch_add(1, std::memory_order_relaxed) < 8) {
      BD_WARN("{}: cube resolve dst needs convert/scale (src={}x{} fmt={} "
              "dst fmt={} scale={}), no per-face RTV, skipping",
              reason, src->width, src->height, static_cast<int>(src->format),
              static_cast<int>(dst->format), resolve_scale);
    }
    return false;
  } else {
    if (src->descriptorIndex == kInvalidDescriptorIndex) {
      BindTextureSRVLocked(s, src);
    }
    if (src->descriptorIndex == kInvalidDescriptorIndex) {
      static std::atomic<u32> s_warn{0};
      if (s_warn.fetch_add(1, std::memory_order_relaxed) < 8) {
        BD_ERROR("{}: source RT 0x{:x} not in bindless heap, can't "
                 "shader-resolve",
                 reason, reinterpret_cast<uintptr_t>(src));
      }
      return false;
    }

    // plume's D3D12Framebuffer asserts the attachment flags, so a depth
    // destination needs its own framebuffer key.
    const bool depth_dst = IsDepthFormat(dst->format);
    if (!depth_dst && !IsRenderTargetCapable(dst->format)) {
      static std::atomic<u32> s_warn{0};
      if (s_warn.fetch_add(1, std::memory_order_relaxed) < 8) {
        BD_ERROR("{}: dst format {} is not RT-capable, skipping shader "
                 "resolve (src={}x{} fmt={} dst={}x{} fmt={})",
                 reason, static_cast<int>(dst->format), src->width, src->height,
                 static_cast<int>(src->format), dst->width, dst->height,
                 static_cast<int>(dst->format));
      }
      return false;
    }

    const plume::RenderTexture *fb_key = depth_dst ? kDepthFbKey : nullptr;

    auto it = dst->framebuffers.find(fb_key);
    plume::RenderFramebuffer *dst_fb = nullptr;
    if (it != dst->framebuffers.end()) {
      dst_fb = it->second.get();
    } else {
      std::unique_ptr<plume::RenderFramebuffer> fb;
      if (depth_dst) {
        plume::RenderFramebufferDesc desc;
        desc.depthAttachment = dst->texture;
        desc.colorAttachmentsCount = 0;
        // Same rule as the colour path: a layered destination needs a view
        // mask or the copy writes layer 0 only.
        desc.viewMask = dst->layers > 1 ? 0x3u : 0u;
        fb = s.device->createFramebuffer(desc);
      } else {
        const plume::RenderTexture *attachments[1] = {dst->texture};
        plume::RenderFramebufferDesc desc(attachments, 1);
        // A layered destination needs a view mask, or the copy writes layer 0
        // and leaves the second eye untouched. Same rule as the scene
        // framebuffer in draw_framebuffer.cpp.
        desc.viewMask = dst->layers > 1 ? 0x3u : 0u;
        fb = s.device->createFramebuffer(desc);
      }
      if (!fb) {
        static std::atomic<u32> s_warn{0};
        if (s_warn.fetch_add(1, std::memory_order_relaxed) < 8) {
          BD_ERROR("{}: createFramebuffer failed (depth_dst={}), skipping "
                   "shader resolve (src={}x{} dst={}x{})",
                   reason, depth_dst ? 1 : 0, src->width, src->height,
                   dst->width, dst->height);
        }
        return false;
      }
      dst_fb = fb.get();
      dst->framebuffers.emplace(fb_key, std::move(fb));
      s.framebuffer_owners.insert(dst);
    }

    const plume::RenderTextureLayout dst_write_layout =
        depth_dst ? plume::RenderTextureLayout::DEPTH_WRITE
                  : plume::RenderTextureLayout::COLOR_WRITE;
    const bool discard_dst =
        (dst->layout == plume::RenderTextureLayout::UNKNOWN);
    plume::RenderTextureBarrier pre[2] = {
        plume::RenderTextureBarrier(src->texture,
                                    plume::RenderTextureLayout::SHADER_READ),
        plume::RenderTextureBarrier(dst->texture, dst_write_layout),
    };
    s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, pre,
                             std::size(pre));
    NoteBarrierCall(2, BarrierSite::Resolve);
    MarkResolve(s.command_list);
    src->layout = plume::RenderTextureLayout::SHADER_READ;
    dst->layout = dst_write_layout;
    if (discard_dst)
      s.command_list->discardTexture(dst->texture);

    s.command_list->setFramebuffer(dst_fb);
    NoteFbBind();
    s.command_list->setViewports(
        plume::RenderViewport(0.0f, 0.0f, static_cast<float>(dst->width),
                              static_cast<float>(dst->height)));
    s.command_list->setScissors(plume::RenderRect(
        0, 0, static_cast<i32>(dst->width), static_cast<i32>(dst->height)));
    plume::RenderPipeline *pipeline = nullptr;
    if (src->sampleCount != plume::RenderSampleCount::COUNT_1) {
      // Bound as a Texture2DMS SRV by plume: resolve N samples, stretching and
      // applying the color exp bias scale on the way.
      pipeline = GetOrCreateResolveMSAAPipeline(
          s, dst->format, src->sampleCount, depth_dst,
          dst->layers > 1 ? 0x3u : 0u);
    } else if (depth_dst) {
      pipeline = GetOrCreateCopyDepthPipeline(s, dst->format,
                                              dst->layers > 1 ? 0x3u : 0u);
    } else {
      pipeline = GetOrCreateResolvePipeline(s, dst->format,
                                            dst->layers > 1 ? 0x3u : 0u);
    }
    if (!pipeline) {
      static std::atomic<u32> s_warn{0};
      if (s_warn.fetch_add(1, std::memory_order_relaxed) < 8) {
        BD_ERROR("{}: createGraphicsPipeline failed for dst format {}", reason,
                 static_cast<int>(dst->format));
      }
      return false;
    }
    s.command_list->setPipeline(pipeline);
    NotePSOSwitch();
    u32 descriptor_index = src->descriptorIndex;
    u32 descriptor_index2 = 0u;

    // A multisampled two-layer source gets one single-slice view per eye, and
    // the MSAA resolve shaders pick the slice by SV_ViewID. This is the
    // multiview flatten: tools/rdc_layer_diff.py read every pass's layers
    // back and found the scene pair collapsing at exactly this draw, because
    // Texture2DMS has no layer axis and both views loaded layer 0. Declaring
    // the heap Texture2DMSArray instead compiled and rendered nothing at all
    // on the desktop, so the slice is selected here, by descriptor, keeping
    // the shader type that is known to work. The non-MSAA path
    // (copy_color_ps) already reads the array by SV_ViewID.
    if (src->layers > 1 && src->sampleCount != plume::RenderSampleCount::COUNT_1) {
      for (u32 layer = 0; layer < 2; ++layer) {
        if (!src->layerView[layer] || src->layerViewOf != src->texture) {
          plume::RenderTextureViewDesc eye;
          eye.format = src->format;
          eye.dimension = plume::RenderTextureViewDimension::TEXTURE_2D;
          eye.mipLevels = 1;
          eye.arraySize = 1;
          eye.arrayIndex = layer;
          src->layerView[layer] = src->texture->createTextureView(eye);
          if (src->layerView[layer]) {
            if (src->layerDescriptorIndex[layer] == kInvalidDescriptorIndex)
              src->layerDescriptorIndex[layer] =
                  Video::AllocateBindlessTextureSlot();
            if (src->layerDescriptorIndex[layer] != kInvalidDescriptorIndex)
              SetBindlessTextureLocked(s, src->layerDescriptorIndex[layer],
                                       src->texture,
                                       src->layerView[layer].get());
          }
        }
      }
      src->layerViewOf = src->texture;
      if (src->layerDescriptorIndex[0] != kInvalidDescriptorIndex &&
          src->layerDescriptorIndex[1] != kInvalidDescriptorIndex) {
        descriptor_index = src->layerDescriptorIndex[0];
        descriptor_index2 = src->layerDescriptorIndex[1];
      }
    }

    u32 box_ratio = 0u;
    if (!depth_dst && src->sampleCount == plume::RenderSampleCount::COUNT_1 &&
        dst->width != 0u && dst->height != 0u && src->width > dst->width &&
        src->height > dst->height && (src->width % dst->width) == 0u &&
        (src->height % dst->height) == 0u &&
        (src->width / dst->width) == (src->height / dst->height)) {
      box_ratio = src->width / dst->width;
    }

    struct CopyPushConstants {
      u32 resourceDescriptorIndex;
      u32 resourceDescriptorIndex2;
      float param0;
      float param1;
    } push{descriptor_index, descriptor_index2,
           depth_dst ? 1.0f : resolve_scale, static_cast<float>(box_ratio)};
    s.command_list->setGraphicsPushConstants(kCopyPushConstantRangeIndex, &push,
                                             kCopyPushConstantByteOffset,
                                             sizeof(push));
    s.command_list->drawInstanced(3, 1, 0, 0);

    s.command_list->setFramebuffer(nullptr);
    // The blit PSO replaced the engine's, so invalidate the cache.
    s.current_pso = nullptr;
    s.dirtyStates.pipelineState = true;
    s.draw_framebuffer_bound = false;
  }

  plume::RenderTextureBarrier post(dst->texture,
                                   plume::RenderTextureLayout::SHADER_READ);
  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, &post, 1);
  NoteBarrierCall(1, BarrierSite::Resolve);
  MarkResolve(s.command_list);
  dst->layout = plume::RenderTextureLayout::SHADER_READ;
  return true;
}

namespace {

// Clear a depth resolve dst (the shadow map) to far. Leaves it in SHADER_READ.
bool ClearDepthTargetToFarLocked(VideoState &s, GuestTexture *dst) {
  if (!dst || !dst->texture || !s.ready)
    return false;
  if (!IsDepthFormat(dst->format))
    return false;
  BeginCommandList(s);
  if (!s.command_list_open)
    return false;
  s.draw_framebuffer_bound = false;

  plume::RenderFramebuffer *dst_fb = nullptr;
  auto it = dst->framebuffers.find(kDepthFbKey);
  if (it != dst->framebuffers.end()) {
    dst_fb = it->second.get();
  } else {
    plume::RenderFramebufferDesc desc;
    desc.depthAttachment = dst->texture;
    desc.colorAttachmentsCount = 0;
    auto fb = s.device->createFramebuffer(desc);
    if (!fb)
      return false;
    dst_fb = fb.get();
    dst->framebuffers.emplace(kDepthFbKey, std::move(fb));
    s.framebuffer_owners.insert(dst);
  }

  const bool discard_dst = (dst->layout == plume::RenderTextureLayout::UNKNOWN);
  plume::RenderTextureBarrier pre(dst->texture,
                                  plume::RenderTextureLayout::DEPTH_WRITE);
  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, &pre, 1);
  NoteBarrierCall(1, BarrierSite::Resolve);
  MarkResolve(s.command_list);
  dst->layout = plume::RenderTextureLayout::DEPTH_WRITE;
  if (discard_dst)
    s.command_list->discardTexture(dst->texture);
  s.command_list->setFramebuffer(dst_fb);
  NoteFbBind();
  s.command_list->setViewports(
      plume::RenderViewport(0.0f, 0.0f, static_cast<float>(dst->width),
                            static_cast<float>(dst->height)));
  s.command_list->setScissors(plume::RenderRect(
      0, 0, static_cast<i32>(dst->width), static_cast<i32>(dst->height)));
  s.command_list->clearDepthStencil(true, true, 1.0f, 0);
  s.command_list->setFramebuffer(nullptr);
  s.draw_framebuffer_bound = false;

  plume::RenderTextureBarrier post(dst->texture,
                                   plume::RenderTextureLayout::SHADER_READ);
  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, &post, 1);
  NoteBarrierCall(1, BarrierSite::Resolve);
  MarkResolve(s.command_list);
  dst->layout = plume::RenderTextureLayout::SHADER_READ;
  return true;
}

} // namespace

// Copy every pending destination out of 'source' and drop the links. A failed
// copy keeps its link so substitution still serves reads, and retries the next
// time a trigger touches this surface.
bool MaterializeOutboundLocked(VideoState &s, GuestTexture *source,
                               bool aliasable_only) {
  if (!source || source->destinationTextures.empty())
    return false;
  bool recorded = false;
  std::vector<GuestTexture *> destinations(source->destinationTextures.begin(),
                                           source->destinationTextures.end());
  for (GuestTexture *dst : destinations) {
    if (!dst || dst == source || dst->sourceSurface != source)
      continue;
    if (dst->pendingDestroy) {
      DetachSourceSurfaceLocked(s, dst);
      continue;
    }
    // Destroy-time materialize takes only the descriptor-free 1:1 path: the
    // shader path samples a bindless slot a concurrent release may null out of
    // order. Anything that does not qualify is left for the detach loop.
    const bool one_to_one =
        source->width == dst->width && source->height == dst->height &&
        source->format == dst->format &&
        source->sampleCount == plume::RenderSampleCount::COUNT_1 &&
        dst->resolveScale == 1.0f &&
        dst->viewDimension != plume::RenderTextureViewDimension::TEXTURE_CUBE;
    if (aliasable_only && !one_to_one)
      continue;
    if (CopySurfaceToTextureLocked(s, source, dst, "Materialize")) {
      NoteResolveOp(ResolveOp::Materialize);
      recorded = true;
      DetachSourceSurfaceLocked(s, dst);
      // Named once in a field scene: each is a full copy the EDRAM model
      // asked for (stage 4 verification, 2026-09-03).
      static u32 listed = 0;
      const u32 frame = FrameStatFrameCount();
      if (frame > 3000 && listed < 12) {
        ++listed;
        BD_INFO("[materialize] frame {} outbound{}: {}x{} fmt {} s{} host {} "
                "-> {}x{} fmt {} scale {:.3f}",
                frame, aliasable_only ? " (destroy)" : "", source->width,
                source->height, u32(source->format), u32(source->sampleCount),
                source->hostOwned ? 1 : 0, dst->width, dst->height,
                u32(dst->format), dst->resolveScale);
      }
    }
  }
  return recorded;
}

// dst is about to be overwritten (bound as RT/DS or CPU-uploaded) while a
// deferred resolve into it is still pending, so absorb that content first.
void MaterializeInboundLocked(VideoState &s, GuestTexture *dst) {
  if (!dst || !dst->sourceSurface || dst->sourceSurface == dst)
    return;
  if (CopySurfaceToTextureLocked(s, dst->sourceSurface, dst, "Materialize")) {
    NoteResolveOp(ResolveOp::Materialize);
    static u32 listed = 0;
    const u32 frame = FrameStatFrameCount();
    if (frame > 3000 && listed < 12) {
      ++listed;
      BD_INFO("[materialize] frame {} inbound: {}x{} fmt {} s{} host {} -> "
              "{}x{} fmt {} scale {:.3f}",
              frame, dst->sourceSurface->width, dst->sourceSurface->height,
              u32(dst->sourceSurface->format),
              u32(dst->sourceSurface->sampleCount),
              dst->sourceSurface->hostOwned ? 1 : 0, dst->width, dst->height,
              u32(dst->format), dst->resolveScale);
    }
    DetachSourceSurfaceLocked(s, dst);
  }
}

// X360 aliases one EDRAM tile across scene, posteff and 2D overlay, each pass
// inheriting the last one's pixels. Output res splits the dims, so membership
// tests back buffer aspect plus a minimum width rather than dim equality.
// Aspect matches within ~3% because Output::LatchedFit rounds to a multiple of 8,
// which BD's true 16:9 chain never equals, a square or half-width buffer is
// >40% off and stays out.
bool FullscreenChainClassLocked(const VideoState &s, const GuestTexture *t) {
  const GuestTexture *bb = s.back_buffer_surface;
  if (!t || !bb || t == bb || IsDepthFormat(t->format))
    return false;
  // Cutscenes render the whole chain at the design width even under a wider
  // backbuffer, so the gate is min(bb width, design width). The bloom pyramid
  // stays below it and the aspect test rejects odd-sized buffers.
  constexpr u32 kDesignWidth = 1280u;
  const u32 min_full_width =
      bb->width > kDesignWidth ? kDesignWidth : bb->width;
  if (t->width < min_full_width)
    return false;
  const u64 a = u64(t->width) * bb->height;
  const u64 b = u64(t->height) * bb->width;
  const u64 diff = a > b ? a - b : b - a;
  return diff * 100ull <= b * 3ull; // aspect_t and aspect_bb within ~3%
}

void Video::TrackResolveSource(u32 flags, GuestTexture *dst, u32 level,
                               u32 face) {
  if (!dst || !dst->texture)
    return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (!s.ready)
    return;
  // Which subresource (cube face / mip) the EDRAM source resolves into, read by
  // CopySurfaceToTextureLocked.
  dst->resolveLevel = level;
  dst->resolveFace = face;
  dst->resolveClearToFar = false;
  // D3DRESOLVE_EXPONENTBIAS scales the EDRAM source by 2^bias on resolve. BD
  // resolves the HDR scene at bias=-2 so the posteff 4x exposure restores it.
  const int exp_bias = static_cast<i32>(flags) >> 26;
  dst->resolveScale = static_cast<float>(std::ldexp(1.0, exp_bias));
  bool used_fallback = false;
  GuestTexture *src = ResolveSourceForFlagsLocked(s, flags, used_fallback);
  if (!src || !src->texture)
    return;
  if (src == dst)
    return;
  dst->resolveSourceFallback = used_fallback;

  // A small color destination fed from a larger, undrawn source means the
  // fallback above picked the main scene rather than the tile X360 would still
  // hold. Pointing sourceSurface at dst takes the src==dst early-out and keeps
  // the last valid content, a drawn source still refreshes normally.
  const bool fp_is_depth = (flags & 0x4u) != 0;
  GuestTexture *fp_bound = fp_is_depth ? s.depth_stencil : s.render_target;
  const bool fp_bound_drawn =
      fp_bound && fp_bound->texture && fp_bound->surfaceDrawn;
  const bool fp_is_cube =
      dst->viewDimension == plume::RenderTextureViewDimension::TEXTURE_CUBE;
  if (!fp_is_depth && !fp_is_cube && !fp_bound_drawn && src != dst &&
      dst->width <= 512 && dst->height <= 512 &&
      (src->width > dst->width || src->height > dst->height)) {
    // Under lazy resolve that content may live only in the linked source, so
    // absorb it before the guard drops the link.
    MaterializeInboundLocked(s, dst);
    DetachSourceSurfaceLocked(s, dst);
    dst->sourceSurface = dst; // sentinel: ResolveRtToTexture skips on src==dst
    return;
  }

  // An empty caster pass clears the shadow atlas to far rather than resolving
  // an uninitialized pool slot, where all-zero reads as fully occluded.
  // Square-and-undrawn excludes screen depth resolves.
  if (fp_is_depth && !fp_is_cube && !fp_bound_drawn &&
      dst->width == dst->height && dst->width >= 1024) {
    DetachSourceSurfaceLocked(s, dst);
    dst->sourceSurface = nullptr;
    dst->resolveClearToFar = true;
    return;
  }

  if (dst->sourceSurface && dst->sourceSurface != dst) {
    // A live link superseded here is a copy that never had to happen.
    NoteResolveOp(ResolveOp::DeadElide);
  }

  if (dst->sourceSurface && dst->sourceSurface != src) {
    dst->sourceSurface->destinationTextures.erase(dst);
  }
  dst->sourceSurface = src;
  src->destinationTextures.insert(dst);
  BindTextureSRVLocked(s, src);
}

namespace {

// A resolve destination holds what the tile held at pass end, so it is what the
// next pass of that class inherits. The head must anchor on unscaled resolves
// too: at 1:1 the scene resolve either aliases lazily or copies unscaled.
// pass_end excludes a last_drawn fallback, which names another surface.
void NoteTileContentLocked(VideoState &s, GuestTexture *dst, bool pass_end) {
  if (!dst || !dst->texture)
    return;
  if (!FullscreenChainClassLocked(s, dst)) {
    // Off-screen RTT link, keyed by exact dims, cleared per frame.
    s.subchain_resolve[(u64(dst->width) << 32) | dst->height] = dst;
  } else if (pass_end) {
    s.fullscreen_chain_head[s.recording_slot()] = dst;
  }
}

} // namespace

bool Video::PublishSceneOutput(GuestTexture *src, GuestTexture *dst, float exposure,
                               bool publish_post_chain) {
  if (!src || !dst || !src->texture || !dst->texture || src == dst ||
      dst->sampleCount != plume::RenderSampleCount::COUNT_1 ||
      dst->viewDimension == plume::RenderTextureViewDimension::TEXTURE_CUBE ||
      src->layers != dst->layers || !std::isfinite(exposure) || exposure <= 0)
    return false;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (!s.ready)
    return false;
  // These are compatibility destination publications, never source selectors.
  // A newer complete scene supersedes the prior inbound publication.
  DetachSourceSurfaceLocked(s, dst);
  dst->resolveLevel = dst->resolveFace = 0;
  dst->resolveClearToFar = dst->resolveSourceFallback = false;
  dst->resolveScale = exposure;
  dst->sourceSurface = src;
  src->destinationTextures.insert(dst);
  BindTextureSRVLocked(s, src);
  if (CanAliasResolveLocked(src, dst)) {
    NoteResolveOp(ResolveOp::LazyLink);
  } else {
    if (!CopySurfaceToTextureLocked(s, src, dst, "Native scene output")) {
      DetachSourceSurfaceLocked(s, dst);
      return false;
    }
    NoteResolveOp(ResolveOp::EagerCopy);
    DetachSourceSurfaceLocked(s, dst);
  }
  s.last_resolved_dst = dst;
  // Remaining post/UI producers still consume the legacy chain publication.
  // This adapter is not native frame scheduling or removal of their tile model.
  if (publish_post_chain)
    NoteTileContentLocked(s, dst, /*pass_end=*/true);
  s.draw_framebuffer_bound = false;
  return true;
}

void Video::ResolveRtToTexture(GuestTexture *dst) {
  BD_CPU_ZONE("ResolveRtToTexture");
  if (!dst || !dst->texture)
    return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (!s.ready)
    return;
  if (dst->resolveClearToFar) {
    dst->resolveClearToFar = false;
    ClearDepthTargetToFarLocked(s, dst);
    return;
  }
  GuestTexture *src = dst->sourceSurface ? dst->sourceSurface : s.render_target;
  if (!src || !src->texture)
    return;
  if (src == dst)
    return;
  // A level the host post chain overwrites every frame: the guest's scaled
  // resolve into it (688x360 on the Quest, 0.78 ms) would be copied and then
  // written over. Neither copied nor aliased - an alias would hand the
  // chain's readers the surface instead of the level.
  // Likewise a downscaled resolve of the full-screen scene: it fed the
  // guest's first quarter pass, which is dropped, and the host pyramid reads
  // the full-res scene texture. Two of them a frame (960x540 on the desktop,
  // 688x360 on the Quest at 0.78 ms each).
  const bool downscaled_scene =
      HostPostActive() && dst->width < src->width && dst->height < src->height &&
      dst->format == src->format && FullscreenChainClassLocked(s, src);
  if (HostPostWillOverwrite(dst) || downscaled_scene) {
    static u32 told = 0;
    if (told++ < 3)
      BD_INFO("[resolve] skipped: {}x{} -> {}x{} ({})", src->width,
              src->height, dst->width, dst->height,
              downscaled_scene ? "downscaled scene, no reader with the host "
                                 "post chain"
                               : "a host post level");
    s.last_resolved_dst = dst;
    s.draw_framebuffer_bound = false;
    return;
  }

  // A fallback-guessed source is not finished pass content: it may be redrawn
  // before this dst is sampled, so aliasing would serve drifted pixels. Only a
  // genuinely drawn source may defer.
  if (dst->sourceSurface == src && !dst->resolveSourceFallback &&
      CanAliasResolveLocked(src, dst)) {
    // Keep the link TrackResolveSource made: the copy happens only if the
    // surface is overwritten, destroyed, or read where substitution cannot
    // reach.
    NoteResolveOp(ResolveOp::LazyLink);
    s.last_resolved_dst = dst;
    // This path already required a tracked, non-fallback source.
    NoteTileContentLocked(s, dst, /*pass_end=*/true);
    s.draw_framebuffer_bound =
        false; // Parity with the eager copy path: a resolve ends the pass.
    return;
  }

  if (CopySurfaceToTextureLocked(s, src, dst, "Resolve")) {
    NoteResolveOp(ResolveOp::EagerCopy);
    // The eager copies of a field frame, listed once: each is a pass of its
    // own on the headset (0.3-0.6 ms with its blit and preemption).
    {
      static u32 listed = 0;
      const u32 frame = FrameStatFrameCount();
      if (frame > 3000 && listed < 12) {
        ++listed;
        BD_INFO("[resolve] frame {} eager: {}x{} fmt {} s{} -> {}x{} fmt {} "
                "scale {:.3f} mips {}",
                frame, src->width, src->height, u32(src->format),
                u32(src->sampleCount), dst->width, dst->height,
                u32(dst->format), dst->resolveScale, dst->mipLevels);
      }
    }
    s.last_resolved_dst = dst;
    // A scaled full-screen resolve (672x720 scene -> 1280x720 at design res) is
    // always the scene history. A 1:1 one is the same pass end, but this path
    // also runs on a last_drawn fallback, so require a tracked src.
    const bool pass_end =
        src->width != dst->width ||
        (dst->sourceSurface == src && !dst->resolveSourceFallback);
    NoteTileContentLocked(s, dst, pass_end);
    if (dst->sourceSurface == src) {
      DetachSourceSurfaceLocked(s, dst);
    }
  }
}

} // namespace bd::gpu
