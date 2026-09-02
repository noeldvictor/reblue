/**
 * @file    gpu/scene/guest_scene.h
 * @brief   Big-endian views of the guest's scene structures, as read off the
 *          recompiled walk (bdSceneTreeDraw -> bdSceneNodeCullTraverse ->
 *          bdSceneNodeDrawSingle) on 2026-09-02. Every offset here is cited
 *          from generated/ and holds only for Blue Dragon's XEX.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstddef>

#include <rex/types.h>

namespace bd::gpu::scene {

// One node of a model's draw tree (reblue_recomp.38.cpp:9318-9545, the
// traversal). The walk reads the flags, descends children and siblings, and
// hands a node with geometry to bdSceneNodeDrawSingle with its matrix.
struct GuestDrawNode {
  be_u32 matrixIndex; // +0x00 index into the visual's palette (64-byte slots)
  be_u32 _04;
  be_u32 flags; // +0x08 kNodePrune / kNodeNoDraw / kNodeHasGeometry
  be_u32 mesh;  // +0x0C GuestMesh
  u8 _10[0x28];
  be_u32 child;   // +0x38
  be_u32 sibling; // +0x3C
};
static_assert(sizeof(GuestDrawNode) == 0x40);

constexpr u32 kNodePrune = 0x40000000u;       // skip this subtree
constexpr u32 kNodeNoDraw = 0x80000000u;      // recurse children, draw nothing
constexpr u32 kNodeHasGeometry = 0x00010000u; // +0x0C is a mesh

// A mesh: the token stream bdSceneNodeDrawSingle interprets (streams, decl,
// index buffer, textures, shaders, material colour), its buffer records, and
// the bounding sphere the walk culls with.
struct GuestMesh {
  be_u32 tokens;      // +0x00 u16 token stream
  be_u32 _04;
  be_u32 ibTable;     // +0x08 index buffer record table
  be_u32 _0C;
  be_u32 vbTableHdr;  // +0x10 -> [+4] vertex buffer / declaration records
  be_f32 centre[3];   // +0x14 sphere centre, model space
  be_f32 radius;      // +0x20
};
static_assert(offsetof(GuestMesh, centre) == 0x14);
static_assert(offsetof(GuestMesh, radius) == 0x20);

// The traversal context bdSceneTreeDraw builds on its stack (r1+96,
// reblue_recomp.88.cpp:9595-9656) and passes down as r6.
struct GuestTraverseCtx {
  be_u32 visual;     // +0x00 the visual object (global 0x82DBA8F8[+52])
  be_u32 sceneGraph; // +0x04 root node at sceneGraph+0x10
  be_u32 palette;    // +0x08 matrix palette base, 64 bytes per slot
  u8 _0C[0x20];
  be_f32 radiusScale; // +0x2C min of the three axis scales
};
static_assert(offsetof(GuestTraverseCtx, radiusScale) == 0x2C);

// Visual object offsets.
constexpr u32 kVisualBoneCount = 0x74C;
constexpr u32 kVisualSkinned = 0x780;
constexpr u32 kVisualWorld = 0x954;
constexpr u32 kVisualBoneContainer = 0xA40;
constexpr u32 kVisualTech = 0xBB8;         // the technique id the PSO predictor tracks
constexpr u32 kVisualMaterialColor = 0xD4C; // copied from +0xBBC..+0xBC8 per draw
constexpr u32 kVisualRenderView = 0x1A40;

// The render-view id the frustum-plane selection in sub_82287788 switches on.
constexpr u32 kRenderViewIdVa = 0x8277405Cu;

} // namespace bd::gpu::scene
