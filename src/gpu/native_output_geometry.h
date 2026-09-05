/**
 * @file    native_output_geometry.h
 * @brief   SDK-independent native eye extents and authored canvas fitting.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

namespace bd::gpu {
struct OutputExtent {
  uint32_t width = 0, height = 0;
  bool operator==(const OutputExtent &) const = default;
};

// The scene and runtime image use exactly the same pixel policy. No design
// aspect fit, packed-eye squeeze or console buffer alignment belongs here.
inline std::optional<OutputExtent> ScaleEyeExtent(OutputExtent recommended, double scale) {
  if (!recommended.width || !recommended.height || !std::isfinite(scale))
    return std::nullopt;
  scale = std::clamp(scale, 0.05, 2.0);
  const double width = std::floor(double(recommended.width) * scale + 0.5);
  const double height = std::floor(double(recommended.height) * scale + 0.5);
  if (width > UINT32_MAX || height > UINT32_MAX)
    return std::nullopt;
  return OutputExtent{std::max(64u, uint32_t(width)), std::max(64u, uint32_t(height))};
}

// UI coordinates remain authored against a separate canvas. Equal pixel
// density on X/Y preserves text and icons inside a differently shaped eye.
inline std::array<float, 2> DesignCanvasScale(double aspect, double design, double epsilon) {
  if (!std::isfinite(aspect) || !std::isfinite(design) || aspect <= 0 || design <= 0 ||
      std::fabs(aspect - design) <= epsilon)
    return {1.0f, 1.0f};
  return aspect > design ? std::array<float, 2>{float(design / aspect), 1.0f}
                         : std::array<float, 2>{1.0f, float(aspect / design)};
}

inline bool FullEyeViewport(bool eye_sized, bool layered, bool projection, bool movie) {
  return eye_sized && layered && projection && !movie;
}
} // namespace bd::gpu
