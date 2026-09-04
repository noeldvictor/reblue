/**
 * @file    mesh_lod.h
 * @brief   Coarse index lists for the shadow and reflection views, generated
 *          from the guest's own meshes (stage 3's first generated asset).
 *
 * The shadow pass rasterises as many triangles as the scene (81k against
 * 79k in the 2026-09-04 census) and the reflection two thirds of it, both at
 * the scene's full detail, into a 1024 shadow map and a 128x72 reflection on
 * the Quest. Culling moved that little (the casters are the visible world),
 * so the caster geometry itself is what shrinks: a mesh drawn into either
 * view draws a decimated index list over its original vertex buffer.
 *
 * The decimation is vertex clustering onto original vertices: the mesh's
 * positions are read from guest memory once, snapped to a grid over the
 * mesh's bounds, each cell keeps the vertex nearest its centroid, every
 * triangle is remapped and degenerate or duplicate ones drop. Nothing about
 * the vertex format changes - the list indexes the same vertices the guest
 * draws - so the vertex shader, the pulled path and the instance records are
 * untouched; only the index view and the topology (strip -> list) differ.
 *
 * @copyright Copyright (c) 2026 reblue contributors
 * @license   MIT
 */
#pragma once

#include <memory>
#include <vector>
#include <rex/types.h>

#include "plume_render_interface.h"

namespace bd::gpu::scene {

struct MeshLodRequest {
  plume::RenderDevice *device = nullptr;
  // The source index range, as the guest draws it. The plume buffer refs are
  // identity: a physical block that refreshes gets a new buffer, and a LOD
  // built over the old one must not be served for it.
  const plume::RenderBuffer *index_buffer = nullptr;
  u32 index_mirror_va = 0; // guest bytes of the index buffer (big-endian)
  u32 index_mirror_size = 0;
  plume::RenderFormat index_format = plume::RenderFormat::R16_UINT;
  u32 start_index = 0;
  u32 count = 0;
  u32 primitive_type = 0; // xe::PrimitiveType
  i32 base_vertex = 0;
  // The position stream.
  const plume::RenderBuffer *vertex_buffer = nullptr;
  u32 vertex_mirror_va = 0; // guest bytes of the stream's buffer
  u32 vertex_mirror_size = 0;
  u32 stream_offset = 0; // the draw's offset into that buffer
  u32 stride = 0;
  u32 position_offset = 0; // within the vertex
  u32 position_type = 0;   // D3DDeclType
  // Cells across the mesh's longest axis; or, when cell > 0, the cell's size
  // in mesh units (the grid follows from the mesh's extent - the scene LOD
  // wants a cell of a few pixels at the node's distance, whatever the mesh).
  u32 grid = 0;
  float cell = 0.0f;
};

struct MeshLodResult {
  plume::RenderIndexBufferView view;
  u32 count = 0; // indices, a triangle list
  // The native asset importer consumes CPU indices, never reads upload RAM.
  std::shared_ptr<const std::vector<u32>> triangles;
};

// The coarse list for the request, built on first sight and cached. False
// when the mesh cannot be read (format, bounds) or the decimation saves too
// little to be worth a separate buffer; the caller draws the original then.
bool MeshLodFor(const MeshLodRequest &req, MeshLodResult &out);

// A `[lod]` line every 300 frames: lists built, hits, triangles in and out.
void MeshLodLogMaybe();

} // namespace bd::gpu::scene
