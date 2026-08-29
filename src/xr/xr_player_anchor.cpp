/**
 * @file    xr/xr_player_anchor.cpp
 * @brief   Supplies the party leader's transform to the VR camera.
 *
 * bd::xr::Camera has always had SubmitCharacter() and nothing has ever called
 * it, so CharacterAnchor stayed invalid and ThirdPerson and FirstPerson both
 * fell back to the game's own camera position. They ran, they just quietly did
 * not do what their names say - the head orbited the game's camera rather than
 * the character.
 *
 * bdPlayerFieldUpdateMain passes the player object in r3 to
 * bdPlayerFieldMovementUpdate (0x82207858), so the object is easy to reach.
 * Which offset inside it holds the world position is the part the recompiled
 * source cannot simply be read off: generated/reblue_recomp.30.cpp shows two
 * candidate runs of three consecutive floats, 5704 and 5716, plus a five-float
 * run at 7436. One of those is position, the others are plausibly velocity and
 * some interpolation scratch.
 *
 * Rather than guess, bd_vr_player_probe logs all three every second. Walk the
 * character and read the log: the one that tracks movement in world units is
 * the position, and bd_vr_player_pos_offset then points at it - both without a
 * rebuild, because they are cvars and args.txt is read at launch.
 *
 * @copyright Copyright (c) 2026 re:Blue contributors
 * @license   BSD 3-Clause - see LICENSE
 */
#include <chrono>
#include <cmath>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/settings.h" // kCvarGroup
#include "xr/xr_camera.h"
#include "xr/xr_settings.h"

namespace {

// The three runs of consecutive floats bdPlayerFieldMovementUpdate writes into
// the object r3 points at. Exactly one of them is the world position.
constexpr u32 kCandidateOffsets[] = {5704, 5716, 7436};

bool Finite3(f32 x, f32 y, f32 z) {
  return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

} // namespace

REXCVAR_DEFINE_INT32(bd_vr_player_pos_offset, 5716, kCvarGroup,
                     "Byte offset of the party leader's world position inside "
                     "the player object. Feeds the character-anchored VR "
                     "camera modes; 0 disables and falls back to the game's "
                     "own camera. Find it with bd_vr_player_probe.")
    .range(0, 65535);

REXCVAR_DEFINE_BOOL(bd_vr_player_probe, false, kCvarGroup,
                    "Log the candidate player-position offsets once a second. "
                    "Walk around and watch which one tracks movement.");

// Raw, on the inherited context: a typed REX_IMPORT re-roots the guest stack at
// ThreadState's r1 and overwrites the frames live underneath it.
REX_EXTERN(__imp__bdPlayerFieldMovementUpdate);
REX_HOOK_RAW(bdPlayerFieldMovementUpdate) {
  const u32 self = ctx.r3.u32;
  __imp__bdPlayerFieldMovementUpdate(ctx, base);

  if (!self || !bd::xr::Settings::Get().Enabled())
    return;

  using Clock = std::chrono::steady_clock;
  static Clock::time_point last{};
  const auto now = Clock::now();
  const bool due =
      std::chrono::duration<double>(now - last).count() >= 1.0;

  if (REXCVAR_GET(bd_vr_player_probe) && due) {
    last = now;
    for (u32 off : kCandidateOffsets) {
      BD_INFO("[xr] player +{}: ({:.2f}, {:.2f}, {:.2f})", off,
              bd::mem::load<float>(self + off),
              bd::mem::load<float>(self + off + 4),
              bd::mem::load<float>(self + off + 8));
    }
  }

  const i32 offset = REXCVAR_GET(bd_vr_player_pos_offset);
  if (offset <= 0)
    return;

  const f32 x = bd::mem::load<float>(self + static_cast<u32>(offset));
  const f32 y = bd::mem::load<float>(self + static_cast<u32>(offset) + 4);
  const f32 z = bd::mem::load<float>(self + static_cast<u32>(offset) + 8);
  // The guest writes NaN into its own fields during loads and transitions, and
  // the camera's anchor is low-passed, so one bad value would stick for the
  // rest of the session. Rejected here rather than clamped downstream - the
  // same mistake already cost a black headset once.
  if (!Finite3(x, y, z))
    return;

  bd::xr::CharacterAnchor anchor;
  anchor.position = {x, y, z};
  // Facing and eye height still have no source. Left at zero deliberately:
  // ComposeAnchor uses the offset in the character's frame, so a zero yaw means
  // the third-person camera sits behind them in world axes rather than behind
  // them as they turn. Better than no anchor, and honest about what is missing.
  anchor.eyeHeight = 0.0f;
  anchor.facingYaw = 0.0f;
  anchor.valid = true;
  bd::xr::Camera::Get().SubmitCharacter(anchor);
}
