/**
 * @file    native_mesh_data.cpp
 * @brief   Checked, little-endian native mesh files and triangle conversion.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license   BSD 3-Clause License
 */
#include "gpu/scene/native_mesh_data.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>

namespace bd::gpu::scene {
namespace {
constexpr uint8_t kMagic[8] = {'B', 'D', 'M', 'E', 'S', 'H', 1, 0};

uint64_t Checksum(std::span<const uint8_t> bytes) {
  uint64_t hash = 14695981039346656037ull;
  for (uint8_t b : bytes)
    hash = (hash ^ b) * 1099511628211ull;
  return hash;
}
void Put(std::vector<uint8_t> &v, uint64_t n, unsigned bytes = 4) {
  for (unsigned i = 0; i < bytes; ++i)
    v.push_back(uint8_t(n >> (8 * i)));
}
struct Reader {
  std::span<const uint8_t> bytes;
  bool ok = true;
  uint64_t Get(unsigned n = 4) {
    if (bytes.size() < n) {
      ok = false;
      return 0;
    }
    uint64_t value = 0;
    for (unsigned i = 0; i < n; ++i)
      value |= uint64_t(bytes[i]) << (8 * i);
    bytes = bytes.subspan(n);
    return value;
  }
};
} // namespace

bool ImportMeshIndices(std::span<const uint8_t> source, bool index32,
                       MeshTopology topology, std::vector<uint32_t> &triangles) {
  triangles.clear();
  const size_t width = index32 ? 4 : 2;
  const size_t count = source.size() / width;
  if (source.size() % width || count > kNativeMeshMaxBytes / 12 ||
      (topology == MeshTopology::Triangles && count % 3))
    return false;
  const uint32_t restart = index32 ? UINT32_MAX : UINT16_MAX;
  uint32_t a = 0, b = 0;
  size_t run = 0;
  triangles.reserve(count * (topology == MeshTopology::Strip ? 3 : 1));
  for (size_t i = 0; i < count; ++i) {
    uint32_t c = 0;
    for (size_t j = 0; j < width; ++j)
      c = (c << 8) | source[i * width + j];
    if (topology == MeshTopology::Triangles) {
      triangles.push_back(c);
      continue;
    }
    if (c == restart) {
      run = 0;
      continue;
    }
    if (run >= 2 && a != b && b != c && a != c) {
      triangles.push_back(a);
      triangles.push_back((run & 1) ? c : b);
      triangles.push_back((run & 1) ? b : c);
    }
    a = b;
    b = c;
    ++run;
  }
  return true;
}

bool ValidateNativeMesh(const NativeMeshData &mesh) {
  if (mesh.indices.empty() || mesh.indices.size() % 3 ||
      mesh.indices.size() > kNativeMeshMaxBytes / 4 || mesh.streams.empty() ||
      mesh.streams.size() > 16)
    return false;
  const auto [lo, hi] = std::minmax_element(mesh.indices.begin(), mesh.indices.end());
  const int64_t first = int64_t(*lo) + mesh.base_vertex;
  const int64_t last = int64_t(*hi) + mesh.base_vertex;
  if (first < 0 || last > UINT32_MAX)
    return false;
  uint32_t slots = 0;
  uint64_t bytes = 36 + mesh.indices.size() * 4;
  for (const auto &s : mesh.streams) {
    if (s.slot >= 16 || (slots & (1u << s.slot)) || s.stride == 0 ||
        (uint64_t(last) + 1) * s.stride > s.bytes.size())
      return false;
    slots |= 1u << s.slot;
    bytes += 12 + s.bytes.size();
    if (bytes > kNativeMeshMaxBytes)
      return false;
  }
  return true;
}

bool EncodeNativeMesh(const NativeMeshData &mesh, std::vector<uint8_t> &file) {
  file.clear();
  if (!ValidateNativeMesh(mesh))
    return false;
  file.insert(file.end(), std::begin(kMagic), std::end(kMagic));
  Put(file, 0, 8);
  Put(file, mesh.layout, 8);
  Put(file, std::bit_cast<uint32_t>(mesh.base_vertex));
  Put(file, mesh.streams.size());
  Put(file, mesh.indices.size());
  for (const auto &s : mesh.streams) {
    Put(file, s.slot);
    Put(file, s.stride);
    Put(file, s.bytes.size());
    file.insert(file.end(), s.bytes.begin(), s.bytes.end());
  }
  for (uint32_t i : mesh.indices)
    Put(file, i);
  const uint64_t sum = Checksum(std::span(file).subspan(16));
  for (unsigned i = 0; i < 8; ++i)
    file[8 + i] = uint8_t(sum >> (8 * i));
  return true;
}

bool DecodeNativeMesh(std::span<const uint8_t> file, NativeMeshData &mesh) {
  // Parse into a temporary: a rejected cache cannot leave a partially usable
  // mesh in the renderer. Counts are bounded by the remaining file first.
  if (file.size() < 36 || file.size() > kNativeMeshMaxBytes ||
      std::memcmp(file.data(), kMagic, sizeof(kMagic)) != 0)
    return false;
  Reader r{file.subspan(8)};
  if (r.Get(8) != Checksum(file.subspan(16)))
    return false;
  NativeMeshData result;
  result.layout = r.Get(8);
  result.base_vertex = std::bit_cast<int32_t>(uint32_t(r.Get()));
  const uint32_t streams = uint32_t(r.Get());
  const uint32_t indices = uint32_t(r.Get());
  if (streams == 0 || streams > 16 || indices > r.bytes.size() / 4)
    return false;
  for (uint32_t i = 0; i < streams; ++i) {
    NativeMeshStream s;
    s.slot = uint32_t(r.Get());
    s.stride = uint32_t(r.Get());
    const uint32_t size = uint32_t(r.Get());
    if (!r.ok || size > r.bytes.size())
      return false;
    s.bytes.assign(r.bytes.begin(), r.bytes.begin() + size);
    r.bytes = r.bytes.subspan(size);
    result.streams.push_back(std::move(s));
  }
  if (uint64_t(indices) * 4 != r.bytes.size())
    return false;
  result.indices.reserve(indices);
  for (uint32_t i = 0; i < indices; ++i)
    result.indices.push_back(uint32_t(r.Get()));
  if (!r.ok || !ValidateNativeMesh(result))
    return false;
  mesh = std::move(result);
  return true;
}
} // namespace bd::gpu::scene
