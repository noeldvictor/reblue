/**
 * @file    deferred_depth_import.h
 * @brief   Temporary engine-data boundary for native deferred depth.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include "gpu/scene/deferred_depth.h"
#include <cstdint>
#include <span>
#include <vector>

namespace bd::gpu::scene {
struct DeferredEntryRecipe;
// Called at the original producer boundary, with its explicit fixed selector.
std::optional<float> ImportDeferredDepth(uint32_t entry, uint32_t matrix,
                                         uint32_t mesh, bool fixed);
std::optional<DeferredDepthRecipe> CapturedDeferredDepth(uint32_t entry);
void ResetDeferredDepthImports();
void VerifyDeferredDepth(uint32_t entry, float expected);
bool PublishDeferredDepth(uint32_t entry, float depth);
void RecordDeferredDepthFallback();
bool CopyDeferredMatrix(uint32_t address, std::array<uint8_t, 64> &image);
// All results are computed before any entry is published. Unknown policy or
// invalid frame inputs refuse the whole batch rather than using old depth.
bool ComposeDeferredDepths(std::span<const DeferredEntryRecipe> entries,
                           std::span<const uint8_t, 64> matrix,
                           std::vector<float> &depths);
// Count only successfully published replay, never speculative preflight work.
void RecordDeferredDepthReplay(std::span<const DeferredEntryRecipe> entries,
                               std::span<const float> depths);
} // namespace bd::gpu::scene
