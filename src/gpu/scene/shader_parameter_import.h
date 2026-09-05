/**
 * @file    shader_parameter_import.h
 * @brief   Temporary engine parameter-copy and dirty-mask publication contract.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include <array>
#include <cstdint>

namespace bd::gpu::scene {
inline bool ParameterRangeSupported(uint32_t first, uint32_t count) {
  return first <= 256 && count <= 256 - first;
}
// Exact bdShaderConstantFlush group mask, including the zero-count case.
// Kept at the boundary; native parameter storage does not use console masks.
inline uint64_t ImportParameterDirtyMask(uint32_t first, uint32_t count) {
  const uint32_t begin = first >> 2;
  const uint32_t distance = ((first + count - 1u) >> 2) - begin;
  const uint32_t shift = (distance & 127) > 63 ? 63 : distance & 127;
  const uint64_t leading = UINT64_MAX << (63 - shift);
  return (begin & 64) ? 0 : leading >> (begin & 127);
}
// The old setter loads a group of four vectors before writing it, followed by
// individual tail vectors. Preserve overlapping-source behavior explicitly;
// neither a whole-range memcpy nor memmove has these sequential semantics.
template <class Load, class Store>
void CopyParameterRows(uint32_t count, Load load, Store store) {
  uint32_t row = 0;
  while (row < count) {
    const uint32_t rows = count - row >= 4 ? 4 : 1;
    std::array<uint32_t, 16> words;
    for (uint32_t i = 0; i < rows * 4; ++i)
      words[i] = load(row * 16 + i * 4);
    for (uint32_t i = 0; i < rows * 4; ++i)
      store(row * 16 + i * 4, words[i]);
    row += rows;
  }
}
} // namespace bd::gpu::scene
