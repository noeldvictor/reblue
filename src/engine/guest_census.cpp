/**
 * @file    engine/guest_census.cpp
 * @brief   How often the suspected-hot guest functions are actually called.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license MIT
 */

#include <atomic>
#include <cmath>
#include <cstring>

#include "core/memory_helpers.h"

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
// Distinct-value counters for the scene node draw. Open addressing over a fixed
// table because this runs 2000+ times a frame and a std::set would cost more
// than the thing being measured.
namespace {
constexpr u32 kDistinctSlots = 8192;
u32 g_seen_r3[kDistinctSlots];
u32 g_seen_r4[kDistinctSlots];
u32 g_distinct_r3 = 0;
u32 g_distinct_r4 = 0;

void NoteDistinct(u32 *table, u32 &count, u32 value) {
  if (value == 0)
    return;
  const u32 h = (value * 2654435761u) % kDistinctSlots;
  for (u32 probe = 0; probe < 64; ++probe) {
    const u32 slot = (h + probe) % kDistinctSlots;
    if (table[slot] == value)
      return;
    if (table[slot] == 0) {
      table[slot] = value;
      ++count;
      return;
    }
  }
}
} // namespace

namespace {
// How many nodes the distance test saw and how many it rejected, so a cull that
// silently does nothing is distinguishable from one that is working and simply
// has nothing to remove.
std::atomic<u64> g_culled_count{0};
std::atomic<u64> g_tested_count{0};
} // namespace

namespace bd::engine {
void CensusReportDistinct() {
  if (!REXCVAR_GET(bd_guest_census))
    return;
  BD_INFO("[census]   sceneNodeDrawSingle distinct r3={} r4={}", g_distinct_r3,
          g_distinct_r4);
  BD_INFO("[census]   distance cull: {} of {} nodes rejected",
          g_culled_count.exchange(0, std::memory_order_relaxed),
          g_tested_count.exchange(0, std::memory_order_relaxed));
  std::memset(g_seen_r3, 0, sizeof(g_seen_r3));
  std::memset(g_seen_r4, 0, sizeof(g_seen_r4));
  g_distinct_r3 = 0;
  g_distinct_r4 = 0;
}
} // namespace bd::engine

void bdCensusbdSceneNodeDrawSingle(PPCRegister &r3, PPCRegister &r4) {
  bd::engine::CensusNote(6);
  if (!REXCVAR_GET(bd_guest_census))
    return;
  NoteDistinct(g_seen_r3, g_distinct_r3, r3.u32);
  NoteDistinct(g_seen_r4, g_distinct_r4, r4.u32);
}
void bdCensusScriptManTaskUpdate() { bd::engine::CensusNote(7); }
void bdCensusbdRenderViewSubmit() { bd::engine::CensusNote(8); }
void bdCensusbdFieldInteractionSearch() { bd::engine::CensusNote(9); }
void bdCensusbdFieldHUDUpdate() { bd::engine::CensusNote(10); }
void bdCensusbdScriptExecute() { bd::engine::CensusNote(11); }
void bdCensusbdFrameSubmitAndDebugHUD() { bd::engine::CensusNote(12); }
void bdCensusbdEffectEmitterUpdate() { bd::engine::CensusNote(13); }

REXCVAR_DECLARE(f64, bd_cull_bias);
REXCVAR_DECLARE(f64, bd_cull_distance);


// Set by the hook before the visibility test, read by the one after it. Per
// thread because the guest culls on its own thread and nothing else must see it.
namespace {
thread_local bool g_cull_this_node = false;
} // namespace

// Shrinks the bounding radius the guest tests against its own view frustum, so
// nodes that only just intersect it are culled and their draw never happens.
// 1.0 is the game's own behaviour.
//
// The CPU floor is real - 43ms of GPU time was freed on a Quest and `elsewhere`
// did not move - and the census says node submission dominates it, so cutting
// nodes is the lever. This is the cheapest form of that: one register, before a
// call the guest already makes.
void bdSceneCullBiasHook(PPCRegister &f1, PPCRegister &r3) {
  const f64 bias = REXCVAR_GET(bd_cull_bias);
  if (bias != 1.0)
    f1.f64 *= bias;

  // r3 is the transformed centre the visibility test reads - sub_82287788 opens
  // with `mr r10,r3` and the VMX chain just above writes three floats through
  // it. Logged once so the units are known: if the camera sits at the origin of
  // whatever space this is, distance culling is length(centre) and needs no
  // camera position at all.
  // Distance. The centre is in view space with the camera at the origin, so the
  // distance is just its length - no camera position, no matrix, no space
  // conversion to get wrong. Stashed for the hook after the call, which is the
  // only place the result can be overridden.
  g_cull_this_node = false;
  const f64 limit = REXCVAR_GET(bd_cull_distance);
  if (limit <= 0.0)
    return;
  const u32 va = r3.u32;
  if (va == 0)
    return;
  float c[3];
  for (int i = 0; i < 3; ++i) {
    const u32 bits = bd::mem::try_load<u32>(va + u32(i) * 4);
    std::memcpy(&c[i], &bits, sizeof(float));
  }
  const f64 len =
      std::sqrt(f64(c[0]) * c[0] + f64(c[1]) * c[1] + f64(c[2]) * c[2]);
  // Keep anything whose own radius reaches inside the limit, so a large distant
  // object - a cliff, a building - does not pop out while the pebble beside it
  // stays.
  g_cull_this_node = (len - f1.f64) > limit;
}

// Zeroes the visibility result the guest is about to compare against 0, which
// sends it down its own "not visible" path and skips the draw. Nothing is
// redirected and no return address is needed.
// The sibling traversal's version: same decision, but the centre arrives in r4.
// Shares g_cull_this_node with the first path because the hook that consumes it
// is the same function, and the two traversals never interleave on one thread.
void bdSceneCullBiasHook2(PPCRegister &f1, PPCRegister &r4) {
  bdSceneCullBiasHook(f1, r4);
}

void bdSceneCullDistanceHook(PPCRegister &r3) {
  if (g_cull_this_node) {
    r3.u64 = 0;
    g_culled_count.fetch_add(1, std::memory_order_relaxed);
  }
  g_tested_count.fetch_add(1, std::memory_order_relaxed);

  // Unconditional, not gated on the census cvar: the census gate produced no
  // output on a run where it had worked before, so gating the diagnostic on it
  // was hiding whether this hook runs at all.
  static std::atomic<int> shown{0};
  const int n = shown.fetch_add(1, std::memory_order_relaxed);
  if (n < 3)
    BD_INFO("[cullhook] fired, limit={:.0f} decision={} r3now={}",
            REXCVAR_GET(bd_cull_distance), g_cull_this_node, r3.u32);
}
