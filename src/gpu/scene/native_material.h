/**
 * @file    gpu/scene/native_material.h
 * @brief   Temporary guest asset boundary for native material properties.
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include <optional>
#include "gpu/scene/native_material_data.h"
#include "gpu/scene/node_tag.h"

namespace bd::gpu::scene {
std::optional<NativeMaterialProperties> ImportNativeMaterial(
    const NodeTag &tag, uint32_t index_va, uint32_t stream_va,
    uint32_t first_index, uint32_t index_count);
uint32_t EvaluateNativeMaterial(const NodeTag &tag,
                               const NativeMaterialProperties &material,
                               std::array<float, 4> values[3]);
void NativeMaterialCheck(uint32_t mask, const std::array<float, 4> values[3],
                         const uint8_t *pixel_constants);
void NativeMaterialNoteReplay(uint32_t mask);
} // namespace bd::gpu::scene
