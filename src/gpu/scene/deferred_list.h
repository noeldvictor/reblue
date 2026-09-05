/**
 * @file    deferred_list.h
 * @brief   Temporary scene-entry bridge for host deferred work.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace bd::gpu::scene {
struct DeferredEntryRecipe;
// The remaining consumer still reads engine entries. Allocation and ordering
// execute on the host; this boundary must disappear with the guest draw loop.
uint32_t AllocateDeferredEntry(uint32_t bytes);
bool CanAppendDeferredEntries(std::span<const DeferredEntryRecipe> entries,
                              std::span<const uint8_t, 64> matrix);
bool AppendDeferredEntries(std::span<const DeferredEntryRecipe> entries,
                           std::span<const uint8_t, 64> matrix,
                           uint32_t palette);
bool OrderDeferredEntries(uint32_t array, int32_t first, int32_t last);
} // namespace bd::gpu::scene
