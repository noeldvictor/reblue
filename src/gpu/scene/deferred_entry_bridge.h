/**
 * @file    deferred_entry_bridge.h
 * @brief   Explicit relocation contract for temporary engine entry images.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>

namespace bd::gpu::scene {
constexpr uint32_t kDeferredEntryBytes = 816;
constexpr uint32_t kDeferredMaxEntryBytes = kDeferredEntryBytes + 127 * 4;

inline bool ValidDeferredEntryImage(std::span<const uint8_t> image) {
  if (image.size() < kDeferredEntryBytes ||
      image.size() > kDeferredMaxEntryBytes)
    return false;
  return image.size() == kDeferredEntryBytes +
                             uint32_t(std::max(0, int(int8_t(image[289])))) * 4;
}

// Not a native asset format: the remaining callback/draw consumer requires
// big-endian engine images. Never retain their old self-relative pointers.
inline bool RelocateDeferredEntry(std::span<const uint8_t> image,
                                  std::span<const uint8_t, 64> matrix,
                                  uint32_t destination, uint32_t palette,
                                  std::span<uint8_t> out) {
  if (!ValidDeferredEntryImage(image) || out.size() != image.size() ||
      !destination || (destination & 3) ||
      uint64_t(destination) + image.size() - 1 > UINT32_MAX)
    return false;
  std::array<uint8_t, 64> world;
  std::memcpy(world.data(), matrix.data(), world.size());
  std::memmove(out.data(), image.data(), image.size());
  std::memcpy(out.data() + 16, world.data(), world.size());
  auto word = [&](size_t offset, uint32_t value) {
    for (size_t i = 0; i < 4; ++i)
      out[offset + i] = uint8_t(value >> (24 - i * 8));
  };
  word(264, destination + 388);
  word(268, palette);
  return true;
}
} // namespace bd::gpu::scene
