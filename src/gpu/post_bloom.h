/**
 * @file    post_bloom.h
 * @brief   Native directional bloom parameters, kernel and atlas dependencies.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include <array>
#include <cmath>
#include <cstdint>

namespace bd::gpu {
struct DirectionalBloom {
  float sigma = 1, gain = 1;
  uint32_t iterations = 0;
  bool enabled = false;
};

// Center and six symmetric pairs. The Gaussian prefactor cancels during
// normalization. A zero-width native kernel is an impulse, not 0/0.
inline std::array<float, 8> MakeBloomKernel(float sigma, float gain) {
  std::array<double, 7> weights{1};
  double total = 1;
  if (sigma != 0) {
    const double variance = 2.0 * double(sigma) * double(sigma);
    for (uint32_t i = 1; i < weights.size(); ++i) {
      weights[i] = std::exp(-double(i * i) / variance);
      total += 2.0 * weights[i];
    }
  }
  std::array<float, 8> result{};
  for (uint32_t i = 0; i < weights.size(); ++i)
    result[i] = float(weights[i] * double(gain) / total);
  return result;
}

struct BloomAtlasStep {
  uint32_t input, output, source_half;
};
// The initial bright image occupies only the left half. Later iterations
// read each direction's own half; horizontal and vertical never feed each other.
inline BloomAtlasStep MakeBloomAtlasStep(uint32_t iteration, uint32_t direction) {
  return {iteration & 1u, (iteration + 1u) & 1u, iteration == 0 ? 0u : direction};
}
} // namespace bd::gpu
