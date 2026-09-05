/**
 * @file    view_composition_scope.h
 * @brief   Per-view ownership of head-tracked camera composition.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include <array>
#include <cstdint>

namespace bd::xr {
// Source tokens belong to the temporary renderer adapter. Only the camera
// pair explicitly submitted by a view owner may drive head tracking; an
// arbitrary matrix setter is not a camera submission. Reusing the result for
// shadow-volume preparation and scene rendering must not advance smoothing.
class ViewCompositionScope {
public:
  using Matrix = std::array<float, 16>;
  ViewCompositionScope(uint64_t view_source, uint64_t projection_source)
      : view_source_(view_source), projection_source_(projection_source) {}

  template <typename Compose>
  const Matrix *Resolve(uint64_t view_source, uint64_t projection_source,
                        bool enabled, Compose &&compose) {
    if (!enabled || !view_source_ || !projection_source_ ||
        view_source != view_source_ || projection_source != projection_source_)
      return nullptr;
    if (!valid_) {
      Matrix candidate{};
      if (!compose(candidate.data()))
        return nullptr;
      composed_ = candidate;
      valid_ = true;
    }
    return &composed_;
  }

private:
  uint64_t view_source_, projection_source_;
  Matrix composed_{};
  bool valid_ = false;
};
} // namespace bd::xr
