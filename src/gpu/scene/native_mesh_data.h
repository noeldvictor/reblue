/**
 * @file    native_mesh_data.h
 * @brief   Portable native mesh payloads, independent of guest memory and Vulkan.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license   BSD 3-Clause License
 */
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace bd::gpu::scene {

enum class MeshTopology { Triangles, Strip };

// The import boundary accepts big-endian indices; everything after it is a
// native triangle list. Restart resets parity; degenerate strips advance it.
bool ImportMeshIndices(std::span<const uint8_t> source, bool index32,
                       MeshTopology topology, std::vector<uint32_t> &triangles);

struct NativeMeshStream {
  uint32_t slot = 0;
  uint32_t stride = 0;
  std::vector<uint8_t> bytes;
};

struct NativeMeshData {
  // Identifies the explicit vertex layout. The first importer retains the
  // existing shader's packed attributes; changing that packing changes the
  // layout key, without putting guest addresses or resource structs on disk.
  uint64_t layout = 0;
  int32_t base_vertex = 0;
  std::vector<uint32_t> indices;
  std::vector<NativeMeshStream> streams;
};

constexpr uint64_t kNativeMeshMaxBytes = 64ull << 20;
bool ValidateNativeMesh(const NativeMeshData &mesh);
bool EncodeNativeMesh(const NativeMeshData &mesh, std::vector<uint8_t> &file);
bool DecodeNativeMesh(std::span<const uint8_t> file, NativeMeshData &mesh);

} // namespace bd::gpu::scene
