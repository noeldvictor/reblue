/**
 * @file    xr/xr_stereo.cpp
 * @brief   Rendering the guest's scene twice per frame, the groundwork for
 *          stereo.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license MIT
 */

#include <rex/hook.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/types.h>

#include "core/logging.h"

// bdCameraRender, the per-view scene render. r3 is the camera.
//
// Calling this a second time is the whole question behind stereo. The per-eye
// matrices already exist and are unit-tested in xr_math/xr_camera, and the
// per-eye targets are a small change to a present path that already owns its
// output - but none of it is worth anything if the guest cannot draw its scene
// twice in one frame. This answers that, and nothing else.
REX_IMPORT(__imp__bdCameraRender, GuestCameraRender, void(u32));

REXCVAR_DECLARE(bool, bd_stereo_test);

namespace {

// bdCameraRender renders sub-views through the same call sites, so without a
// guard the second render recurses and multiplies rather than doubles.
// Per-thread: the guest renders on its own thread and this must not be visible
// to any other.
thread_local bool g_in_second_view = false;

} // namespace

// Renders the scene a second time from the same camera. The same camera is
// deliberate - this measures whether a second full scene render is possible and
// what it costs, without also introducing a second set of matrices. Two views
// of one eye are wrong on screen and right for the question being asked.
void bdStereoSecondViewHook(PPCRegister &r31) {
  if (!REXCVAR_GET(bd_stereo_test) || g_in_second_view)
    return;

  const u32 camera = r31.u32;
  if (camera == 0)
    return;

  g_in_second_view = true;
  GuestCameraRender(camera);
  g_in_second_view = false;
}
