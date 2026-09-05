/**
 * @file    gpu/post_chain.h
 * @brief   Native DoF/bloom/flare passes and explicit combined output, with counted
 *          remaining effect, scheduling and image adapters.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once
#include <rex/types.h>
#include <array>

namespace bd::gpu {

struct VideoState;
struct GuestTexture;
struct DofParameters;
struct BloomParameters;
struct LensFlareParameters;
struct PostAdjustments;
struct ScanlineParameters;
struct GradeParameters;
struct HeatShimmerParameters;

// Whole native atlas + folded bloom/composite + instanced optical sprites.
// The caller supplies an explicit attachment and native optical image mirrors.
// No draw interception, shader-register import or tile allocation. False is
// a preflight refusal; failures after GPU work starts are fatal, not replayed.
bool HostPostRender(GuestTexture *scene, GuestTexture *depth, GuestTexture *output,
                    const DofParameters &dof, const BloomParameters &bloom,
                    const LensFlareParameters &flare,
                    const std::array<GuestTexture *, 4> &flare_images,
                    const PostAdjustments &adjustments, const ScanlineParameters &scanline,
                    const GradeParameters &grade, GuestTexture *grain_image,
                    const HeatShimmerParameters &heat, GuestTexture *heat_image);

// Authored-property adapters shared by direct scheduling and the transitional
// DoF entry pair. No original rendering code executes in these functions.
bool ReadDofProducerParameters(u32 owner, DofParameters &parameters);
void PublishDofProducerProperties(u32 owner);

// Diagnostic-only comparison at the original combined draw boundary.
void VerifyNativePostParameters(const BloomParameters &parameters);

// Direct native producer. Resolves no D3D slots or shader-register constants;
// the caller supplies current scene/depth images and authored native values.
// Returns false before committing preparation if this path is unavailable.
bool HostPostPrepareDof(GuestTexture *scene, GuestTexture *depth,
                        const DofParameters &parameters);

// Called from the draw path once the framebuffer for a guest draw is bound and
// before its state is flushed, with the pixel shader hash the draw would use.
// Returns true when the host consumes the draw. The normal DoF producer uses
// HostPostPrepareDof directly and never issues a guest DoF draw. This intercept
// handles explicit compatibility scopes; native post scheduling does not enter it.
bool HostPostIntercept(VideoState &s, u64 ps_hash, u32 device_guest);

// The producer half of the intercept, asked BEFORE the draw binds its
// framebuffer: a guest quoter/ms_weight/brightpass draw into a pyramid level
// is dropped here, so the level is never bound - binding a fresh target
// seeds it from its predecessor (a full-surface copy), and those seeds were
// ten of the frame's fourteen (2026-09-02).
bool HostPostProducerSkip(VideoState &s, u64 ps_hash);

// True for a guest draw the host chain replaces with a full-target write (the
// ms_tex composite into the frame), so its bind need not seed the target.
bool HostPostOverwritesTarget(VideoState &s, u64 ps_hash);

// True for a texture the host chain wrote every pixel of last frame and will
// again this frame (a dof pyramid level, the bloom mask): a guest resolve into
// it is a copy the host overwrites, so the resolve is skipped outright.
bool HostPostWillOverwrite(const GuestTexture *dst);

// True once the host composite has run: every guest post draw is being
// dropped, so a downscaled resolve of the scene (the input of the guest's
// first quarter pass) has no reader - the host pyramid samples the full-res
// scene texture itself.
bool HostPostActive();

// True for a guest draw the host chain will take at the intercept (the dof
// and ms_tex composites) - such a draw never samples its textures itself.
bool HostPostWillIntercept(u64 ps_hash);

} // namespace bd::gpu
