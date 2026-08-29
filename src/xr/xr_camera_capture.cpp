/**
 * @file    xr/xr_camera_capture.cpp
 * @brief   Captures the guest's projection matrix where the engine sets it.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license MIT
 */

#include <atomic>
#include <cstring>
#include <mutex>

#include <rex/hook.h>
#include <rex/ppc.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "xr/xr_camera_capture.h"

namespace bd::xr {

namespace {

std::mutex g_mutex;
GuestProjection g_proj{};
bool g_have = false;

} // namespace

bool LastGuestProjection(GuestProjection &out) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_have)
    return false;
  out = g_proj;
  return true;
}

void StoreGuestProjection(const GuestProjection &p) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_proj = p;
  g_have = true;
}

} // namespace bd::xr

// bdCameraViewSetProjMatrix (0x821351A8), r5 = the source matrix. Its sibling
// bdCameraViewSetMatrix (0x82135128) carries the view; both are named in
// config/functions.toml and are called in turn by bdCameraViewSetMatrices.
//
// Read out of a live field scene, this is:
//
//   [ 2.41421  0        0         0     ]
//   [ 0        4.29203  0         0     ]
//   [ 0        0       -1.00005  -1.0   ]
//   [ 0        0       -1.00005   0     ]
//
// which is an ordinary perspective projection: 2.41421 is cot(22.5), so a 45
// degree horizontal field of view; 4.29203 is that times 16/9; and the -1 at
// [2][3] is the perspective divide taken from -Z, so right-handed with -Z
// forward - the same convention OpenXR uses, which is a piece of luck.
//
// An off-centre per-eye frustum is then a single term: [2][0] shifts the
// frustum horizontally without moving the eye, and is what xr_math's per-eye
// projection produces. That is the principled replacement for the clip-space
// skew in DispatchDraw, which shears whatever transform happens to sit in VS
// register 32.
//
// bd::mem::try_load already returns host order. Byte-swapping it again is what
// made this look like uninitialised garbage for three attempts.
void bdCameraProjMatrixHook(PPCRegister &r5) {
  const u32 va = r5.u32;
  if (!va)
    return;

  bd::xr::GuestProjection p{};
  p.va = va;
  for (int i = 0; i < 16; ++i) {
    const u32 bits = bd::mem::try_load<u32>(va + u32(i) * 4);
    std::memcpy(&p.m[i], &bits, sizeof(float));
  }
  bd::xr::StoreGuestProjection(p);

  static std::atomic<int> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) == 400) {
    BD_INFO("[proj] guest projection at 0x{:08X}", va);
    for (int r = 0; r < 4; ++r)
      BD_INFO("[proj]   [{: .5f} {: .5f} {: .5f} {: .5f}]", p.m[r * 4 + 0],
              p.m[r * 4 + 1], p.m[r * 4 + 2], p.m[r * 4 + 3]);
  }
}
