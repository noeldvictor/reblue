/**
 * @file    blend_import.h
 * @brief   Temporary engine blend setter/getter ABI; never a native asset.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include "gpu/scene/native_blend.h"
#include <array>
#include <cstdint>
#include <optional>

namespace bd::gpu::scene {
// Dispatch table 0x82751D68: enable, separate, RGB src/dst/op, alpha
// src/dst/op. Offset 68 is blend constant, not an operation; its producer
// remains separate.
inline constexpr std::array<uint32_t, 8> kBlendOffsets{60, 64, 72, 76,
                                                       80, 84, 88, 92};
inline constexpr std::array<uint32_t, 8> kBlendSetters{
    0x824713C0, 0x82471750, 0x824714E0, 0x82471570,
    0x82471450, 0x82471670, 0x824716E0, 0x82471600};
inline constexpr std::array<uint32_t, 4> kBlendWordOffsets{10424, 10456, 10460,
                                                           10464};
inline std::optional<size_t> BlendImportIndex(uint32_t offset) {
  for (size_t i = 0; i < kBlendOffsets.size(); ++i)
    if (kBlendOffsets[i] == offset)
      return i;
  return {};
}
inline bool SupportedBlendFactor(uint32_t value) {
  return value <= 1 || (value >= 4 && value <= 11) || value == 16;
}
inline plume::RenderBlend ImportBlendFactor(uint32_t value) {
  using F = plume::RenderBlend;
  switch (value) {
  case 1:
    return F::ONE;
  case 4:
    return F::SRC_COLOR;
  case 5:
    return F::INV_SRC_COLOR;
  case 6:
    return F::SRC_ALPHA;
  case 7:
    return F::INV_SRC_ALPHA;
  case 8:
    return F::DEST_COLOR;
  case 9:
    return F::INV_DEST_COLOR;
  case 10:
    return F::DEST_ALPHA;
  case 11:
    return F::INV_DEST_ALPHA;
  case 16:
    return F::SRC_ALPHA_SAT;
  default:
    return F::ZERO; // Existing converter fallback, not constant support.
  }
}
inline plume::RenderBlendOperation ImportBlendOperation(uint32_t value) {
  using O = plume::RenderBlendOperation;
  constexpr std::array table{O::ADD, O::SUBTRACT, O::MIN, O::MAX,
                             O::REV_SUBTRACT};
  return value < table.size() ? table[value] : O::ADD;
}
inline bool SupportedBlendWord(uint32_t word) {
  return SupportedBlendFactor(word & 31) &&
         SupportedBlendFactor((word >> 8) & 31) &&
         SupportedBlendFactor((word >> 16) & 31) &&
         SupportedBlendFactor((word >> 24) & 31) && ((word >> 5) & 7) <= 4 &&
         ((word >> 21) & 7) <= 4;
}
inline BlendState DecodeBlendImport(uint32_t word, uint32_t flags) {
  return {.alphaBlendEnable = (flags & 0x80000000u) != 0,
          .srcBlend = ImportBlendFactor(word & 31),
          .destBlend = ImportBlendFactor((word >> 8) & 31),
          .blendOp = ImportBlendOperation((word >> 5) & 7),
          .srcBlendAlpha = ImportBlendFactor((word >> 16) & 31),
          .destBlendAlpha = ImportBlendFactor((word >> 24) & 31),
          .blendOpAlpha = ImportBlendOperation((word >> 21) & 7)};
}

struct BlendShadow {
  uint32_t requested = 0, flags = 0;
  std::array<uint32_t, 4> effective{0x10001, 0x10001, 0x10001, 0x10001};
  uint64_t dirty16 = 0;
  bool operator==(const BlendShadow &) const = default;
};
inline uint32_t FoldBlendAlpha(uint32_t requested) {
  const uint32_t rgb = requested & 0xffff;
  // SDK shared-alpha rule includes SRC_ALPHA_SAT -> ONE in the alpha lane.
  return rgb | ((((rgb << 4) | (rgb & 0x1010)) << 12) & 0xefef0000u);
}
inline bool PublishBlendShadow(BlendShadow &s, uint32_t offset,
                               uint32_t value) {
  const auto index = BlendImportIndex(offset);
  if (!index)
    return false; // Refuse before effects.
  uint32_t effective = s.requested;
  if (offset == 60) {
    s.flags = (s.flags & 0x7fffffffu) | ((value & 1) << 31);
    effective =
        (s.flags & 0x40000000u) ? s.requested : FoldBlendAlpha(s.requested);
    if (value == 0)
      effective = 0x10001;
  } else if (offset == 64) {
    s.flags = (s.flags & 0xbfffffffu) | ((value & 1) << 30);
    effective = value ? s.requested : FoldBlendAlpha(s.requested);
    if (!(s.flags & 0x80000000u))
      effective = 0x10001;
  } else {
    constexpr std::array<unsigned, 6> shifts{0, 8, 5, 16, 24, 21};
    const auto shift = shifts[*index - 2];
    const uint32_t mask = (offset == 80 || offset == 92 ? 7u : 31u) << shift;
    s.requested = (s.requested & ~mask) | ((value << shift) & mask);
    if (!(s.flags & 0x80000000u) || (offset >= 84 && !(s.flags & 0x40000000u)))
      return true; // Requested values survive while their output is gated.
    effective =
        (s.flags & 0x40000000u) ? s.requested : FoldBlendAlpha(s.requested);
  }
  s.effective.fill(effective);
  s.dirty16 |= 0x407;
  return true;
}

// Only locations that changed are published; unrelated flag bits/dirty marks
// come from the live boundary, while blend inputs come from the host shadow.
template <class Store>
void WriteBlendChanges(const BlendShadow &before, const BlendShadow &after,
                       Store store) {
  if (before.requested != after.requested)
    store(11576, after.requested);
  if (before.flags != after.flags)
    store(11580, after.flags);
  for (size_t i = 0; i < after.effective.size(); ++i)
    if (before.effective[i] != after.effective[i])
      store(kBlendWordOffsets[i], after.effective[i]);
  if (before.dirty16 != after.dirty16)
    store(16, after.dirty16);
}
} // namespace bd::gpu::scene
