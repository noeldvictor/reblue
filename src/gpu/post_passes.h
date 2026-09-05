/**
 * @brief Native post-pass ordering with at most two scratch attachments.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include <array>
#include <cstdint>
namespace bd::gpu {
enum class PostEffect { Adjust, Scanline, Grade };
struct PostStep { PostEffect effect; uint32_t input, output; };
struct PostPasses {
  static constexpr uint32_t kOutput = 2;
  std::array<PostStep, 3> steps{};
  uint32_t count = 0, scratch_count = 0, composite_output = kOutput;
};
inline PostPasses MakePostPasses(bool adjust, bool scanline, bool grade) {
  PostPasses plan;
  const std::array<bool, 3> enabled{adjust, scanline, grade};
  for (uint32_t i = 0; i < enabled.size(); ++i)
    if (enabled[i]) plan.steps[plan.count++].effect = PostEffect(i);
  if (!plan.count) return plan;
  plan.scratch_count = plan.count > 1 ? 2 : 1;
  plan.composite_output = 0;
  for (uint32_t i = 0; i < plan.count; ++i) {
    plan.steps[i].input = i % 2;
    plan.steps[i].output = i + 1 == plan.count ? PostPasses::kOutput : (i + 1) % 2;
  }
  return plan;
}
} // namespace bd::gpu
