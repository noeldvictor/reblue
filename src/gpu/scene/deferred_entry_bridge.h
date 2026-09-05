/**
 * @file    deferred_entry_bridge.h
 * @brief   Explicit relocation contract for temporary engine entry images.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include "gpu/scene/deferred_depth.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace bd::gpu::scene {
constexpr uint32_t kDeferredEntryBytes = 816;
constexpr uint32_t kDeferredMaxEntryBytes = kDeferredEntryBytes + 127 * 4;

// Native depth is owned with its corresponding entry, not in an independently
// indexed side array. The rest of the entry remains an explicit compatibility
// image until material/pass and draw producers are replaced.
struct DeferredEntryRecipe {
  std::vector<uint8_t> compatibility_image;
  std::optional<DeferredDepthRecipe> depth;
};

inline bool EvaluateDeferredEntryDepths(
    std::span<const DeferredEntryRecipe> entries, const DeferredMatrix &world,
    const DeferredMatrix &view, std::vector<float> &depths) {
  std::vector<float> next;
  next.reserve(entries.size());
  for (const auto &entry : entries) {
    if (!entry.depth)
      return false;
    const auto depth = EvaluateDeferredDepth(*entry.depth, world, view);
    if (!depth)
      return false;
    next.push_back(*depth);
  }
  depths = std::move(next);
  return true;
}

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
                                  std::span<uint8_t> out,
                                  std::optional<float> depth = {}) {
  if (!ValidDeferredEntryImage(image) || out.size() != image.size() ||
      !destination || (destination & 3) ||
      uint64_t(destination) + image.size() - 1 > UINT32_MAX ||
      (depth && !std::isfinite(*depth)))
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
  if (depth)
    word(276, std::bit_cast<uint32_t>(*depth));
  return true;
}
} // namespace bd::gpu::scene
