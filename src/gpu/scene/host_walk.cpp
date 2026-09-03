/**
 * @file    gpu/scene/host_walk.cpp
 * @brief   The scene tree walk on the host: bdSceneNodeCullTraverse
 *          replaced by a host function that reads the guest's draw nodes,
 *          culls them and hands the survivors to the per-node draw. Stage 2a
 *          of "The direction" in CLAUDE.md: the traversal is ours, the
 *          per-node interpreter (bdSceneNodeDrawSingle) is still the guest's.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */

// What the guest does (bdSceneNodeCullTraverse, 0x82282490, read off
// generated/reblue_recomp.38.cpp on 2026-09-02), per node of a sibling chain:
//
//   flags & 0x40000000  -> skip the node and its children, next sibling
//   flags & 0x80000000  -> no draw, children only
//   flags & 0x00010000  -> node+0x0C is a mesh, else nothing to draw
//   matrix = palette + (node.matrixIndex << 6)     a 3x4, rows at +0/+16/+32,
//                                                  translation at +48
//   centre = T + x*row0 + y*row1 + z*row2         of mesh+0x14
//   radius = ctx.radiusScale * mesh.radius         (+0x2C, +0x20)
//   bdSceneCullBiasHook(radius, &centre)           host: distance cull
//   visible = sub_82287788(&centre, radius)        the guest's frustum test
//   bdSceneCullDistanceHook(visible)               host: belt and braces
//   if (visible) {
//     if (renderView == 1) visual.nodeDrawCounts[matrixIndex]++   (+0xEE8)
//     bdSceneNodeDrawSingle(mesh, matrixIndex, matrix, ctx)
//   }
//   recurse into node.child, then continue with node.sibling
//
// This file does exactly that, iteratively, with the two host hooks and the
// guest's own visibility test called the way the guest calls them, so the
// flat path is bit-identical and the census counters keep counting. What it
// buys is ownership: the walk is host code now, and culling, LOD and the
// host-issued draw (stage 2b) attach here instead of inside 1,935 guest
// instructions.

#include <cstring>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc/context.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/frame_stats.h"
#include "gpu/occlusion_cull.h"
#include "gpu/device.h"
#include "gpu/resources.h"
#include "gpu/scene/guest_scene.h"

REXCVAR_DECLARE(bool, bd_host_walk);
REXCVAR_DECLARE(bool, bd_host_cull);
REXCVAR_DECLARE(bool, bd_host_cull_diag);
REXCVAR_DECLARE(bool, bd_occlusion_cull);
namespace {
u32 g_cull_walks = 0, g_cull_host_walks = 0, g_cull_tested = 0,
    g_cull_disagreed = 0;
} // namespace
REXCVAR_DECLARE(bool, bd_reflections);
REXCVAR_DECLARE(bool, bd_walk_skip_stubs);

REX_EXTERN(__imp__bdSceneNodeCullTraverse);
REX_EXTERN(bdSceneNodeDrawSingle);
REX_EXTERN(sub_82287788);

// The two midasm hooks the guest walk carries (config/hooks/guest_census.toml),
// defined in engine/guest_census.cpp. Called here the way the recompiled
// walk calls them.
bool bdSceneCullBiasHook(PPCRegister &f1, PPCRegister &r3);
void bdSceneCullDistanceHook(PPCRegister &r3);
bool bdSceneCullBiasHost(f64 &radius, const float c[3]);
bool bdSceneCullDistanceHost(bool visible);

namespace {

using namespace bd::gpu::scene;

inline float LoadF32(u32 va) {
  const u32 bits = bd::mem::try_load<u32>(va);
  float f;
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}

inline void StoreF32(u32 va, float f) {
  u32 bits;
  std::memcpy(&bits, &f, sizeof(bits));
  bd::mem::try_store<u32>(va, bits);
}

// The guest frame this walk stands in: the guest's own function allots 208
// bytes and keeps the transformed centre at +80, where the visibility test
// reads it. Same layout, so the test and the hooks see what they always saw.
constexpr u32 kFrameBytes = 208;
constexpr u32 kCentreOffset = 80;

thread_local std::vector<u32> t_stack;
u32 g_walks = 0;

void Walk(PPCContext &ctx, uint8_t *base, u32 root, u32 ctx_va) {
  if (!root)
    return;
  const u32 palette =
      bd::mem::try_field<u32>(ctx_va, offsetof(GuestTraverseCtx, palette));
  const float radius_scale =
      LoadF32(ctx_va + offsetof(GuestTraverseCtx, radiusScale));

  const u32 saved_r1 = ctx.r1.u32;
  const u32 frame = saved_r1 - kFrameBytes;
  bd::mem::try_store<u32>(frame, saved_r1); // the back chain, as stwu would
  ctx.r1.u32 = frame;
  const u32 centre_va = frame + kCentreOffset;

  auto &stack = t_stack;
  // The guest's visibility test, sub_82287788, on the host for its default
  // path: the cull-off switch clear and a render view other than 0, 1, 4, 5,
  // 7, 8, 9 (those take their own tables), it builds (x, y, z, r) and asks
  // sub_821CE028 whether dot((x, y, z, 1), plane) > r for any of the six
  // planes at the global table. The addresses are the function's own
  // lis/addi constants; the planes are read once per walk, not per node.
  const u32 cull_switch_va = u32(u32(-32036) << 16) + u32(-5536) + 520u;
  const u32 plane_table_va = u32(u32(-32033) << 16) + u32(-30608);
  bool host_cull = REXCVAR_GET(bd_host_cull) &&
                   bd::mem::try_load<u32>(cull_switch_va) == 0;
  // Occlusion culling applies to the camera's drawing pass, render view 3
  // (the scene draws fetch their constants under it; view 1 is the
  // collecting walk): the proxies are queried against that pass's depth
  // (gpu/occlusion_cull.h).
  const bool occlusion = REXCVAR_GET(bd_occlusion_cull) &&
                         bd::mem::try_load<u32>(kRenderViewIdVa) == 3;
  float planes[6][4];
  if (host_cull) {
    const u32 view = bd::mem::try_load<u32>(kRenderViewIdVa);
    host_cull = !(view == 0 || view == 1 || view == 4 || view == 5 ||
                  view == 7 || view == 8 || view == 9);
    const auto *pp = host_cull
                         ? bd::mem::try_at<const be_u32>(plane_table_va + 64)
                         : nullptr;
    if (!pp)
      host_cull = false;
    else
      for (u32 i = 0; i < 6; ++i)
        for (u32 k = 0; k < 4; ++k) {
          const u32 bits = static_cast<u32>(pp[i * 4 + k]);
          std::memcpy(&planes[i][k], &bits, sizeof(float));
        }
  }
  ++g_cull_walks;
  if (host_cull)
    ++g_cull_host_walks;
  {
    static u32 last_frame = 0;
    const u32 f = bd::gpu::FrameStatFrameCount();
    if (f - last_frame >= 300) {
      if (last_frame)
        BD_INFO("[cull] per frame: walks {:.1f}, host-tested walks {:.1f}, nodes "
                "tested {:.1f}, disagreements {:.2f}",
                g_cull_walks / 300.0, g_cull_host_walks / 300.0,
                g_cull_tested / 300.0, g_cull_disagreed / 300.0);
      g_cull_walks = g_cull_host_walks = g_cull_tested = g_cull_disagreed = 0;
      last_frame = f;
    }
  }
  stack.clear();
  stack.push_back(root);
  while (!stack.empty()) {
    u32 node = stack.back();
    stack.pop_back();
    while (node) {
      // One translation per node, not one per field: try_translate was 5%
      // of the Draw Thread's samples (2026-09-03 desktop profile), most of
      // it from here and the replay. An unreadable node ends its sibling
      // chain, as five null-fallback reads did.
      const auto *n = bd::mem::try_at<const GuestDrawNode>(node);
      if (!n)
        break;
      const u32 flags = static_cast<u32>(n->flags);
      const u32 next = static_cast<u32>(n->sibling);
      if (flags & kNodePrune) {
        node = next;
        continue;
      }
      const u32 child = static_cast<u32>(n->child);
      if (!(flags & kNodeNoDraw)) {
        const u32 mesh =
            (flags & kNodeHasGeometry) ? static_cast<u32>(n->mesh) : 0;
        if (mesh) {
          const u32 index = static_cast<u32>(n->matrixIndex);
          const u32 matrix = palette + (index << 6);
          // One translation per object, not one per float: the walk visits
          // every node of every visual, and the per-read validation showed
          // up in the Quest profile.
          float m[16];
          float c[3];
          const auto *mp = bd::mem::try_at<const be_u32>(matrix);
          const auto *cp = bd::mem::try_at<const be_u32>(mesh + offsetof(GuestMesh, centre));
          if (!mp || !cp)
            goto children; // nothing to draw; the subtree still walks
          for (u32 i = 0; i < 16; ++i) {
            const u32 bits = static_cast<u32>(mp[i]);
            std::memcpy(&m[i], &bits, sizeof(float));
          }
          for (u32 i = 0; i < 3; ++i) {
            const u32 bits = static_cast<u32>(cp[i]);
            std::memcpy(&c[i], &bits, sizeof(float));
          }
          float out[3];
          for (u32 k = 0; k < 3; ++k)
            out[k] = m[12 + k] + c[0] * m[k] + c[1] * m[4 + k] + c[2] * m[8 + k];
          const float radius = radius_scale * LoadF32(mesh + offsetof(GuestMesh, radius));
          bool visible = false;
          if (host_cull) {
            // Host floats end to end: the centre never goes through guest
            // scratch, and the census hooks take it as floats. The bias
            // hook may scale the radius (bd_cull_bias), so the plane test
            // uses the scaled one, as the guest test would have.
            f64 r = double(radius);
            if (!bdSceneCullBiasHost(r, out)) {
              bool outside = false;
              float dist[6];
              for (u32 i = 0; i < 6; ++i) {
                dist[i] = out[0] * planes[i][0] + out[1] * planes[i][1] +
                          out[2] * planes[i][2] + planes[i][3];
                outside = outside || f64(dist[i]) > r;
              }
              ++g_cull_tested;
              // Diagnostic: the guest's verdict beside the host's, the
              // disagreements logged with the numbers (2026-09-03).
              if (REXCVAR_GET(bd_host_cull_diag)) {
                for (u32 k = 0; k < 3; ++k)
                  StoreF32(centre_va + k * 4, out[k]);
                ctx.f1.f64 = r;
                ctx.r3.u64 = centre_va;
                sub_82287788(ctx, base);
                const bool guest_visible = ctx.r3.s32 != 0;
                if (guest_visible == outside) {
                  ++g_cull_disagreed;
                  static u32 told = 0;
                  if (told++ < 24)
                    BD_INFO("[cull] disagree view {} guest {} host {} c ({:.2f} "
                            "{:.2f} {:.2f}) r {:.3f} d [{:.3f} {:.3f} {:.3f} "
                            "{:.3f} {:.3f} {:.3f}] mesh {:08X}",
                            bd::mem::try_load<u32>(kRenderViewIdVa),
                            guest_visible ? 1 : 0, outside ? 0 : 1, out[0],
                            out[1], out[2], r, dist[0], dist[1], dist[2],
                            dist[3], dist[4], dist[5], mesh);
                }
              }
              visible = bdSceneCullDistanceHost(!outside);
            }
          } else {
            for (u32 k = 0; k < 3; ++k)
              StoreF32(centre_va + k * 4, out[k]);
            ctx.f1.f64 = double(radius);
            ctx.r3.u64 = centre_va;
            if (!bdSceneCullBiasHook(ctx.f1, ctx.r3)) {
              sub_82287788(ctx, base);
              bdSceneCullDistanceHook(ctx.r3);
              visible = ctx.r3.s32 != 0;
            }
          }
          if (visible && occlusion) {
            // A sphere that holds the camera (the terrain, the sky dome)
            // has its proxy clipped by the near plane and would read as
            // occluded; it is never tested. The centre is camera-relative.
            const f64 d2 = f64(out[0]) * out[0] + f64(out[1]) * out[1] +
                           f64(out[2]) * out[2];
            const f64 r_near = f64(radius) * 1.3 + 8.0;
            // The draw is still dispatched: the node's texture and constant
            // bindings must happen for the nodes after it, which inherit
            // them; the queue drops an occluded node's draw instead
            // (hooks/draw.cpp), keyed as the dispatch tag sees it.
            if (d2 > r_near * r_near) {
              static u32 told = 0;
              if (told++ < 3)
                BD_INFO("[occ] walk key {:016X} (matrix {:08X} mesh {:08X})",
                        (u64(matrix) << 32) | u64(mesh), matrix, mesh);
              bd::gpu::OcclusionCullNote((u64(matrix) << 32) | u64(mesh), out,
                                         radius);
            }
          }
          if (visible) {
            if (bd::mem::try_load<u32>(kRenderViewIdVa) == 1) {
              const u32 visual = bd::mem::try_field<u32>(ctx_va, offsetof(GuestTraverseCtx, visual));
              const u32 table = bd::mem::try_field<u32>(visual, kVisualNodeDrawCounts);
              if (table) {
                const u32 at = table + index;
                bd::mem::try_store<u8>(at, u8(bd::mem::try_load<u8>(at) + 1));
              }
            }
            ctx.r3.u64 = mesh;
            ctx.r4.u64 = index;
            ctx.r5.u64 = matrix;
            ctx.r6.u64 = ctx_va;
            bdSceneNodeDrawSingle(ctx, base);
          }
        }
      }
    children:
      if (child) {
        if (next)
          stack.push_back(next);
        node = child;
      } else {
        node = next;
      }
    }
  }
  ctx.r1.u32 = saved_r1;
}

} // namespace

REX_HOOK_RAW(bdSceneNodeCullTraverse) {
  if (!REXCVAR_GET(bd_host_walk)) {
    __imp__bdSceneNodeCullTraverse(ctx, base);
    return;
  }
  if (g_walks++ == 0)
    BD_INFO("[walk] host scene walk is live");
  // Which render view draws into which target, once per view id: the pass
  // gates below need the ids, and the ids are the guest's.
  {
    static u32 seen_views = 0;
    const u32 view = bd::mem::try_load<u32>(kRenderViewIdVa);
    if (view < 32 && !((seen_views >> view) & 1u)) {
      seen_views |= 1u << view;
      const auto &s = bd::gpu::state();
      const bd::gpu::GuestTexture *rt = s.render_target;
      const bd::gpu::GuestTexture *ds = s.depth_stencil;
      BD_INFO("[walk] render view {} draws into {}x{} (guest 0x{:08X}), depth "
              "{}x{}, clear flags 0x{:X} depth {:.3f} stencil {} pending {}",
              view, rt ? rt->width : 0, rt ? rt->height : 0,
              rt ? rt->selfVa : 0, ds ? ds->width : 0, ds ? ds->height : 0,
              s.clear_flags, s.clear_depth, s.clear_stencil,
              s.clear_pending ? 1 : 0);
    }
  }
  // The reflection stub. With reflections off the guest still re-renders the
  // scene into a 128x72 map (50 draws, 1.3 ms of Quest GPU with its
  // preemption, 2026-09-02); render view 0 is that pass (view 1 the shadow
  // map, view 3 the scene, logged above). Skipping its walk leaves the
  // guest's own clear in the map; the frame is pixel-identical on the
  // desktop. The shadow stub is not skipped: an empty shadow map shadows the
  // whole scene, and with shadows off the 64x64 map is blocky stripes anyway
  // - the shadow pass is host work (stage 5), not a gate.
  const u32 view = bd::mem::try_load<u32>(kRenderViewIdVa);
  if (view == 0 && REXCVAR_GET(bd_walk_skip_stubs) && !REXCVAR_GET(bd_reflections))
    return;
  Walk(ctx, base, ctx.r3.u32, ctx.r4.u32);
}
