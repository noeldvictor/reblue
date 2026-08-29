/**
 * @file    engine/tourist_mode.cpp
 * @brief   Keeps the party alive so the world can be looked at rather than
 *          survived.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license MIT
 */

#include <atomic>

#include <rex/hook.h>
#include <rex/ppc.h>
#include <rex/types.h>

#include "core/logging.h"
#include "engine/character.h"
#include "engine/game.h"
#include "engine/party.h"
#include "engine/tourist_mode.h"

REXCVAR_DECLARE(bool, bd_tourist_mode);

namespace {

// SetHP mirrors the engine's own clamp, so asking for more than the maximum
// lands on the maximum rather than corrupting the field. That is the whole
// trick: no need to find where max HP lives.
constexpr u32 kPlenty = 999999;

} // namespace

// Called once per rendered view, from the camera projection hook.
//
// bdPlayerFieldMovementUpdate (0x82207858) was the obvious seam - the field's
// per-frame movement step - and it never fires: with the player standing still
// there is no movement to update. Driving this from a hook that provably runs
// every frame is the difference between a feature and a plausible one, and the
// camera is set once per view whether or not anybody is walking.
//
// The point is VR sightseeing: standing in a Blue Dragon field and looking at
// it is the reason for the port, and being killed by a wandering monster while
// doing so is not part of that.
namespace bd::engine {

void TouristModeTick() {
  if (!REXCVAR_GET(bd_tourist_mode))
    return;

  auto party = bd::engine::Game::Get().Party();
  if (!party)
    return;

  const size_t n = party.Size();
  for (size_t i = 0; i < n; ++i) {
    auto member = party.At(i);
    if (!member)
      continue;
    // Deliberately absurd: SetHP mirrors the engine's own clamp, so this lands
    // on the member's real maximum without needing to find where that maximum
    // is stored. Verified on desktop - SetHP returns true and a level-1 Shu
    // reads 40 both before and after, which is his maximum.
    member.SetHP(kPlenty);
    member.SetMP(kPlenty);
  }
}

} // namespace bd::engine
