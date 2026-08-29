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
#include "generated/reblue_funcs.h"

// Calling the scene render a second time is the whole question behind stereo.
// The per-eye matrices already exist and are unit-tested in xr_math/xr_camera,
// and the per-eye targets are a small change to a present path that already
// owns its output - but none of it is worth anything if the guest cannot draw
// its scene twice in one frame.
//
// bdCameraRender (0x82142D30) alone is the wrong unit. Repeating it adds only
// ~15% more draws, because the first call drains the per-frame render list and
// the second finds little left to walk.
//
// sub_822D3598 is the view driver that contains both of its call sites, and its
// prologue is `mr r31,r3`, so the argument it was handed is the same pointer it
// passes on as the camera - which means the hook can re-enter the driver with
// the argument it already has in hand. That repeats whatever per-view setup
// lives above the render, which is the part that was missing.

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
  {
    // A frame of its own: the call sites read r26/r29 immediately after this,
    // and one of them sits between a matched sub_82173DF8 pair, so the register
    // state the caller resumes with has to be exactly what it left.
    rex::CallFrame frame(rex::ppc::detail::current_ctx());
    rex::ppc::stack_guard guard(frame.ctx);
    frame.ctx.r3.u64 = camera;
    sub_822D3598(frame, rex::ppc::detail::current_base());
  }
  g_in_second_view = false;
}
