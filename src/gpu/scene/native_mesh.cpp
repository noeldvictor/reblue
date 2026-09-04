/**
 * @file    native_mesh.cpp
 * @brief   Persistent model geometry and shared buffers for indirect drawing.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license   BSD 3-Clause License
 */
#include "gpu/scene/native_mesh.h"
#include "gpu/scene/native_mesh_data.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <fmt/format.h>
#include <xxhash.h>
#include <rex/graphics/xenos.h>
#include <rex/runtime.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/byte_swap.h"
#include "gpu/device.h"
#include "gpu/frame_stats.h"
#include "gpu/resources.h"

namespace bd::gpu::scene {
namespace {
static_assert(std::endian::native == std::endian::little);
constexpr u32 kChunkBytes = 32u << 20;
// Geometry's share of the project's 1.5 GB asset budget. A full arena is
// reported explicitly; it must never grow with every scene reload forever.
constexpr u64 kGeometryBudget = 256ull << 20;
struct Chunk {
  std::unique_ptr<plume::RenderBuffer> buffer;
  u32 size = 0, used = 0;
};
struct Store {
  std::mutex mutex;
  std::vector<Chunk> chunks;
  std::unordered_map<u64, std::shared_ptr<const NativeGeometry>> meshes;
  u64 allocated = 0;
  u32 built = 0, loaded = 0, refused = 0, budget_refused = 0;
  u32 native_draws = 0, legacy_draws = 0, last_frame = 0;
};
Store &store() {
  static Store s;
  return s;
}

std::filesystem::path CacheDir() {
  std::filesystem::path root;
  if (auto *runtime = rex::Runtime::instance())
    root = runtime->cache_root();
  if (root.empty())
    root = std::filesystem::current_path();
  return root / "native_meshes" / "v1";
}

bool ReadFile(const std::filesystem::path &path, NativeMeshData &data) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file)
    return false;
  const auto size = file.tellg();
  if (size < 36 || uint64_t(size) > kNativeMeshMaxBytes)
    return false;
  std::vector<u8> bytes(static_cast<size_t>(size));
  file.seekg(0);
  return bool(file.read(reinterpret_cast<char *>(bytes.data()), size)) &&
         DecodeNativeMesh(bytes, data);
}

void WriteFile(const std::filesystem::path &path, const NativeMeshData &data) {
  std::vector<u8> bytes;
  if (!EncodeNativeMesh(data, bytes))
    return;
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    BD_WARN("[native-mesh] cannot create cache: {}", ec.message());
    return;
  }
  // These are derived cache files. An interrupted write fails the length or
  // checksum check on the next launch and is rebuilt from the owned assets.
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  if (!file)
    BD_WARN("[native-mesh] cannot write {}", path.string());
}

u32 Align(u32 n) { return (n + 15u) & ~15u; }

std::shared_ptr<const NativeGeometry> Upload(Store &s, const NativeMeshData &data) {
  u32 bytes = Align(u32(data.indices.size() * 4));
  for (const auto &stream : data.streams)
    bytes += Align(u32(stream.bytes.size()));
  if (s.chunks.empty() || s.chunks.back().size - s.chunks.back().used < bytes) {
    const u32 size = std::max(kChunkBytes, bytes);
    if (s.allocated + size > kGeometryBudget) {
      ++s.budget_refused;
      return {};
    }
    auto *device = Video::HostDevice();
    plume::RenderBufferDesc desc;
    desc.size = size;
    desc.heapType = GeometryHeapType(device, GeometryClass::Static);
    desc.flags = plume::RenderBufferFlag::VERTEX | plume::RenderBufferFlag::INDEX |
                 plume::RenderBufferFlag::STORAGE;
    auto buffer = CreateHostBuffer(device, desc, "native-mesh-arena");
    if (!buffer)
      return {};
    s.chunks.push_back(Chunk{std::move(buffer), size, 0});
    s.allocated += size;
  }
  auto &chunk = s.chunks.back();
  auto *mapped = static_cast<u8 *>(chunk.buffer->map());
  if (!mapped)
    return {};
  auto result = std::make_shared<NativeGeometry>();
  result->count = u32(data.indices.size());
  result->base_vertex = data.base_vertex;
  result->start_index = chunk.used / 4;
  // Every mesh in a chunk binds the same index view. Its range lives in the
  // indirect command, allowing distinct meshes to share a multi-draw call.
  result->index = plume::RenderIndexBufferView(chunk.buffer->at(0), chunk.size,
                                               plume::RenderFormat::R32_UINT);
  std::memcpy(mapped + chunk.used, data.indices.data(), data.indices.size() * 4);
  chunk.used += Align(u32(data.indices.size() * 4));
  for (const auto &stream : data.streams) {
    result->streams[stream.slot] = plume::RenderVertexBufferView(
        chunk.buffer->at(chunk.used), u32(stream.bytes.size()));
    result->stream_mask |= 1u << stream.slot;
    std::memcpy(mapped + chunk.used, stream.bytes.data(), stream.bytes.size());
    chunk.used += Align(u32(stream.bytes.size()));
  }
  chunk.buffer->unmap();
  return result;
}

std::shared_ptr<const NativeGeometry> Import(Store &s, const NativeMeshImport &r) {
  if (!r.declaration || !r.index || r.index->ownsMirror || !r.count)
    return {};
  NativeMeshData data;
  data.layout = r.declaration->hash;
  data.base_vertex = r.base_vertex;
  if (!r.lod_indices.empty()) {
    if (r.lod_indices.size() > kNativeMeshMaxBytes / 4)
      return {};
    data.indices.assign(r.lod_indices.begin(), r.lod_indices.end());
  } else {
    using rex::graphics::xenos::PrimitiveType;
    const auto primitive = static_cast<PrimitiveType>(r.primitive_type);
    if (primitive != PrimitiveType::kTriangleList &&
        primitive != PrimitiveType::kTriangleStrip)
      return {};
    const bool index32 = r.index->format == plume::RenderFormat::R32_UINT;
    if (!index32 && r.index->format != plume::RenderFormat::R16_UINT)
      return {};
    const u32 width = index32 ? 4 : 2;
    const u64 offset = u64(r.start_index) * width;
    const u64 size = u64(r.count) * width;
    if (offset + size > r.index->dataSize || size > kNativeMeshMaxBytes)
      return {};
    const auto *source = bd::mem::try_at<const u8>(r.index->guestMirrorVa);
    if (!source || !ImportMeshIndices({source + offset, size}, index32,
          primitive == PrimitiveType::kTriangleStrip ? MeshTopology::Strip
                                                     : MeshTopology::Triangles,
          data.indices))
      return {};
  }
  if (data.indices.empty())
    return {};
  const auto [lo, hi] = std::minmax_element(data.indices.begin(), data.indices.end());
  const i64 first = i64(*lo) + data.base_vertex;
  const i64 last = i64(*hi) + data.base_vertex;
  if (first < 0 || last > UINT32_MAX)
    return {};

  // Hash bytes and layout, never allocation addresses. Re-loading a model
  // resolves the same native mesh, including across separate desktop runs.
  u64 key = XXH3_64bits(data.indices.data(), data.indices.size() * 4);
  const u64 identity[] = {1, data.layout, u64(u32(data.base_vertex))};
  key = XXH3_64bits_withSeed(identity, sizeof(identity), key);
  struct Source { const u8 *bytes; u32 size, slot, stride; } sources[16];
  u32 n = 0;
  u64 total = 36 + data.indices.size() * 4;
  for (u32 slot = 0; slot < 16; ++slot) {
    if (!r.declaration->vertexStreams[slot])
      continue;
    const auto *buffer = r.streams[slot];
    if (!buffer || buffer->ownsMirror || !r.strides[slot])
      return {};
    const u64 size = (u64(last) + 1) * r.strides[slot];
    total += 12 + size;
    if (total > kNativeMeshMaxBytes || u64(r.offsets[slot]) + size > buffer->dataSize ||
        size % 4)
      return {};
    const auto *source = bd::mem::try_at<const u8>(buffer->guestMirrorVa);
    if (!source)
      return {};
    source += r.offsets[slot];
    const u32 meta[] = {slot, r.strides[slot], u32(size)};
    key = XXH3_64bits_withSeed(meta, sizeof(meta), key);
    key = XXH3_64bits_withSeed(source, size, key);
    sources[n++] = Source{source, u32(size), slot, r.strides[slot]};
  }
  if (!n)
    return {};
  if (auto it = s.meshes.find(key); it != s.meshes.end())
    return it->second;

  const auto path = CacheDir() / fmt::format("{:016x}.bdmesh", key);
  NativeMeshData cached;
  bool loaded = ReadFile(path, cached) && cached.layout == data.layout &&
                cached.base_vertex == data.base_vertex &&
                cached.indices == data.indices && cached.streams.size() == n;
  for (u32 i = 0; loaded && i < n; ++i)
    loaded = cached.streams[i].slot == sources[i].slot &&
             cached.streams[i].stride == sources[i].stride &&
             cached.streams[i].bytes.size() == sources[i].size;
  if (loaded) {
    data = std::move(cached);
  } else {
    for (u32 i = 0; i < n; ++i) {
      const auto &source = sources[i];
      NativeMeshStream stream;
      stream.slot = source.slot;
      stream.stride = source.stride;
      stream.bytes.resize(source.size);
      // Match the current GPU vertex layout at this import boundary. The
      // persistent bytes never require guest endian conversion at draw time.
      ByteSwapElements(stream.bytes.data(), source.bytes, source.size, 4);
      data.streams.push_back(std::move(stream));
    }
    if (!ValidateNativeMesh(data))
      return {};
  }
  auto result = Upload(s, data);
  if (!result)
    return {};
  if (loaded)
    ++s.loaded;
  else {
    WriteFile(path, data);
    ++s.built;
  }
  s.meshes.emplace(key, result);
  return result;
}
} // namespace

std::shared_ptr<const NativeGeometry> ImportNativeMesh(const NativeMeshImport &r) {
  auto &s = store();
  std::lock_guard lock(s.mutex);
  auto result = Import(s, r);
  if (!result)
    ++s.refused;
  return result;
}

void NativeMeshNoteDraw(bool native) {
  auto &s = store();
  std::lock_guard lock(s.mutex);
  ++(native ? s.native_draws : s.legacy_draws);
  const u32 frame = FrameStatFrameCount();
  if (frame - s.last_frame < 300)
    return;
  const u32 frames = frame - s.last_frame;
  BD_INFO("[native-mesh] {} cooked, {} loaded, {} live meshes, {} MiB; indexed "
          "replays/frame {:.1f} native / {:.1f} unconverted; import refusals {}, "
          "budget refusals {}",
          s.built, s.loaded, s.meshes.size(), s.allocated >> 20,
          double(s.native_draws) / frames, double(s.legacy_draws) / frames,
          s.refused, s.budget_refused);
  s.native_draws = s.legacy_draws = 0;
  s.last_frame = frame;
}
} // namespace bd::gpu::scene
