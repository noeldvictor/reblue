/**
 * @file    draw_verify.h
 * @brief   Bounded, recurring diagnostics for native draw verification.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace bd::gpu::scene {
enum class DrawVerifyKind : size_t { Registers, Textures, Booleans, Structure, Count };

// Give each view and failure kind its own allowance. Startup noise cannot
// consume a later scene's examples, and a common texture difference cannot
// suppress a rare geometry failure. Only logging uses this budget.
struct DrawVerifyLogBudget {
  uint32_t window = UINT32_MAX;
  std::array<std::array<uint8_t, size_t(DrawVerifyKind::Count)>, 17> used{};

  bool Take(uint32_t frame, uint32_t view, DrawVerifyKind kind) {
    const auto next = frame / 300;
    if (window != next) {
      window = next;
      used = {};
    }
    auto &count = used[view < 16 ? view : 16][size_t(kind)];
    if (count >= 2)
      return false;
    ++count;
    return true;
  }
};

// Metadata is a declaration mask, not proof that a branch executed or that
// undeclared registers are irrelevant. Keep the full comparison independently.
inline bool DeclaresDrawRegister(const uint32_t *mask, uint32_t reg) {
  return mask && reg < 256 && ((mask[reg / 32] >> (reg % 32)) & 1u);
}

inline bool DrawVerificationNodeWrong(uint32_t wrong_before, uint32_t wrong_after,
                                      size_t expected, size_t actual) {
  return wrong_before != wrong_after || expected != actual;
}
} // namespace bd::gpu::scene
