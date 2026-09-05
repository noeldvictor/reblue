/**
 * @file    gpu/host_targets.h
 * @brief   Host-owned render targets (stage 4): the shadow map and the scene
 *          colour and depth are persistent host textures the guest's
 *          CreateSurface hands back every frame instead of a pooled scratch
 *          surface. Their clears are the host's (applied as the load op of
 *          the pass that draws them), their resolves are links that never
 *          copy, and the guest's Release of the handle frees nothing.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/types.h>

namespace bd::gpu {

struct GuestTexture;
struct VideoState;

enum class HostTargetClass : u8 {
  None = 0,
  Shadow,
  SceneColor,
  SceneDepth,
  ReflectionColor,
  ReflectionDepth,
  PostColor,
  Count
};

// What the guest is asking for, from the CreateSurface arguments alone: a
// square depth surface is the sun shadow map (4096 on the desktop, the 64x64
// stub when shadows are off); the multisample-requested pair is the scene;
// the small 8-bit colour + depth pair is the water reflection view.
HostTargetClass ClassifyHostTarget(u32 width, u32 height, u32 guest_format,
                                   bool msaa_requested);

// The class's persistent surface, created on first use and recreated when
// the requested size, format or sample count changes; nullptr when the host
// does not own this class (bd_host_targets off) or the guest still holds the
// previous handle, in which case the caller takes the pool.
GuestTexture *HostTargetAcquire(HostTargetClass cls, u32 width, u32 height,
                                u32 guest_format, u32 sample_count);

// The guest released its handle: the target stays, the handle can be handed
// out again.
void HostTargetReleased(GuestTexture *target);

// The guest's Clear on a bound host target, kept on the target until its
// pass binds. Returns false if the target is not host-owned.
bool HostTargetRequestClear(GuestTexture *target, u32 flags, u32 color_argb,
                            float depth, u32 stencil);

// After setFramebuffer for a pass on host targets: issue the clears the
// guest asked for as this pass's load op.
void HostTargetApplyClears(VideoState &s, GuestTexture *rt, GuestTexture *ds);

// Drops every resolve link out of the target without copying: a host target
// is only redrawn after every reader of its previous content has run.
void HostTargetDropLinks(VideoState &s, GuestTexture *target);

const char *HostTargetClassName(HostTargetClass cls);

} // namespace bd::gpu
