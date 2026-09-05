/**
 * @file    sampler_import.h
 * @brief   Temporary sampler publication ABI for host-executed producers.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include <array>
#include <bit>
#include <cstdint>
#include <optional>

namespace bd::gpu::scene {
// These are boundary fields, NOT a native material/sampler format. Ordinary
// draws still import fetch state because several material producers write it
// inline. Do not make a live native sampler authoritative until those writers
// have been replaced. Retained native recipes use RenderSamplerDesc already.
enum class SamplerField : uint32_t {
  AddressU, AddressV, AddressW, BorderColor, MagFilter, MinFilter, MipFilter
};
inline constexpr std::array<uint32_t, 7> kSamplerSetters{
    0x82473298, 0x824732E8, 0x82473338, 0x82473228,
    0x82472CF0, 0x82472B68, 0x82472E78};
inline std::optional<SamplerField> SamplerImportField(uint32_t offset) {
  if (offset <= 24 && !(offset & 3))
    return SamplerField(offset / 4);
  return {};
}
inline constexpr uint32_t SamplerOffset(SamplerField field) {
  return uint32_t(field) * 4;
}
struct SamplerShadow {
  std::array<uint32_t, 6> fetch{};
  uint64_t dirty = 0;
  uint32_t anisotropy_lookup = 0;
  uint8_t z_filter = 0;
  bool operator==(const SamplerShadow &) const = default;
};
inline bool PublishSamplerShadow(SamplerShadow &s, uint32_t slot,
                                 SamplerField field, uint32_t value) {
  if (slot >= 32 || uint32_t(field) >= kSamplerSetters.size())
    return false;
  auto bits = [](uint32_t &word, uint32_t mask, uint32_t value) {
    word = (word & ~mask) | (value & mask);
  };
  switch (field) {
  case SamplerField::AddressU: bits(s.fetch[0], 0x1c00, value << 10); break;
  case SamplerField::AddressV: bits(s.fetch[0], 0xe000, value << 13); break;
  case SamplerField::AddressW: bits(s.fetch[0], 0x70000, value << 16); break;
  case SamplerField::BorderColor:
    bits(s.fetch[5], 3, value != 0);
    break;
  case SamplerField::MipFilter: bits(s.fetch[3], 0x1800000, value << 23); break;
  case SamplerField::MinFilter:
  case SamplerField::MagFilter: {
    const bool min = field == SamplerField::MinFilter;
    const unsigned own = min ? 11 : 10, other = min ? 10 : 11;
    const unsigned shift = min ? 21 : 19;
    const auto requested = value >> 2;
    const auto combined_aniso = ((s.fetch[4] >> other) & 1) | requested;
    const auto lookup = s.anisotropy_lookup & ~(combined_aniso - 1u);
    bits(s.fetch[4], 1u << own, requested << own);
    const auto combined = (lookup << (min ? 4 : 6)) | requested | value;
    bits(s.fetch[3], (3u << shift) | 0xe000000, combined << shift);
    // Separate Z filtering shares packed min/mag state in the old getter ABI.
    // Preserve it even though the native sampler consumer does not use it.
    const auto rotated = std::rotl(s.fetch[3], 31);
    const auto packed = (s.fetch[3] & ~0x7ff7ffffu) | (rotated & 0x7ff7ffffu);
    const auto z_fields = std::rotl(packed, 13) & 0xfff;
    const auto z_mask = (uint32_t(s.z_filter) >> 2) - 1u;
    bits(s.fetch[4], 3, (z_fields & z_mask) + (uint32_t(s.z_filter) & ~z_mask));
    break;
  }
  }
  s.dirty |= uint64_t(1) << (43 - slot);
  return true;
}
template <class Store>
void WriteSamplerShadow(const SamplerShadow &s, uint32_t slot,
                        SamplerField field, Store store) {
  const auto at = 1024 + slot * 24;
  switch (field) {
  case SamplerField::AddressU:
  case SamplerField::AddressV:
  case SamplerField::AddressW: store(at, s.fetch[0]); break;
  case SamplerField::BorderColor: store(at + 20, s.fetch[5]); break;
  case SamplerField::MinFilter:
  case SamplerField::MagFilter:
    store(at + 16, s.fetch[4]);
    [[fallthrough]];
  case SamplerField::MipFilter: store(at + 12, s.fetch[3]); break;
  }
  store(16, s.dirty);
}
struct SamplerCommand {
  uint32_t slot;
  SamplerField field;
  uint32_t value;
};
inline uint32_t ImportSceneFilterSetting(uint32_t setting) {
  switch (setting) {
  case 1: return 0;
  case 2: return 4;
  case 3: return 2;
  default: return 1;
  }
}
// Complete sub_82184A88 plan, in source order. Only the first five slots and
// these five fields are reset; W, border, and all other fields inherit.
inline std::array<SamplerCommand, 25> SceneSamplerDefaults(
    uint32_t min_setting, uint32_t mag_setting, uint32_t mip_setting) {
  std::array<SamplerCommand, 25> plan{};
  for (uint32_t slot = 0; slot < 5; ++slot) {
    plan[slot * 5] = {slot, SamplerField::MinFilter,
                     slot ? 1 : ImportSceneFilterSetting(min_setting)};
    plan[slot * 5 + 1] = {slot, SamplerField::MagFilter,
                         slot ? 1 : ImportSceneFilterSetting(mag_setting)};
    plan[slot * 5 + 2] = {slot, SamplerField::MipFilter,
                         slot ? 1 : ImportSceneFilterSetting(mip_setting)};
    plan[slot * 5 + 3] = {slot, SamplerField::AddressU, 0};
    plan[slot * 5 + 4] = {slot, SamplerField::AddressV, 0};
  }
  return plan;
}
} // namespace bd::gpu::scene
