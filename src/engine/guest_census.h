/**
 * @file    engine/guest_census.h
 * @brief   How often the suspected-hot guest functions are actually called.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license MIT
 */

#pragma once

#include <rex/types.h>

namespace bd::engine {

void CensusNote(u32 index);

// Prints the counts and resets them. Cheap when bd_guest_census is off.
void CensusReport(u32 frames);

} // namespace bd::engine
