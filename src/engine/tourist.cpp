/**
 * @file    engine/tourist.cpp
 * @brief   Tourist mode: sightseeing in VR without the game fighting back.
 *
 * Standing inside Blue Dragon's world is the point of the fork, and a random
 * encounter every forty seconds is the biggest obstacle to it. It is also a
 * measurement problem: `bd_xr_autoplay` walks, walking rolls encounters, and a
 * benchmark run therefore wanders into a battle partway through - 26% of frames
 * after the walk begins came in under 100 draws in one desktop run, against a
 * field scene's 500-600.
 *
 * The obvious seam does not exist. `bdPlayerFieldCheckEncounter` is never
 * called, and neither is anything else in the `0x8220xxxx` block - see
 * research/20260830_0300_the-player-field-family-is-dead.md. Every route into a
 * battle does converge on `bdBattleEncounterBegin`, so that is where this looks
 * instead.
 *
 * **This file currently only watches.** Suppressing there would also catch
 * `bdScriptOpGosub`, which is how *story* battles start, and stopping one of
 * those would leave a script waiting forever for a battle that never happens.
 * Distinguishing the random-encounter caller from the scripted one needs the
 * call site, which a whole-function replacement cannot see - so the counter
 * comes first and tells us whether this seam is even live during autoplay.
 *
 * @copyright Copyright (c) 2026 re:Blue contributors
 * @license   BSD 3-Clause - see LICENSE
 */
#include "core/hooks.h"
#include "core/logging.h"

#include <rex/ppc.h>
#include <rex/types.h>

#include <atomic>

// bdBattleEncounterBegin(r3, ...) - the convergence point for all five routes
// into a battle. Instrumented, not intercepted: a hook on a function nobody has
// watched fire is a guess, and this codebase has now produced two named guest
// functions that never run at all.
REX_EXTERN(__imp__bdBattleEncounterBegin);
REX_HOOK_RAW(bdBattleEncounterBegin) {
  static std::atomic<u32> entered{0};
  const u32 n = entered.fetch_add(1, std::memory_order_relaxed);
  if (n < 8)
    BD_INFO("[tourist] bdBattleEncounterBegin #{} r3={:08X} r4={:08X}", n + 1,
            ctx.r3.u32, ctx.r4.u32);
  __imp__bdBattleEncounterBegin(ctx, base);
}
