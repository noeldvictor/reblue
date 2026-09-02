/**
 * @file    gpu/post_chain.h
 * @brief   The host-owned post chain: the depth-of-field pyramid and the bloom
 *          mask, produced by host passes into the guest's own textures.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once
#include <rex/types.h>

namespace bd::gpu {

struct VideoState;

// Called from the draw path once the framebuffer for a guest draw is bound and
// before its state is flushed, with the pixel shader hash the draw would use.
// Returns true when the host has consumed the draw: the thirteen producer
// draws of Blue Dragon's post chain (quarter downsamples, weighted blurs, the
// bright pass) are dropped, and at the two composite draws that consume their
// results the host first fills the sampled textures itself. The composites run
// as the guest wrote them.
//
// Measured 2026-09-02: a field frame's post chain is 15 full-screen quads,
// each through the tile and a resolve. The host does the same work in ~17
// small passes over 1/2 to 1/16 of the scene with no resolves between them.
bool HostPostIntercept(VideoState &s, u64 ps_hash, u32 device_guest);

} // namespace bd::gpu
