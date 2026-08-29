/**
 * @file    engine/guest_census.cpp
 * @brief   How often the suspected-hot guest functions are actually called.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license MIT
 */

#include <atomic>

#include <rex/hook.h>
#include <rex/ppc.h>
#include <rex/types.h>

#include "core/logging.h"
#include "engine/guest_census.h"

REXCVAR_DECLARE(bool, bd_guest_census);

namespace bd::engine {

namespace {

struct Entry {
  const char *name;
  std::atomic<u64> calls{0};
};

// Order matches the hook bodies below and the table in guest_census.toml.
Entry g_entries[] = {
    {"bdAnimBoneEvaluate"},     {"bdAnimationUpdate"},
    {"bdMatrixInverse4x4"},     {"bdMatrixTransformVector"},
    {"bdAnimCurveSample3"},     {"bdMatrix4x4Copy"},
};

// Bytes of guest code per function, from config/functions.toml. Calls alone
// rank by frequency; calls x size ranks by roughly how much recompiled code is
// executed, which is closer to the thing that costs time.
constexpr u32 kSizes[] = {0x15E8, 0x71C, 0x238, 0x234, 0xE8, 0x84};

} // namespace

void CensusNote(u32 index) {
  if (index < std::size(g_entries))
    g_entries[index].calls.fetch_add(1, std::memory_order_relaxed);
}

void CensusReport(u32 frames) {
  if (!REXCVAR_GET(bd_guest_census) || frames == 0)
    return;
  BD_INFO("[census] per frame, over {} frames:", frames);
  for (size_t i = 0; i < std::size(g_entries); ++i) {
    const u64 c = g_entries[i].calls.exchange(0, std::memory_order_relaxed);
    const u64 per = c / frames;
    BD_INFO("[census]   {:28} {:8} calls  {:10} bytes of guest code",
            g_entries[i].name, per, per * kSizes[i]);
  }
}

} // namespace bd::engine

void bdCensusAnimBoneEvaluate() { bd::engine::CensusNote(0); }
void bdCensusAnimationUpdate() { bd::engine::CensusNote(1); }
void bdCensusMatrixInverse4x4() { bd::engine::CensusNote(2); }
void bdCensusMatrixTransformVector() { bd::engine::CensusNote(3); }
void bdCensusAnimCurveSample3() { bd::engine::CensusNote(4); }
void bdCensusMatrix4x4Copy() { bd::engine::CensusNote(5); }
