/**
 * @file    resource_bridge.h
 * @brief   Temporary engine reference ownership for native pass adapters.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include <cstdint>
namespace bd::gpu {
uint32_t RetainResourceAdapter(uint32_t address);
uint32_t ReleaseResourceAdapter(uint32_t address);
} // namespace bd::gpu
