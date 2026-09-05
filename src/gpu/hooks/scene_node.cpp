/**
 * @file    gpu/hooks/scene_node.cpp
 * @brief   The seam for replacing the guest's per-node draw submission with
 *          host code.
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 * @license     BSD 3-Clause - see LICENSE
 */

// bdSceneNodeDrawSingle (0x8227FEE8, 0x1E3C bytes) is the per-node draw
// submission: ~2084 calls a frame on device, more than any other named guest
// function, and about 370 guest memory operations each - 285 stw, 245 lwz,
// 100 stfs, 65 lfs - marshalling a transform and a material into big-endian
// guest memory so that a Xenos command processor could read it back. Our hooks
// then read it straight back out. That round trip is the X360 ABI this port is
// supposed to remove.
//
// Replacing it wholesale means reproducing 1,935 guest instructions and 73
// calls to 38 distinct functions, including five D3DDevice_SetTexture (already
// host), eleven bdSetSamplerState (which the guest itself early-outs on an
// unchanged value) and two bdSetRenderState. That is not a change to make in
// one step and hope.
//
// So this takes the seam first and does nothing with it. REX_HOOK_RAW defines
// a strong `bdSceneNodeDrawSingle`, which overrides the weak alias the
// recompiler emits, and the body tail-calls __imp__bdSceneNodeDrawSingle - the
// always-original entry point. Behaviour is unchanged by construction, and
// what it proves is that the override links and runs for THIS symbol, which
// was an open question: the same mechanism fails with `duplicate symbol` on
// Visual__DrawVerticesUP for reasons still unexplained.
//
// With the seam held, work moves host-side one piece at a time, each verified
// against a capture, instead of as one 1,935-instruction leap.

#include <atomic>
#include <cstring>
#include <string>

#include <fmt/format.h>
#include <xxhash.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc/context.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/scene/guest_scene.h"
#include "gpu/scene/host_draw.h"
#include "gpu/scene/node_tag.h"
#include "gpu/scene/scene_recorder.h"

extern "C" void __imp__bdSceneNodeDrawSingle(PPCContext &__restrict ctx,
                                             uint8_t *base);

REXCVAR_DECLARE(bool, bd_node_write_diag);
REXCVAR_DECLARE(bool, bd_material_source);
REXCVAR_DECLARE(bool, bd_draw_ledger);

namespace {
std::atomic<u64> g_node_calls{0};

// Which constant registers the per-node interpreter writes. Snapshot the
// guest's vertex and pixel register files on entry, diff on return, and
// after a few thousand node draws print, per register, how many draws
// changed it. This is what a host-issued node draw has to reproduce; the
// registers it does NOT touch stay whatever the material or the pass set.
constexpr u32 kRegBytes = 256 * 16;
thread_local u8 t_vs_before[kRegBytes];
thread_local u8 t_ps_before[kRegBytes];
u32 g_diag_hist_vs[256];
u32 g_diag_hist_ps[256];
u32 g_diag_draws = 0;
bool g_diag_told = false;

void DiagBefore(u32 device) {
  const auto *vs = bd::mem::try_at<const u8>(device + 0x700);
  const auto *ps = bd::mem::try_at<const u8>(device + 0x1700);
  if (!vs || !ps)
    return;
  std::memcpy(t_vs_before, vs, kRegBytes);
  std::memcpy(t_ps_before, ps, kRegBytes);
}

void DiagAfter(u32 device) {
  const auto *vs = bd::mem::try_at<const u8>(device + 0x700);
  const auto *ps = bd::mem::try_at<const u8>(device + 0x1700);
  if (!vs || !ps)
    return;
  for (u32 r = 0; r < 256; ++r) {
    if (std::memcmp(vs + r * 16, t_vs_before + r * 16, 16) != 0)
      ++g_diag_hist_vs[r];
    if (std::memcmp(ps + r * 16, t_ps_before + r * 16, 16) != 0)
      ++g_diag_hist_ps[r];
  }
  if (++g_diag_draws == 4000 && !g_diag_told) {
    g_diag_told = true;
    std::string vs_s, ps_s;
    for (u32 r = 0; r < 256; ++r) {
      if (g_diag_hist_vs[r])
        vs_s += fmt::format(" c{}:{}", r, g_diag_hist_vs[r]);
      if (g_diag_hist_ps[r])
        ps_s += fmt::format(" c{}:{}", r, g_diag_hist_ps[r]);
    }
    BD_INFO("[node] registers changed by bdSceneNodeDrawSingle over {} node "
            "draws - VS:{} | PS:{}",
            g_diag_draws, vs_s.empty() ? " none" : vs_s,
            ps_s.empty() ? " none" : ps_s);
  }
}
} // namespace

REX_HOOK_RAW(bdSceneNodeDrawSingle) {
  // Proof the override is live, once. A hook on a function nobody has watched
  // fire is a guess - this file exists to remove that doubt before anything is
  // built on it.
  const u64 n = g_node_calls.fetch_add(1, std::memory_order_relaxed);
  if (n == 0)
    BD_INFO("[node] host bdSceneNodeDrawSingle is live - the override links");
  if (n == 200000)
    BD_INFO("[node] host bdSceneNodeDrawSingle has run {} times", n);

  // The material base, from the PowerPC rather than a guess:
  //   addi r10,r23,3404 ; addi r11,r1,336 ; lwz x4 from r10 ; stw x4 to r11
  // so a float4 at visual + 3404 is copied into the stack slots that later
  // become pixel constant c3, after a component-wise multiply by the
  // modulator at the staging struct's +396 (2026-09-04).
  if (REXCVAR_GET(bd_material_source)) {
    static u32 shown = 0;
    if (shown < 6) {
      const u32 visual = bd::mem::try_load<u32>(ctx.r6.u32);
      if (visual) {
        float b[4];
        for (u32 i = 0; i < 4; ++i) {
          const u32 w = bd::mem::try_load<u32>(visual + 3404 + i * 4);
          std::memcpy(&b[i], &w, 4);
        }
        if (b[0] != 0.0f || b[1] != 0.0f || b[2] != 0.0f) {
          ++shown;
          BD_INFO("[material] visual {:08x} base at +3404: {:.3f} {:.3f} "
                  "{:.3f} {:.3f}",
                  visual, b[0], b[1], b[2], b[3]);
        }
      }
    }
  }

  // A probe for the material base at visual + 5040 lived here on 2026-09-04
  // and is removed: the window is zero or denormal noise for every visual
  // sampled, before the interpreter has run, so that offset is not where these
  // draws' material comes from. See
  // research/20260904_1500_the-per-object-material-constants.md - the load it
  // came from is on a branch these draws do not take, and the source is still
  // open. Probing offsets has been tried enough; the next attempt should read
  // the interpreter's material path systematically.

  // Identity for the draws this call is about to issue (gpu/scene/node_tag.h):
  // the four arguments - mesh, node index, the node's palette slot, the
  // traverse context - and what the context points at. Only while the
  // recorder's window is open; the tag is the seam the host walk will later
  // stand on, and every draw of this node reaches the queue with it set.
  if (REXCVAR_GET(bd_node_write_diag) && !g_diag_told) {
    const u32 device = bd::gpu::scene::LastGuestDeviceVa();
    if (device) {
      DiagBefore(device);
      __imp__bdSceneNodeDrawSingle(ctx, base);
      DiagAfter(device);
      return;
    }
  }

  if (bd::gpu::scene::RecordingArmed() || bd::gpu::scene::HostDrawEnabled() ||
      REXCVAR_GET(bd_draw_ledger)) {
    using namespace bd::gpu::scene;
    NodeTag tag;
    tag.mesh_va = ctx.r3.u32;
    tag.node_index = ctx.r4.u32;
    tag.matrix_va = ctx.r5.u32;
    tag.ctx_va = ctx.r6.u32;
    tag.visual_va = bd::mem::try_field<u32>(tag.ctx_va,
                                            offsetof(GuestTraverseCtx, visual));
    tag.palette_va = bd::mem::try_field<u32>(
        tag.ctx_va, offsetof(GuestTraverseCtx, palette));
    tag.render_view = bd::mem::try_load<u32>(kRenderViewIdVa);
    tag.tech = bd::mem::try_field<u32>(tag.visual_va, kVisualTech);
    tag.seq = static_cast<u32>(n);
    tag.valid = true;
    SetCurrentNodeTag(tag);
    // The host issues this node's draws itself when it has a template for
    // them, and builds its render-list entries from its list record; the
    // interpreter runs otherwise, and what it writes becomes both. A node
    // with both parts replays both or neither: replaying the draws and
    // leaving the list to no one lost its translucent part on every replayed
    // frame (2026-09-03), and replaying the list while the interpreter also
    // ran would append it twice.
    const u32 list_status = HostListBuildStatus(tag);
    const bool has_draws = HostDrawHasDrawTemplate(tag);
    bool replayed = false;
    if (has_draws && list_status != 2) {
      if (HostDrawReplay(tag)) {
        replayed = true;
        if (list_status == 1 && !HostListBuildReplay(tag)) {
          static u32 told = 0;
          if (told++ < 4)
            BD_WARN("[node] list part not built after a draw replay");
        }
      }
    } else if (!has_draws && list_status == 1) {
      replayed = HostListBuildReplay(tag);
    }
    if (!replayed) {
      const bool capture = HostDrawEnabled() && HostDrawWantsCapture(tag);
      const u32 list_before = RenderListCount();
      if (capture)
        HostDrawSnapshotBefore();
      __imp__bdSceneNodeDrawSingle(ctx, base);
      if (capture)
        HostDrawCommit(tag);
      // The list part, whether or not the run also drew.
      HostListBuildCapture(tag, list_before);
    }
    ClearCurrentNodeTag();
    return;
  }

  __imp__bdSceneNodeDrawSingle(ctx, base);
}

// ---------------------------------------------------------------------------
// The deferred render list (config/hooks/render_list.toml).
//
// A node whose material is sorted or translucent leaves bdSceneNodeDrawSingle
// without a draw: the interpreter resolves its tokens into a render-list entry
// and sub_8227F360 draws the whole list later, depth-sorted. 415 of the
// village's 599 node keys drew nothing directly and 319 draws a frame arrived
// untagged (2026-09-02) - the majority of the scene. The entry carries all the
// identity a template needs, so the hook at the loop head tags it, replays
// it when a template exists (and skips the guest's iteration), or opens a
// capture that the next entry, or the function's return, commits.
extern "C" void __imp__sub_8227F360(PPCContext &__restrict ctx, uint8_t *base);

namespace {
// Render-list entry layout, from sub_8227F360's loop (reblue_recomp.84.cpp).
constexpr u32 kEntryWorld = 16;      // 4x4 matrix, bdBuildViewMatrix(entry+16)
constexpr u32 kEntryNodeIndex = 252; // foliage table index, like r4 of DrawSingle
constexpr u32 kEntryPalette = 268;   // bone palette base, 64 bytes a slot
constexpr u32 kEntryVisual = 272;
constexpr u32 kEntryBoneCount = 289; // s8
constexpr u32 kEntryBoneTable = 800; // u32 slot indices
constexpr u32 kEntryDrawParams = 280; // u16 start, count, base, +286 u16
constexpr u32 kEntryFlags = 288;      // bytes 288..294 select the passes
constexpr u32 kEntryDecl = 376;
constexpr u32 kEntryStreams = 380;
constexpr u32 kEntryIndices = 384;

thread_local bool t_list_capturing = false;
thread_local bd::gpu::scene::NodeTag t_list_tag;
std::atomic<u64> g_list_entries{0};

bd::gpu::scene::NodeTag ListEntryTag(u32 entry) {
  using namespace bd::gpu::scene;
  NodeTag tag;
  const u32 visual = bd::mem::try_load<u32>(entry + kEntryVisual);
  if (!visual)
    return tag;
  // The draw identity: geometry, draw parameters and the pass flags. Not the
  // entry's address (a pooled slot whose occupant changes with the sort) and
  // not the matrix (per frame).
  u32 id[8];
  id[0] = bd::mem::try_load<u32>(entry + kEntryDecl);
  id[1] = bd::mem::try_load<u32>(entry + kEntryStreams);
  id[2] = bd::mem::try_load<u32>(entry + kEntryIndices);
  id[3] = bd::mem::try_load<u32>(entry + kEntryDrawParams);
  id[4] = bd::mem::try_load<u32>(entry + kEntryDrawParams + 4);
  // Bytes 291 and 294 move every frame and the draw loop never reads them
  // (9518 keys with them, 644 without, 2026-09-02); 295 is scratch the loop
  // clears. The rest select the passes.
  id[5] = bd::mem::try_load<u32>(entry + kEntryFlags) & 0xFFFFFF00u;
  id[6] = bd::mem::try_load<u32>(entry + kEntryFlags + 4) & 0xFFFF0000u;
  id[7] = visual;
  const u64 h = XXH3_64bits(id, sizeof(id));
  tag.mesh_va = static_cast<u32>(h ^ (h >> 32)) | 1u;
  tag.node_index = bd::mem::try_load<u32>(entry + kEntryNodeIndex);
  tag.matrix_va = entry + kEntryWorld;
  tag.visual_va = visual;
  tag.render_view = bd::mem::try_load<u32>(kRenderViewIdVa);
  tag.tech = bd::mem::try_field<u32>(visual, kVisualTech);
  tag.seq = static_cast<u32>(
      g_list_entries.fetch_add(1, std::memory_order_relaxed));
  tag.from_list = true;
  const i32 bones = static_cast<i8>(bd::mem::try_load<u8>(entry + kEntryBoneCount));
  if (bones > 0) {
    tag.palette_va = bd::mem::try_load<u32>(entry + kEntryPalette);
    tag.bone_table_va = entry + kEntryBoneTable;
    tag.bone_count = static_cast<u32>(bones);
  }
  tag.valid = true;
  return tag;
}

void CloseListCapture() {
  if (t_list_capturing) {
    bd::gpu::scene::HostDrawCommit(t_list_tag);
    t_list_capturing = false;
  }
  bd::gpu::scene::ClearCurrentNodeTag();
}
} // namespace

// Loop head of sub_8227F360, r31 = the entry, r23 = the loop's current
// visual. True skips the iteration.
bool bdRenderListEntryHook(PPCRegister &r31, PPCRegister &r23) {
  using namespace bd::gpu::scene;
  CloseListCapture();
  if (!(RecordingArmed() || HostDrawEnabled()))
    return false;
  const NodeTag tag = ListEntryTag(r31.u32);
  if (!tag.valid)
    return false;
  if (tag.seq == 0)
    BD_INFO("[node] render-list entry hook is live");
  SetCurrentNodeTag(tag);
  // The loop's visual switch (end the previous visual, begin this one, its
  // constant block and states) happens only when r23 changes, and a skipped
  // iteration leaves r23 as it was. So the first entry of every run of a
  // visual is the guest's, and only the entries after it replay: the guest's
  // own state machine then never misses a switch (2026-09-03).
  const bool visual_current = r23.u32 == tag.visual_va;
  if (visual_current && HostDrawReplay(tag)) {
    ClearCurrentNodeTag();
    return true;
  }
  if (HostDrawEnabled() && HostDrawWantsCapture(tag)) {
    HostDrawSnapshotBefore();
    t_list_capturing = true;
    t_list_tag = tag;
  }
  return false;
}

REX_HOOK_RAW(sub_8227F360) {
  __imp__sub_8227F360(ctx, base);
  CloseListCapture();
}

// ---------------------------------------------------------------------------
// bdSetSamplerState: the fast path, host-side.
//
// Called 11 times per scene node - about 23,000 times a frame - and the guest
// itself early-outs on an unchanged value, so the overwhelming majority of
// those calls do nothing but prove they have nothing to do. Each one still pays
// a recompiled prologue: a guest stack frame pushed and popped through
// REX_STORE_U32, PPCRegister/XER/CR temporaries, and the whole indirect-call
// scaffolding, for a comparison.
//
// From the recompiled body (0x82287060, 0x78 bytes):
//
//   index  = sampler * 20 + state / 4          (srawi+addze = signed /4)
//   cached = *(u32 *)(0x82DBE330 + index * 4)
//   if (cached == value) return;               <- the common case
//   ... indirect call to a per-state setter, then update cache and a dirty flag
//
// Only the early-out is reimplemented. Anything that would actually change
// state falls through to __imp__bdSetSamplerState, so the setter dispatch, the
// cache write and the dirty flag stay exactly where they were - this cannot
// drift from guest behaviour, because on that path it *is* guest behaviour.
REXCVAR_DECLARE(bool, bd_host_sampler_state);

extern "C" void __imp__bdSetSamplerState(PPCContext &__restrict ctx,
                                         uint8_t *base);

namespace {
// 0x82DC0000 - 7376. Both halves are in the disassembly above the load.
constexpr u32 kSamplerStateCacheVa = 0x82DBE330u;
// No counters here, deliberately. Two atomic read-modify-writes per call, at
// ~23,000 calls a frame, measured +1.4% on us/draw in a within-run A/B - the
// instrumentation cost more than the guest prologue it was measuring. What it
// established before being removed: 2,000,001 of 2,000,000 calls (100.0%) take
// the early-out, so the fast path is the only path that matters.
} // namespace

REX_HOOK_RAW(bdSetSamplerState) {
  bd::gpu::scene::NoteSamplerSet(ctx.r3.u32);
  if (!REXCVAR_GET(bd_host_sampler_state)) {
    __imp__bdSetSamplerState(ctx, base);
    return;
  }

  const u32 sampler = ctx.r3.u32;
  const i32 state = ctx.r4.s32;
  const u32 value = ctx.r5.u32;

  // srawi r10,r4,2 + addze r10,r10 is signed division by 4 rounding toward
  // zero, not an arithmetic shift - they differ for negative state. States are
  // small positive offsets in practice, but matching the guest costs nothing.
  const i32 quarter = state / 4;
  const u32 index = sampler * 20u + static_cast<u32>(quarter);
  const u32 ea = kSamplerStateCacheVa + (index << 2);

  // Through the host accessor rather than REX_LOAD_U32, which lives in the
  // generated PCH: src/ should not reach into codegen output for a plain read.
  const auto *cached = bd::mem::at<const be_u32>(ea);
  if (!cached) {
    __imp__bdSetSamplerState(ctx, base);
    return;
  }

  if (u32(*cached) == value)
    return;
  __imp__bdSetSamplerState(ctx, base);
}
