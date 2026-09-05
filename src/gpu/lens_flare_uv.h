/**
 * @file    lens_flare_uv.h
 * @brief   Shared C++/HLSL sampling of authored quarter-image optical sprites.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#ifdef __cplusplus
#include <cmath>
namespace bd::gpu {
using std::abs;
#endif

// The original fan's center maps to (0,1), its horizontal edge midpoints to
// (1,1), and its vertical edge midpoints to (0,0). Folding per pixel preserves
// its four linear quadrants with just two triangles and no duplicated assets.
inline float LensFlareU(float x) { return abs(2.0f * x - 1.0f); }
inline float LensFlareV(float y) { return 1.0f - abs(2.0f * y - 1.0f); }

#ifdef __cplusplus
} // namespace bd::gpu
#endif
