/**
 * @file    alpha_import.h
 * @brief   Temporary alpha setter/getter ABI, separate from host alpha policy.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include "gpu/scene/native_alpha.h"
#include <array>
#include <bit>
#include <optional>

namespace bd::gpu::scene {
inline constexpr std::array<uint32_t, 4> kAlphaOffsets{96, 100, 104, 336};
inline constexpr std::array<uint32_t, 4> kAlphaSetters{0x82471380, 0x824717E8,
                                                       0x82471850, 0x82472930};
// Exact float at 0x8200167C used by the SDK AlphaRef setter.
inline constexpr float kAlphaImportScale = std::bit_cast<float>(0x3b808081u);
inline std::optional<size_t> AlphaImportIndex(uint32_t offset) {
  for (size_t i = 0; i < kAlphaOffsets.size(); ++i)
    if (kAlphaOffsets[i] == offset)
      return i;
  return {};
}
struct AlphaImport {
  std::array<uint32_t, 4> words{};
  bool operator==(const AlphaImport &) const = default;
};
inline AlphaState DecodeAlphaImport(const AlphaImport &s) {
  using C = AlphaCompare;
  constexpr std::array compare{C::Never,        C::Less,    C::Equal,
                               C::LessEqual,    C::Greater, C::NotEqual,
                               C::GreaterEqual, C::Always};
  return {.enabled = (s.words[0] & 1) != 0,
          .compare = compare[s.words[2] & 7],
          .threshold = float(s.words[1]) * kAlphaImportScale,
          .alpha_to_coverage = (s.words[3] & 1) != 0};
}
struct AlphaShadow {
  uint32_t control = 0, reference_bits = 0;
  uint64_t dirty16 = 0, dirty24 = 0;
  bool operator==(const AlphaShadow &) const = default;
};
inline bool PublishAlphaShadow(AlphaShadow &s, uint32_t offset,
                               uint32_t value) {
  switch (offset) {
  case 96:
    s.control = (s.control & ~8u) | ((value & 1) << 3);
    s.dirty16 |= 512 | (uint64_t(1) << 50);
    return true;
  case 100:
    s.reference_bits =
        std::bit_cast<uint32_t>(float(value) * kAlphaImportScale);
    s.dirty24 |= 256;
    return true;
  case 104:
    s.control = (s.control & ~7u) | (value & 7);
    s.dirty16 |= 512;
    return true;
  case 336:
    s.control = (s.control & ~16u) | ((value & 1) << 4);
    s.dirty16 |= 512;
    return true;
  default:
    return false;
  }
}
template <class Store>
void WriteAlphaShadow(const AlphaShadow &s, uint32_t offset, Store store) {
  if (offset == 100) {
    store(10372, s.reference_bits);
    store(24, s.dirty24);
  } else {
    store(10428, s.control);
    store(16, s.dirty16);
  }
}
} // namespace bd::gpu::scene
