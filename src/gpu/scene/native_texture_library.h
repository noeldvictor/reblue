/**
 * @file    native_texture_library.h
 * @brief   Persistent native texture ownership and versioned mip cooking.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license   BSD 3-Clause License
 */
#pragma once
#include "gpu/scene/native_texture_data.h"
#include <filesystem>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace bd::gpu::scene {
struct NativeTextureAsset {
  uint64_t id;
  NativeTextureData data;
};
using NativeTextureHandle = std::shared_ptr<const NativeTextureAsset>;
struct NativeTextureStats {
  uint64_t cooked = 0, loaded = 0, memory_hits = 0, generated_mips = 0,
           cached_mips = 0;
  uint64_t invalid = 0, write_failures = 0, budget_refusals = 0, bytes = 0;
  size_t resident = 0;
};
class NativeTextureLibrary {
public:
  explicit NativeTextureLibrary(std::filesystem::path directory,
                                uint64_t byte_budget = 128ull << 20);
  NativeTextureHandle Resolve(NativeTextureData data,
                              bool generate_mips = false);
  NativeTextureHandle Load(uint64_t id);
  NativeTextureStats Stats() const;
  static std::filesystem::path FileName(uint64_t id);

private:
  struct Entry {
    NativeTextureHandle asset;
    std::list<uint64_t>::iterator recent;
    uint64_t bytes;
  };
  NativeTextureHandle Find(uint64_t id);
  bool Read(uint64_t id, NativeTextureData &data);
  NativeTextureHandle Insert(uint64_t id, NativeTextureData data);
  bool Write(const std::filesystem::path &path, std::span<const uint8_t> file);
  std::filesystem::path directory_;
  uint64_t byte_budget_;
  mutable std::mutex mutex_;
  std::unordered_map<uint64_t, Entry> entries_;
  std::list<uint64_t> recent_;
  NativeTextureStats stats_;
};
} // namespace bd::gpu::scene
