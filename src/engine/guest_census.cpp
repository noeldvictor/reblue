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
    {"bdSceneNodeDrawSingle"},
    {"ScriptManTaskUpdate"},
    {"bdRenderViewSubmit"},
    {"bdFieldInteractionSearch"},
    {"bdFieldHUDUpdate"},
    {"bdScriptExecute"},
    {"bdFrameSubmitAndDebugHUD"},
    {"bdEffectEmitterUpdate"},
};

// Bytes of guest code per function, from config/functions.toml. Calls alone
// rank by frequency; calls x size ranks by roughly how much recompiled code is
// executed, which is closer to the thing that costs time.
constexpr u32 kSizes[] = {0x15E8, 0x71C, 0x238, 0x234, 0xE8, 0x84,
                          0x1E3C, 0x36F4, 0x19AC, 0x1714, 0x13D4, 0x1040, 0xE70, 0xE34};

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
void bdCensusbdSceneNodeDrawSingle() { bd::engine::CensusNote(6); }
void bdCensusScriptManTaskUpdate() { bd::engine::CensusNote(7); }
void bdCensusbdRenderViewSubmit() { bd::engine::CensusNote(8); }
void bdCensusbdFieldInteractionSearch() { bd::engine::CensusNote(9); }
void bdCensusbdFieldHUDUpdate() { bd::engine::CensusNote(10); }
void bdCensusbdScriptExecute() { bd::engine::CensusNote(11); }
void bdCensusbdFrameSubmitAndDebugHUD() { bd::engine::CensusNote(12); }
void bdCensusbdEffectEmitterUpdate() { bd::engine::CensusNote(13); }

REXCVAR_DECLARE(f64, bd_cull_bias);

// Shrinks the bounding radius the guest tests against its own view frustum, so
// nodes that only just intersect it are culled and their draw never happens.
// 1.0 is the game's own behaviour.
//
// The CPU floor is real - 43ms of GPU time was freed on a Quest and `elsewhere`
// did not move - and the census says node submission dominates it, so cutting
// nodes is the lever. This is the cheapest form of that: one register, before a
// call the guest already makes.
void bdSceneCullBiasHook(PPCRegister &f1) {
  const f64 bias = REXCVAR_GET(bd_cull_bias);
  if (bias != 1.0)
    f1.f64 *= bias;
}
