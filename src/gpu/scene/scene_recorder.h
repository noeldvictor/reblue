/**
 * @file    gpu/scene/scene_recorder.h
 * @brief   Records what every scene node draw is - mesh, material, textures,
 *          transform, pass - for a window of frames, and writes it out. This
 *          is how the host learns the guest's scene tree before it walks
 *          the tree itself (stage 1 of "The direction" in CLAUDE.md).
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/types.h>

namespace bd::gpu {
struct VideoState;
struct QueuedDraw;
} // namespace bd::gpu

namespace bd::gpu::scene {

// The file (`<cache_root>/scene_walk/walk_<stamp>.bdsw`): a header, then
// flat POD arrays in host byte order. tools/scene_walk_dump.py reads it.
// Keys are content hashes, never guest addresses - the guest recycles those
// within a session and asset names collide.
constexpr char kWalkMagic[4] = {'B', 'D', 'S', 'W'};
constexpr u32 kWalkVersion = 1;

struct WalkHeader {
  char magic[4];
  u32 version;
  u32 first_frame;
  u32 frame_count;
  u32 node_count;
  u32 mesh_count;
  u32 material_count;
  u32 texture_count;
};

// One node draw of one frame.
struct NodeDrawRecord {
  u32 frame;
  u32 pass_id;     // CurrentRenderPassId()
  u32 render_view; // the guest's render-view id
  u32 seq;         // node draw ordinal
  u32 visual_va;   // per-frame identity only, never a key
  u32 node_index;
  u32 tech;
  u32 blended;
  float world[16]; // the palette slot, host floats, as the guest laid it out
  u32 palette_va;
  u32 bone_count;
  u64 mesh_key;
  u64 material_key;
  float depth_sq; // the walk's view distance, the queue's sort key
  u32 _pad;
};

struct MeshStream {
  u32 slot;
  u32 offset; // from the block base
  u32 stride;
  u32 size;
};

struct MeshRecord {
  u64 mesh_key;
  u64 block_hash; // XXH3 of the model's whole physical block
  u32 block_size;
  u32 stream_count;
  MeshStream streams[16];
  u32 ib_offset; // from the block base; ~0u when not indexed
  u32 ib_bytes;
  u32 ib_format; // plume::RenderFormat
  u32 indexed;
  u32 count;
  u32 start_index;
  i32 base_vertex;
  u32 start_vertex;
  u64 decl_hash;
  float centre[3];
  float radius;
};

struct MaterialRecord {
  u64 material_key;
  u64 vs_hash;
  u64 ps_hash;
  u64 decl_hash;
  u64 state_hash; // PipelineState with the pointers replaced by hashes
  u64 ps_block_hash; // the pixel block over the registers the PS declares
  u64 tex_key[16];   // 0 = unbound or a render surface
  u32 fetch[16][6];  // the six fetch-constant dwords per slot
  u32 tech;
  u32 spec_constants;
};

struct TextureRecord {
  u64 tex_key;
  u32 format; // plume::RenderFormat
  u32 width;
  u32 height;
  u32 depth;
  u32 mips;
  u32 array_size;
  u32 dimension; // plume::RenderTextureViewDimension
  u32 _pad;
  char name[32];
};

// Cheap, per node draw: whether the window is open. The DrawSingle hook
// fills the node tag only when this is true.
bool RecordingArmed();

// From the draw hook, once the queued draw is complete, under the video
// state's mutex.
void OnQueuedDraw(const VideoState &s, const QueuedDraw &q, u32 device_guest);

// Frame boundary, from present: opens the window at bd_scene_record_after_s,
// closes it after bd_scene_record_frames and writes the file.
void OnFrameEnd();

} // namespace bd::gpu::scene
