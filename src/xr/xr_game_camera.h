/**
 * @file    xr/xr_game_camera.h
 * @brief   The seam between the headset and Blue Dragon's own camera.
 *
 * The game builds a view matrix per frame in bdBuildViewMatrix, from a camera
 * struct it owns and continues to own: the follow-camera controller reads that
 * matrix back during the concurrent logic phase, so writing head tracking into
 * it in place would feed back into the controller and the camera would chase
 * the player's neck. Instead the hook redirects the callee at a scratch copy,
 * exactly as frame interpolation already does, and this is where that copy is
 * composed.
 *
 * Deliberately free of any OpenXR header. The render layer pushes the eye pose
 * in, so this compiles - and is testable - in every configuration, including
 * the ones with no loader present. Everything it needs is in xr_math.h.
 */
#pragma once

#include <rex/types.h>

#include "xr/xr_math.h"

namespace bd::xr {

// Latches the eye the next scene render is for, in OpenXR space. Called by the
// present path once per eye, before the guest draws.
void SubmitEye(const Pose &openxrEyePose, const Fov &fov);

// Drops the latch at end of frame, so a frame the runtime skipped renders flat
// rather than with a stale pose from whenever the headset last reported.
void ClearEye();

// True when the guest's view matrix should be replaced this frame: VR is on,
// the mode is one of the 3D ones, and an eye pose has been latched. Cinema
// mode returns false - there the game keeps its own camera and the flat image
// goes on a quad.
bool ViewOverrideActive();

// Composes the head-tracked view. gameView is the guest's own matrix, 16
// floats, row-major row-vector, already byte-swapped into host order; out
// receives the replacement in the same layout. Returns false if nothing should
// change, in which case out is untouched.
bool ComposeView(const f32 gameView[16], f32 out[16]);

// The projection the headset wants for the latched eye, same layout. Returns
// false when there is no latched eye.
bool ComposeProjection(f32 out[16]);

// The field of view the guest must render with, pushed in by the XR session.
//
// A projection layer only looks right if the image was drawn with exactly the
// frustum the layer claims it was drawn with. Rather than rewrite the guest's
// projection matrix wholesale - which would fight every one of the four places
// Blue Dragon builds one - the session works out a symmetric frustum matching
// the headset, and the existing bdProjectionAspectHook substitutes its half
// angle and aspect. Same seam bd_fov_offset already uses.
void SetRenderFov(f32 halfVerticalRadians, f32 aspect);

// False when VR is not driving the projection, in which case the guest keeps
// whatever it was going to use.
bool RenderFov(f32 &halfVerticalRadians, f32 &aspect);

} // namespace bd::xr
