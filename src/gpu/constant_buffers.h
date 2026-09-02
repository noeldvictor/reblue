/**
 * @file    gpu/constant_buffers.h
 * @brief   Shader constant upload path.
 *
 *   VS f4 constants at device+0x700, PS f4 at device+0x1700, SharedConstants
 *   bound through the main RenderPipelineLayout's three root descriptors
 *   (b0/b1/b2, space4).
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstddef>
#include <rex/types.h>

#include <plume_render_interface.h>

namespace bd::gpu {

// Mirror of the SharedConstants cbuffer (b2, space4). Layout matches the
// packoffset(c<N>) emit in the recompiled shader prelude, so reordering or
// resizing means regenerating every shader in the DXIL cache.
struct SharedConstants {
  u32 texture2DIndices[16]{};   // c0..c3,  bytes 0..63
  u32 texture3DIndices[16]{};   // c4..c7,  bytes 64..127
  u32 textureCubeIndices[16]{}; // c8..c11, bytes 128..191
  u32 samplerIndices[16]{};     // c12..c15, bytes 192..255
  u32 booleansArr[8]{};         // c16, c17 (g_BooleansArr[2]) bytes 256..287
  u32 swappedTexcoords{};       // c18.x, byte 288
  float halfPixelOffsetX{};     // c18.y, byte 292
  float halfPixelOffsetY{};     // c18.z, byte 296
  float alphaThreshold{};       // c18.w, byte 300
  u32 swappedNormals{};         // c19.x, byte 304
  u32 swappedBinormals{};       // c19.y, byte 308
  u32 swappedTangents{};        // c19.z, byte 312
  u32 swappedBlendWeights{};    // c19.w, byte 316
  u32 swappedPositions{};       // c20.x, byte 320
  // Bit N set when TEXCOORD<N> is bound R16G16(B16A16)_UINT, and the shader's
  // sintTexcoord() sign-extends to recover the X360 raw-int-as-float vfetch.
  u32 sintTexcoords{}; // c20.y, byte 324
  // Sun shadow PCF tap scale (g_ShadowPcfScale). The kernel's tap offsets are
  // shader literals in 1/1024-of-the-map UV units, so their world footprint
  // grows with the bd_shadow_distance coverage box, so the recompiler
  // multiplies each by this to hold the penumbra constant in world space.
  // max(1/distance, 1024/dimension) never scales a tap below one map texel.
  float shadowPcfScale{1.0f}; // c20.z, byte 328
  // Multiview stereo, read by every recompiled vertex shader. Zero when
  // bd_stereo is off, which makes the skew below a no-op rather than something
  // that has to be branched around.
  float stereoSeparation{}; // c20.w, byte 332
  // Half-pixel NDC nudge read only by the substituted 2D blit VS
  float blitHalfPixelOffsetX{}; // c21.x, byte 336
  float blitHalfPixelOffsetY{}; // c21.y, byte 340
  float stereoConvergence{};    // c21.z, byte 344
  u32 _pad_c21_w{};             // c21.w, byte 348
};
static_assert(sizeof(SharedConstants) == 352);

// memory stays valid until ResetFrame().
struct ConstantAllocation {
  u8 *memory = nullptr;
  plume::RenderBufferReference ref{};
  u64 gpuAddress = 0;
  u32 size = 0;
  // Base offset into the single shared constant buffer, supplied to Vulkan as
  // a dynamic uniform buffer offset at bind time. Replaces gpuAddress: a 64-bit
  // device address made every constant read an uncached global load and forced
  // OpCapability Int64, which an Adreno 740 cannot compile. D3D12 still binds a
  // root descriptor and ignores this.
  u32 dynamicOffset = 0;
};

bool TryInit();

// Rewind a frame slot's upload chunks. Caller must have awaited that slot's
// GPU fence
// (DrainSlot), never while in flight.
void ResetFrame(u32 slot);

// Byte-swap VS/PS float constants from guest device+0x700 / device+0x1700 into
// the upload heap.
// Per-eye stereo, applied to the view-projection at VS registers 32-35:
//
//   clip.x' = clip.x + eye_skew * clip.z + eye_shift * clip.w
//
// eye_skew displaces a vertex in proportion to its depth, which is the parallax
// and the whole depth cue. eye_shift moves the projection centre, setting the
// distance at which parallax is zero - the convergence plane. Together they are
// the two halves of an off-axis frustum, and skew alone puts the entire world
// behind the screen.
//
// Both zero leaves the block exactly as the guest wrote it.
//
// Both return size == 0 when the block is byte-identical to the one already
// bound on this command list, and the caller keeps the bound offset.
//
// register_mask: the 256-bit set of float4 registers the bound vertex shader
// declares (ShaderCacheEntry::constantRegisterMask); the compare and the
// content hash cover only those, so registers the guest writes and no shader
// reads do not keep the window moving. Null means every register.
ConstantAllocation UploadVertexShaderConstants(u32 device_guest,
                                               float eye_skew = 0.0f,
                                               float eye_shift = 0.0f,
                                               const u32 *register_mask = nullptr);
// Byte-swaps the guest's vertex block into the scratch StageInstanceRecord
// reads, without uploading it: a draw whose vertex shader reads the instance
// record never touches the uniform window, so the window is left where it is
// and the guest's dirty flag survives for the next plain draw.
void SnapshotVertexShaderConstants(u32 device_guest);
ConstantAllocation UploadPixelShaderConstants(u32 device_guest);

// One scene node's whole vertex constant block, as the instanced vertex
// shader variant reads it (shader_common.h BDInstanceRecord, bit-exact with
// the uniform window). Staged per draw from the block the guest wrote,
// committed to the GPU in emit order by the draw queue so that every
// instanced group's records are contiguous.
struct InstanceRecord {
  float regs[256 * 4];
};
static_assert(sizeof(InstanceRecord) == 4096);
// Records staged per frame. The GPU region is twice this per frame slot,
// because the side-by-side path pushes one draw per eye off one record.
constexpr u32 kInstanceRecordsPerFrame = 2048;

// The instance record buffer exists and is bound (binding 3 of the constant
// set). False on D3D12 and when its creation failed; nothing is instanced then.
bool InstanceRecordsReady();
// Room for one more staged record this frame. Checked BEFORE the vertex
// upload decides to leave the node ranges to the record, so a draw is never
// left with neither.
bool InstanceRecordsRoom();
// Copies the node ranges of the vertex block this draw uploaded (or kept) into
// the frame's staging list; the index, or ~0u when the frame is out of room.
u32 StageInstanceRecord();
// Copies n staged records contiguously into the GPU buffer; the first
// instance index to draw with, or ~0u when the slot's region is exhausted.
u32 CommitInstanceRecords(const u32 *staged, u32 n);

// Diagnostic: the host-visible bytes of a constant block by its dynamic
// offset (the ring is mapped for the life of the device). Null when unmapped.
const u8 *ConstantBlockBytes(u32 dynamic_offset);
// A host-filled block in the same ring, for host passes that read parameters
// through the pixel constant binding (the host post chain's composite).
ConstantAllocation UploadHostConstants(const void *data, u32 size);

// Rebuilt from live guest state every draw: sampler fetch constants and bool
// constants come from unhooked recompiled code, so there is no dirty signal.
// size == 0 means byte-identical to what is already bound on this command list
// and the caller can skip the upload + root rebind.
ConstantAllocation UploadSharedConstants(u32 device_guest);

// Root bindings do not survive begin(), so this drops the 'block still bound'
// reuse gates of the shared, vertex and pixel uploads. Call on every command
// list reset.
void InvalidateSharedBinding();

// alignment defaults to 4 (the upload buffer is WRITE_COMBINE), and CBV callers
// pass 256.
ConstantAllocation UploadGuestBytesByteSwap32(u32 guest_va, u32 size,
                                              u32 alignment = 4);

// Verbatim, no swap, for texture pixel data.
ConstantAllocation UploadGuestBytes(u32 guest_va, u32 size, u32 alignment = 4);

// For pixel data already untiled to host staging.
ConstantAllocation UploadHostBytes(const void *host_data, u32 size,
                                   u32 alignment = 4);

} // namespace bd::gpu
