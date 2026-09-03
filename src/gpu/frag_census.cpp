/**
 * @file    gpu/frag_census.cpp
 * @brief   Fragment census over pipeline-statistics queries.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/frag_census.h"

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>

#include "core/logging.h"
#include "gpu/device.h"

REXCVAR_DECLARE(bool, bd_frag_census);

namespace bd::gpu {
namespace {

// Queries per frame: every queued draw of the frame gets one. A field scene
// is ~800 draws on the desktop; the cap only truncates the tail of a heavier
// frame and says so.
constexpr u32 kQueriesPerSlot = 4096;

struct Slot {
  std::unique_ptr<plume::RenderQueryPool> pool;
  std::vector<u64> hashes; // per query index, the pixel shader
  u32 used = 0;
  bool open = false;
  bool pending = false;
  bool saturated = false;
};

// A pixel shader and the boolean constant words it was drawn with: one path
// through an uber-shader.
struct PathKey {
  u64 ps = 0;
  u32 bools[4] = {};
  bool operator==(const PathKey &o) const {
    return ps == o.ps && bools[0] == o.bools[0] && bools[1] == o.bools[1] &&
           bools[2] == o.bools[2] && bools[3] == o.bools[3];
  }
};
struct PathKeyHash {
  size_t operator()(const PathKey &k) const {
    u64 h = k.ps;
    for (u32 b : k.bools)
      h = (h ^ (u64(b) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2)));
    return size_t(h);
  }
};

struct Census {
  Slot slots[kNumFrames];
  u32 active = ~0u;
  bool unsupported = false;
  // Accumulated over the report window.
  std::unordered_map<u64, u64> per_shader;
  std::unordered_map<PathKey, u32, PathKeyHash> per_path; // draws per path
  u64 total = 0;
  u32 frames = 0;
  u32 draws_counted = 0;
};

Census &census() {
  static Census c;
  return c;
}

} // namespace

void FragCensusFrameBegin(plume::RenderDevice *device,
                          plume::RenderCommandList *cmd, u32 slot) {
  auto &c = census();
  c.active = ~0u;
  if (!REXCVAR_GET(bd_frag_census) || c.unsupported || !device || !cmd ||
      slot >= kNumFrames)
    return;
  Slot &st = c.slots[slot];
  if (!st.pool) {
    st.pool = device->createStatisticsQueryPool(kQueriesPerSlot);
    if (!st.pool) {
      c.unsupported = true;
      BD_INFO("[frag] pipeline statistics queries are not available on this "
              "device; the fragment census is off");
      return;
    }
    st.hashes.resize(kQueriesPerSlot, 0ull);
  }
  st.used = 0;
  st.open = false;
  st.pending = false;
  st.saturated = false;
  cmd->resetQueryPool(st.pool.get(), 0, kQueriesPerSlot);
  c.active = slot;
}

bool FragCensusBegin(plume::RenderCommandList *cmd, u64 ps_hash) {
  auto &c = census();
  if (c.active >= kNumFrames || !cmd)
    return false;
  Slot &st = c.slots[c.active];
  if (st.open)
    return false;
  if (st.used >= kQueriesPerSlot) {
    st.saturated = true;
    return false;
  }
  st.hashes[st.used] = ps_hash;
  cmd->beginQuery(st.pool.get(), st.used);
  st.open = true;
  return true;
}

void FragCensusEnd(plume::RenderCommandList *cmd) {
  auto &c = census();
  if (c.active >= kNumFrames || !cmd)
    return;
  Slot &st = c.slots[c.active];
  if (!st.open)
    return;
  cmd->endQuery(st.pool.get(), st.used);
  st.open = false;
  ++st.used;
  st.pending = true;
}

void FragCensusCollect(u32 slot) {
  auto &c = census();
  if (slot >= kNumFrames)
    return;
  Slot &st = c.slots[slot];
  if (!st.pending || !st.pool || st.used == 0)
    return;
  st.pending = false;
  st.pool->queryResults(st.used);
  const u64 *results = st.pool->getResults();
  for (u32 i = 0; i < st.used; ++i) {
    c.per_shader[st.hashes[i]] += results[i];
    c.total += results[i];
  }
  c.draws_counted += st.used;
  ++c.frames;
  if (st.saturated) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      BD_INFO("[frag] census saturated at {} draws a frame; the tail of the "
              "frame is not counted",
              kQueriesPerSlot);
    }
  }
  // A report every 300 frames: the shaders that produce the fragments, and
  // the whole as fragments per frame.
  if (c.frames < 300)
    return;
  std::vector<std::pair<u64, u64>> rows(c.per_shader.begin(),
                                        c.per_shader.end());
  std::sort(rows.begin(), rows.end(),
            [](const auto &a, const auto &b) { return a.second > b.second; });
  const double frames = static_cast<double>(c.frames);
  BD_INFO("[frag] {:.1f} M fragments a frame over {} draws ({} shaders); the "
          "top ten:",
          c.total / frames / 1.0e6, c.draws_counted / c.frames, rows.size());
  u32 shown = 0;
  for (const auto &[hash, count] : rows) {
    if (shown++ >= 10)
      break;
    BD_INFO("[frag]   ps {:016X}: {:.2f} M a frame ({:.1f}%)", hash,
            count / frames / 1.0e6,
            c.total ? 100.0 * static_cast<double>(count) / c.total : 0.0);
  }
  // The paths: which boolean constant words each of the top shaders is drawn
  // with, and how often. A host material implements exactly these.
  std::vector<std::pair<PathKey, u32>> paths(c.per_path.begin(),
                                             c.per_path.end());
  std::sort(paths.begin(), paths.end(),
            [](const auto &a, const auto &b) { return a.second > b.second; });
  BD_INFO("[frag] {} distinct (pixel shader, bool words) paths; the top twelve "
          "by draws a frame:",
          paths.size());
  shown = 0;
  for (const auto &[key, draws] : paths) {
    if (shown++ >= 12)
      break;
    BD_INFO("[frag]   ps {:016X} bools {:08X} {:08X} {:08X} {:08X}: {:.1f} "
            "draws a frame",
            key.ps, key.bools[0], key.bools[1], key.bools[2], key.bools[3],
            draws / frames);
  }
  c.per_shader.clear();
  c.per_path.clear();
  c.total = 0;
  c.frames = 0;
  c.draws_counted = 0;
}

void FragCensusNoteDraw(u64 ps_hash, const u32 bools[4]) {
  auto &c = census();
  if (c.active >= kNumFrames)
    return; // the census is off this frame
  PathKey k;
  k.ps = ps_hash;
  for (u32 i = 0; i < 4; ++i)
    k.bools[i] = bools ? bools[i] : 0u;
  ++c.per_path[k];
}

} // namespace bd::gpu
