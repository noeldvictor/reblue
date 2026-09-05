/**
 * @file    native_alpha.h
 * @brief   Native cutout/coverage intent, with a shared CPU/shader predicate.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include <cstdint>
#include <shader_common.h>

namespace bd::gpu::scene {
enum class AlphaCompare : uint32_t {
  GreaterEqual = BD_ALPHA_GREATER_EQUAL,
  Never = BD_ALPHA_NEVER,
  Less = BD_ALPHA_LESS,
  Equal = BD_ALPHA_EQUAL,
  LessEqual = BD_ALPHA_LESS_EQUAL,
  Greater = BD_ALPHA_GREATER,
  NotEqual = BD_ALPHA_NOT_EQUAL,
  Always = BD_ALPHA_ALWAYS,
};
struct AlphaState {
  bool enabled = false;
  AlphaCompare compare = AlphaCompare::GreaterEqual;
  float threshold = 0.0f;
  bool alpha_to_coverage = false;
  bool operator==(const AlphaState &) const = default;
};
inline bool AlphaPass(const AlphaState &state, float alpha) {
  return !state.enabled ||
         BD_AlphaPass(uint32_t(state.compare), alpha, state.threshold);
}
// The bound pipeline is a destination, never the source of current intent.
// Suppressing cutouts is an explicit diagnostic, not material policy.
template <class Pipeline>
void ApplyAlphaState(const AlphaState &source, Pipeline &target, bool &dirty,
                     bool multisampled, bool suppress_cutout = false) {
  uint32_t spec = target.specConstants & ~(SPEC_CONSTANT_ALPHA_TEST |
                                           SPEC_CONSTANT_ALPHA_COMPARE_MASK);
  if (source.enabled && !suppress_cutout) {
    spec |= SPEC_CONSTANT_ALPHA_TEST;
    spec |= uint32_t(source.compare) << SPEC_CONSTANT_ALPHA_COMPARE_SHIFT;
  }
  if (target.specConstants != spec) {
    target.specConstants = spec;
    dirty = true;
  }
  const bool coverage = source.alpha_to_coverage && multisampled;
  if (target.enableAlphaToCoverage != coverage) {
    target.enableAlphaToCoverage = coverage;
    dirty = true;
  }
}
} // namespace bd::gpu::scene
