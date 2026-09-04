/**
 * @file    gpu/sampler_cache.h
 * @brief   Native sampler cache and compatibility fetch-state importer.
 *
 *   One bindless entry per complete native sampler description. Compatibility
 *   callers can import a recipe from D3DDevice::fetchConstants[N].
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/types.h>

#include <plume_render_interface.h>

namespace bd::gpu {

// Decode sampler bits from a 6-dword X360 fetch constant (host endianness).
plume::RenderSamplerDesc DecodeFromFetch(const u32 fc[6]);

// Import once at the compatibility boundary, without live renderer policy.
plume::RenderSamplerDesc DecodeSamplerRecipe(const u32 fc[6]);
// Applies host policy to a native recipe; no guest fetch data is read.
plume::RenderSamplerDesc ApplySamplerPolicy(plume::RenderSamplerDesc recipe,
                                          i32 anisotropy, float mip_bias,
                                          bool clamp_volume);

// Returns the cached sampler's slot in sampler_descriptor_set, or 0 (reserved
// default slot) on creation failure or heap full. Caller holds state().mutex (the miss
// path allocates from the bindless sampler heap).
u32 ResolveSlotLocked(const plume::RenderSamplerDesc &desc);

} // namespace bd::gpu
