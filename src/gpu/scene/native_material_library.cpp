/**
 * @file    native_material_library.cpp
 * @brief   Bounded, pinned material library and checked derived asset files.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license   BSD 3-Clause License
 */
#include "gpu/scene/native_material_library.h"

#include <charconv>
#include <fstream>
#include <string>

namespace bd::gpu::scene {
NativeMaterialLibrary::NativeMaterialLibrary(std::filesystem::path directory,
                                           size_t capacity)
    : directory_(std::move(directory)), capacity_(capacity) {}

std::filesystem::path NativeMaterialLibrary::FileName(NativeMaterialId id) {
  char digits[16];
  const auto result = std::to_chars(std::begin(digits), std::end(digits), id, 16);
  std::string name(16 - (result.ptr - digits), '0');
  name.append(digits, result.ptr);
  return name + ".bdmat";
}

NativeMaterialHandle NativeMaterialLibrary::Find(NativeMaterialId id) {
  auto it = entries_.find(id);
  if (it == entries_.end())
    return {};
  recent_.splice(recent_.begin(), recent_, it->second.recent);
  ++stats_.memory_hits;
  return it->second.material;
}

bool NativeMaterialLibrary::MakeRoom() {
  if (entries_.size() < capacity_)
    return true;
  for (auto it = recent_.rbegin(); it != recent_.rend(); ++it) {
    auto entry = entries_.find(*it);
    if (entry->second.material.use_count() != 1)
      continue;
    recent_.erase(entry->second.recent);
    entries_.erase(entry);
    return true;
  }
  ++stats_.budget_refusals;
  return false;
}

NativeMaterialHandle NativeMaterialLibrary::Insert(NativeMaterialId id,
                                                 const NativeMaterialAsset &asset) {
  if (!MakeRoom())
    return {};
  auto material = std::make_shared<const NativeMaterial>(NativeMaterial{id, asset});
  recent_.push_front(id);
  entries_.emplace(id, Entry{material, recent_.begin()});
  return material;
}

bool NativeMaterialLibrary::Read(NativeMaterialId id, NativeMaterialAsset &asset) {
  if (directory_.empty())
    return {};
  std::ifstream file(directory_ / FileName(id), std::ios::binary | std::ios::ate);
  if (!file)
    return {};
  if (file.tellg() != std::streamoff(kNativeMaterialFileBytes)) {
    ++stats_.invalid;
    return {};
  }
  std::array<uint8_t, kNativeMaterialFileBytes> bytes;
  file.seekg(0);
  if (!file.read(reinterpret_cast<char *>(bytes.data()), bytes.size()) ||
      NativeMaterialContentId(bytes) != id || !DecodeNativeMaterial(bytes, asset)) {
    ++stats_.invalid;
    return {};
  }
  return true;
}

bool NativeMaterialLibrary::Write(NativeMaterialId id, std::span<const uint8_t> bytes) {
  if (directory_.empty())
    return false;
  std::error_code error;
  std::filesystem::create_directories(directory_, error);
  if (error)
    return false;
  // A derived cache, never an original asset. Length, checksum and identity
  // validation reject interrupted writes; Resolve recooks them from the source.
  std::ofstream file(directory_ / FileName(id), std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  file.close();
  return bool(file);
}

NativeMaterialHandle NativeMaterialLibrary::Load(NativeMaterialId id) {
  std::lock_guard lock(mutex_);
  if (auto found = Find(id))
    return found;
  NativeMaterialAsset asset;
  if (!Read(id, asset))
    return {};
  auto material = Insert(id, asset);
  if (material)
    ++stats_.loaded;
  return material;
}

NativeMaterialHandle NativeMaterialLibrary::Resolve(const NativeMaterialAsset &asset) {
  std::vector<uint8_t> bytes;
  NativeMaterialAsset canonical;
  if (!EncodeNativeMaterial(asset, bytes) || !DecodeNativeMaterial(bytes, canonical))
    return {};
  const auto id = NativeMaterialContentId(bytes);
  std::lock_guard lock(mutex_);
  auto found = Find(id);
  if (found) {
    if (found->asset == canonical)
      return found;
    ++stats_.invalid; // hash collision: never alias or overwrite another asset
    return {};
  }
  NativeMaterialAsset cached;
  const bool loaded = Read(id, cached);
  if (loaded && cached != canonical) {
    ++stats_.invalid;
    return {}; // a content-hash collision must not overwrite another asset
  }
  auto material = Insert(id, loaded ? cached : canonical);
  if (!material)
    return {};
  if (loaded) {
    ++stats_.loaded;
    return material;
  }
  ++stats_.cooked;
  if (!Write(id, bytes))
    ++stats_.write_failures;
  return material;
}

NativeMaterialLibraryStats NativeMaterialLibrary::Stats() const {
  std::lock_guard lock(mutex_);
  auto result = stats_;
  result.resident = entries_.size();
  return result;
}
} // namespace bd::gpu::scene
