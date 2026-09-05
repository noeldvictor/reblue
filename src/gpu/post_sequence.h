/**
 * @file    post_sequence.h
 * @brief   Bounded native post-root ordering and non-aliasing output roles.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include <cstdint>
#include <optional>

namespace bd::gpu {
struct PostSequence {
  static constexpr uint32_t kCapacity = 64;
  uint32_t count = 0;
  uint32_t target_count = 0;
  // Stage zero samples the incoming scene. Each later stage samples its
  // predecessor's completed output, never the target it is about to write.
  constexpr uint32_t Output(uint32_t stage) const { return stage % 2; }
};
constexpr std::optional<PostSequence> MakePostSequence(uint32_t count) {
  if (count > PostSequence::kCapacity) return {};
  return PostSequence{count, count < 2 ? count : 2};
}
} // namespace bd::gpu
