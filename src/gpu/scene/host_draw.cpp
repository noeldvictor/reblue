/**
 * @file    gpu/scene/host_draw.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */

// What a node draw is, as the interpreter leaves it at the D3D layer: one or
// more draws (a mesh has a draw per material range), each a pipeline state,
// the texture slots it bound, the vertex streams and index view, and the
// registers it wrote into the guest's constant files and fetch constants.
// Observed on 2026-09-02 (the setter hooks over a village frame): every node
// run writes vertex c0..c4 and c20..c23 and pixel c0..c13; foliage writes c57
// and skinned nodes the bone palette at c60... The world rows c20..c23 come
// from the node's palette slot (c20+r = (M[0][r], M[1][r], M[2][r], T[r]),
// verified over 3728 recorded draws). Of the rest, some registers hold the
// same value every frame (the material: UV offsets, colours) and some move
// every frame (the visual's lighting and camera terms, the same for every node
// of that visual within a frame).
//
// So the host keeps, per (mesh, render view, technique), a template of the
// node's draws: the host state each draw needs, the registers and samplers
// it writes, and for each register whether its value has ever moved between
// sightings. A replay takes stable values from the template and moving ones
// from the latest interpreted node of the same visual in the same frame; when
// no node of the visual has been interpreted yet this frame, this one is,
// which is what keeps those values fresh. Nodes whose vertex shader reads c57
// or the bone palette keep the interpreter, and so does a template whose
// draw structure changed (volatile).
//
// The replay goes through the ordinary draw dispatch with the host state set
// to the template and the constant sources overridden, so every gate, the
// instancing key and the queue see exactly what an interpreted draw gives
// them. The host state is put back afterwards, because the guest's own
// redundant-state elision assumes the D3D state is what it last set.

#include "gpu/scene/host_draw.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/constant_buffers.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/draw_queue.h"
#include "gpu/frame_stats.h"
#include "gpu/hooks/draw_dispatch.h"
#include "gpu/scene/guest_scene.h"
#include "gpu/scene/node_tag.h"
#include "gpu/scene/scene_recorder.h"

REXCVAR_DECLARE(bool, bd_host_draw);
REXCVAR_DECLARE(i32, bd_host_draw_refresh);

namespace bd::gpu::scene {

namespace {

constexpr u32 kBlockBytes = 256 * 16;
constexpr u32 kShadowVsRegs = 5;
constexpr u32 kShadowPsRegs = 14;

struct RegDelta {
  u16 reg;
  bool stable = true; // the same value in every sighting so far
  u32 value[4];       // host order
};

struct FetchDelta {
  u16 slot;
  bool stable = true;
  u32 dword[6];
};

// One of a node's draws.
struct SubDraw {
  PipelineState pipelineState{};
  GuestTexture *textures[16]{};
  u32 tex_mask = 0; // slots SetTexture bound by this draw
  plume::RenderVertexBufferView vertex_views[16]{};
  plume::RenderInputSlot input_slots[16]{};
  u32 vertex_first = 0;
  u32 vertex_count = 0;
  plume::RenderIndexBufferView index_view{plume::RenderBufferReference{}, 0,
                                          plume::RenderFormat::R16_UINT};
  bool indexed = false;
  u32 count = 0;
  u32 start_index = 0;
  i32 base_vertex = 0;
  u32 start_vertex = 0;
  u32 primitive_type = 0;
  float alpha_threshold = 0.0f;

  std::vector<RegDelta> vs_delta; // c20..c23 excluded; cumulative from entry
  std::vector<RegDelta> ps_delta;
  std::vector<FetchDelta> fetch_delta;
  u32 bools[8]{};
};

struct NodeTemplate {
  u32 captured_frame = 0;
  bool volatile_material = false;
  u32 replays = 0;
  std::vector<SubDraw> draws;
};

// The registers and samplers the interpreter last wrote for a visual, with
// the frame it wrote them in: the source of a replay's moving values.
struct VisualRegs {
  u32 vs_frame[256] = {};
  u32 ps_frame[256] = {};
  u32 fetch_frame[32] = {};
  u32 vs[256][4] = {};
  u32 ps[256][4] = {};
  u32 fetch[32][6] = {};
};

struct Store {
  std::mutex mutex;
  std::unordered_map<u64, NodeTemplate> templates;
  std::unordered_map<u64, u32> never; // keys that cannot replay: frame noted
  std::unordered_map<u64, VisualRegs> visuals;
  u32 volatile_count = 0;
  u32 replayed = 0;
  u32 interpreted = 0;
  u32 stale_bail = 0; // replays refused for want of this frame's visual values
  u32 acc_replayed = 0, acc_interpreted = 0, acc_frames = 0, acc_stale = 0;
  u32 last_frame = 0;
};

Store &store() {
  static Store s;
  return s;
}

// The files as they were when the interpreter started on this node, what it
// has set since, and the draws it has issued.
struct Pending {
  bool valid = false;
  bool replayable = true;
  alignas(16) u8 vs[kBlockBytes];
  alignas(16) u8 ps[kBlockBytes];
  u32 fetch[32][6];
  u32 shadow_vs[kShadowVsRegs][4];
  u32 shadow_ps[kShadowPsRegs][4];
  u32 set_mask = 0;
  u32 sampler_mask = 0;
  u32 vs_set[8] = {};
  u32 ps_set[8] = {};
  std::vector<SubDraw> draws;
};
thread_local Pending t_pending;
thread_local bool t_replaying = false;
alignas(16) thread_local u8 t_vs_block[kBlockBytes];
alignas(16) thread_local u8 t_ps_block[kBlockBytes];
thread_local u32 t_fetch[32][6];

u64 KeyOf(const NodeTag &tag) {
  return (u64(tag.mesh_va) << 32) ^ (u64(tag.render_view) << 8) ^ u64(tag.tech);
}

u64 VisualKeyOf(const NodeTag &tag) {
  return (u64(tag.visual_va) << 32) ^ (u64(tag.render_view) << 8) ^
         u64(tag.tech);
}

// The foliage vector at c57, as bdSceneNodeDrawSingle computes it for a
// visual of technique 3 (read off the recompiled body at 0x82280390..0x822804E8
// on 2026-09-02): a per-node 20-byte entry in the table at visual+3540 scaled
// by the object at visual+3532, or the global default vector at 0x82DDA9AC
// when the entry is empty; y from visual+3536 times the object's +36; w from
// the per-index table at 0x82DBA948; bool 31 says whether the entry was there.
// Verified against the interpreter's own writes before a replay may use it
// (g_foliage_checked / g_foliage_wrong below).
constexpr u32 kFoliageDefaultVa = 0x82DDA9ACu;
constexpr u32 kFoliagePhaseTableVa = 0x82DBA948u;
constexpr u32 kFoliageDefaultScalarVa = 0x82055230u;
constexpr u32 kVisualFoliageTable = 3540;
constexpr u32 kVisualFoliageObject = 3532;
constexpr u32 kVisualFoliageScale = 3536;
constexpr u32 kTechFoliage = 3;

struct Foliage {
  float v[4];
  bool flag;
};

inline float LoadF32At(u32 va) {
  const u32 bits = bd::mem::try_load<u32>(va);
  float f;
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}

bool ComputeFoliage(const NodeTag &tag, Foliage &out) {
  if (bd::mem::try_field<u32>(tag.visual_va, kVisualTech) != kTechFoliage)
    return false;
  const float dflt = LoadF32At(kFoliageDefaultScalarVa);
  float x = dflt, y = dflt, z = dflt;
  bool flag = false;
  const u32 table = bd::mem::try_field<u32>(tag.visual_va, kVisualFoliageTable);
  const u32 obj = bd::mem::try_field<u32>(tag.visual_va, kVisualFoliageObject);
  const u32 e = table ? table + tag.node_index * 20 : 0;
  if (e && bd::mem::try_load<u32>(e) != 0) {
    const float s = LoadF32At(obj + 72);
    const float k = LoadF32At(e + 12);
    x = LoadF32At(e + 4) * s * k;
    z = LoadF32At(e + 8) * s * k;
    flag = true;
  } else {
    x = LoadF32At(kFoliageDefaultVa + 0);
    z = LoadF32At(kFoliageDefaultVa + 8);
  }
  y = obj ? LoadF32At(tag.visual_va + kVisualFoliageScale) * LoadF32At(obj + 36)
          : LoadF32At(kFoliageDefaultVa + 4);
  out.v[0] = x;
  out.v[1] = y;
  out.v[2] = z;
  out.v[3] = LoadF32At(kFoliagePhaseTableVa + tag.node_index * 4);
  out.flag = flag;
  return true;
}

// The guest's deferred state shadow at 0x82DD80D8: the interpreter copies
// vertex c0..c4 from +0 and pixel c0..c13 from +0x50 (the setter hooks named
// the source, 2026-09-02). Whether those bytes already hold the node's values
// when its run begins - the pass having set them - decides whether the host
// can read the shadow at replay instead of interpreting a node per visual per
// frame. Counted per register; a register the interpreter itself fills during
// the run (a material colour) will disagree and stays on the visual path.
constexpr u32 kShadowVa = 0x82DD80D8u;
constexpr u32 kShadowPsOffset = 0x50;
u32 g_shadow_checked = 0;
u32 g_shadow_vs_wrong[kShadowVsRegs];
u32 g_shadow_ps_wrong[kShadowPsRegs];

// The registers the shadow already holds when a node's run begins - the
// pass's state, which the interpreter only copies (400 village runs, 0
// disagreements on 2026-09-02): vertex c0, c1, c4 and pixel c0..c2, c5..c9,
// c13. A replay reads these from the live shadow, so they are always the
// current pass's. The others (vertex c2/c3, pixel c3, c4, c10..c12) the
// interpreter fills itself - the material colour among them - and stay on
// the template / per-visual path.
constexpr u32 kShadowSourcedVs = (1u << 0) | (1u << 1) | (1u << 4);
constexpr u32 kShadowSourcedPs =
    (1u << 0) | (1u << 1) | (1u << 2) | (1u << 5) | (1u << 6) | (1u << 7) |
    (1u << 8) | (1u << 9) | (1u << 13);

inline bool ShadowSourced(bool vertex, u32 reg) {
  return vertex ? (reg < 32 && ((kShadowSourcedVs >> reg) & 1u))
                : (reg < 32 && ((kShadowSourcedPs >> reg) & 1u));
}

// A shadow float as the constant file would hold it (the upload flushes NaN).
inline u32 ShadowBits(u32 raw) {
  if ((raw & 0x7F800000u) == 0x7F800000u && (raw & 0x007FFFFFu))
    return 0;
  return raw;
}

void ReadShadow(u32 vs_out[kShadowVsRegs][4], u32 ps_out[kShadowPsRegs][4]) {
  const auto *v = bd::mem::try_at<const be_u32>(kShadowVa);
  const auto *p = bd::mem::try_at<const be_u32>(kShadowVa + kShadowPsOffset);
  for (u32 r = 0; r < kShadowVsRegs; ++r)
    for (u32 k = 0; k < 4; ++k)
      vs_out[r][k] = v ? static_cast<u32>(v[r * 4 + k]) : 0;
  for (u32 r = 0; r < kShadowPsRegs; ++r)
    for (u32 k = 0; k < 4; ++k)
      ps_out[r][k] = p ? static_cast<u32>(p[r * 4 + k]) : 0;
}

u32 g_foliage_checked = 0;
u32 g_foliage_wrong = 0;
constexpr u32 kFoliageTrustAfter = 200;

bool FoliageTrusted() {
  return g_foliage_checked >= kFoliageTrustAfter && g_foliage_wrong == 0;
}

// The vertex shader reads only registers the template can supply: the
// collision vector is computed here (once verified), the bone palette
// (c60..c151) is not yet.
bool VertexShaderReplayable(const PipelineState &st) {
  const auto *vs = st.vertexShader;
  if (!vs || !vs->shaderCacheEntry)
    return false;
  const u32 *m = vs->shaderCacheEntry->constantRegisterMask;
  if ((m[1] & (1u << 25)) && !FoliageTrusted())   // c57
    return false;
  if (m[1] & 0xF0000000u)  // c60..c63
    return false;
  if (m[2] || m[3])        // c64..c127
    return false;
  if (m[4] & 0x00FFFFFFu)  // c128..c151
    return false;
  return true;
}

void ReadFetch(const D3DDevice *dev, u32 out[32][6]) {
  for (u32 i = 0; i < 32; ++i)
    for (u32 k = 0; k < 6; ++k)
      out[i][k] = static_cast<u32>(dev->fetchConstants[i].dword[k]);
}

// The registers the run wrote (the setter hooks) or that moved anyway (a
// store the hooks did not see), except the world rows.
void DiffBlock(const u8 *before, const u8 *now, const u32 *set,
               std::vector<RegDelta> &out, bool skip_world) {
  out.clear();
  for (u32 r = 0; r < 256; ++r) {
    if (skip_world && (r >= 20 && r < 24))
      continue;
    if (skip_world && r == 57)
      continue; // the foliage vector: computed per node at replay
    const bool written = (set[r / 32] >> (r % 32)) & 1u;
    if (written || std::memcmp(before + r * 16, now + r * 16, 16) != 0) {
      RegDelta d;
      d.reg = static_cast<u16>(r);
      std::memcpy(d.value, now + r * 16, 16);
      out.push_back(d);
    }
  }
}

// Same register set: the structure. Values that moved turn the register's
// stable flag off in `have`.
bool MergeDelta(std::vector<RegDelta> &have, const std::vector<RegDelta> &now,
                bool vertex) {
  if (have.size() != now.size())
    return false;
  for (size_t i = 0; i < have.size(); ++i) {
    if (have[i].reg != now[i].reg)
      return false;
    if (ShadowSourced(vertex, have[i].reg))
      continue; // read live at replay, whatever it did between sightings
    if (std::memcmp(have[i].value, now[i].value, 16) != 0)
      have[i].stable = false;
  }
  return true;
}

bool MergeFetchDelta(std::vector<FetchDelta> &have,
                     const std::vector<FetchDelta> &now) {
  if (have.size() != now.size())
    return false;
  for (size_t i = 0; i < have.size(); ++i) {
    if (have[i].slot != now[i].slot)
      return false;
    if (std::memcmp(have[i].dword, now[i].dword, sizeof(have[i].dword)) != 0)
      have[i].stable = false;
  }
  return true;
}

// Why two sightings of a node differed structurally, for the tally line.
u32 g_why[8];
const char *const kWhy[8] = {"count", "pipeline", "textures", "params",
                             "vs",    "ps",       "fetch",    "-"};

// Folds a later sighting into the template. False when the structure
// differs, which makes the template volatile.
bool MergeDraws(std::vector<SubDraw> &have, const std::vector<SubDraw> &now) {
  if (have.size() != now.size()) {
    ++g_why[0];
    return false;
  }
  for (size_t i = 0; i < have.size(); ++i) {
    SubDraw &x = have[i];
    const SubDraw &y = now[i];
    if (std::memcmp(&x.pipelineState, &y.pipelineState, sizeof(PipelineState)) != 0) {
      ++g_why[1];
      return false;
    }
    if (x.tex_mask != y.tex_mask) {
      ++g_why[2];
      return false;
    }
    for (u32 k = 0; k < 16; ++k)
      if ((x.tex_mask >> k) & 1u && x.textures[k] != y.textures[k]) {
        ++g_why[2];
        return false;
      }
    if (x.count != y.count || x.start_index != y.start_index ||
        x.base_vertex != y.base_vertex || x.indexed != y.indexed) {
      ++g_why[3];
      return false;
    }
    if (!MergeDelta(x.vs_delta, y.vs_delta, true)) {
      ++g_why[4];
      return false;
    }
    if (!MergeDelta(x.ps_delta, y.ps_delta, false)) {
      ++g_why[5];
      return false;
    }
    if (!MergeFetchDelta(x.fetch_delta, y.fetch_delta)) {
      ++g_why[6];
      return false;
    }
    // The state that is not compared follows the latest sighting.
    std::memcpy(x.vertex_views, y.vertex_views, sizeof(x.vertex_views));
    std::memcpy(x.input_slots, y.input_slots, sizeof(x.input_slots));
    x.vertex_first = y.vertex_first;
    x.vertex_count = y.vertex_count;
    x.index_view = y.index_view;
    x.start_vertex = y.start_vertex;
    x.primitive_type = y.primitive_type;
    x.alpha_threshold = y.alpha_threshold;
    std::memcpy(x.bools, y.bools, sizeof(x.bools));
  }
  return true;
}

void Tally(Store &st, bool replayed) {
  const u32 frame = FrameStatFrameCount();
  if (frame != st.last_frame) {
    {
      static u32 prev_total = 0, told = 0;
      const u32 total = st.replayed + st.interpreted;
      if (prev_total > 200 && total < prev_total * 4 / 5 && told++ < 6)
        BD_INFO("[node] frame {} had {} node draws ({} host-issued) after a "
                "frame of {}",
                st.last_frame, total, st.replayed, prev_total);
      prev_total = total;
    }
    st.acc_replayed += st.replayed;
    st.acc_interpreted += st.interpreted;
    st.acc_stale += st.stale_bail;
    ++st.acc_frames;
    st.replayed = st.interpreted = st.stale_bail = 0;
    st.last_frame = frame;
    if (st.acc_frames == 300) {
      std::string why;
      for (u32 i = 0; i < 7; ++i)
        if (g_why[i])
          why += fmt::format(" {}:{}", kWhy[i], g_why[i]);
      BD_INFO("[node] host-issued {} of {} node draws a frame ({} interpreted "
              "for fresh visual values); {} templates, {} volatile (why:{})",
              st.acc_replayed / st.acc_frames,
              (st.acc_replayed + st.acc_interpreted) / st.acc_frames,
              st.acc_stale / st.acc_frames, st.templates.size(),
              st.volatile_count, why.empty() ? " -" : why);
      st.acc_replayed = st.acc_interpreted = st.acc_frames = st.acc_stale = 0;
    }
  }
  if (replayed)
    ++st.replayed;
  else
    ++st.interpreted;
}

} // namespace

bool HostDrawEnabled() { return REXCVAR_GET(bd_host_draw); }

void NoteTextureSet(u32 index) {
  auto &p = t_pending;
  if (p.valid && index < 16)
    p.set_mask |= 1u << index;
}

void NoteConstantsSet(bool vertex, u32 start, u32 count) {
  auto &p = t_pending;
  if (!p.valid)
    return;
  u32 *set = vertex ? p.vs_set : p.ps_set;
  const u32 end = std::min<u32>(start + count, 256u);
  for (u32 r = start; r < end; ++r)
    set[r / 32] |= 1u << (r % 32);
}

// One-shot: where the interpreter's constant writes come from, relative to
// the objects the node tag names. Printed for the first few node runs.
void NoteConstantsSource(bool vertex, u32 start, u32 count, u32 src_va) {
  auto &p = t_pending;
  if (!p.valid)
    return;
  static u32 told = 0;
  if (told >= 40 || (vertex && (start == 0 || start == 20)) || !vertex)
    return;
  ++told;
  const NodeTag &tag = CurrentNodeTag();
  auto rel = [&](const char *name, u32 base, u32 span) -> std::string {
    if (base && src_va >= base && src_va < base + span)
      return fmt::format("{}+0x{:X}", name, src_va - base);
    return "";
  };
  std::string where = rel("visual", tag.visual_va, 0x2000);
  if (where.empty()) where = rel("mesh", tag.mesh_va, 0x100);
  if (where.empty()) where = rel("matrix", tag.matrix_va, 0x40);
  if (where.empty()) where = rel("ctx", tag.ctx_va, 0x100);
  if (where.empty()) where = rel("palette", tag.palette_va, 0x4000);
  if (where.empty()) {
    const u32 sp = bd::mem::try_load<u32>(tag.ctx_va);
    (void)sp;
    where = fmt::format("0x{:08X}", src_va);
  }
  BD_INFO("[node] {} c{}..c{} <- {} (visual 0x{:08X} mesh 0x{:08X})",
          vertex ? "VS" : "PS", start, start + count - 1, where,
          tag.visual_va, tag.mesh_va);
}

void NoteSamplerSet(u32 slot) {
  auto &p = t_pending;
  if (p.valid && slot < 32)
    p.sampler_mask |= 1u << slot;
}

void HostDrawSnapshotBefore() {
  auto &p = t_pending;
  p.valid = false;
  p.replayable = true;
  p.set_mask = 0;
  p.sampler_mask = 0;
  std::memset(p.vs_set, 0, sizeof(p.vs_set));
  std::memset(p.ps_set, 0, sizeof(p.ps_set));
  p.draws.clear();
  const u32 device_guest = LastGuestDeviceVa();
  const auto *dev = bd::mem::try_at<const D3DDevice>(device_guest);
  if (!dev)
    return;
  CopyGuestVertexBlock(device_guest, p.vs);
  CopyGuestPixelBlock(device_guest, p.ps);
  ReadFetch(dev, p.fetch);
  ReadShadow(p.shadow_vs, p.shadow_ps);
  p.valid = true;
}

void HostDrawCapture(const VideoState &s, const QueuedDraw &q, u32 device_guest,
                     u32 primitive_type) {
  if (t_replaying)
    return;
  const NodeTag &tag = CurrentNodeTag();
  if (!tag.valid)
    return;
  auto &p = t_pending;
  if (!p.valid || !p.replayable)
    return;
  if (!VertexShaderReplayable(s.pipelineState)) {
    p.replayable = false;
    auto &st = store();
    std::lock_guard lock(st.mutex);
    st.never[KeyOf(tag)] = FrameStatFrameCount();
    return;
  }
  const auto *dev = bd::mem::try_at<const D3DDevice>(device_guest);
  if (!dev) {
    p.replayable = false;
    return;
  }

  SubDraw d;
  CopyGuestVertexBlock(device_guest, t_vs_block);
  CopyGuestPixelBlock(device_guest, t_ps_block);
  // The foliage vector the interpreter just wrote against the host's own
  // computation of it, while the interpreter still runs for these nodes.
  {
    Foliage f;
    if (ComputeFoliage(tag, f) && ((p.vs_set[1] >> 25) & 1u)) {
      float wrote[4];
      std::memcpy(wrote, t_vs_block + 57 * 16, sizeof(wrote));
      const bool bit = (static_cast<u32>(dev->vsBoolConstants[0]) >> 31) & 1u;
      bool same = bit == f.flag;
      for (int k = 0; same && k < 4; ++k)
        same = std::memcmp(&wrote[k], &f.v[k], sizeof(float)) == 0;
      ++g_foliage_checked;
      if (!same) {
        ++g_foliage_wrong;
        static u32 told = 0;
        if (told++ < 3)
          BD_INFO("[node] foliage c57 mismatch: interpreter ({:.4f} {:.4f} "
                  "{:.4f} {:.4f} b{}) host ({:.4f} {:.4f} {:.4f} {:.4f} b{})",
                  wrote[0], wrote[1], wrote[2], wrote[3], bit ? 1 : 0, f.v[0],
                  f.v[1], f.v[2], f.v[3], f.flag ? 1 : 0);
      } else if (g_foliage_checked == kFoliageTrustAfter) {
        BD_INFO("[node] foliage c57: host computation agreed with the "
                "interpreter on {} nodes, replaying foliage",
                g_foliage_checked);
      }
    }
  }
  DiffBlock(p.vs, t_vs_block, p.vs_set, d.vs_delta, true);
  DiffBlock(p.ps, t_ps_block, p.ps_set, d.ps_delta, false);
  // The shadow as it stood at entry against what the run wrote. (The
  // constant files hold NaN-flushed host floats and the shadow raw guest
  // bits, so the compare goes through the same flush.)
  {
    bool any = false;
    for (const RegDelta &r : d.vs_delta)
      if (r.reg < kShadowVsRegs) {
        any = true;
        for (u32 k = 0; k < 4; ++k) {
          u32 s = p.shadow_vs[r.reg][k];
          if ((s & 0x7F800000u) == 0x7F800000u && (s & 0x007FFFFFu)) s = 0;
          if (s != r.value[k]) { ++g_shadow_vs_wrong[r.reg]; break; }
        }
      }
    for (const RegDelta &r : d.ps_delta)
      if (r.reg < kShadowPsRegs) {
        any = true;
        for (u32 k = 0; k < 4; ++k) {
          u32 s = p.shadow_ps[r.reg][k];
          if ((s & 0x7F800000u) == 0x7F800000u && (s & 0x007FFFFFu)) s = 0;
          if (s != r.value[k]) { ++g_shadow_ps_wrong[r.reg]; break; }
        }
      }
    if (any && ++g_shadow_checked == 400) {
      std::string vs, ps;
      for (u32 r = 0; r < kShadowVsRegs; ++r)
        vs += fmt::format(" c{}:{}", r, g_shadow_vs_wrong[r]);
      for (u32 r = 0; r < kShadowPsRegs; ++r)
        ps += fmt::format(" c{}:{}", r, g_shadow_ps_wrong[r]);
      BD_INFO("[node] shadow at entry vs what the run wrote, over {} runs - "
              "disagreements VS:{} | PS:{}",
              g_shadow_checked, vs, ps);
    }
  }
  ReadFetch(dev, t_fetch);
  for (u32 i = 0; i < 32; ++i) {
    const bool touched = (p.sampler_mask >> i) & 1u;
    if (touched ||
        std::memcmp(t_fetch[i], p.fetch[i], sizeof(t_fetch[i])) != 0) {
      FetchDelta f;
      f.slot = static_cast<u16>(i);
      std::memcpy(f.dword, t_fetch[i], sizeof(f.dword));
      d.fetch_delta.push_back(f);
    }
  }
  d.pipelineState = s.pipelineState;
  d.tex_mask = p.set_mask;
  for (u32 i = 0; i < 16; ++i)
    d.textures[i] = s.textures[i];
  for (u32 i = 0; i < 16; ++i) {
    d.vertex_views[i] = q.vertex_views[i];
    d.input_slots[i] = q.input_slots[i];
  }
  d.vertex_first = q.vertex_first;
  d.vertex_count = q.vertex_count;
  d.index_view = q.index_view;
  d.indexed = q.indexed;
  d.count = q.count;
  d.start_index = q.start_index;
  d.base_vertex = q.base_vertex;
  d.start_vertex = q.start_vertex;
  d.primitive_type = primitive_type;
  d.alpha_threshold = Video::AlphaThreshold();
  for (u32 i = 0; i < 4; ++i) {
    d.bools[i] = static_cast<u32>(dev->vsBoolConstants[i]);
    d.bools[4 + i] = static_cast<u32>(dev->psBoolConstants[i]);
  }
  p.draws.push_back(std::move(d));
}

void HostDrawCommit(const NodeTag &tag) {
  auto &p = t_pending;
  auto &st = store();
  std::lock_guard lock(st.mutex);
  Tally(st, false);
  if (!p.valid || !p.replayable || p.draws.empty() || !tag.valid) {
    p.valid = false;
    return;
  }
  const u32 frame = FrameStatFrameCount();

  // Whatever this run wrote is the visual's freshest word on those registers.
  {
    VisualRegs &v = st.visuals[VisualKeyOf(tag)];
    for (const SubDraw &d : p.draws) {
      for (const RegDelta &r : d.vs_delta) {
        v.vs_frame[r.reg] = frame;
        std::memcpy(v.vs[r.reg], r.value, 16);
      }
      for (const RegDelta &r : d.ps_delta) {
        v.ps_frame[r.reg] = frame;
        std::memcpy(v.ps[r.reg], r.value, 16);
      }
      for (const FetchDelta &f : d.fetch_delta) {
        v.fetch_frame[f.slot] = frame;
        std::memcpy(v.fetch[f.slot], f.dword, sizeof(f.dword));
      }
    }
  }

  const u64 key = KeyOf(tag);
  auto it = st.templates.find(key);
  if (it != st.templates.end()) {
    NodeTemplate &t = it->second;
    if (t.volatile_material) {
      p.valid = false;
      return;
    }
    if (t.captured_frame != frame) {
      if (!MergeDraws(t.draws, p.draws)) {
        t.volatile_material = true;
        t.draws.clear();
        ++st.volatile_count;
      } else {
        t.captured_frame = frame;
      }
      p.valid = false;
      return;
    }
  }
  NodeTemplate &t = st.templates[key];
  t.captured_frame = frame;
  t.draws = std::move(p.draws);
  p.draws.clear();
  p.valid = false;
}

bool HostDrawWantsCapture(const NodeTag &tag) {
  auto &st = store();
  std::lock_guard lock(st.mutex);
  if (auto it = st.never.find(KeyOf(tag)); it != st.never.end()) {
    // Ask again now and then: a foliage node becomes replayable once the
    // host's vector is trusted.
    if (FrameStatFrameCount() - it->second < 300)
      return false;
    st.never.erase(it);
  }
  if (auto it = st.templates.find(KeyOf(tag));
      it != st.templates.end() && it->second.volatile_material)
    return false;
  return true;
}

bool HostDrawReplay(const NodeTag &tag) {
  if (!tag.valid || t_replaying)
    return false;
  const u32 device_guest = LastGuestDeviceVa();
  if (!device_guest || !DrawQueueEnabled() || !InstanceRecordsReady())
    return false;
  const auto *dev = bd::mem::try_at<const D3DDevice>(device_guest);
  if (!dev)
    return false;
  auto &st = store();
  const NodeTemplate *t = nullptr;
  const VisualRegs *v = nullptr;
  const u32 frame = FrameStatFrameCount();
  {
    std::lock_guard lock(st.mutex);
    auto it = st.templates.find(KeyOf(tag));
    if (it == st.templates.end() || it->second.volatile_material ||
        it->second.draws.empty())
      return false;
    const u32 refresh =
        static_cast<u32>(std::max(1, REXCVAR_GET(bd_host_draw_refresh)));
    if (frame - it->second.captured_frame >= refresh)
      return false; // the interpreter runs once and refreshes the template
    t = &it->second;
    // Every moving value must have been written by an interpreted node of
    // this visual in this frame; otherwise this node is the one to interpret.
    auto vit = st.visuals.find(VisualKeyOf(tag));
    v = vit != st.visuals.end() ? &vit->second : nullptr;
    for (const SubDraw &d : t->draws) {
      for (const RegDelta &r : d.vs_delta)
        if (!r.stable && !ShadowSourced(true, r.reg) &&
            (!v || v->vs_frame[r.reg] != frame)) {
          ++st.stale_bail;
          return false;
        }
      for (const RegDelta &r : d.ps_delta)
        if (!r.stable && !ShadowSourced(false, r.reg) &&
            (!v || v->ps_frame[r.reg] != frame)) {
          ++st.stale_bail;
          return false;
        }
      for (const FetchDelta &f : d.fetch_delta)
        if (!f.stable && (!v || v->fetch_frame[f.slot] != frame)) {
          ++st.stale_bail;
          return false;
        }
    }
    Tally(st, true);
    ++it->second.replays;
  }

  // The foliage vector for this node, when its visual is foliage.
  Foliage foliage;
  const bool has_foliage = ComputeFoliage(tag, foliage);

  // The world rows for every draw of the node, from its palette slot.
  float world_rows[16];
  {
    float m[16];
    for (u32 i = 0; i < 16; ++i) {
      const u32 bits = bd::mem::try_load<u32>(tag.matrix_va + i * 4);
      std::memcpy(&m[i], &bits, sizeof(float));
    }
    for (u32 r = 0; r < 4; ++r) {
      world_rows[r * 4 + 0] = m[0 * 4 + r];
      world_rows[r * 4 + 1] = m[1 * 4 + r];
      world_rows[r * 4 + 2] = m[2 * 4 + r];
      world_rows[r * 4 + 3] = m[3 * 4 + r];
    }
  }

  auto &s = state();
  struct Saved {
    PipelineState pipelineState;
    GuestTexture *textures[16];
    plume::RenderVertexBufferView vertex_views[16];
    plume::RenderInputSlot input_slots[16];
    u32 vertex_first, vertex_count;
    plume::RenderIndexBufferView index_view;
    float alpha;
  };
  thread_local Saved saved;
  auto mark_dirty = [&s]() {
    s.texture_bindings_dirty = true;
    s.dirtyStates.pipelineState = true;
    s.dirtyStates.vertexShaderConstants = true;
    s.dirtyStates.pixelShaderConstants = true;
    s.dirtyStates.indices = true;
    s.dirtyStates.vertexStreamFirst = 0;
    s.dirtyStates.vertexStreamLast = 15;
  };
  {
    std::lock_guard lock(s.mutex);
    saved.pipelineState = s.pipelineState;
    std::memcpy(saved.textures, s.textures, sizeof(saved.textures));
    std::memcpy(saved.vertex_views, s.vertex_views, sizeof(saved.vertex_views));
    std::memcpy(saved.input_slots, s.input_slots, sizeof(saved.input_slots));
    saved.vertex_first = s.bound_vertex_first;
    saved.vertex_count = s.bound_vertex_count;
    saved.index_view = s.index_view;
    saved.alpha = Video::AlphaThreshold();
  }

  // The pass's registers, straight from the guest's shadow as it stands now.
  u32 shadow_vs[kShadowVsRegs][4];
  u32 shadow_ps[kShadowPsRegs][4];
  ReadShadow(shadow_vs, shadow_ps);
  for (auto &reg : shadow_vs)
    for (u32 &b : reg)
      b = ShadowBits(b);
  for (auto &reg : shadow_ps)
    for (u32 &b : reg)
      b = ShadowBits(b);

  t_replaying = true;
  MaterialOverride ov;
  for (const SubDraw &d : t->draws) {
    // The constant sources: the live files, the shadow for the pass's
    // registers, the template's stable values, the visual's fresh values,
    // and the world rows.
    CopyGuestVertexBlock(device_guest, t_vs_block);
    CopyGuestPixelBlock(device_guest, t_ps_block);
    for (const RegDelta &r : d.vs_delta) {
      const void *src = ShadowSourced(true, r.reg) ? static_cast<const void *>(shadow_vs[r.reg])
                        : r.stable                 ? static_cast<const void *>(r.value)
                                                   : static_cast<const void *>(v->vs[r.reg]);
      std::memcpy(t_vs_block + r.reg * 16, src, 16);
    }
    for (const RegDelta &r : d.ps_delta) {
      const void *src = ShadowSourced(false, r.reg) ? static_cast<const void *>(shadow_ps[r.reg])
                        : r.stable                  ? static_cast<const void *>(r.value)
                                                    : static_cast<const void *>(v->ps[r.reg]);
      std::memcpy(t_ps_block + r.reg * 16, src, 16);
    }
    std::memcpy(t_vs_block + 20 * 16, world_rows, sizeof(world_rows));
    u32 bools[8];
    std::memcpy(bools, d.bools, sizeof(bools));
    if (has_foliage) {
      std::memcpy(t_vs_block + 57 * 16, foliage.v, sizeof(foliage.v));
      if (foliage.flag)
        bools[0] |= 1u << 31;
      else
        bools[0] &= ~(1u << 31);
    }
    ReadFetch(dev, t_fetch);
    for (const FetchDelta &f : d.fetch_delta)
      std::memcpy(t_fetch[f.slot], f.stable ? f.dword : v->fetch[f.slot],
                  sizeof(f.dword));
    ov.vs = t_vs_block;
    ov.ps = t_ps_block;
    ov.fetch = t_fetch;
    ov.bools = bools;
    {
      std::lock_guard lock(s.mutex);
      s.pipelineState = d.pipelineState;
      for (u32 k = 0; k < 16; ++k)
        if ((d.tex_mask >> k) & 1u)
          s.textures[k] = d.textures[k];
      std::memcpy(s.vertex_views, d.vertex_views, sizeof(s.vertex_views));
      std::memcpy(s.input_slots, d.input_slots, sizeof(s.input_slots));
      s.bound_vertex_first = d.vertex_first;
      s.bound_vertex_count = d.vertex_count;
      s.index_view = d.index_view;
      Video::SetAlphaThreshold(d.alpha_threshold);
      mark_dirty();
      s.material_override = &ov;
    }
    bd::gpu::hooks::DispatchHostNodeDraw(device_guest, d.primitive_type,
                                         d.indexed, d.count, d.start_index,
                                         d.base_vertex, d.start_vertex);
  }
  t_replaying = false;
  {
    std::lock_guard lock(s.mutex);
    s.material_override = nullptr;
    s.pipelineState = saved.pipelineState;
    std::memcpy(s.textures, saved.textures, sizeof(s.textures));
    std::memcpy(s.vertex_views, saved.vertex_views, sizeof(s.vertex_views));
    std::memcpy(s.input_slots, saved.input_slots, sizeof(s.input_slots));
    s.bound_vertex_first = saved.vertex_first;
    s.bound_vertex_count = saved.vertex_count;
    s.index_view = saved.index_view;
    Video::SetAlphaThreshold(saved.alpha);
    mark_dirty();
  }
  return true;
}

} // namespace bd::gpu::scene
