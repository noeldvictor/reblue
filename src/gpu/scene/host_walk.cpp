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
#include "gpu/scene/guest_scene.h"

REXCVAR_DECLARE(bool, bd_host_walk);

REX_EXTERN(__imp__bdSceneNodeCullTraverse);
REX_EXTERN(bdSceneNodeDrawSingle);
REX_EXTERN(sub_82287788);

// The two midasm hooks the guest walk carries (config/hooks/guest_census.toml),
// defined in engine/guest_census.cpp. Called here the way the recompiled
// walk calls them.
bool bdSceneCullBiasHook(PPCRegister &f1, PPCRegister &r3);
void bdSceneCullDistanceHook(PPCRegister &r3);

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
  stack.clear();
  stack.push_back(root);
  while (!stack.empty()) {
    u32 node = stack.back();
    stack.pop_back();
    while (node) {
      const u32 flags = bd::mem::try_field<u32>(node, offsetof(GuestDrawNode, flags));
      const u32 next = bd::mem::try_field<u32>(node, offsetof(GuestDrawNode, sibling));
      if (flags & kNodePrune) {
        node = next;
        continue;
      }
      const u32 child = bd::mem::try_field<u32>(node, offsetof(GuestDrawNode, child));
      if (!(flags & kNodeNoDraw)) {
        const u32 mesh = (flags & kNodeHasGeometry)
                             ? bd::mem::try_field<u32>(node, offsetof(GuestDrawNode, mesh))
                             : 0;
        if (mesh) {
          const u32 index = bd::mem::try_field<u32>(node, offsetof(GuestDrawNode, matrixIndex));
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
          for (u32 k = 0; k < 3; ++k)
            StoreF32(centre_va + k * 4, out[k]);
          const float radius = radius_scale * LoadF32(mesh + offsetof(GuestMesh, radius));

          ctx.f1.f64 = double(radius);
          ctx.r3.u64 = centre_va;
          bool visible = false;
          if (!bdSceneCullBiasHook(ctx.f1, ctx.r3)) {
            sub_82287788(ctx, base);
            bdSceneCullDistanceHook(ctx.r3);
            visible = ctx.r3.s32 != 0;
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
  Walk(ctx, base, ctx.r3.u32, ctx.r4.u32);
}
