/**
 * @file    native_skin.h
 * @brief   Explicit per-draw joint identity and transactional palette
 * gathering.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace bd::gpu::scene {
// Model-local joint indices, never guest addresses or identities guessed from
// equal pose matrices. The current import ABI can publish 49 matrices; native
// scene assets and a dedicated GPU palette can lift this adapter limit later.
struct NativeSkinBinding {
  static constexpr size_t kCapacity = 49;
  std::array<uint16_t, kCapacity> joints{};
  uint8_t count = 0;
  bool operator==(const NativeSkinBinding &) const = default;
};

inline std::optional<NativeSkinBinding>
DecodeNativeSkinBinding(std::span<const uint16_t> joints) {
  if (joints.size() > NativeSkinBinding::kCapacity)
    return {};
  NativeSkinBinding binding;
  binding.count = uint8_t(joints.size());
  std::copy(joints.begin(), joints.end(), binding.joints.begin());
  return binding;
}

// Matrix representation and source ownership belong to the caller, not to a
// shader register file. A failed joint load leaves the destination untouched.
template <class Matrix, class LoadJoint, size_t Extent>
bool GatherNativeSkinPalette(const NativeSkinBinding &binding,
                             LoadJoint load_joint,
                             std::span<Matrix, Extent> output) {
  if (binding.count > binding.joints.size() || output.size() < binding.count)
    return false;
  std::array<Matrix, NativeSkinBinding::kCapacity> gathered{};
  for (size_t i = 0; i < binding.count; ++i)
    if (!load_joint(binding.joints[i], gathered[i]))
      return false;
  std::copy_n(gathered.begin(), binding.count, output.begin());
  return true;
}
} // namespace bd::gpu::scene
