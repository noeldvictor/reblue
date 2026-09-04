/**
 * @file    gpu/foveation.h
 * @brief   Fixed foveated rendering: a fragment density map for the scene pass.
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 * @license     BSD 3-Clause - see LICENSE
 */
#pragma once

#include <plume_render_interface.h>
#include <rex/types.h>

namespace bd::gpu {

// The scene pass is ~45ms of a 56ms frame on a Quest 2 - 81% of all GPU time,
// measured per render target rather than inferred - and it is a two-layer
// target, so every fragment it shades is shaded twice. That is exactly what
// foveation exists for.
//
// This does not use XR_FB_foveation. That decorates an XR swapchain, and this
// renderer does not render into one: the guest owns its surfaces and present
// composites into the runtime's image. VK_EXT_fragment_density_map decorates an
// ordinary render pass instead, which is why foveation needs no present rewrite
// - the constraint the port plan carried from the beginning and which turned
// out to apply only to the OpenXR path.
//
// The map is one texel per tile (the device reports 32x32 on a Quest 2), and
// each texel says what fraction of full rate to shade that tile at. Centre
// stays at full rate; the periphery falls off, which is where a lens puts the
// least acuity anyway.

// The density map for a target of this size, created on first use and cached.
// Null when the device has no VK_EXT_fragment_density_map, when foveation is
// off, or when the target is not one worth foveating.
plume::RenderTexture *FoveationMapFor(u32 width, u32 height);

// Whether draws into a target of this size should get a foveated render pass.
// The pipeline and the framebuffer must agree - a mismatch is an incompatible
// render pass, which Vulkan leaves undefined rather than reporting - so both
// sides ask this, and it must not depend on anything that changes per draw.
bool FoveationWanted(u32 width, u32 height, u32 layers);

// Queue a density map for this target size if there is not one yet. Called from
// the framebuffer bind; the map goes live at the next frame start, so the first
// frames at a new size are simply unfoveated.
// `layers` counts a two-layer multiview target's fragments twice, which is
// what makes the half-width stereo scene qualify.
void FoveationEnsure(u32 width, u32 height, u32 layers);

// Called at the start of a frame, with a freshly opened command list and no
// render pass active. Uploads any map created since the last frame.
//
// The upload cannot happen where the map is first wanted - that is inside a
// framebuffer bind, inside a draw, with the render thread's mutex held and a
// command list mid-recording. Doing it there deadlocked on a non-recursive
// mutex, and then corrupted the command list badly enough that present crashed
// dereferencing a null one.
void FoveationBeginFrame(plume::RenderCommandList *cmd);

// Drop the cached maps. Device teardown.
void FoveationShutdown();

} // namespace bd::gpu
