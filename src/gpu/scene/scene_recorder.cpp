/**
 * @file    gpu/scene/scene_recorder.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/scene/scene_recorder.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <system_error>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/runtime.h>
#include <xxhash.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "engine/guest_census.h"
#include "gpu/constant_buffers.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/draw_queue.h"
#include "gpu/frame_stats.h"
#include "gpu/physical_buffers.h"
#include "gpu/scene/guest_scene.h"
#include "gpu/scene/node_tag.h"

REXCVAR_DECLARE(double, bd_scene_record_after_s);
REXCVAR_DECLARE(i32, bd_scene_record_frames);

namespace bd::gpu::scene {

namespace {

struct RecorderState {
  std::mutex mutex;
  std::atomic<bool> armed{false};
  bool done = false;
  u32 first_frame = 0;
  u32 last_frame = 0; // exclusive
  std::vector<NodeDrawRecord> nodes;
  std::unordered_map<u64, MeshRecord> meshes;
  std::unordered_map<u64, MaterialRecord> materials;
  std::unordered_map<u64, TextureRecord> textures;
  u32 untagged = 0;
  u32 tagged = 0;
};

RecorderState &state() {
  static RecorderState s;
  return s;
}

std::filesystem::path WalkDir() {
  std::filesystem::path root;
  if (auto *rt = rex::Runtime::instance())
    root = rt->cache_root();
  if (root.empty())
    root = std::filesystem::current_path();
  return root / "scene_walk";
}

template <typename T> void Append(std::ofstream &f, const std::vector<T> &v) {
  if (!v.empty())
    f.write(reinterpret_cast<const char *>(v.data()),
            static_cast<std::streamsize>(v.size() * sizeof(T)));
}

void WriteFile(RecorderState &r) {
  const auto dir = WalkDir();
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const auto now = std::chrono::floor<std::chrono::seconds>(
      std::chrono::system_clock::now());
  const auto path = dir / std::format("walk_{:%Y%m%d-%H%M%S}.bdsw", now);
  auto tmp = path;
  tmp += ".tmp";

  std::vector<MeshRecord> meshes;
  meshes.reserve(r.meshes.size());
  for (auto &[k, m] : r.meshes)
    meshes.push_back(m);
  std::vector<MaterialRecord> materials;
  materials.reserve(r.materials.size());
  for (auto &[k, m] : r.materials)
    materials.push_back(m);
  std::vector<TextureRecord> textures;
  textures.reserve(r.textures.size());
  for (auto &[k, t] : r.textures)
    textures.push_back(t);

  WalkHeader h{};
  std::memcpy(h.magic, kWalkMagic, sizeof(kWalkMagic));
  h.version = kWalkVersion;
  h.first_frame = r.first_frame;
  h.frame_count = r.last_frame - r.first_frame;
  h.node_count = static_cast<u32>(r.nodes.size());
  h.mesh_count = static_cast<u32>(meshes.size());
  h.material_count = static_cast<u32>(materials.size());
  h.texture_count = static_cast<u32>(textures.size());
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) {
      BD_ERROR("[scene] cannot write {}", tmp.string());
      return;
    }
    f.write(reinterpret_cast<const char *>(&h), sizeof(h));
    Append(f, r.nodes);
    Append(f, meshes);
    Append(f, materials);
    Append(f, textures);
  }
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    BD_ERROR("[scene] rename failed: {}", ec.message());
    return;
  }
  BD_INFO("[scene] wrote {}: frames {}..{}, {} node draws ({} untagged draws "
          "skipped), {} meshes, {} materials, {} textures",
          path.string(), r.first_frame, r.last_frame - 1, r.nodes.size(),
          r.untagged, meshes.size(), materials.size(), textures.size());
}

// The block a stream or index view lives in, by its host buffer.
bool BlockOf(const plume::RenderBufferReference &ref, PhysicalBlockInfo &info) {
  return ref.ref && PhysicalBlockOfBuffer(ref.ref, info);
}

u64 MeshKeyOf(const MeshRecord &m) {
  // Everything but the key itself, which sits first.
  return XXH3_64bits(reinterpret_cast<const u8 *>(&m) + sizeof(u64),
                     sizeof(MeshRecord) - sizeof(u64));
}

u64 MaterialKeyOf(const MaterialRecord &m) {
  return XXH3_64bits(reinterpret_cast<const u8 *>(&m) + sizeof(u64),
                     sizeof(MaterialRecord) - sizeof(u64));
}

} // namespace

bool RecordingArmed() {
  return state().armed.load(std::memory_order_relaxed);
}

namespace {
std::atomic<u32> g_device_va{0};
} // namespace

u32 LastGuestDeviceVa() { return g_device_va.load(std::memory_order_relaxed); }
void NoteGuestDeviceVa(u32 device_guest) {
  if (device_guest)
    g_device_va.store(device_guest, std::memory_order_relaxed);
}

void OnFrameEnd() {
  auto &r = state();
  if (r.done)
    return;
  const double after = REXCVAR_GET(bd_scene_record_after_s);
  if (after <= 0.0) {
    r.done = true;
    return;
  }
  using Clock = std::chrono::steady_clock;
  static const Clock::time_point start = Clock::now();
  const u32 frame = FrameStatFrameCount();
  if (!r.armed.load(std::memory_order_relaxed)) {
    if (std::chrono::duration<double>(Clock::now() - start).count() < after)
      return;
    std::lock_guard lock(r.mutex);
    r.first_frame = frame;
    r.last_frame = frame + static_cast<u32>(
                               std::max(1, REXCVAR_GET(bd_scene_record_frames)));
    r.armed.store(true, std::memory_order_relaxed);
    BD_INFO("[scene] recording frames {}..{}", r.first_frame, r.last_frame - 1);
    return;
  }
  if (frame >= r.last_frame) {
    r.armed.store(false, std::memory_order_relaxed);
    std::lock_guard lock(r.mutex);
    WriteFile(r);
    r.done = true;
  }
}

void OnQueuedDraw(const VideoState &s, const QueuedDraw &q, u32 device_guest) {
  auto &r = state();
  if (!r.armed.load(std::memory_order_relaxed))
    return;
  const NodeTag &tag = CurrentNodeTag();
  std::lock_guard lock(r.mutex);
  if (!tag.valid) {
    ++r.untagged;
    return;
  }
  ++r.tagged;

  // --- the mesh ---------------------------------------------------------
  MeshRecord m{};
  PhysicalBlockInfo block{};
  bool have_block = false;
  m.stream_count = 0;
  const u32 end = std::min<u32>(q.vertex_first + q.vertex_count, 16u);
  for (u32 i = q.vertex_first; i < end; ++i) {
    const auto &v = q.vertex_views[i];
    if (!v.buffer.ref)
      continue;
    PhysicalBlockInfo b{};
    if (BlockOf(v.buffer, b)) {
      if (!have_block) {
        block = b;
        have_block = true;
      }
    }
    MeshStream &st = m.streams[m.stream_count++];
    st.slot = i;
    st.offset = static_cast<u32>(v.buffer.offset);
    st.stride = q.input_slots[i].stride;
    st.size = v.size;
  }
  m.ib_offset = ~0u;
  if (q.has_index_buffer && q.index_view.buffer.ref) {
    PhysicalBlockInfo b{};
    if (BlockOf(q.index_view.buffer, b) && !have_block) {
      block = b;
      have_block = true;
    }
    m.ib_offset = static_cast<u32>(q.index_view.buffer.offset);
    m.ib_bytes = q.index_view.size;
    m.ib_format = static_cast<u32>(q.index_view.format);
  }
  m.block_hash = have_block ? block.content_hash : 0;
  m.block_size = have_block ? block.size : 0;
  m.indexed = q.indexed;
  m.count = q.count;
  m.start_index = q.start_index;
  m.base_vertex = q.base_vertex;
  m.start_vertex = q.start_vertex;
  m.decl_hash = s.pipelineState.vertexDeclaration
                    ? s.pipelineState.vertexDeclaration->hash
                    : 0;
  if (const auto *mesh = bd::mem::try_at<const GuestMesh>(tag.mesh_va)) {
    for (int i = 0; i < 3; ++i)
      m.centre[i] = mesh->centre[i];
    m.radius = mesh->radius;
  }
  m.mesh_key = MeshKeyOf(m);
  r.meshes.emplace(m.mesh_key, m);

  // --- the material -----------------------------------------------------
  MaterialRecord mat{};
  const auto *vs = s.pipelineState.vertexShader;
  const auto *ps = s.pipelineState.pixelShader;
  mat.vs_hash = (vs && vs->shaderCacheEntry) ? vs->shaderCacheEntry->hash : 0;
  mat.ps_hash = (ps && ps->shaderCacheEntry) ? ps->shaderCacheEntry->hash : 0;
  mat.decl_hash = m.decl_hash;
  {
    // The state with hashes in the pointer slots, like the PSO recorder's
    // residual key, so it names the same pipeline in every process.
    PipelineState st = s.pipelineState;
    st.vertexShader = reinterpret_cast<GuestShader *>(mat.vs_hash);
    st.pixelShader = reinterpret_cast<GuestShader *>(mat.ps_hash);
    st.vertexDeclaration =
        reinterpret_cast<GuestVertexDeclaration *>(mat.decl_hash);
    mat.state_hash = XXH3_64bits(&st, sizeof(st));
    mat.spec_constants = st.specConstants;
  }
  mat.ps_block_hash = HashConstantBlock(
      q.constant_offsets[1],
      (ps && ps->shaderCacheEntry) ? ps->shaderCacheEntry->constantRegisterMask
                                   : nullptr);
  const auto *dev = bd::mem::try_at<const D3DDevice>(device_guest);
  for (u32 i = 0; i < 16; ++i) {
    const GuestTexture *t = s.textures[i];
    if (!t)
      continue;
    mat.tex_key[i] = t->contentHash;
    if (dev) {
      for (int k = 0; k < 6; ++k)
        mat.fetch[i][k] = static_cast<u32>(dev->fetchConstants[i].dword[k]);
    }
    if (t->contentHash && !r.textures.count(t->contentHash)) {
      TextureRecord tr{};
      tr.tex_key = t->contentHash;
      tr.format = static_cast<u32>(t->format);
      tr.width = t->width;
      tr.height = t->height;
      tr.depth = t->depth;
      tr.mips = t->mipLevels;
      tr.array_size = t->layers;
      tr.dimension = static_cast<u32>(t->viewDimension);
      std::memcpy(tr.name, t->nameTag, sizeof(tr.name));
      r.textures.emplace(tr.tex_key, tr);
    }
  }
  mat.tech = tag.tech;
  mat.material_key = MaterialKeyOf(mat);
  r.materials.emplace(mat.material_key, mat);

  // --- the node draw ----------------------------------------------------
  NodeDrawRecord n{};
  n.frame = FrameStatFrameCount();
  n.pass_id = CurrentRenderPassId();
  n.render_view = tag.render_view;
  n.seq = tag.seq;
  n.visual_va = tag.visual_va;
  n.node_index = tag.node_index;
  n.tech = tag.tech;
  n.blended = q.blended;
  if (const auto *w = bd::mem::try_at<const be_f32>(tag.matrix_va)) {
    for (int i = 0; i < 16; ++i)
      n.world[i] = w[i];
  }
  n.palette_va = tag.palette_va;
  n.bone_count = bd::mem::try_field<u32>(tag.visual_va, kVisualBoneCount);
  n.mesh_key = m.mesh_key;
  n.material_key = mat.material_key;
  n.depth_sq = static_cast<float>(bd::engine::LastNodeViewDistanceSq());
  {
    const float *vs_regs = nullptr;
    if (q.record_index != ~0u) {
      if (const auto *rec = StagedInstanceRecord(q.record_index))
        vs_regs = rec->regs;
    } else if (const u8 *b = ConstantBlockBytes(q.constant_offsets[0])) {
      vs_regs = reinterpret_cast<const float *>(b);
    }
    if (vs_regs)
      std::memcpy(n.vs_c20, vs_regs + 20 * 4, sizeof(n.vs_c20));
  }
  r.nodes.push_back(n);
}

} // namespace bd::gpu::scene
