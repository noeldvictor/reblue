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
#include "gpu/output.h"
#include "xr/xr_game_camera.h"
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
std::atomic<u64> g_detail_culled{0};
std::atomic<u64> g_tested_count{0};
} // namespace

namespace bd::engine {
void CensusReportDistinct() {
  if (!REXCVAR_GET(bd_guest_census))
    return;
  BD_INFO("[census]   sceneNodeDrawSingle distinct r3={} r4={}", g_distinct_r3,
          g_distinct_r4);
  BD_INFO("[census]   detail cull: {} nodes under bd_cull_min_pixels",
          g_detail_culled.exchange(0, std::memory_order_relaxed));
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
REXCVAR_DECLARE(bool, bd_cull_early);
REXCVAR_DECLARE(f64, bd_cull_min_pixels);


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
// Returns true when the node is culled, and the hook table redirects to the
// guest's own "not visible" continuation on true.
//
// It used to return void and merely record the decision for the hook after the
// call to apply, deliberately, so that no control flow was redirected. That is
// safe and it is also why the decision cost nothing to make and everything to
// act on: the visibility test between the two hooks is sub_82287788, measured
// at 7.5% of all CPU samples and the hottest function in the process, and the
// distance cull rejects about 95% of nodes. We were computing full visibility
// for 95% of the scene and then throwing it away.
//
// Jumping is safe here because the target is not a new path: the guest already
// branches to loc_822825E0 from two earlier tests in the same traversal, before
// it ever reaches the call, and nothing after that label reads the call's
// result.
bool bdSceneCullBiasHook(PPCRegister &f1, PPCRegister &r3) {
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
    return false;
  const u32 va = r3.u32;
  if (va == 0)
    return false;
  // One translation for the block instead of one per component - this runs for
  // every node in the scene, and try_translate showed up in the profile under
  // it. Both ends are validated, so a centre straddling the end of a mapping is
  // still rejected rather than read past.
  const auto *centre = bd::mem::try_at<const rex::be<u32>>(va);
  if (!centre || !bd::mem::try_at<const rex::be<u32>>(va + 8))
    return false;
  float c[3];
  for (int i = 0; i < 3; ++i) {
    const u32 bits = static_cast<u32>(centre[i]);
    std::memcpy(&c[i], &bits, sizeof(float));
  }
  const f64 len_sq = f64(c[0]) * c[0] + f64(c[1]) * c[1] + f64(c[2]) * c[2];
  // Keep anything whose own radius reaches inside the limit, so a large distant
  // object - a cliff, a building - does not pop out while the pebble beside it
  // stays.
  // (len - radius) > limit, without the square root: for a non-negative
  // threshold the comparison squares exactly, and a negative one means the
  // radius alone already reaches past the limit so nothing can be culled.
  const f64 threshold = limit + f1.f64;
  g_cull_this_node = threshold >= 0.0 && len_sq > threshold * threshold;
  if (g_cull_this_node) {
    g_culled_count.fetch_add(1, std::memory_order_relaxed);
    g_tested_count.fetch_add(1, std::memory_order_relaxed);
    // Off, the decision is still made and still applied - just after the
    // visibility test rather than instead of it, which is what this used to do.
    // Kept as a runtime switch so the two can be compared without a rebuild.
    return REXCVAR_GET(bd_cull_early);
  }

  // Detail culling: drop what is too small on screen to be worth a draw.
  //
  // This is the one kind of culling a 2006 Xenon engine had no reason to do.
  // The hardware command processor made draws nearly free, so Blue Dragon
  // submits every node its frustum test keeps, however small it lands - and on
  // an Adreno each of those costs a full trip through bdSceneNodeDrawSingle,
  // which is 23x the next consumer of guest CPU on device.
  //
  // The projected radius of a sphere at view-space depth z is
  // r/z * (height/2) / tan(vfov/2), and everything on the right of that is
  // known: the centre is already in view space with the camera at the origin,
  // so there is no matrix and no space conversion to get wrong.
  const f64 min_px = REXCVAR_GET(bd_cull_min_pixels);
  if (min_px <= 0.0)
    return false;
  // +Z forward, D3D9-era left-handed. Anything at or behind the eye is either
  // already gone or degenerate here, and dividing by it would invert the test.
  const f64 z = f64(c[2]);
  if (z <= 1.0)
    return false;

  f32 half_v = 0.0f, aspect = 1.0f;
  const f64 tan_v = bd::xr::RenderFov(half_v, aspect) && half_v > 0.0f
                        ? f64(std::tan(half_v))
                        // Not in VR: Blue Dragon's own vertical half angle.
                        : 0.4142f;
  const f64 height = f64(bd::gpu::kDesignCanvasHeight);
  const f64 radius_px = (f1.f64 / z) * (height * 0.5) / tan_v;
  if (radius_px < min_px) {
    g_cull_this_node = true;
    g_detail_culled.fetch_add(1, std::memory_order_relaxed);
    g_culled_count.fetch_add(1, std::memory_order_relaxed);
    g_tested_count.fetch_add(1, std::memory_order_relaxed);
    return REXCVAR_GET(bd_cull_early);
  }
  return false;
}

// Zeroes the visibility result the guest is about to compare against 0, which
// sends it down its own "not visible" path and skips the draw. Nothing is
// redirected and no return address is needed.
void bdSceneCullDistanceHook(PPCRegister &r3) {
  // Only survivors reach here now - a culled node jumped past the call from the
  // hook before it - so this is belt and braces plus the survivor count.
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

// Deliberately inert: no guest memory read, no register written, no cvar
// lookup. Its only job is to answer whether hooking 0x82282760 at all is what
// hung the guest.
void bdSceneCullSiteProbe() {
  static std::atomic<u64> hits{0};
  const u64 n = hits.fetch_add(1, std::memory_order_relaxed);
  if (n == 0 || n == 5000)
    BD_INFO("[siteprobe] second-path compare reached, hit {}", n);
}
