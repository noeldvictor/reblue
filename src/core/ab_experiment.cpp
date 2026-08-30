/**
 * @file    core/ab_experiment.cpp
 * @copyright Copyright (c) 2026 re:Blue contributors
 * @license   BSD 3-Clause - see LICENSE
 */
#include "core/ab_experiment.h"

#include "core/logging.h"

#include <rex/types.h>

#include <string>

REXCVAR_DECLARE(std::string, bd_ab_flag);
REXCVAR_DECLARE(i32, bd_ab_period);

namespace bd {

u8 ABExperimentTick() {
  const std::string &flag = REXCVAR_GET(bd_ab_flag);
  if (flag.empty())
    return 255;

  const i32 period = REXCVAR_GET(bd_ab_period) > 0
                         ? REXCVAR_GET(bd_ab_period)
                         : 300;

  static u64 frame = 0;
  const u8 arm = static_cast<u8>((frame++ / u64(period)) & 1u);

  // Only written when it changes. SetFlagByName parses a string and walks the
  // cvar registry, which is not something to do sixty times a second for a
  // value that changes every few hundred frames.
  static u8 applied = 255;
  if (arm != applied) {
    applied = arm;
    if (!rex::cvar::SetFlagByName(flag, arm ? "true" : "false")) {
      BD_ERROR("[ab] no such boolean cvar '{}' - experiment disabled", flag);
      // Leave `applied` set so this complains once rather than every frame.
      return 255;
    }
    BD_INFO("[ab] {} = {} (frame {}, period {})", flag, arm ? "true" : "false",
            frame - 1, period);
  }
  return arm;
}

} // namespace bd
