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

// How many distinct values bdSceneNodeDrawSingle's first two arguments take in
// a frame - individually placed props against instanced geometry.
void CensusReportDistinct();


// Squared view-space distance of the scene node most recently visited by the
// guest's cull traverse, which is the node the draws right after it belong to.
//
// The draw queue sorts opaque draws by this so near geometry is submitted
// first, which is what lets a tiler's low-resolution Z reject hidden fragments
// before shading them. Squared and unnormalised: only the ordering matters.
double LastNodeViewDistanceSq();

} // namespace bd::engine
