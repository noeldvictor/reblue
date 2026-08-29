/**
 * @file    xr/xr_camera_capture.h
 * @brief   The guest's own projection matrix, captured where it is set.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license MIT
 */

#pragma once

#include <rex/types.h>

namespace bd::xr {

// The guest's projection, in the engine's own layout: 4 rows of 4 floats,
// right-handed with -Z forward and the perspective divide at [2][3]. An
// off-centre per-eye frustum is [2][0].
struct GuestProjection {
  float m[16]{};
  u32 va = 0;
};

// False until the engine has set a projection at least once.
bool LastGuestProjection(GuestProjection &out);

void StoreGuestProjection(const GuestProjection &p);

} // namespace bd::xr
