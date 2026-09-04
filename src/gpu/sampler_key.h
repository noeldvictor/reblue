/**
 * @file    sampler_key.h
 * @brief   Complete, padding-independent native sampler identity.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license   BSD 3-Clause License
 */
#pragma once
#include <array>
#include <bit>
#include <cstdint>
#include <plume_render_interface_types.h>

namespace bd::gpu {
struct SamplerKey {
  std::array<uint32_t, 15> words;
  explicit SamplerKey(const plume::RenderSamplerDesc &d) : words{
      uint32_t(d.minFilter), uint32_t(d.magFilter), uint32_t(d.mipmapMode),
      uint32_t(d.addressU), uint32_t(d.addressV), uint32_t(d.addressW),
      FloatBits(d.mipLODBias), d.maxAnisotropy, uint32_t(d.anisotropyEnabled),
      uint32_t(d.comparisonFunc), uint32_t(d.comparisonEnabled),
      uint32_t(d.borderColor), FloatBits(d.minLOD), FloatBits(d.maxLOD),
      uint32_t(d.shaderVisibility)} {}
  bool operator==(const SamplerKey &) const = default;
  static uint32_t FloatBits(float f) {
    return f == 0.0f ? 0u : std::bit_cast<uint32_t>(f);
  }
};
struct SamplerKeyHash {
  size_t operator()(const SamplerKey &k) const noexcept {
    size_t h = 0;
    for (uint32_t word : k.words)
      h ^= size_t(word) + size_t(0x9e3779b9u) + (h << 6) + (h >> 2);
    return h;
  }
};
} // namespace bd::gpu
