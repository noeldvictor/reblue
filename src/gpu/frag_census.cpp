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
#include <string>
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
  std::vector<u64> owners; // per query index, (visual << 16) | (view << 1) | blended
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
  // Fragments and draws per (visual, view, blended), and per render view:
  // the scene's fragments per pixel named by what draws them (2026-09-04).
  struct Owner {
    u64 fragments = 0;
    u64 draws = 0;
  };
  std::unordered_map<u64, Owner> per_owner;
  u64 per_view[17] = {}; // 16 = outside any node
  u64 per_view_blended[17] = {};
  // Blended, depth-writing draws whose textures are all opaque: opaque in
  // effect, the candidates for front-to-back order.
  u64 per_view_promotable[17] = {};
  u64 per_view_slot0[17] = {}; // blended, depth write, slot 0 the only partial texture
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
    st.owners.resize(kQueriesPerSlot, 0ull);
  }
  st.used = 0;
  st.open = false;
  st.pending = false;
  st.saturated = false;
  cmd->resetQueryPool(st.pool.get(), 0, kQueriesPerSlot);
  c.active = slot;
}

bool FragCensusBegin(plume::RenderCommandList *cmd, u64 ps_hash, u32 visual_va,
                     u32 render_view, u32 flags) {
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
  // Owner key: visual << 16 | flags << 8 | view << 1 | blended.
  st.owners[st.used] = (u64(visual_va) << 16) | (u64(flags & 0xFF) << 8) |
                       (u64(render_view < 16 ? render_view : 16u) << 1) |
                       (flags & 1u);
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
    Census::Owner &o = c.per_owner[st.owners[i]];
    o.fragments += results[i];
    ++o.draws;
    const u32 view = u32((st.owners[i] >> 1) & 0x1F);
    c.per_view[view < 17 ? view : 16] += results[i];
    if (st.owners[i] & 1)
      c.per_view_blended[view < 17 ? view : 16] += results[i];
    const u32 flags = u32((st.owners[i] >> 8) & 0xFF);
    if ((flags & 7u) == 7u)
      c.per_view_promotable[view < 17 ? view : 16] += results[i];
    if ((flags & 0xDu) == 0xDu)
      c.per_view_slot0[view < 17 ? view : 16] += results[i];
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
  for (u32 v = 0; v < 17; ++v) {
    if (!c.per_view[v])
      continue;
    BD_INFO("[frag]   view {}: {:.2f} M a frame ({:.1f}%), {:.2f} M of them "
            "blended, {:.2f} M blended with depth write over opaque textures, "
            "{:.2f} M with slot 0 the only partial-alpha texture",
            v == 16 ? std::string("none") : std::to_string(v),
            c.per_view[v] / frames / 1.0e6,
            c.total ? 100.0 * static_cast<double>(c.per_view[v]) / c.total : 0.0,
            c.per_view_blended[v] / frames / 1.0e6,
            c.per_view_promotable[v] / frames / 1.0e6,
            c.per_view_slot0[v] / frames / 1.0e6);
  }
  {
    std::vector<std::pair<u64, Census::Owner>> owners(c.per_owner.begin(),
                                              c.per_owner.end());
    std::sort(owners.begin(), owners.end(), [](const auto &a, const auto &b) {
      return a.second.fragments > b.second.fragments;
    });
    BD_INFO("[frag] {} (visual, view, blended) owners; the top sixteen by "
            "fragments a frame:",
            owners.size());
    u32 n = 0;
    for (const auto &[key, o] : owners) {
      if (n++ >= 16)
        break;
      const u32 view = u32((key >> 1) & 0x1F);
      const u32 flags = u32((key >> 8) & 0xFF);
      BD_INFO("[frag]   visual {:08x} view {} {}{}{}: {:.2f} M a frame ({:.1f}%), "
              "{:.1f} draws a frame",
              u32(key >> 16), view == 16 ? std::string("none") : std::to_string(view),
              (key & 1) ? "blended" : "opaque", (flags & 2) ? " opaque-tex" : "",
              (flags & 4) ? " zwrite" : "", o.fragments / frames / 1.0e6,
              c.total ? 100.0 * static_cast<double>(o.fragments) / c.total : 0.0,
              o.draws / frames);
    }
  }
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
  c.per_owner.clear();
  std::fill(std::begin(c.per_view), std::end(c.per_view), 0ull);
  std::fill(std::begin(c.per_view_promotable), std::end(c.per_view_promotable), 0ull);
  std::fill(std::begin(c.per_view_slot0), std::end(c.per_view_slot0), 0ull);
  std::fill(std::begin(c.per_view_blended), std::end(c.per_view_blended), 0ull);
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
