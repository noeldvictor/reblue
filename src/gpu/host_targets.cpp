/**
 * @file    gpu/host_targets.cpp
 * @brief   Host-owned render targets: persistent surfaces for the shadow map
 *          and the scene pair, host-held clears, copy-free resolve links.
 *
 * Why (2026-09-03): the Xbox 360 frame model had the guest create the scene
 * and shadow surfaces every frame, so the host pooled them, seeded fresh
 * ones from their predecessors, held their clears in plume across the
 * unbind, and copied a resolve link out of them whenever the guest released
 * the handle or drew into them again. A desktop framebuffer trace of one
 * frame showed the shadow map's clear running as a zero-draw pass of its
 * own (the scene colour clear's framebuffer switch pushed it out of plume's
 * hold, and a stale texture slot's layout barrier flushed it), and a
 * materialise copy of the shadow map every frame at release. None of it is
 * needed once the targets are the host's: the scene is cleared and fully
 * redrawn each frame, the shadow map likewise, and every reader of the
 * previous frame's content has run before the next pass draws.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/host_targets.h"

#include <atomic>
#include <mutex>

#include <plume_render_interface.h>
#include <rex/cvar.h>

#include "core/logging.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/format.h"
#include "gpu/frame.h"
#include "gpu/frame_stats.h"
#include "gpu/resources.h"
#include "gpu/surface_pool.h"

REXCVAR_DECLARE(bool, bd_host_targets);

namespace bd::gpu {

namespace {

struct Slot {
  GuestTexture *target = nullptr;
  u32 width = 0;
  u32 height = 0;
  u32 guest_format = 0;
  u32 sample_count = 0;
  u32 recreated = 0;
};

std::mutex g_mutex;
Slot g_slots[static_cast<u32>(HostTargetClass::Count)];

Slot &SlotFor(HostTargetClass cls) { return g_slots[static_cast<u32>(cls)]; }

// A surface the host no longer owns goes back through the guest's normal
// release: if the guest still holds it, its Release frees it; if the guest
// already released it (the release was a no-op while it was host-owned),
// it is queued now.
void Disown(GuestTexture *old) {
  if (!old)
    return;
  old->hostOwned = false;
  old->hostTargetClass = static_cast<u8>(HostTargetClass::None);
  if (!old->hostTargetLive)
    Video::QueueResourceDestroy(old->selfVa, old->type);
}

} // namespace

const char *HostTargetClassName(HostTargetClass cls) {
  switch (cls) {
  case HostTargetClass::Shadow:
    return "shadow";
  case HostTargetClass::SceneColor:
    return "scene colour";
  case HostTargetClass::SceneDepth:
    return "scene depth";
  case HostTargetClass::ReflectionColor:
    return "reflection colour";
  case HostTargetClass::ReflectionDepth:
    return "reflection depth";
  case HostTargetClass::PostColor:
    return "post colour";
  default:
    return "none";
  }
}

HostTargetClass ClassifyHostTarget(u32 width, u32 height, u32 guest_format,
                                   bool msaa_requested) {
  if (!REXCVAR_GET(bd_host_targets))
    return HostTargetClass::None;
  const bool is_depth = IsDepthFormat(ConvertGuestFormat(guest_format));
  // The sun shadow map is the only square depth surface the game creates:
  // 4096x4096 on the desktop, the 64x64 stub with shadows off.
  if (is_depth && width == height && width >= 64)
    return HostTargetClass::Shadow;
  // The scene pair is the only surface pair the game asks multisampling for
  // (the CreateSurface hook has relied on that since the MSAA cvar).
  if (msaa_requested)
    return is_depth ? HostTargetClass::SceneDepth : HostTargetClass::SceneColor;
  // The reflection view: a small pair created right after the shadow map,
  // 480x270 on the desktop and 128x72 on the Quest. Its depth is the only
  // small non-square depth surface; its colour is the only small surface in
  // the 8-bit format (the post chain's small targets are fp16).
  if (width <= 512 && height <= 512) {
    if (is_depth)
      return HostTargetClass::ReflectionDepth;
    if (guest_format == 0x18280186u)
      return HostTargetClass::ReflectionColor;
  }
  return HostTargetClass::None;
}

GuestTexture *HostTargetAcquire(HostTargetClass cls, u32 width, u32 height,
                                u32 guest_format, u32 sample_count) {
  if (cls == HostTargetClass::None || cls >= HostTargetClass::Count)
    return nullptr;
  std::lock_guard lock(g_mutex);
  Slot &slot = SlotFor(cls);
  GuestTexture *t = slot.target;
  if (t && (slot.width != width || slot.height != height ||
            slot.guest_format != guest_format ||
            slot.sample_count != sample_count)) {
    BD_INFO("[targets] {} {}x{} fmt 0x{:X} s{} -> {}x{} fmt 0x{:X} s{}: "
            "recreated (#{})",
            HostTargetClassName(cls), slot.width, slot.height,
            slot.guest_format, slot.sample_count, width, height, guest_format,
            sample_count, ++slot.recreated);
    Disown(t);
    t = slot.target = nullptr;
  }
  if (t && t->hostTargetLive) {
    // The guest asked for a second surface of this class while holding the
    // first. Not seen; the pool serves it so nothing breaks.
    static std::atomic<u32> told{0};
    if (told.fetch_add(1, std::memory_order_relaxed) < 4)
      BD_WARN("[targets] {} requested while the guest still holds it; pooled",
              HostTargetClassName(cls));
    return nullptr;
  }
  if (!t) {
    t = SurfacePool::CreateUnpooled(width, height, guest_format, sample_count);
    if (!t)
      return nullptr;
    t->hostOwned = true;
    t->hostTargetClass = static_cast<u8>(cls);
    slot.target = t;
    slot.width = width;
    slot.height = height;
    slot.guest_format = guest_format;
    slot.sample_count = sample_count;
    BD_INFO("[targets] {} {}x{} fmt 0x{:X} s{} layers {}: host-owned",
            HostTargetClassName(cls), width, height, guest_format,
            sample_count, t->layers);
  } else {
    // The same handle again for a new frame. The image and its layout are
    // real and stay; only the guest-facing header and the per-frame marks
    // reset. surfaceDrawn stays false until the pass draws, which is what
    // the resolve source logic reads (an undrawn shadow map resolves as a
    // clear to far).
    InitResourceHeader(t->x360.as_surface.resource, D3DResourceType::kSurface);
    t->surfaceDrawn = false;
    t->pendingDestroy = false;
    t->pendingGPURead = false;
    t->resolveSourceFallback = false;
  }
  t->hostTargetLive = true;
  return t;
}

void HostTargetReleased(GuestTexture *target) {
  if (!target)
    return;
  target->hostTargetLive = false;
}

bool HostTargetRequestClear(GuestTexture *target, u32 flags, u32 color_argb,
                            float depth, u32 stencil) {
  if (!target || !target->hostOwned)
    return false;
  if (flags & 0x1u) {
    target->hostClearFlags |= 0x1u;
    target->hostClearColor = color_argb;
  }
  if (flags & 0x30u) {
    target->hostClearFlags |= (flags & 0x30u);
    target->hostClearDepth = depth;
    target->hostClearStencil = stencil;
  }
  return true;
}

void HostTargetApplyClears(VideoState &s, GuestTexture *rt, GuestTexture *ds) {
  if (rt && rt->hostOwned && (rt->hostClearFlags & 0x1u)) {
    s.command_list->clearColor(0, ArgbToRenderColor(rt->hostClearColor));
    rt->hostClearFlags &= ~0x1u;
  }
  if (ds && ds->hostOwned && (ds->hostClearFlags & 0x30u)) {
    s.command_list->clearDepthStencil((ds->hostClearFlags & 0x10u) != 0,
                                      (ds->hostClearFlags & 0x20u) != 0,
                                      ds->hostClearDepth, ds->hostClearStencil);
    ds->hostClearFlags &= ~0x30u;
  }
}

void HostTargetDropLinks(VideoState &s, GuestTexture *target) {
  if (!target || target->destinationTextures.empty())
    return;
  // Copy the set: DetachSourceSurfaceLocked erases from it.
  std::vector<GuestTexture *> dsts(target->destinationTextures.begin(),
                                   target->destinationTextures.end());
  for (GuestTexture *dst : dsts) {
    if (dst && dst->sourceSurface == target) {
      DetachSourceSurfaceLocked(s, dst);
      NoteResolveOp(ResolveOp::DeadElide);
    }
  }
  target->destinationTextures.clear();
}

} // namespace bd::gpu
