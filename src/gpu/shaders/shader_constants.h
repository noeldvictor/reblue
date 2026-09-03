/**
 * @file    gpu/shaders/shader_constants.h
 * @brief   Spec constant bits, named for host code. The values come from the
 *          recompiler's shader_common.h, which the host HLSL includes too.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/types.h>

#define SHADER_COMMON_SPEC_CONSTANTS_ONLY
#include <shader_common.h>

namespace bd::gpu {
// The three the REBLUE_RECOMP recompiler emits, the rest are Unleashed's.
inline constexpr u32 kSpecConstantR11G11B10Normal =
    SPEC_CONSTANT_R11G11B10_NORMAL;
inline constexpr u32 kSpecConstantAlphaTest = SPEC_CONSTANT_ALPHA_TEST;
// The vertex shader reads its per-node constants from the instance record
// (constant_buffers.h InstanceRecord) instead of the uniform block. Only a
// vertex shader whose cache entry carries this bit in specConstantsMask has
// the redirect; the host builds that shader's instanced twin by setting it.
inline constexpr u32 kSpecConstantInstanced = SPEC_CONSTANT_INSTANCED;
inline constexpr u32 kSpecConstantPulled = SPEC_CONSTANT_PULLED;
// Cel banding on the exported colour; set per draw for skinned nodes when
// bd_cel_characters is on (gpu/hooks/draw.cpp).
inline constexpr u32 kSpecConstantCel = SPEC_CONSTANT_CEL;
} // namespace bd::gpu
