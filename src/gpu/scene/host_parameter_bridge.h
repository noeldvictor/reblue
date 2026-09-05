/**
 * @file    host_parameter_bridge.h
 * @brief   Temporary engine entry point for host float-parameter publication.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include <cstdint>
struct PPCContext;
namespace bd::gpu::scene {
void SetHostFloatParameters(PPCContext &ctx, uint8_t *base, bool vertex);
} // namespace bd::gpu::scene
