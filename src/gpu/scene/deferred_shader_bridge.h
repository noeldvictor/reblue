/**
 * @file    deferred_shader_bridge.h
 * @brief   Checked temporary shader-ABI packing, not a native asset format.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include <cstdint>
#include <optional>

namespace bd::gpu::scene {
inline std::optional<uint64_t> DeferredConstantMask(uint32_t first,
                                                    uint32_t count) {
  if (first > 256 || count > 256 - first)
    return {};
  uint64_t mask = 0;
  if (count)
    for (uint32_t group = first / 4; group <= (first + count - 1) / 4; ++group)
      mask |= uint64_t(1) << (63 - group);
  return mask;
}

inline std::optional<uint32_t>
DeferredBooleanWord(uint32_t previous, uint32_t bit, uint32_t value) {
  if (bit >= 32)
    return {};
  const auto mask = uint32_t(1) << bit;
  // The engine ABI consumes the low bit, not general C++ truthiness.
  return (previous & ~mask) | ((value & 1) ? mask : 0);
}
} // namespace bd::gpu::scene
