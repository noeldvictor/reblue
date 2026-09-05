/**
 * @file    output_geometry.cpp
 * @brief   Native full-eye extent and independent authored-canvas checks.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/native_output_geometry.h"
#include <limits>
#include <stdexcept>
using namespace bd::gpu;
namespace {
void Require(bool value) {
  if (!value) throw std::runtime_error("native output geometry check failed");
}
}
int main() {
  Require(ScaleEyeExtent({1440, 1584}, 1.0) == OutputExtent{1440, 1584});
  Require(ScaleEyeExtent({1440, 1584}, 0.65) == OutputExtent{936, 1030});
  Require(ScaleEyeExtent({1024, 1024}, 1.0) == OutputExtent{1024, 1024});
  Require(ScaleEyeExtent({1441, 1585}, 1.0) == OutputExtent{1441, 1585});
  Require(ScaleEyeExtent({2000, 1000}, 0) == OutputExtent{100, 64});
  Require(ScaleEyeExtent({200, 100}, 3) == OutputExtent{400, 200});
  Require(!ScaleEyeExtent({0, 1584}, 1.0));
  Require(!ScaleEyeExtent({1440, 0}, 1.0));
  Require(!ScaleEyeExtent({1440, 1584}, std::numeric_limits<double>::quiet_NaN()));
  Require(!ScaleEyeExtent({1440, 1584}, std::numeric_limits<double>::infinity()));
  Require(!ScaleEyeExtent({UINT32_MAX, 1584}, 2.0));
  Require(!ScaleEyeExtent({1440, UINT32_MAX}, 2.0));
  Require(ScaleEyeExtent({UINT32_MAX, UINT32_MAX}, 1.0) ==
          OutputExtent{UINT32_MAX, UINT32_MAX});
  constexpr double design = 1280.0 / 720.0;
  for (const OutputExtent extent : {OutputExtent{1440, 1584}, {936, 1030}, {1024, 1024},
                                   {1920, 1080}, {2560, 1080}}) {
    const auto scale = DesignCanvasScale(double(extent.width) / extent.height, design, 0.01);
    const double x_density = extent.width * double(scale[0]) / 1280.0;
    const double y_density = extent.height * double(scale[1]) / 720.0;
    Require(std::fabs(x_density - y_density) < 0.000001);
    Require(std::max(scale[0], scale[1]) == 1.0f);
  }
  Require(DesignCanvasScale(design + 0.001, design, 0.01) == std::array<float, 2>{1, 1});
  Require(DesignCanvasScale(0, design, 0.01) == std::array<float, 2>{1, 1});
  for (uint32_t bits = 0; bits < 16; ++bits) {
    const bool eye = bits & 1, layered = bits & 2, projection = bits & 4, movie = bits & 8;
    Require(FullEyeViewport(eye, layered, projection, movie) == (bits == 7));
  }
  return 0;
}
