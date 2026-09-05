/**
 * @file    raster_import.h
 * @brief   Temporary engine raster import and getter-shadow publication ABI.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include "gpu/scene/native_raster.h"
#include <array>
#include <bit>
#include <optional>

namespace bd::gpu::scene {
// Byte offsets and SDK identities never enter RasterState or a native asset.
inline constexpr std::array<uint32_t, 15> kRasterOffsets{
    40, 44, 48, 52, 56, 108, 112, 116, 120, 124, 128, 132, 136, 140, 212};
inline constexpr std::array<uint32_t, 15> kRasterSetters{
    0x82471998, 0x82471A10, 0x824719E0, 0x82471350, 0x82471320,
    0x82471A50, 0x82471A98, 0x82471B08, 0x82471B48, 0x82471B88,
    0x82471AD8, 0x82471C98, 0x82471CB8, 0x82471CD8, 0x82471F70};
inline std::optional<size_t> RasterImportIndex(uint32_t offset) {
  for (size_t i = 0; i < kRasterOffsets.size(); ++i)
    if (kRasterOffsets[i] == offset)
      return i;
  return {};
}
struct RasterImport {
  std::array<uint32_t, 15> words{};
  bool operator==(const RasterImport &) const = default;
};
inline plume::RenderComparisonFunction ImportRasterCompare(uint32_t value) {
  using F = plume::RenderComparisonFunction;
  constexpr std::array table{F::NEVER,         F::LESS,    F::EQUAL,
                             F::LESS_EQUAL,    F::GREATER, F::NOT_EQUAL,
                             F::GREATER_EQUAL, F::ALWAYS};
  return value < table.size() ? table[value] : F::LESS_EQUAL;
}
inline plume::RenderStencilOp ImportRasterStencil(uint32_t value) {
  using S = plume::RenderStencilOp;
  constexpr std::array table{S::KEEP,
                             S::ZERO,
                             S::REPLACE,
                             S::INCREMENT_AND_CLAMP,
                             S::DECREMENT_AND_CLAMP,
                             S::INVERT,
                             S::INCREMENT_AND_WRAP,
                             S::DECREMENT_AND_WRAP};
  return value < table.size() ? table[value] : S::KEEP;
}
inline RasterState DecodeRasterImport(const RasterImport &imported) {
  const auto &v = imported.words;
  RasterState result;
  result.zEnable = v[0] != 0;
  // Preserve the existing uninitialized-getter conventions only at import.
  // Native intent can express NEVER and zero masks without sentinel meanings.
  result.zFunc = v[1] ? ImportRasterCompare(v[1])
                      : plume::RenderComparisonFunction::LESS_EQUAL;
  result.zWriteEnable = !v[1] || v[2] != 0;
  result.fillMode = v[3] == 37 ? plume::RenderFillMode::WIREFRAME
                               : plume::RenderFillMode::SOLID;
  result.cullMode = v[4] == 2   ? plume::RenderCullMode::FRONT
                    : v[4] == 6 ? plume::RenderCullMode::BACK
                                : plume::RenderCullMode::NONE;
  result.stencilEnable = v[5] != 0;
  result.stencilTwoSided = v[6] != 0;
  result.stencilFail = ImportRasterStencil(v[7]);
  result.stencilZFail = ImportRasterStencil(v[8]);
  result.stencilPass = ImportRasterStencil(v[9]);
  result.stencilFunc = ImportRasterCompare(v[10]);
  result.stencilRef = uint8_t(v[11]);
  result.stencilMask = v[12] ? uint8_t(v[12]) : 255;
  result.stencilWriteMask = v[13] ? uint8_t(v[13]) : 255;
  result.colorWriteEnable = v[14] & 15;
  return result;
}

// These are compatibility getter/dirty shadows, not native GPU state. Keep
// untouched bits exact until all remaining engine readers have been removed.
struct RasterShadow {
  uint32_t depth_control = 0, raster_control = 0, color_mask = 0;
  uint32_t stencil_bytes = 0, depth_enable = 0, stencil_enable = 0,
           color_enable = 0;
  uint32_t depth_gate = 0, color_gate = 0;
  uint64_t dirty16 = 0, dirty24 = 0;
  bool operator==(const RasterShadow &) const = default;
};
inline bool PublishRasterShadow(RasterShadow &s, uint32_t offset,
                                uint32_t value) {
  const auto index = RasterImportIndex(offset);
  if (!index)
    return false;
  auto insert = [](uint32_t &word, uint32_t mask, uint32_t input,
                   int rotation) {
    word = (word & ~mask) | (std::rotl(input, rotation) & mask);
  };
  constexpr uint64_t depth_dirty = (uint64_t(1) << 49) | 2048;
  switch (offset) {
  case 40:
    s.depth_enable = value;
    insert(s.depth_control, 2, s.depth_gate ? value : 0, 1);
    s.dirty16 |= depth_dirty;
    break;
  case 44:
    insert(s.depth_control, 0x70, value, 4);
    s.dirty16 |= depth_dirty;
    break;
  case 48:
    insert(s.depth_control, 4, value, 2);
    s.dirty16 |= 2048;
    break;
  case 52:
    insert(s.raster_control, 0x7f8, value, 3);
    s.dirty16 |= 64;
    break;
  case 56:
    insert(s.raster_control, 7, value, 0);
    s.dirty16 |= 64;
    break;
  case 108:
    s.stencil_enable = value;
    insert(s.depth_control, 1, s.depth_gate ? value : 0, 0);
    s.dirty16 |= depth_dirty;
    break;
  case 112:
    insert(s.depth_control, 0x80, value, 7);
    s.dirty16 |= depth_dirty;
    break;
  case 116:
    insert(s.depth_control, 0x3800, value, 11);
    s.dirty16 |= depth_dirty;
    break;
  case 120:
    insert(s.depth_control, 0xe0000, value, 17);
    s.dirty16 |= depth_dirty;
    break;
  case 124:
    insert(s.depth_control, 0x1c000, value, 14);
    s.dirty16 |= 2048;
    break;
  case 128:
    insert(s.depth_control, 0x700, value, 8);
    s.dirty16 |= 2048;
    break;
  case 132:
    insert(s.stencil_bytes, 0xff, value, 0);
    s.dirty24 |= 512;
    break;
  case 136:
    insert(s.stencil_bytes, 0xff00, value, 8);
    s.dirty24 |= 512;
    break;
  case 140:
    insert(s.stencil_bytes, 0xff0000, value, 16);
    s.dirty24 |= 512;
    break;
  case 212:
    s.color_enable = value;
    insert(s.color_mask, 15, s.color_gate ? value : 0, 0);
    s.dirty24 |= 262144;
    break;
  }
  return true;
}
} // namespace bd::gpu::scene
