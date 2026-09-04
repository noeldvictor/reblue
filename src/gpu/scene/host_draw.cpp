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
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>
#include <xxhash.h>
#include <rex/cvar.h>
#include <rex/graphics/xenos.h>
#include <rex/hook.h>
#include <rex/ppc/context.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "engine/guest_census.h"
#include "gpu/constant_buffers.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/host_resource_heap.h"
#include "gpu/physical_buffers.h"
#include "gpu/draw_queue.h"
#include "gpu/format.h"
#include "gpu/frame_stats.h"
#include "gpu/hooks/draw_dispatch.h"
#include "gpu/scene/guest_scene.h"
#include "gpu/scene/mesh_lod.h"
#include "gpu/scene/node_tag.h"
#include "gpu/scene/scene_recorder.h"

REXCVAR_DECLARE(bool, bd_host_draw);
REXCVAR_DECLARE(i32, bd_host_draw_refresh);
REXCVAR_DECLARE(bool, bd_host_list_build);
REXCVAR_DECLARE(bool, bd_host_draw_verify);
REXCVAR_DECLARE(i32, bd_host_draw_verify_every);
REXCVAR_DECLARE(bool, bd_lod);
REXCVAR_DECLARE(bool, bd_host_draw_fast);
REXCVAR_DECLARE(bool, bd_host_draw_empty);
REXCVAR_DECLARE(bool, bd_material_diag);
REXCVAR_DECLARE(bool, bd_material_census);
REXCVAR_DECLARE(bool, bd_material_source);
REXCVAR_DECLARE(bool, bd_material_from_entry);
REXCVAR_DECLARE(bool, bd_material_from_visual);
REXCVAR_DECLARE(i32, bd_lod_shadow_grid);
REXCVAR_DECLARE(i32, bd_lod_reflection_grid);
REXCVAR_DECLARE(f64, bd_lod_scene_distance);
REXCVAR_DECLARE(f64, bd_lod_scene_cell);

namespace bd::gpu::scene {

namespace {

constexpr u32 kBlockBytes = 256 * 16;

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
  // The guest address each ordinary texture had at capture: a GuestTexture
  // object freed and reallocated for another texture keeps its host
  // pointer, and a replay through the stale pointer samples the new texture
  // until the refresh (2026-09-03). An address that moved refuses the
  // replay. (contentHash is a marker - 1 for a mirrored texture, 0 for the
  // rest - not a hash, so it neither identifies a texture nor a surface.)
  u32 tex_va[16]{};
  u32 tex_mask = 0; // slots SetTexture bound by this draw
  // Of those, the slots holding a render surface rather than an asset (a
  // shadow map, a reflection). A surface is pooled and re-pointed between
  // frames, so the template's pointer goes stale; the replay keeps the live
  // binding for these, which the pass set before any node ran. Binding the
  // old object also flipped its layout mid-pass and flushed the queue in the
  // middle of the node (the vanishing rock, 2026-09-02).
  u32 surface_mask = 0;
  plume::RenderVertexBufferView vertex_views[16]{};
  plume::RenderInputSlot input_slots[16]{};
  // The guest buffers behind the streams and the index buffer at capture.
  // The replay re-resolves the plume buffers from these: a physical block
  // is evicted when its scene graph streams out and replaced when it
  // refreshes, and a template holding the old plume reference drew nothing
  // where the ground pieces at the village rock should be, on the frames
  // they replayed (2026-09-03). A buffer that is gone refuses the replay.
  u32 stream_va[16]{};
  u32 stream_offset[16]{};
  u32 index_va = 0;
  u32 vertex_first = 0;
  u32 vertex_count = 0;
  plume::RenderIndexBufferView index_view{plume::RenderBufferReference{}, 0,
                                          plume::RenderFormat::R16_UINT};
  // The guest buffers the streams and index buffer resolved to, and the
  // physical-buffer generation they were resolved at: valid while the
  // generation holds (bd_host_draw_fast). The lookup was 135 of 7,101
  // profile samples (2026-09-04).
  mutable GuestBuffer *cached_stream[16]{};
  mutable GuestBuffer *cached_index = nullptr;
  mutable u64 cached_generation = 0;
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
  // Which bool bits the node set (VS words 0-3, PS words 4-7): the replay
  // takes these from the template and the rest from the live device.
  u32 bools_set[8]{};
};

struct NodeTemplate {
  u32 captured_frame = 0;
  bool volatile_material = false;
  // The node's interpreted run issues no draws at all, every time it has been
  // seen. Without this the empty template is refused as "no template" and the
  // node interprets for ever to produce nothing - 15 render-list entries a
  // frame did exactly that (2026-09-04). The refresh interval still expires
  // it, so a node that starts drawing is picked up within that window.
  bool draws_nothing = false;
  u32 replays = 0;
  std::vector<SubDraw> draws;
  // A skinned node's bone palette is a gather: bdSceneNodeDrawSingle copies
  // palette slot bone_slots[i] (64 bytes at ctx.palette + slot * 64) into a
  // stack buffer and uploads it at c60 + 4i. The slots come from the mesh's
  // bone-index tokens and never change; the matrices do, every frame, so the
  // replay gathers them live from the same palette.
  std::vector<u16> bone_slots;
};

// The registers and samplers the interpreter last wrote for a visual, with
// the frame it wrote them in: the source of a replay's moving values.
struct VisualRegs {
  u32 vs_frame[256] = {};
  u32 ps_frame[256] = {};
  // A register two sub-draws of this visual wrote differently: per sub-draw
  // material state, not a visual-wide value. Tracked but deliberately NOT used
  // to suppress the drift check - see the note below and
  // research/20260904_1500_the-per-object-material-constants.md.
  bool vs_pernode[256] = {};
  bool ps_pernode[256] = {};
  u32 fetch_frame[32] = {};
  u32 vs[256][4] = {};
  u32 ps[256][4] = {};
  u32 fetch[32][6] = {};
  // The render-target slots the interpreted node bound this frame: a pooled
  // surface changes pointer every frame, so a replay cannot keep the
  // capture's pointer, and inheriting whatever the previous host-ordered
  // draw left in the slot painted the ground around the village rock with
  // the reflection map in 186 of 300 frames (tools/capture_cyan.py,
  // 2026-09-03). The visual's own interpreted node in the same pass is the
  // binding the guest meant.
  GuestTexture *tex[16] = {};
  u32 tex_frame[16] = {};
  // The bool constants the visual's interpreted node ran with: the guest
  // toggles them between frames (VS bit 30, PS bit 5 in the verifier's
  // count of 503 draws, 2026-09-03), so a template's frozen copy is stale.
  u32 bools[8] = {};
  u32 bools_frame = 0;
};

// The camera block of a render pass, VS c0-c4 and the eye at PS c1, as the
// last interpreted draw of that view wrote it this frame. The render-list
// loop writes the block on the first entry of a visual in a pass and not on
// the next, so a template holds it from one sighting or not at all, and a
// replayed entry composed it from the live block: in the reflection view
// that was an eye at the origin, every reflected surface fully fogged, and
// the puddle at the village rock reflected flat sky on the frames its
// entries replayed (the "cyan skirt"; bd_host_draw_verify, 2026-09-03).
// c0-c1 only: c2-c4 are the node's (the verifier read them per node once
// they were taken from the pass, 3,352 draws).
constexpr u32 kPassVsRegs = 2;
constexpr u32 kPassPsRegs = 2; // c0, c1
struct PassRegs {
  u32 vs[kPassVsRegs][4] = {};
  u32 vs_frame[kPassVsRegs] = {};
  u32 ps[kPassPsRegs][4] = {};
  u32 ps_frame[kPassPsRegs] = {};
};

struct Store {
  std::mutex mutex;
  PassRegs pass_regs[16]; // by render view
  u32 why_pass = 0;       // replays refused: the pass's camera not seen yet
  u32 stale_buffer = 0;   // replays refused: a stream's buffer is gone
  u32 moved_buffer = 0;   // replayed streams whose plume buffer had moved
  u32 why_drift = 0;
  // Which registers a template is recaptured for: a "stable" register that
  // moves every frame is a misclassification, and each one costs an
  // interpreter run (2026-09-04).
  // Distinct sub-draw material keys seen (bd_material_census): the cook's size.
  std::unordered_set<u64> material_keys;
  std::unordered_set<u64> none_keys; // distinct nodes that never had a template
  // material identity (no constants) -> the distinct colour sets seen under it
  std::unordered_map<u64, std::unordered_set<u64>> identity_colours;
  u32 identity_ambiguous = 0;
  size_t identity_total = 0;
  u32 cap_invalid = 0, cap_not_replayable = 0, replayed_empty = 0;
  std::unordered_map<u64, u32> cap_reason; // key -> 1 no snapshot, 2 not replayable
  u32 drift_vs[256] = {};
  u32 drift_ps[256] = {};      // templates recaptured: a stable register moved
  std::unordered_map<u64, NodeTemplate> templates;
  std::unordered_map<u64, u32> never; // keys that cannot replay: frame noted
  std::unordered_map<u64, VisualRegs> visuals;
  u32 volatile_count = 0;
  u32 replayed = 0;
  u32 interpreted = 0;
  u32 stale_bail = 0; // replays refused for want of this frame's visual values
  // Why a replay was refused, per frame: no template yet, refresh due,
  // volatile, never-replayable shader (from the hook's gate).
  u32 why_none = 0, why_refresh = 0, why_volatile = 0, why_never = 0;
  // Render-target slots a replay had to inherit (no fresh binding this frame).
  u32 surface_inherited = 0;
  u32 stale_tex = 0; // replays refused on a reused texture object
  u32 acc_none = 0, acc_refresh = 0, acc_volatile = 0, acc_never = 0;
  u32 acc_surface_inherited = 0;
  u32 acc_replayed = 0, acc_interpreted = 0, acc_frames = 0, acc_stale = 0;
  u32 last_frame = 0;
  // Per key: runs that issued no draw at all, runs that issued some. A key
  // that is always empty is a node the interpreter visits and never draws in
  // that view; a mixed key draws under a condition the key does not carry.
  std::unordered_map<u64, std::pair<u32, u32>> runs;
  u32 untagged = 0, acc_untagged = 0; // scene-pass draws issued outside a node
  // The render-list share of the above, per frame and accumulated.
  u32 list_replayed = 0, list_interpreted = 0;
  u32 acc_list_replayed = 0, acc_list_interpreted = 0;
  // Render-list entries built by the host instead of the interpreter.
  struct ListTemplate {
    std::vector<std::vector<u8>> entries; // the guest's bytes, per entry
    u32 captured_frame = 0;
    u32 replays = 0;
  };
  std::unordered_map<u64, ListTemplate> lists;
  u32 list_built = 0, list_built_runs = 0; // entries, runs this frame
  u32 acc_list_built = 0, acc_list_built_runs = 0;
  u32 matrix_agree = 0, matrix_disagree = 0;
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
  u32 set_mask = 0;
  u32 sampler_mask = 0;
  u32 vs_set[8] = {};
  u32 ps_set[8] = {};
  // The bool registers the run set (a bit per bool, 128 a stage).
  u32 vs_bools_set[4] = {};
  u32 ps_bools_set[4] = {};
  // The bools as the run found them: a bit that moved by a store the
  // setter hooks never saw counts as set (the ground pieces' PS bit 5,
  // sampled verifier 2026-09-03), as DiffBlock does for the float block.
  u32 vs_bools_before[4] = {};
  u32 ps_bools_before[4] = {};
  std::vector<SubDraw> draws;
  u32 bone_count = 0;  // matrices uploaded at c60 by this run, 0 if none
  bool needs_bones = false; // a draw's vertex shader reads c60..c155
};
thread_local Pending t_pending;
thread_local bool t_replaying = false;
alignas(16) thread_local u8 t_vs_block[kBlockBytes];
alignas(16) thread_local u8 t_ps_block[kBlockBytes];

// bd_host_draw_verify: a node the replay would issue is composed by the
// replay exactly as it would be dispatched, kept here, and then the
// interpreter runs the node anyway; each interpreted sub-draw is diffed
// against the replay's composition at capture (VerifyAgainstReplay). The
// frame stays the interpreter's; the log names what the replay would have
// got wrong, register by register and slot by slot. Built 2026-09-03 for
// the cyan skirt: a within-run A/B put it on the replay (38 of 120 frames
// against 2), and the A/Bs of its sub-paths took two minutes a question.
struct VerifyDraw {
  alignas(16) u8 vs[kBlockBytes];
  alignas(16) u8 ps[kBlockBytes];
  u32 fetch[32][6];
  u32 bools[8];
  GuestTexture *textures[16];
  PipelineState pipelineState;
  plume::RenderVertexBufferView vertex_views[16];
  plume::RenderInputSlot input_slots[16];
  plume::RenderIndexBufferView index_view;
  u32 vertex_first, vertex_count;
  u32 count, start_index, start_vertex, primitive_type;
  i32 base_vertex;
  bool indexed;
  float alpha;
};
struct Verify {
  bool active = false;
  u64 key = 0;
  u32 next = 0;
  std::vector<VerifyDraw> expected;
  u32 nodes = 0, nodes_wrong = 0, draws = 0, draws_wrong = 0;
  u32 wrong_vs = 0, wrong_ps = 0, wrong_fetch = 0, wrong_tex = 0,
      wrong_state = 0, wrong_geom = 0, wrong_bools = 0, wrong_world = 0;
  u32 wrong_at_node_start = 0;
  u32 told = 0;
  u32 told_regs = 0, told_bools = 0, told_texset = 0;
  u32 tex_inherited = 0; // slots the node never set differ: noise, counted
  u32 last_report_frame = 0;
  // Which registers differ, over the run: the histogram names the culprit.
  u32 vs_reg_hits[256] = {};
  u32 ps_reg_hits[256] = {};
};
thread_local Verify t_verify;
thread_local u32 t_fetch[32][6];

u64 KeyOf(const NodeTag &tag) {
  return (u64(tag.mesh_va) << 32) ^ (u64(tag.render_view) << 8) ^ u64(tag.tech) ^
         (tag.from_list ? (u64(1) << 24) : 0);
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
  return true;
}

// Whether the vertex shader reads the bone palette, c60..c155.
bool VertexShaderReadsBones(const PipelineState &st) {
  const auto *vs = st.vertexShader;
  if (!vs || !vs->shaderCacheEntry)
    return false;
  const u32 *m = vs->shaderCacheEntry->constantRegisterMask;
  return (m[1] & 0xF0000000u) || m[2] || m[3] || (m[4] & 0x0FFFFFFFu);
}

constexpr u32 kBoneBase = 60;
constexpr u32 kBoneMax = 24; // bdSceneNodeDrawSingle refuses more (cmpwi 24)
constexpr u32 kBoneRegs = kBoneMax * 4;

// A palette slot as the constant file would hold it after the guest's
// SetVertexShaderConstantFN and the host's byte-swapping copy (NaN flushed).
void LoadPaletteSlot(u32 va, u32 out[16]) {
  const u8 *src = bd::mem::try_at<u8>(va);
  for (u32 i = 0; i < 16; ++i) {
    u32 v = 0;
    if (src) {
      std::memcpy(&v, src + i * 4, 4);
      v = __builtin_bswap32(v);
    }
    if ((v & 0x7F800000u) == 0x7F800000u && (v & 0x007FFFFFu))
      v = 0;
    out[i] = v;
  }
}

// Recover which palette slots the run's c60.. matrices came from, by value.
bool RecoverBoneSlots(const NodeTag &tag, const u8 *block, u32 count,
                      std::vector<u16> &slots) {
  slots.clear();
  if (!tag.palette_va || count == 0 || count > kBoneMax)
    return false;
  if (tag.bone_table_va) {
    // The entry names them; check the first against the upload anyway.
    if (tag.bone_count != count)
      return false;
    for (u32 i = 0; i < count; ++i) {
      const u32 slot = bd::mem::try_load<u32>(tag.bone_table_va + i * 4);
      if (slot > 4096)
        return false;
      slots.push_back(static_cast<u16>(slot));
    }
    u32 first[16];
    LoadPaletteSlot(tag.palette_va + slots[0] * 64u, first);
    if (std::memcmp(first, block + kBoneBase * 16, 64) != 0) {
      slots.clear();
      return false;
    }
    return true;
  }
  u32 span = bd::mem::try_field<u32>(tag.visual_va, kVisualBoneCount);
  if (span == 0 || span > 1024)
    span = 512;
  std::vector<std::array<u32, 16>> palette(span);
  for (u32 j = 0; j < span; ++j)
    LoadPaletteSlot(tag.palette_va + j * 64, palette[j].data());
  for (u32 i = 0; i < count; ++i) {
    const u8 *want = block + (kBoneBase + 4 * i) * 16;
    bool found = false;
    for (u32 j = 0; j < span && !found; ++j)
      if (std::memcmp(palette[j].data(), want, 64) == 0) {
        slots.push_back(static_cast<u16>(j));
        found = true;
      }
    if (!found) {
      slots.clear();
      return false;
    }
  }
  return true;
}

void GatherBones(const NodeTag &tag, const std::vector<u16> &slots, u8 *block) {
  for (size_t i = 0; i < slots.size(); ++i)
    LoadPaletteSlot(tag.palette_va + slots[i] * 64u,
                    reinterpret_cast<u32 *>(block + (kBoneBase + 4 * i) * 16));
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
// The union of the two sightings' registers, in register order. A register
// in one sighting only is kept as it is: a delta records a register that was
// written or moved, so one absent from a sighting kept the value it had -
// the render-list loop sets the camera block c0-c4 on the first entry of a
// visual and not on the next, and which entry comes first follows the depth
// sort (2026-09-03; it made 26 list templates volatile).
bool MergeDelta(std::vector<RegDelta> &have, const std::vector<RegDelta> &now) {
  std::vector<RegDelta> merged;
  merged.reserve(have.size() + now.size());
  size_t i = 0, j = 0;
  while (i < have.size() || j < now.size()) {
    if (j >= now.size() || (i < have.size() && have[i].reg < now[j].reg)) {
      // Seen in one sighting only: not a constant of the node. The replay
      // takes it from the visual's fresh values (the first entry of the
      // visual writes the camera block every frame); replayed as a stable
      // value it was the capture frame's camera, and the rock vanished.
      RegDelta r = have[i++];
      r.stable = false;
      merged.push_back(r);
    } else if (i >= have.size() || now[j].reg < have[i].reg) {
      RegDelta r = now[j++];
      r.stable = false;
      merged.push_back(r);
    } else {
      RegDelta r = have[i];
      if (std::memcmp(r.value, now[j].value, 16) != 0)
        r.stable = false;
      merged.push_back(r);
      ++i;
      ++j;
    }
  }
  have.swap(merged);
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
  for (size_t i = 0; i < have.size() && i < now.size(); ++i) {
    if (std::memcmp(have[i].stream_va, now[i].stream_va, sizeof(have[i].stream_va)) != 0 ||
        std::memcmp(have[i].stream_offset, now[i].stream_offset, sizeof(have[i].stream_offset)) != 0 ||
        have[i].index_va != now[i].index_va)
      return false;
  }
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
    if (x.tex_mask != y.tex_mask || x.surface_mask != y.surface_mask) {
      ++g_why[2];
      return false;
    }
    for (u32 k = 0; k < 16; ++k)
      if (((x.tex_mask & ~x.surface_mask) >> k) & 1u &&
          x.textures[k] != y.textures[k]) {
        ++g_why[2];
        return false;
      }
    if (x.count != y.count || x.start_index != y.start_index ||
        x.base_vertex != y.base_vertex || x.indexed != y.indexed) {
      ++g_why[3];
      return false;
    }
    if (!MergeDelta(x.vs_delta, y.vs_delta)) {
      ++g_why[4];
      return false;
    }
    if (!MergeDelta(x.ps_delta, y.ps_delta)) {
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

void Tally(Store &st, bool replayed, bool from_list) {
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
    st.acc_none += st.why_none;
    st.acc_refresh += st.why_refresh;
    st.acc_volatile += st.why_volatile;
    st.acc_surface_inherited += st.surface_inherited;
    st.acc_never += st.why_never;
    st.acc_untagged += st.untagged;
    st.untagged = 0;
    st.acc_list_replayed += st.list_replayed;
    st.acc_list_interpreted += st.list_interpreted;
    st.list_replayed = st.list_interpreted = 0;
    st.acc_list_built += st.list_built;
    st.acc_list_built_runs += st.list_built_runs;
    st.list_built = st.list_built_runs = 0;
    ++st.acc_frames;
    st.replayed = st.interpreted = st.stale_bail = 0;
    st.why_none = st.why_refresh = st.why_volatile = st.why_never = 0;
    st.surface_inherited = 0;
    st.last_frame = frame;
    if (st.acc_frames == 300) {
      std::string why;
      for (u32 i = 0; i < 7; ++i)
        if (g_why[i])
          why += fmt::format(" {}:{}", kWhy[i], g_why[i]);
      u32 always_empty = 0, mixed = 0;
      for (const auto &[k, r] : st.runs) {
        if (r.first && !r.second)
          ++always_empty;
        else if (r.first && r.second)
          ++mixed;
      }
      BD_INFO("[node] keys: {} always empty, {} mixed of {}; {} untagged draws "
              "a frame; render list: {} of {} entries host-issued, {} entries "
              "in {} runs host-built ({} list templates; matrix check {} ok, "
              "{} off)",
              always_empty, mixed, st.runs.size(),
              st.acc_untagged / st.acc_frames,
              st.acc_list_replayed / st.acc_frames,
              (st.acc_list_replayed + st.acc_list_interpreted) / st.acc_frames,
              st.acc_list_built / st.acc_frames,
              st.acc_list_built_runs / st.acc_frames, st.lists.size(),
              st.matrix_agree, st.matrix_disagree);
      st.acc_untagged = 0;
      st.acc_list_replayed = st.acc_list_interpreted = 0;
      st.acc_list_built = st.acc_list_built_runs = 0;
      BD_INFO("[node] host-issued {} of {} node draws a frame (refused: {} fresh "
              "values, {} no template, {} refresh, {} volatile, {} never, {} "
              "pass camera, {} drift); {} templates, {} volatile (why:{})",
              st.acc_replayed / st.acc_frames,
              (st.acc_replayed + st.acc_interpreted) / st.acc_frames,
              st.acc_stale / st.acc_frames, st.acc_none / st.acc_frames,
              st.acc_refresh / st.acc_frames, st.acc_volatile / st.acc_frames,
              st.acc_never / st.acc_frames, st.why_pass / st.acc_frames,
              st.why_drift / st.acc_frames, st.templates.size(),
              st.volatile_count, why.empty() ? " -" : why);
      if (st.stale_buffer || st.moved_buffer)
        BD_INFO("[node] since the last report: {} replays refused (a stream's "
                "buffer gone, the template recaptures), {} replayed streams "
                "re-resolved to a moved plume buffer",
                st.stale_buffer, st.moved_buffer);
      {
        // The registers behind the drift, named: each is a "stable" value that
        // moved, and each costs an interpreter run for that node.
        std::string top;
        for (int pass = 0; pass < 2; ++pass) {
          const u32 *a = pass ? st.drift_ps : st.drift_vs;
          std::vector<std::pair<u32, u32>> rows;
          for (u32 i = 0; i < 256; ++i)
            if (a[i])
              rows.push_back({a[i], i});
          std::sort(rows.rbegin(), rows.rend());
          for (size_t i = 0; i < rows.size() && i < 6; ++i)
            top += fmt::format(" {}c{}x{}", pass ? "ps" : "vs", rows[i].second,
                               rows[i].first / std::max(1u, st.acc_frames));
        }
        if (!top.empty())
          BD_INFO("[node] drift by register a frame:{}", top);
        BD_INFO("[node] no-template: {} distinct nodes over the window, {} "
                "refusals a frame",
                st.none_keys.size(), st.acc_none / std::max(1u, st.acc_frames));
        {
          u32 none_no_snapshot = 0, none_not_replayable = 0, none_never_tried = 0;
          for (u64 k : st.none_keys) {
            auto it = st.cap_reason.find(k);
            if (it == st.cap_reason.end())
              ++none_never_tried;
            else if (it->second == 1)
              ++none_no_snapshot;
            else
              ++none_not_replayable;
          }
          BD_INFO("[node] the never-captured nodes: {} of them - {} reached "
                  "capture with no snapshot, {} with an unreplayable one, {} "
                  "never reached it at all",
                  st.none_keys.size(), none_no_snapshot, none_not_replayable,
                  none_never_tried);
        }
        st.cap_invalid = st.cap_not_replayable = 0;
        st.cap_reason.clear();
        st.none_keys.clear();
        if (!st.material_keys.empty())
          BD_INFO("[material] {} distinct sub-draw materials, {} distinct "
                  "identities (shaders+state+textures, no constants), {} of "
                  "them carrying more than one colour set",
                  st.material_keys.size(), st.identity_total,
                  st.identity_ambiguous);
        std::memset(st.drift_vs, 0, sizeof(st.drift_vs));
        std::memset(st.drift_ps, 0, sizeof(st.drift_ps));
      }
      st.why_pass = st.why_drift = st.stale_buffer = st.moved_buffer = 0;
      st.acc_replayed = st.acc_interpreted = st.acc_frames = st.acc_stale = 0;
      st.acc_none = st.acc_refresh = st.acc_volatile = st.acc_never = 0;
    }
  }
  if (replayed)
    ++st.replayed;
  else
    ++st.interpreted;
  if (from_list) {
    if (replayed)
      ++st.list_replayed;
    else
      ++st.list_interpreted;
  }
}

} // namespace

bool HostDrawEnabled() { return REXCVAR_GET(bd_host_draw); }

bool PipelineReadsBones(const PipelineState &st) {
  return VertexShaderReadsBones(st);
}

bool HostDrawReplaying() { return t_replaying; }

bool HostDrawHasDraws() { return t_pending.valid && !t_pending.draws.empty(); }

void NoteTextureSet(u32 index) {
  auto &p = t_pending;
  if (p.valid && index < 16)
    p.set_mask |= 1u << index;
}

void NoteBoolsSet(bool vertex, u32 start, u32 count) {
  auto &p = t_pending;
  if (!p.valid)
    return;
  u32 *set = vertex ? p.vs_bools_set : p.ps_bools_set;
  const u32 end = std::min<u32>(start + count, 128u);
  for (u32 b = start; b < end; ++b)
    set[b / 32] |= 1u << (b % 32);
}

void NoteConstantsSet(bool vertex, u32 start, u32 count) {
  auto &p = t_pending;
  if (!p.valid)
    return;
  u32 *set = vertex ? p.vs_set : p.ps_set;
  const u32 end = std::min<u32>(start + count, 256u);
  for (u32 r = start; r < end; ++r)
    set[r / 32] |= 1u << (r % 32);
  if (vertex && start == kBoneBase)
    p.bone_count = count / 4;
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
  p.bone_count = 0;
  p.needs_bones = false;
  p.sampler_mask = 0;
  std::memset(p.vs_set, 0, sizeof(p.vs_set));
  std::memset(p.ps_set, 0, sizeof(p.ps_set));
  std::memset(p.vs_bools_set, 0, sizeof(p.vs_bools_set));
  std::memset(p.ps_bools_set, 0, sizeof(p.ps_bools_set));
  if (const auto *dev0 = bd::mem::try_at<const D3DDevice>(LastGuestDeviceVa()))
    for (u32 i = 0; i < 4; ++i) {
      p.vs_bools_before[i] = static_cast<u32>(dev0->vsBoolConstants[i]);
      p.ps_bools_before[i] = static_cast<u32>(dev0->psBoolConstants[i]);
    }
  p.draws.clear();
  const u32 device_guest = LastGuestDeviceVa();
  const auto *dev = bd::mem::try_at<const D3DDevice>(device_guest);
  if (!dev)
    return;
  CopyGuestVertexBlock(device_guest, p.vs);
  CopyGuestPixelBlock(device_guest, p.ps);
  ReadFetch(dev, p.fetch);
  p.valid = true;
}

namespace {

void VerifyAgainstReplay(const NodeTag &tag, const SubDraw &d, bool needs_bones) {
  Verify &vf = t_verify;
  const u32 idx = vf.next++;
  ++vf.draws;
  if (idx >= vf.expected.size()) {
    if (vf.told++ < 24)
      BD_INFO("[verify] node {:016X} sub {}: the interpreter issued more draws "
              "than the replay has ({})", KeyOf(tag), idx, vf.expected.size());
    ++vf.draws_wrong;
    ++vf.wrong_geom;
    return;
  }
  const VerifyDraw &e = vf.expected[idx];
  std::string why;
  u32 n_vs = 0, n_ps = 0, n_fetch = 0, n_tex = 0, n_world = 0;
  auto f4 = [](const u8 *b, u32 r) {
    float v[4];
    std::memcpy(v, b + r * 16, 16);
    return fmt::format("({:.3f} {:.3f} {:.3f} {:.3f})", v[0], v[1], v[2], v[3]);
  };
  for (u32 r = 0; r < 256; ++r) {
    if (needs_bones && r >= kBoneBase && r < kBoneBase + kBoneRegs)
      continue;
    if (std::memcmp(t_vs_block + r * 16, e.vs + r * 16, 16) == 0)
      continue;
    const bool world = r >= 20 && r < 24;
    if (world)
      ++n_world;
    else {
      ++n_vs;
      ++vf.vs_reg_hits[r];
    }
    if (n_vs + n_world <= 6)
      why += fmt::format(" vs c{} interp {} replay {};", r, f4(t_vs_block, r),
                         f4(e.vs, r));
  }
  for (u32 r = 0; r < 256; ++r) {
    if (std::memcmp(t_ps_block + r * 16, e.ps + r * 16, 16) == 0)
      continue;
    ++n_ps;
    ++vf.ps_reg_hits[r];
    if (n_ps <= 6)
      why += fmt::format(" ps c{} interp {} replay {};", r, f4(t_ps_block, r),
                         f4(e.ps, r));
  }
  for (u32 k = 0; k < 32; ++k) {
    if (std::memcmp(t_fetch[k], e.fetch[k], sizeof(e.fetch[k])) == 0)
      continue;
    ++n_fetch;
    if (n_fetch <= 4)
      why += fmt::format(" fetch{} interp {:08X}.. replay {:08X}..;", k,
                         t_fetch[k][0], e.fetch[k][0]);
  }
  auto desc = [](const GuestTexture *t) {
    return t ? fmt::format("va{:08X}/{}x{}/t{}", t->selfVa, t->width, t->height,
                           u32(t->type))
             : std::string("null");
  };
  for (u32 k = 0; k < 16; ++k) {
    if (d.textures[k] == e.textures[k])
      continue;
    if (!((d.tex_mask >> k) & 1u)) {
      ++vf.tex_inherited; // a slot this node never set; the guest's own
      // order left something else there. Reported when the slot's fetch
      // constant is configured (a sampler the shader may read) and the
      // interpreter's binding is a real texture.
      if (k < 32 && t_fetch[k][0] != 0 && d.textures[k]) {
        ++n_tex;
        if (n_tex <= 6)
          why += fmt::format(" tex{}(inherited) interp {} replay {};", k,
                             desc(d.textures[k]), desc(e.textures[k]));
      }
      continue;
    }
    ++n_tex;
    if (n_tex <= 6)
      why += fmt::format(" tex{}(set) interp {} replay {};", k,
                         desc(d.textures[k]), desc(e.textures[k]));
  }
  const bool state_diff =
      std::memcmp(&d.pipelineState, &e.pipelineState, sizeof(PipelineState)) != 0;
  const bool geom_diff =
      std::memcmp(d.vertex_views, e.vertex_views, sizeof(d.vertex_views)) != 0 ||
      std::memcmp(d.input_slots, e.input_slots, sizeof(d.input_slots)) != 0 ||
      std::memcmp(&d.index_view, &e.index_view, sizeof(d.index_view)) != 0 ||
      d.vertex_first != e.vertex_first || d.vertex_count != e.vertex_count ||
      d.count != e.count || d.start_index != e.start_index ||
      d.base_vertex != e.base_vertex || d.start_vertex != e.start_vertex ||
      d.indexed != e.indexed || d.primitive_type != e.primitive_type ||
      d.alpha_threshold != e.alpha;
  const bool bools_diff = std::memcmp(d.bools, e.bools, sizeof(d.bools)) != 0;
  if (state_diff)
    why += " pipelineState differs;";
  if (geom_diff)
    why += fmt::format(" geometry differs (count {}/{} start {}/{} base {}/{});",
                       d.count, e.count, d.start_index, e.start_index,
                       d.base_vertex, e.base_vertex);
  if (bools_diff)
    why += fmt::format(" bools interp {:08X}/{:08X} replay {:08X}/{:08X};",
                       d.bools[0], d.bools[4], e.bools[0], e.bools[4]);
  if (why.empty())
    return;
  ++vf.draws_wrong;
  vf.wrong_vs += n_vs ? 1 : 0;
  vf.wrong_world += n_world ? 1 : 0;
  vf.wrong_ps += n_ps ? 1 : 0;
  vf.wrong_fetch += n_fetch ? 1 : 0;
  vf.wrong_tex += n_tex ? 1 : 0;
  vf.wrong_state += state_diff ? 1 : 0;
  vf.wrong_geom += geom_diff ? 1 : 0;
  vf.wrong_bools += bools_diff ? 1 : 0;
  // A dozen examples per kind, so a rare kind is not drowned by a common one.
  bool tell = false;
  if ((n_vs || n_ps || n_world || n_fetch) && vf.told_regs++ < 16)
    tell = true;
  if (bools_diff && !(n_vs || n_ps) && vf.told_bools++ < 12)
    tell = true;
  if (n_tex && vf.told_texset++ < 600)
    tell = true;
  if ((state_diff || geom_diff) && vf.told++ < 12)
    tell = true;
  if (tell)
    BD_INFO("[verify] frame {} node {:016X} (visual {:08X} view {} list {}) "
            "sub {}: vs {} world {} ps {} fetch {} tex {}:{}",
            FrameStatFrameCount(), KeyOf(tag), tag.visual_va, tag.render_view,
            tag.from_list ? 1 : 0, idx, n_vs, n_world, n_ps, n_fetch, n_tex,
            why);
}

void VerifyReport(Verify &vf) {
  std::string vs_hist, ps_hist;
  for (u32 r = 0; r < 256; ++r) {
    if (vf.vs_reg_hits[r])
      vs_hist += fmt::format(" c{}:{}", r, vf.vs_reg_hits[r]);
    if (vf.ps_reg_hits[r])
      ps_hist += fmt::format(" c{}:{}", r, vf.ps_reg_hits[r]);
  }
  BD_INFO("[verify] {} nodes verified, {} wrong; {} draws, {} wrong: vs {} "
          "world {} ps {} fetch {} tex(set) {} (inherited slots differ on {}) "
          "state {} geometry {} bools {} | vs regs{} | ps regs{}",
          vf.nodes, vf.nodes_wrong, vf.draws, vf.draws_wrong, vf.wrong_vs,
          vf.wrong_world, vf.wrong_ps, vf.wrong_fetch, vf.wrong_tex,
          vf.tex_inherited, vf.wrong_state, vf.wrong_geom, vf.wrong_bools,
          vs_hist.empty() ? " none" : vs_hist.c_str(),
          ps_hist.empty() ? " none" : ps_hist.c_str());
}

} // namespace

void HostDrawCapture(const VideoState &s, const QueuedDraw &q, u32 device_guest,
                     u32 primitive_type) {
  if (t_replaying)
    return;
  const NodeTag &tag = CurrentNodeTag();
  if (!tag.valid) {
    ++store().untagged;
    return;
  }
  auto &p = t_pending;
  if (!p.valid || !p.replayable) {
    // Which gate the permanently-uncaptured nodes hit: 18 distinct nodes
    // refuse a template every frame for ever, and each is an interpreter run
    // that never goes away (2026-09-04).
    // Only real node draws: effects and UI reach here with no snapshot at all
    // and swamped the count (56,584 against 24) the first time this was
    // measured.
    if (tag.valid) {
      auto &st = store();
      std::lock_guard lock(st.mutex);
      if (p.valid)
        ++st.cap_not_replayable;
      else
        ++st.cap_invalid;
      // Keyed, not totalled: a replayed node reaches here with a valid tag and
      // no snapshot too, so the totals are dominated by draws that are working.
      // Per key, the nodes that never get a template can be picked out.
      st.cap_reason[KeyOf(tag)] = p.valid ? 2u : 1u;
    }
    return;
  }
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
  if (VertexShaderReadsBones(s.pipelineState)) {
    // The palette is gathered live at replay; keep it out of the delta.
    p.needs_bones = true;
    std::memset(p.vs + kBoneBase * 16, 0, kBoneRegs * 16);
    std::memset(t_vs_block + kBoneBase * 16, 0, kBoneRegs * 16);
    for (u32 r = kBoneBase; r < kBoneBase + kBoneRegs; ++r)
      p.vs_set[r / 32] &= ~(1u << (r % 32));
  }
  DiffBlock(p.vs, t_vs_block, p.vs_set, d.vs_delta, true);
  DiffBlock(p.ps, t_ps_block, p.ps_set, d.ps_delta, false);
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
  d.surface_mask = 0;
  for (u32 i = 0; i < 16; ++i) {
    d.textures[i] = s.textures[i];
    d.tex_va[i] = s.textures[i] ? s.textures[i]->selfVa : 0u;
    // A render-target or depth surface by its resource type: the pooled
    // ones change host pointer every frame. Classifying by contentHash
    // filed every non-mirrored texture here and inherited it from the last
    // draw - the flat cyan skirt at the village rock's base (2026-09-03).
    if (((d.tex_mask >> i) & 1u) && s.textures[i] &&
        (s.textures[i]->type == ResourceType::RenderTarget ||
         s.textures[i]->type == ResourceType::DepthStencil))
      d.surface_mask |= 1u << i;
  }
  for (u32 i = 0; i < 16; ++i) {
    d.vertex_views[i] = q.vertex_views[i];
    d.input_slots[i] = q.input_slots[i];
    d.stream_va[i] = s.vertex_stream_va[i];
    d.stream_offset[i] = s.vertex_stream_offset[i];
  }
  d.index_va = s.index_va;
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
    d.bools_set[i] = p.vs_bools_set[i] | (d.bools[i] ^ p.vs_bools_before[i]);
    d.bools_set[4 + i] = p.ps_bools_set[i] | (d.bools[4 + i] ^ p.ps_bools_before[i]);
  }
  if (t_verify.active && t_verify.key == KeyOf(tag))
    VerifyAgainstReplay(tag, d, p.needs_bones);
  p.draws.push_back(std::move(d));
}

void HostDrawCommit(const NodeTag &tag) {
  if (t_verify.active) {
    Verify &vf = t_verify;
    vf.active = false;
    ++vf.nodes;
    if (vf.next != vf.expected.size() && vf.told++ < 24)
      BD_INFO("[verify] node {:016X}: interpreter issued {} draws, replay {}",
              vf.key, vf.next, vf.expected.size());
    if (vf.draws_wrong != vf.wrong_at_node_start)
      ++vf.nodes_wrong;
    vf.wrong_at_node_start = vf.draws_wrong;
    const u32 frame = FrameStatFrameCount();
    if (frame - vf.last_report_frame >= 300) {
      vf.last_report_frame = frame;
      VerifyReport(vf);
    }
  }
  auto &p = t_pending;
  auto &st = store();
  std::lock_guard lock(st.mutex);
  Tally(st, false, tag.from_list);
  if (tag.valid && p.valid) {
    auto &r = st.runs[KeyOf(tag)];
    if (p.draws.empty())
      ++r.first;
    else
      ++r.second;
  }
  if (!p.valid || !p.replayable || p.draws.empty() || !tag.valid) {
    // A run that issued nothing, from a node that has never issued anything:
    // record that as the template rather than discarding it, so the replay can
    // honour it instead of refusing an empty one every frame.
    if (tag.valid && p.valid && p.replayable && p.draws.empty() &&
        REXCVAR_GET(bd_host_draw_empty)) {
      const auto &r = st.runs[KeyOf(tag)];
      if (r.second == 0 && r.first >= 8) {
        auto &t = st.templates[KeyOf(tag)];
        if (t.draws.empty()) {
          t.draws_nothing = true;
          t.volatile_material = false;
          t.captured_frame = FrameStatFrameCount();
        }
      }
    }
    p.valid = false;
    return;
  }
  std::vector<u16> bone_slots;
  if (p.needs_bones) {
    const u32 device_guest = LastGuestDeviceVa();
    CopyGuestVertexBlock(device_guest, t_vs_block);
    if (!RecoverBoneSlots(tag, t_vs_block, p.bone_count, bone_slots)) {
      static u32 told = 0;
      if (told++ < 4)
        BD_INFO("[node] bones not recovered: {} uploaded, palette {:08X}, "
                "visual {:08X} (list {})",
                p.bone_count, tag.palette_va, tag.visual_va,
                tag.from_list ? 1 : 0);
      st.never[KeyOf(tag)] = FrameStatFrameCount();
      p.valid = false;
      return;
    }
    static u32 said = 0;
    if (said++ < 2)
      BD_INFO("[node] bones recovered: {} slots from palette {:08X} for "
              "visual {:08X}",
              p.bone_count, tag.palette_va, tag.visual_va);
  }
  const u32 frame = FrameStatFrameCount();

  // Whatever this run wrote is the visual's freshest word on those registers.
  {
    VisualRegs &v = st.visuals[VisualKeyOf(tag)];
    for (const SubDraw &d : p.draws) {
      for (const RegDelta &r : d.vs_delta) {
        if (v.vs_frame[r.reg] == frame &&
            std::memcmp(v.vs[r.reg], r.value, 16) != 0)
          v.vs_pernode[r.reg] = true; // two writers, two values, one frame
        v.vs_frame[r.reg] = frame;
        std::memcpy(v.vs[r.reg], r.value, 16);
      }
      for (const RegDelta &r : d.ps_delta) {
        if (v.ps_frame[r.reg] == frame &&
            std::memcmp(v.ps[r.reg], r.value, 16) != 0)
          v.ps_pernode[r.reg] = true;
        v.ps_frame[r.reg] = frame;
        std::memcpy(v.ps[r.reg], r.value, 16);
      }
      for (const FetchDelta &f : d.fetch_delta) {
        v.fetch_frame[f.slot] = frame;
        std::memcpy(v.fetch[f.slot], f.dword, sizeof(f.dword));
      }
      for (u32 k = 0; k < 16; ++k) {
        if (((d.tex_mask & d.surface_mask) >> k) & 1u) {
          v.tex[k] = d.textures[k];
          v.tex_frame[k] = frame;
        }
      }
      std::memcpy(v.bools, d.bools, sizeof(v.bools));
      v.bools_frame = frame;
      if (tag.render_view < 16) {
        PassRegs &pr = st.pass_regs[tag.render_view];
        for (const RegDelta &r : d.vs_delta)
          if (r.reg < kPassVsRegs) {
            std::memcpy(pr.vs[r.reg], r.value, 16);
            pr.vs_frame[r.reg] = frame;
          }
        for (const RegDelta &r : d.ps_delta)
          if (r.reg < kPassPsRegs) {
            std::memcpy(pr.ps[r.reg], r.value, 16);
            pr.ps_frame[r.reg] = frame;
          }
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
      if (t.bone_slots != bone_slots || !MergeDraws(t.draws, p.draws)) {
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
  // The cook's unit of work: a content key per sub-draw, over the state that
  // makes a material rather than the node that happens to carry it - the
  // pixel shader, the pipeline's blend and depth, the textures by content,
  // and the pixel constants the run set. Two sub-draws with the same key want
  // the same cooked material record, whichever visual they belong to. Counted
  // here so the cook's size is known before it is written (2026-09-04).
  // Where a tree draw's material colours sit, searched rather than guessed:
  // the drift is all list draws, but a tree draw has a real mesh address and
  // the same material structure, so the offset found here applies to both.
  if (REXCVAR_GET(bd_material_source)) {
    static u32 shown = 0;
    if (!tag.from_list && tag.mesh_va && shown < 8) {
      for (const SubDraw &d : p.draws) {
        for (const RegDelta &r : d.ps_delta) {
          if (r.reg != 3 && r.reg != 4)
            continue;
          const float *want = reinterpret_cast<const float *>(r.value);
          // A needle of only 0s and 1s matches anything; wait for a material
          // with a real colour in it.
          bool interesting = false;
          for (u32 i = 0; i < 4; ++i)
            if (want[i] != 0.0f && want[i] != 1.0f)
              interesting = true;
          if (!interesting)
            continue;
          const u32 bases[5] = {tag.visual_va, tag.mesh_va,
                                bd::mem::try_load<u32>(tag.mesh_va),
                                bd::mem::try_load<u32>(tag.mesh_va + 0x10),
                                tag.ctx_va};
          const char *names[5] = {"visual", "mesh", "mesh[0]", "mesh+0x10 ptr",
                                  "ctx"};
          std::string found;
          for (u32 bi = 0; bi < 5 && found.empty(); ++bi) {
            const u32 b0 = bases[bi]; // try_load already converted them
            if (!b0)
              continue;
            for (u32 off = 0; off + 16 <= 4096 && found.empty(); off += 4) {
              bool all = true;
              for (u32 i = 0; i < 4 && all; ++i) {
                const u32 w = bd::mem::try_load<u32>(b0 + off + i * 4);
                float f;
                std::memcpy(&f, &w, 4);
                all = std::fabs(f - want[i]) <= 1e-6f * (1.0f + std::fabs(want[i]));
              }
              if (all)
                found = fmt::format(" FOUND at {}+{}", names[bi], off);
            }
          }
          // Read out of the interpreter (reblue_recomp.40.cpp): it loads the
          // material float4 from r23 + 4932 + 108, where r23 is ctx[0], the
          // visual. So visual + 5040 should hold this sub-draw's colour.
          // r23 is ctx[0]. If the host's visual_va is not that object, the
          // per-visual register cache is keyed on the wrong thing - which
          // would be the whole explanation for sibling materials rotating
          // through one slot.
          // try_load reads through be<T> and already converts; the extra
          // bswap here was double-swapping and every search below compared
          // against garbage (2026-09-04).
          const u32 ctx0 = bd::mem::try_load<u32>(tag.ctx_va);
          float vm[4] = {};
          for (u32 i = 0; i < 4; ++i) {
            const u32 w = bd::mem::try_load<u32>(ctx0 + 5040 + i * 4);
            std::memcpy(&vm[i], &w, 4);
          }
          ++shown;
          BD_INFO("[material] tree ps c{} ({:.3f} {:.3f} {:.3f} {:.3f}) "
                  "ctx0 {:08x} vs visual {:08x}, ctx0+5040 "
                  "({:.3f} {:.3f} {:.3f} {:.3f}){}{}",
                  r.reg, want[0], want[1], want[2], want[3], ctx0,
                  tag.visual_va, vm[0], vm[1], vm[2], vm[3],
                  (std::fabs(vm[0] - want[0]) < 1e-5f &&
                   std::fabs(vm[1] - want[1]) < 1e-5f)
                      ? "  <== MATCH"
                      : "",
                  found.empty() ? "" : found);
        }
      }
    }
  }
  if (REXCVAR_GET(bd_material_census)) {
    for (const SubDraw &d : p.draws) {
      u64 h = 0xC0FFEEull;
      auto mix = [&h](u64 v) {
        h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
      };
      const auto &ps = d.pipelineState;
      mix(ps.pixelShader ? XXH3_64bits(&ps.pixelShader, sizeof(void *)) : 0);
      mix(ps.vertexShader ? XXH3_64bits(&ps.vertexShader, sizeof(void *)) : 0);
      mix(u64(ps.alphaBlendEnable) | (u64(ps.srcBlend) << 1) |
          (u64(ps.destBlend) << 9) | (u64(ps.zWriteEnable) << 17) |
          (u64(ps.zFunc) << 18) | (u64(ps.cullMode) << 24));
      for (u32 k = 0; k < 16; ++k)
        if (d.textures[k])
          mix(u64(d.tex_va[k]) ^ (u64(k) << 56));
      // No lock: the capture path already holds st.mutex, and taking it again
      // on a non-recursive mutex deadlocked the app on the first frame.
      //
      // Two keys, deliberately. `h` so far is the material's *identity* - the
      // shaders, the blend and depth state, the textures - with no constants
      // in it. If that identity determines the material colours, the host can
      // build its own table and stop asking the interpreter; if one identity
      // carries several colour sets, it cannot. That is the question, and it
      // is answerable from what the host already captures (2026-09-04).
      const u64 identity = h;
      u64 colours = 0x51DEull;
      for (const RegDelta &r : d.ps_delta)
        if (r.reg == 3 || r.reg == 4)
          colours ^= XXH3_64bits(r.value, 16) + (u64(r.reg) << 40);
      for (const RegDelta &r : d.ps_delta)
        mix(XXH3_64bits(r.value, 16) ^ (u64(r.reg) << 32));
      st.material_keys.insert(h);
      auto &set = st.identity_colours[identity];
      if (set.insert(colours).second && set.size() > 1)
        ++st.identity_ambiguous;
      st.identity_total = st.identity_colours.size();
    }
  }
  t.captured_frame = frame;
  t.draws = std::move(p.draws);
  t.bone_slots = std::move(bone_slots);
  p.draws.clear();
  p.valid = false;
}

// The guest's render list (reblue_recomp.84.cpp, sub_8227F360 / sub_8227DB50):
// +4 bump cursor, +8 pool base, +12 entry pointer array, +16 array cursor,
// +20 count. An entry is (204 + bones) * 4 bytes, the bone count an s8 at
// +289 and its table at +800.
constexpr u32 kRenderListVa = 0x82DBA8F8u; // lis -32036, addi -22280
constexpr u32 kEntryMatrix = 16;
constexpr u32 kEntryPalette = 268;
constexpr u32 kEntryBoneCount = 289;

u32 RenderListCount() { return bd::mem::try_load<u32>(kRenderListVa + 20); }

u32 EntrySize(u32 entry) {
  const i32 bones = static_cast<i8>(bd::mem::try_load<u8>(entry + kEntryBoneCount));
  return (204u + u32(std::max(0, bones))) * 4u;
}

void HostListBuildCapture(const NodeTag &tag, u32 count_before) {
  if (!tag.valid || !REXCVAR_GET(bd_host_list_build))
    return;
  const u32 count_after = RenderListCount();
  if (count_after <= count_before || count_after - count_before > 64)
    return;
  const u32 array = bd::mem::try_load<u32>(kRenderListVa + 12);
  if (!array)
    return;
  Store::ListTemplate lt;
  lt.captured_frame = FrameStatFrameCount();
  for (u32 i = count_before; i < count_after; ++i) {
    const u32 entry = bd::mem::try_load<u32>(array + i * 4);
    const u32 size = entry ? EntrySize(entry) : 0;
    const u8 *bytes = entry ? bd::mem::try_at<u8>(entry) : nullptr;
    if (!bytes || size < 816)
      return;
    lt.entries.emplace_back(bytes, bytes + size);
  }
  auto &st = store();
  std::lock_guard lock(st.mutex);
  // The matrix at +16 is the node's r5 matrix as handed to DrawSingle; the
  // replay copies it from tag.matrix_va, so check that here.
  {
    const u8 *m = bd::mem::try_at<u8>(tag.matrix_va);
    if (m && std::memcmp(m, lt.entries[0].data() + kEntryMatrix, 64) == 0)
      ++st.matrix_agree;
    else
      ++st.matrix_disagree;
  }
  st.lists[KeyOf(tag)] = std::move(lt);
}

REX_EXTERN(sub_8227DB50);

bool HostSceneEye(float out[3]) {
  auto &st = store();
  std::lock_guard lock(st.mutex);
  const PassRegs &pr = st.pass_regs[3];
  if (pr.vs_frame[1] == 0)
    return false;
  std::memcpy(out, pr.vs[1], 12);
  return true;
}

bool HostDrawHasDrawTemplate(const NodeTag &tag) {
  if (!tag.valid)
    return false;
  auto &st = store();
  std::lock_guard lock(st.mutex);
  auto it = st.templates.find(KeyOf(tag));
  return it != st.templates.end() && !it->second.draws.empty() &&
         !it->second.volatile_material;
}

u32 HostListBuildStatus(const NodeTag &tag) {
  if (!tag.valid || !REXCVAR_GET(bd_host_list_build) || tag.from_list)
    return 0;
  auto &st = store();
  std::lock_guard lock(st.mutex);
  auto it = st.lists.find(KeyOf(tag));
  if (it == st.lists.end())
    return 0;
  const u32 refresh =
      static_cast<u32>(std::max(1, REXCVAR_GET(bd_host_draw_refresh)));
  if (FrameStatFrameCount() - it->second.captured_frame >= refresh)
    return 2;
  if (st.matrix_disagree > st.matrix_agree)
    return 2;
  return 1;
}

bool HostListBuildReplay(const NodeTag &tag, PPCContext &ctx, uint8_t *base) {
  if (!tag.valid || !REXCVAR_GET(bd_host_list_build) || tag.from_list)
    return false;
  auto &st = store();
  std::vector<std::vector<u8>> const *entries = nullptr;
  {
    std::lock_guard lock(st.mutex);
    auto it = st.lists.find(KeyOf(tag));
    if (it == st.lists.end())
      return false;
    const u32 refresh =
        static_cast<u32>(std::max(1, REXCVAR_GET(bd_host_draw_refresh)));
    if (FrameStatFrameCount() - it->second.captured_frame >= refresh)
      return false; // the interpreter runs once and re-records
    // Only a run that agreed on the matrix source is replayed.
    if (st.matrix_disagree > st.matrix_agree)
      return false;
    entries = &it->second.entries;
    ++it->second.replays;
  }
  const u8 *matrix = bd::mem::try_at<u8>(tag.matrix_va);
  if (!matrix)
    return false;
  const u32 r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32, r6 = ctx.r6.u32;
  u32 built = 0;
  for (const auto &image : *entries) {
    ctx.r3.u64 = image.size();
    sub_8227DB50(ctx, base); // the guest's own bump allocator
    const u32 entry = ctx.r3.u32;
    u8 *dst = entry ? bd::mem::try_at<u8>(entry) : nullptr;
    if (!dst)
      break; // the list is full; the entries already placed still draw
    std::memcpy(dst, image.data(), image.size());
    std::memcpy(dst + kEntryMatrix, matrix, 64);
    const u32 palette_be = __builtin_bswap32(tag.palette_va);
    std::memcpy(dst + kEntryPalette, &palette_be, 4);
    ++built;
  }
  ctx.r3.u32 = r3; ctx.r4.u32 = r4; ctx.r5.u32 = r5; ctx.r6.u32 = r6;
  {
    std::lock_guard lock(st.mutex);
    Tally(st, true, false);
    st.list_built += built;
    ++st.list_built_runs;
  }
  return true;
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
  // A volatile template is one whose material moves between frames, and
  // replaying it would be wrong - but only if it has something to replay. A
  // volatile *and empty* template is a stalemate: the replay refuses it for
  // being empty, and this refuses the capture that would fill it, so the node
  // interprets for ever. Measured at 19 of the 20 permanently-uncaptured nodes
  // reaching the capture with no snapshot (2026-09-04).
  if (auto it = st.templates.find(KeyOf(tag));
      it != st.templates.end() && it->second.volatile_material &&
      !it->second.draws.empty())
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
  bool sampled_verify = false;
  {
    std::lock_guard lock(st.mutex);
    auto it = st.templates.find(KeyOf(tag));
    if (it != st.templates.end() && it->second.draws_nothing &&
        it->second.draws.empty()) {
      const u32 refresh_n =
          static_cast<u32>(std::max(1, REXCVAR_GET(bd_host_draw_refresh)));
      if (frame - it->second.captured_frame < refresh_n) {
        ++st.replayed_empty;
        return true; // the node draws nothing; replaying it is doing nothing
      }
    }
    if (it == st.templates.end() || it->second.draws.empty()) {
      if (st.never.count(KeyOf(tag))) {
        ++st.why_never;
      } else {
        ++st.why_none;
        // Are these the same nodes every frame, or genuinely new geometry?
        // 20 a frame against 610 stable templates says the former, and a node
        // that never gets a template is a node the interpreter runs for ever
        // (2026-09-04).
        st.none_keys.insert(KeyOf(tag));
      }
      return false;
    }
    if (it->second.volatile_material) {
      ++st.why_volatile;
      return false;
    }
    const u32 refresh =
        static_cast<u32>(std::max(1, REXCVAR_GET(bd_host_draw_refresh)));
    if (frame - it->second.captured_frame >= refresh) {
      ++st.why_refresh;
      return false; // the interpreter runs once and refreshes the template
    }
    t = &it->second;
    // Every moving value must have been written by an interpreted node of
    // this visual in this frame; otherwise this node is the one to interpret.
    auto vit = st.visuals.find(VisualKeyOf(tag));
    v = vit != st.visuals.end() ? &vit->second : nullptr;
    // A stable register the visual's interpreted node wrote differently
    // this frame: the template is out of date (a screen-size constant after
    // a resolution change read 480x270 against 320x180 for 492 draws in the
    // verifier), so it is recaptured on the next sighting.
    // Why the drift check stays on ps c3/c4 even though it fires spuriously:
    // those are g_vObjectDiffuse and g_vObjectSpecular, per sub-draw material
    // colours, so the visual's copy belongs to whichever mesh was interpreted
    // last and a difference says nothing about *this* node. Suppressing the
    // check for them takes drift 29 -> 0 a frame and host-issued draws 450 ->
    // 474 - and the replay verifier prices it at ps c4 wrong on 5,718 draws
    // against 1,354, because the check was also catching genuine material
    // animation that the 16-frame refresh would otherwise let sit stale
    // (2026-09-04). Both ways of removing it were measured and reverted.
    auto drifted = [&](const RegDelta &r, bool vertex) {
      if (!r.stable || !v)
        return false;
      // ps c3/c4 of a list draw come from its entry now, so a mismatch
      // against a sibling says nothing about the template.
      if (!vertex && tag.from_list && REXCVAR_GET(bd_material_from_entry) &&
          (r.reg == 3 || r.reg == 4))
        return false;
      const u32 *fresh = vertex ? v->vs[r.reg] : v->ps[r.reg];
      const u32 seen = vertex ? v->vs_frame[r.reg] : v->ps_frame[r.reg];
      return seen == frame && std::memcmp(fresh, r.value, 16) != 0;
    };
    const PassRegs *pr = tag.render_view < 16 ? &st.pass_regs[tag.render_view]
                                              : nullptr;
    // Drift recaptures the template, and must: reclassifying the drifting
    // register as moving and taking the visual's value instead was tried on
    // 2026-09-04 and is wrong. It lifted host-issued draws from 483 to 511 of
    // 579 and the replay verifier immediately named the price - ps c4 wrong on
    // 44,924 draws and c3 on 19,627, the exact two registers it reclassified.
    // They are per-node values, not per-visual, so the visual's last
    // interpreted node does not hold this node's. The way to host-issue those
    // draws is for the host to know what c3 and c4 mean, which is the material
    // cook, not a copy from a neighbour.
    for (const SubDraw &d : t->draws) {
      for (const RegDelta &r : d.vs_delta) {
        if (r.reg < kPassVsRegs)
          continue; // the pass camera: composed below
        if (!r.stable && (!v || v->vs_frame[r.reg] != frame)) {
          ++st.stale_bail;
          return false;
        }
        if (drifted(r, true)) {
          ++st.why_drift;
          if (r.reg < 256)
            ++st.drift_vs[r.reg];
          it->second.captured_frame = 0;
          return false;
        }
      }
      for (const RegDelta &r : d.ps_delta) {
        if (r.reg < kPassPsRegs)
          continue;
        if (!r.stable && (!v || v->ps_frame[r.reg] != frame)) {
          ++st.stale_bail;
          return false;
        }
        if (drifted(r, false)) {
          ++st.why_drift;
          if (r.reg < 256)
            ++st.drift_ps[r.reg];
          // What the drifting value is a function of. ps c3 is
          // g_vObjectDiffuse and c4 g_vObjectSpecular (the recompiled
          // shader's own names), which the guest copies per draw from
          // visual+0xBBC into visual+0xD4C - so if the host is to compute
          // them instead of watching the interpreter, this is the source to
          // check (2026-09-04).
          if (REXCVAR_GET(bd_material_diag)) {
            static u32 shown = 0;
            if (shown++ < 10) {
              const float *tpl = reinterpret_cast<const float *>(r.value);
              const float *now = reinterpret_cast<const float *>(v->ps[r.reg]);
              float mc[4] = {}, src[4] = {};
              for (u32 i = 0; i < 4; ++i) {
                const u32 a = bd::mem::try_load<u32>(
                    tag.visual_va + kVisualMaterialColor + i * 4);
                const u32 b = bd::mem::try_load<u32>(tag.visual_va + 0xBBC + i * 4);
                std::memcpy(&mc[i], &a, 4);
                std::memcpy(&src[i], &b, 4);
              }
              // Where a list draw's own material lives is still open. The
              // render-list loop (sub_8227F360) uploads PS c0..c13 in one
              // SetPixelShaderConstantFN(device, 0, r30 + 80, 14), so the
              // register sits at that buffer + N*16 - but r30 is not the
              // entry. Tried and wrong: entry + 80 + N*16 (all zeros) and
              // entry + 468 + N*16 (garbage; that came from r30 = r31 + 388
              // further up the loop, which is a different r30). Read the loop
              // properly rather than guess a third (2026-09-04).
              // Search rather than guess: scan the entry for the float4 the
              // interpreter is about to upload. If the loop's own buffer is
              // inside the entry, the value is there and this reports the
              // offset; if nothing matches, it is not in the entry at all.
              std::string found;
              // Two places a material could live: the per-frame render-list
              // entry, and the visual that owns the mesh. Search both.
              const u32 bases[2] = {tag.from_list ? tag.matrix_va - 16 : 0,
                                    tag.visual_va};
              const char *names[2] = {"entry", "visual"};
              for (u32 bi = 0; bi < 2 && found.empty(); ++bi) {
                const u32 entry = bases[bi];
                if (!entry)
                  continue;
                for (u32 off = 0; off + 16 <= 8192 && found.empty(); off += 4) {
                  bool all = (tpl[0] != 0.0f || tpl[1] != 0.0f || tpl[2] != 0.0f ||
                              tpl[3] != 0.0f); // all-zero matches any hole
                  for (u32 i = 0; i < 4 && all; ++i) {
                    const u32 b = bd::mem::try_load<u32>(entry + off + i * 4);
                    float f;
                    std::memcpy(&f, &b, 4);
                    // The template's value, not the fresh one: fresh is a
                    // sibling mesh's material, the template is this node's own.
                    all = std::fabs(f - tpl[i]) <= 1e-6f * (1.0f + std::fabs(tpl[i]));
                  }
                  if (all)
                    found = fmt::format(" FOUND at {}+{}", names[bi], off);
                }
              }
              if (found.empty())
                found = " not in entry or visual +0..8192";
              BD_INFO("[material] ps c{} drift{}: template ({:.3f} {:.3f} {:.3f} "
                      "{:.3f}) fresh ({:.3f} {:.3f} {:.3f} {:.3f}){}",
                      r.reg, tag.from_list ? " (list)" : " (tree)", tpl[0],
                      tpl[1], tpl[2], tpl[3], now[0], now[1], now[2], now[3],
                      found);
            }
          }
          it->second.captured_frame = 0;
          return false;
        }
      }
      for (const FetchDelta &f : d.fetch_delta)
        if (!f.stable && (!v || v->fetch_frame[f.slot] != frame)) {
          ++st.stale_bail;
          return false;
        }
      // A render-target slot needs this frame's binding by the visual's
      // interpreted node in this pass; before that node ran, this one
      // interprets (a replay that inherited the slot painted the ground
      // with the reflection map, 2026-09-03).
      for (u32 k = 0; k < 16; ++k) {
        if (((d.tex_mask & d.surface_mask) >> k) & 1u &&
            (!v || v->tex_frame[k] != frame)) {
          ++st.stale_bail;
          return false;
        }
        // An ordinary texture whose object was reused since the capture.
        if (((d.tex_mask & ~d.surface_mask) >> k) & 1u && d.textures[k] &&
            d.textures[k]->selfVa != d.tex_va[k]) {
          ++st.stale_tex;
          it->second.captured_frame = 0; // recapture on the next sighting
          return false;
        }
      }
    }
    // The pass's camera block has to have been written this frame by an
    // interpreted draw of this view; until then this draw interprets (and
    // writes it, if it is the first entry of its visual).
    for (u32 r = 0; r < kPassVsRegs; ++r)
      if (!pr || pr->vs_frame[r] != frame) {
        ++st.why_pass;
        return false;
      }
    for (u32 r = 0; r < kPassPsRegs; ++r)
      if (!pr || pr->ps_frame[r] != frame) {
        ++st.why_pass;
        return false;
      }
    // Sampled verification: every Nth replay candidate is composed and then
    // interpreted and diffed, in an otherwise normal run, so a run that goes
    // wrong reports what its replays would have composed differently.
    static u32 sample_counter = 0;
    const i32 every = REXCVAR_GET(bd_host_draw_verify_every);
    sampled_verify = every > 0 && (++sample_counter % static_cast<u32>(every)) == 0;
    if (!REXCVAR_GET(bd_host_draw_verify) && !sampled_verify) {
      Tally(st, true, tag.from_list);
      ++it->second.replays;
    }
  }
  // The streams as the guest holds them now. The plume buffer behind a
  // physical block moves under the template (streaming, refresh); a slot
  // whose buffer is gone refuses the replay and recaptures.
  struct ResolvedStreams {
    plume::RenderVertexBufferView views[16];
    plume::RenderIndexBufferView index;
    // The guest bytes behind the streams and the index buffer, for the
    // coarse lists (mesh_lod.cpp) to read positions and indices from.
    u32 mirror_va[16]{};
    u32 mirror_size[16]{};
    u32 index_mirror_va = 0;
    u32 index_mirror_size = 0;
  };
  static thread_local std::vector<ResolvedStreams> resolved;
  resolved.resize(t->draws.size());
  const bool verify_requested =
      REXCVAR_GET(bd_host_draw_verify) || sampled_verify;
  const u64 phys_gen = PhysicalBufferGeneration();
  const bool fast = REXCVAR_GET(bd_host_draw_fast);
  for (size_t di = 0; di < t->draws.size(); ++di) {
    const SubDraw &d = t->draws[di];
    ResolvedStreams &rs = resolved[di];
    const bool cached = fast && d.cached_generation == phys_gen;
    for (u32 k = 0; k < 16; ++k) {
      rs.views[k] = d.vertex_views[k];
      if (!d.stream_va[k] || !d.vertex_views[k].buffer.ref)
        continue;
      GuestBuffer *b = cached ? d.cached_stream[k] : nullptr;
      if (!b)
        b = HostResourceHeap::FromGuest<GuestBuffer>(d.stream_va[k]);
      if (!b)
        b = ResolveGuestBufferVa(d.stream_va[k], ResourceType::VertexBuffer);
      d.cached_stream[k] = b;
      if (!b || !b->hasBuffer()) {
        std::lock_guard lock(st.mutex);
        ++st.stale_buffer;
        if (auto it = st.templates.find(KeyOf(tag)); it != st.templates.end())
          it->second.captured_frame = 0;
        return false;
      }
      rs.views[k].buffer = b->bufferRef(d.stream_offset[k]);
      rs.views[k].size = d.stream_offset[k] < b->dataSize
                             ? b->dataSize - d.stream_offset[k]
                             : 0;
      rs.mirror_va[k] = b->guestMirrorVa;
      rs.mirror_size[k] = b->dataSize;
      if (rs.views[k].buffer.ref != d.vertex_views[k].buffer.ref)
        ++st.moved_buffer; // the plume buffer changed under the template
    }
    rs.index = d.index_view;
    if (d.indexed && d.index_va) {
      GuestBuffer *ib = cached ? d.cached_index : nullptr;
      if (!ib)
        ib = HostResourceHeap::FromGuest<GuestBuffer>(d.index_va);
      if (!ib)
        ib = ResolveGuestBufferVa(d.index_va, ResourceType::IndexBuffer);
      d.cached_index = ib;
      if (!ib || !ib->hasBuffer()) {
        std::lock_guard lock(st.mutex);
        ++st.stale_buffer;
        if (auto it = st.templates.find(KeyOf(tag)); it != st.templates.end())
          it->second.captured_frame = 0;
        return false;
      }
      rs.index.buffer = ib->bufferRef(0);
      rs.index.size = ib->dataSize;
      rs.index_mirror_va = ib->guestMirrorVa;
      rs.index_mirror_size = ib->dataSize;
    }
    d.cached_generation = phys_gen;
  }
  // The shadow and reflection views draw coarse lists over the same
  // vertices (mesh_lod.h). Not under the verifier: it compares against the
  // interpreter's own draw.
  float lod_cell = 0.0f; // the scene view: a cell of a few pixels at distance
  i32 lod_grid = (verify_requested || !REXCVAR_GET(bd_lod)) ? 0
                       : tag.render_view == 1 ? REXCVAR_GET(bd_lod_shadow_grid)
                       : tag.render_view == 0 ? REXCVAR_GET(bd_lod_reflection_grid)
                                              : 0;
  const bool verify = REXCVAR_GET(bd_host_draw_verify) || sampled_verify;
  if (verify) {
    t_verify.active = false;
    t_verify.key = KeyOf(tag);
    t_verify.next = 0;
    t_verify.expected.clear();
  }

  // The foliage vector for this node, when its visual is foliage.
  Foliage foliage;
  const bool has_foliage = ComputeFoliage(tag, foliage);

  // The world rows for every draw of the node, from its palette slot.
  float world_rows[16];
  {
    float m[16];
    if (const u8 *src = bd::mem::try_at<u8>(tag.matrix_va)) {
      // One translation for the 64 bytes; the guest holds them big-endian.
      for (u32 i = 0; i < 16; ++i) {
        u32 bits;
        std::memcpy(&bits, src + i * 4, 4);
        bits = __builtin_bswap32(bits);
        std::memcpy(&m[i], &bits, sizeof(float));
      }
    } else {
      std::memset(m, 0, sizeof(m));
    }
    for (u32 r = 0; r < 4; ++r) {
      world_rows[r * 4 + 0] = m[0 * 4 + r];
      world_rows[r * 4 + 1] = m[1 * 4 + r];
      world_rows[r * 4 + 2] = m[2 * 4 + r];
      world_rows[r * 4 + 3] = m[3 * 4 + r];
    }
    // The scene view's distance LOD, from the view distance the walk
    // published for this node (the sphere centre through the node matrix; a
    // terrain piece's matrix is identity and its translation says nothing).
    // Render-list entries have no published distance yet and skinned nodes
    // keep full detail (a character's silhouette is the point of it).
    if (lod_grid == 0 && tag.render_view == 3 && t->bone_slots.empty() &&
        REXCVAR_GET(bd_lod) && !verify_requested &&
        REXCVAR_GET(bd_lod_scene_distance) > 0.0) {
      // A render-list entry carries the depth the guest sorts the list by
      // (sub_8227F290 compares entry+276); the entry lies 16 bytes before
      // its inline world matrix, which the tag points at.
      float dist = 0.0f;
      if (tag.from_list) {
        const u32 bits = bd::mem::try_load<u32>(tag.matrix_va - 16 + 276);
        float key;
        std::memcpy(&key, &bits, sizeof(key));
        dist = std::isfinite(key) ? std::fabs(key) : 0.0f;
      } else {
        dist = std::sqrt(float(bd::engine::LastNodeViewDistanceSq()));
      }
      const float d0 = float(REXCVAR_GET(bd_lod_scene_distance));
      if (dist > d0) {
        // Half-octave bands of distance, so a mesh holds a few lists.
        const float band = std::exp2(std::round(2.0f * std::log2(dist)) * 0.5f);
        lod_cell = band * float(REXCVAR_GET(bd_lod_scene_cell));
        lod_grid = 1; // marks the request; the cell decides the grid
      }
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

  t_replaying = true;
  MaterialOverride ov;
  // The live blocks are byte-swapped once per template, not once per
  // sub-draw: nothing writes the device's constants between the sub-draws of
  // a replay, and the two 4 KB swaps per draw were 2.7% of the Draw Thread's
  // samples on the desktop (2026-09-03), scalar on the Quest's ARM64 tail.
  alignas(16) static thread_local u8 vs_base[kBlockBytes];
  alignas(16) static thread_local u8 ps_base[kBlockBytes];
  // The guest's blocks change only through its constant setters (hooked, a
  // generation each write); between two host-issued nodes nothing writes
  // them, so the previous copy stands (bd_host_draw_fast).
  {
    static thread_local u64 copied_gen = 0;
    static thread_local u32 copied_device = 0;
    const u64 gen = GuestConstantWriteGeneration();
    if (!fast || copied_gen != gen || copied_device != device_guest) {
      CopyGuestVertexBlock(device_guest, vs_base);
      CopyGuestPixelBlock(device_guest, ps_base);
      copied_gen = gen;
      copied_device = device_guest;
    }
  }
  for (size_t di = 0; di < t->draws.size(); ++di) {
    const SubDraw &d = t->draws[di];
    const ResolvedStreams &rs = resolved[di];
    u32 lod_count = d.count, lod_start = d.start_index,
        lod_prim = d.primitive_type;
    // The constant sources: the live files, the template's stable values,
    // the visual's fresh values, and the world rows.
    std::memcpy(t_vs_block, vs_base, kBlockBytes);
    std::memcpy(t_ps_block, ps_base, kBlockBytes);
    // The pass camera's own registers are composed below and the check above
    // deliberately skips them, so a node whose visual has no interpreted draw
    // this frame reaches here with a null `v` - and a moving delta at c0/c1
    // then dereferenced it. That is every multiview XR frame under xrsim
    // (ACCESS_VIOLATION reading v->vs[1], 2026-09-04); the value would be
    // overwritten by the pass block a few lines down in any case.
    for (const RegDelta &r : d.vs_delta) {
      if (r.reg < kPassVsRegs)
        continue;
      std::memcpy(t_vs_block + r.reg * 16, r.stable ? r.value : v->vs[r.reg], 16);
    }
    for (const RegDelta &r : d.ps_delta) {
      if (r.reg < kPassPsRegs)
        continue;
      std::memcpy(t_ps_block + r.reg * 16, r.stable ? r.value : v->ps[r.reg], 16);
    }
    // A render-list draw's own pixel constants, straight from its entry.
    //
    // The loop uploads them itself - SetPixelShaderConstantFN(device, 0,
    // r30 + 80, 14) with r30 = entry + 388 - so register N is at
    // entry + 468 + N*16, and the search confirms it: c3's captured value
    // turns up at exactly entry+516. That removes the sibling guesswork for
    // the per-object material colours, which is what all the template drift
    // was (2026-09-04).
    if (tag.from_list && REXCVAR_GET(bd_material_from_entry)) {
      const u32 entry = tag.matrix_va - 16;
      for (u32 reg = 3; reg <= 4; ++reg) {
        u32 v[4];
        bool ok = true;
        for (u32 i = 0; i < 4; ++i) {
          v[i] = bd::mem::try_load<u32>(entry + 468 + reg * 16 + i * 4);
          float f;
          std::memcpy(&f, &v[i], 4);
          if (!std::isfinite(f))
            ok = false;
        }
        if (ok)
          std::memcpy(t_ps_block + reg * 16, v, 16);
      }
    }
    // A tree draw has no entry, but both operands of the same formula are
    // addressable: the base is a float4 at visual + 3404, and the staging
    // struct at 0x82DE80D8 holds the gate at +392 and a per-component
    // modulator at +396..+404. The interpreter stores the base straight
    // through when the gate is positive, and base * modulator otherwise
    // (loc_822807F4 / loc_82280824). Reproducing that removes the sibling
    // inference for tree draws too (2026-09-04).
    if (!tag.from_list && REXCVAR_GET(bd_material_from_visual) &&
        tag.visual_va) {
      constexpr u32 kStaging = 0x82DE80D8u;
      float base[4];
      bool ok = true;
      for (u32 i = 0; i < 4; ++i) {
        const u32 w = bd::mem::try_load<u32>(tag.visual_va + 3404 + i * 4);
        std::memcpy(&base[i], &w, 4);
        if (!std::isfinite(base[i]))
          ok = false;
      }
      const i32 gate = static_cast<i32>(bd::mem::try_load<u32>(kStaging + 392));
      if (ok) {
        float out[4] = {base[0], base[1], base[2], base[3]};
        if (gate <= 0) {
          for (u32 i = 0; i < 3; ++i) {
            const u32 w = bd::mem::try_load<u32>(kStaging + 396 + i * 4);
            float m;
            std::memcpy(&m, &w, 4);
            if (std::isfinite(m))
              out[i] = base[i] * m;
          }
        }
        std::memcpy(t_ps_block + 3 * 16, out, 16);
      }
    }
    // The pass camera, from this frame's interpreted draws of this view.
    {
      const PassRegs &pass = st.pass_regs[tag.render_view];
      for (u32 r = 0; r < kPassVsRegs; ++r)
        std::memcpy(t_vs_block + r * 16, pass.vs[r], 16);
      for (u32 r = 0; r < kPassPsRegs; ++r)
        std::memcpy(t_ps_block + r * 16, pass.ps[r], 16);
    }
    std::memcpy(t_vs_block + 20 * 16, world_rows, sizeof(world_rows));
    if (!t->bone_slots.empty())
      GatherBones(tag, t->bone_slots, t_vs_block);
    u32 bools[8];
    // The bits the node's run set come from the template; the rest (the
    // pass and visual bits the guest toggles - VS bit 30, PS bit 5 in the
    // verifier) from the live device, which the guest's own visual switch
    // wrote before any replay of the visual. Taking everything live gave
    // the ground pieces at the rock the previous node's PS bits 0 and 3
    // (sampled verifier, 2026-09-03); taking everything from the template
    // gave 503 draws the capture frame's pass bits.
    // The bits the node did not set are the visual's: an earlier node of
    // the visual in the guest's order set them and nothing cleared them
    // (PS bit 5 on the ground pieces, sampled verifier 2026-09-03), so they
    // come from the visual's interpreted node this frame, and from the live
    // device only before it has one.
    const bool visual_fresh = v && v->bools_frame == frame;
    for (u32 i = 0; i < 4; ++i) {
      const u32 rest_vs = visual_fresh ? v->bools[i]
                                       : static_cast<u32>(dev->vsBoolConstants[i]);
      const u32 rest_ps = visual_fresh ? v->bools[4 + i]
                                       : static_cast<u32>(dev->psBoolConstants[i]);
      bools[i] = (rest_vs & ~d.bools_set[i]) | (d.bools[i] & d.bools_set[i]);
      bools[4 + i] = (rest_ps & ~d.bools_set[4 + i]) |
                     (d.bools[4 + i] & d.bools_set[4 + i]);
    }
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
      for (u32 k = 0; k < 16; ++k) {
        if (((d.tex_mask & ~d.surface_mask) >> k) & 1u) {
          s.textures[k] = d.textures[k];
        } else if (((d.tex_mask & d.surface_mask) >> k) & 1u) {
          // A render-target slot: this frame's binding by the visual's
          // interpreted node in this pass, never the capture's pointer.
          if (v && v->tex_frame[k] == frame && v->tex[k])
            s.textures[k] = v->tex[k];
          else
            ++st.surface_inherited;
        } else if (d.textures[k] &&
                   d.textures[k]->type == ResourceType::Texture &&
                   d.textures[k]->selfVa == d.tex_va[k]) {
          // A slot the node did not set: the guest inherits it from whatever
          // drew before, in its own order. The host draws in another order
          // and skips nodes, so the replay binds what the capture saw there
          // (the terrain skirt's texture at the village rock, 2026-09-03).
          s.textures[k] = d.textures[k];
        }
      }
      std::memcpy(s.vertex_views, rs.views, sizeof(s.vertex_views));
      std::memcpy(s.input_slots, d.input_slots, sizeof(s.input_slots));
      s.bound_vertex_first = d.vertex_first;
      s.bound_vertex_count = d.vertex_count;
      s.index_view = rs.index;
      if (lod_grid > 0 && d.indexed && rs.index.buffer.ref &&
          d.pipelineState.vertexDeclaration) {
        const GuestVertexDeclaration *decl = d.pipelineState.vertexDeclaration;
        for (u32 ei = 0; ei < decl->vertexElementCount; ++ei) {
          const GuestVertexElement &el = decl->vertexElements[ei];
          if (el.usage != u8(D3DDeclUsage::kPosition) || el.usageIndex != 0)
            continue;
          const u32 stream = u32(el.stream);
          if (stream >= 16 || !rs.views[stream].buffer.ref ||
              !rs.mirror_va[stream] || !d.input_slots[stream].stride)
            break;
          MeshLodRequest req;
          req.device = Video::HostDevice();
          req.index_buffer = rs.index.buffer.ref;
          req.index_mirror_va = rs.index_mirror_va;
          req.index_mirror_size = rs.index_mirror_size;
          req.index_format = rs.index.format;
          req.start_index = d.start_index;
          req.count = d.count;
          req.primitive_type = d.primitive_type;
          req.base_vertex = d.base_vertex;
          req.vertex_buffer = rs.views[stream].buffer.ref;
          req.vertex_mirror_va = rs.mirror_va[stream];
          req.vertex_mirror_size = rs.mirror_size[stream];
          req.stream_offset = d.stream_offset[stream];
          req.stride = d.input_slots[stream].stride;
          req.position_offset = u32(el.offset);
          req.position_type = u32(el.type);
          req.grid = lod_cell > 0.0f ? 0u : u32(lod_grid);
          req.cell = lod_cell;
          MeshLodResult res;
          if (MeshLodFor(req, res)) {
            s.index_view = res.view;
            lod_count = res.count;
            lod_start = 0;
            lod_prim = u32(rex::graphics::xenos::PrimitiveType::kTriangleList);
          }
          break;
        }
      }
      Video::SetAlphaThreshold(d.alpha_threshold);
      mark_dirty();
      s.material_override = &ov;
    }
    if (verify) {
      // What the replay would dispatch, kept for the interpreter's draws
      // to be checked against (VerifyAgainstReplay).
      VerifyDraw e;
      std::memcpy(e.vs, t_vs_block, kBlockBytes);
      std::memcpy(e.ps, t_ps_block, kBlockBytes);
      std::memcpy(e.fetch, t_fetch, sizeof(e.fetch));
      std::memcpy(e.bools, bools, sizeof(e.bools));
      {
        std::lock_guard lock(s.mutex);
        std::memcpy(e.textures, s.textures, sizeof(e.textures));
        e.pipelineState = s.pipelineState;
        std::memcpy(e.vertex_views, s.vertex_views, sizeof(e.vertex_views));
        std::memcpy(e.input_slots, s.input_slots, sizeof(e.input_slots));
        e.index_view = s.index_view;
        e.vertex_first = s.bound_vertex_first;
        e.vertex_count = s.bound_vertex_count;
        e.alpha = Video::AlphaThreshold();
        s.material_override = nullptr;
      }
      e.count = d.count;
      e.start_index = d.start_index;
      e.base_vertex = d.base_vertex;
      e.start_vertex = d.start_vertex;
      e.indexed = d.indexed;
      e.primitive_type = d.primitive_type;
      t_verify.expected.push_back(e);
      continue;
    }
    bd::gpu::hooks::DispatchHostNodeDraw(device_guest, lod_prim, d.indexed,
                                         lod_count, lod_start, d.base_vertex,
                                         d.start_vertex);
  }
  if (lod_grid > 0)
    MeshLodLogMaybe();
  t_replaying = false;
  if (verify) {
    // The interpreter runs this node now, from the state it found: put the
    // host state back and check its draws at capture.
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
    t_verify.active = true;
    return false;
  }
  // The guest's device block is not written back: a replay that wrote the
  // template's bool constants into it left a persistent flat patch (the
  // guest inherits bools it believes it set; 2026-09-03).
  {
    // The host bindings go back to what the node found: the interpreter's
    // deferred-state shadow (0x82DD80D8) still describes the last interpreted
    // node, and the next interpreted node skips a set the shadow calls
    // current, which is right only if the host state is the shadow's.
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
