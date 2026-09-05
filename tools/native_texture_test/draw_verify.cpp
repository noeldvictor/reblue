/**
 * @file    draw_verify.cpp
 * @brief   Recurring diagnostics and complete draw-count accounting.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "gpu/scene/draw_verify.h"
#include <cassert>
#include <iostream>

int main() {
  using namespace bd::gpu::scene;
  DrawVerifyLogBudget budget;
  using Kind = DrawVerifyKind;
  assert(budget.Take(0, 1, Kind::Registers));
  assert(budget.Take(1, 1, Kind::Registers));
  for (int i = 0; i < 1000; ++i)
    assert(!budget.Take(299, 1, Kind::Registers));
  assert(budget.Take(299, 2, Kind::Registers));
  assert(budget.Take(299, 1, Kind::Structure));
  assert(budget.Take(300, 1, Kind::Registers));
  assert(budget.Take(301, 1, Kind::Registers));
  assert(!budget.Take(599, 1, Kind::Registers));
  assert(budget.Take(15000, 1, Kind::Registers)); // later scene
  assert(budget.Take(15000, UINT32_MAX, Kind::Registers));
  assert(budget.Take(15000, 16, Kind::Registers));
  assert(!budget.Take(15000, 100, Kind::Registers));
  uint32_t mask[8]{};
  for (uint32_t reg : {0u, 31u, 32u, 60u, 255u})
    mask[reg / 32] |= 1u << (reg % 32);
  for (uint32_t reg : {0u, 31u, 32u, 60u, 255u})
    assert(DeclaresDrawRegister(mask, reg));
  for (uint32_t reg : {1u, 61u, 256u, UINT32_MAX})
    assert(!DeclaresDrawRegister(mask, reg));
  assert(!DeclaresDrawRegister(nullptr, 0));
  assert(!DrawVerificationNodeWrong(7, 7, 2, 2));
  assert(DrawVerificationNodeWrong(7, 8, 2, 2));
  assert(DrawVerificationNodeWrong(7, 7, 2, 1)); // fewer, even no draw mismatch
  assert(DrawVerificationNodeWrong(7, 7, 2, 3));
  assert(DrawVerificationNodeWrong(7, 7, 2, 0));
  std::cout << "Draw verification diagnostics passed\n";
}
