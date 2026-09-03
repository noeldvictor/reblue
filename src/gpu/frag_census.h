/**
 * @file    gpu/frag_census.h
 * @brief   Fragment census: fragment shader invocations per guest pixel
 *          shader, from pipeline-statistics queries around every queued
 *          draw. Desktop only (Vulkan pipeline statistics); the Quest's
 *          counters give the total, this gives the split.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <plume_render_interface.h>
#include <rex/types.h>

namespace bd::gpu {

// At command-list begin: resets the slot's pool. Off (bd_frag_census false,
// or no statistics pool) it does nothing.
void FragCensusFrameBegin(plume::RenderDevice *device,
                          plume::RenderCommandList *cmd, u32 slot);
// Around one draw. Begin returns whether a query was opened; End closes it.
bool FragCensusBegin(plume::RenderCommandList *cmd, u64 ps_hash);
void FragCensusEnd(plume::RenderCommandList *cmd);
// After the slot's fence: reads the counts, folds them per pixel shader, and
// prints the top shaders every few hundred frames.
void FragCensusCollect(u32 slot);
// Per dispatched draw: the pixel shader and the guest's four pixel-shader
// boolean constant words, which steer the uber-shaders' paths. Reported with
// the fragment report as the paths a host material has to implement.
void FragCensusNoteDraw(u64 ps_hash, const u32 bools[4]);

} // namespace bd::gpu
