/**
 * @file    native_texture_library.cpp
 * @brief   Checked source-free loading, bounded ownership and mip recipe cache.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license   BSD 3-Clause License
 */
#include "gpu/scene/native_texture_library.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <string>

namespace bd::gpu::scene {
namespace {
constexpr std::array<uint8_t, 8> kMipMagic{'B', 'D', 'M', 'I', 'P', 0, 1, 0};
bool MatchesMipSource(const NativeTextureData &base,
                      const NativeTextureData &chain) {
  uint32_t levels = 1, w = base.width, h = base.height;
  while (w > 4 || h > 4) {
    w = std::max(w / 2, 1u);
    h = std::max(h / 2, 1u);
    ++levels;
  }
  return chain.dimension == base.dimension && chain.format == base.format &&
         chain.width == base.width && chain.height == base.height &&
         chain.depth == base.depth && chain.mip_levels == levels &&
         chain.images[0] == base.images[0];
}
} // namespace
NativeTextureLibrary::NativeTextureLibrary(std::filesystem::path directory,
                                           uint64_t byte_budget)
    : directory_(std::move(directory)), byte_budget_(byte_budget) {}
std::filesystem::path NativeTextureLibrary::FileName(uint64_t id) {
  char digits[16];
  const auto r = std::to_chars(std::begin(digits), std::end(digits), id, 16);
  std::string name(16 - (r.ptr - digits), '0');
  name.append(digits, r.ptr);
  return name + ".bdtex";
}
NativeTextureHandle NativeTextureLibrary::Find(uint64_t id) {
  auto it = entries_.find(id);
  if (it == entries_.end())
    return {};
  recent_.splice(recent_.begin(), recent_, it->second.recent);
  ++stats_.memory_hits;
  return it->second.asset;
}
bool NativeTextureLibrary::Read(uint64_t id, NativeTextureData &data) {
  if (directory_.empty())
    return false;
  std::ifstream file(directory_ / FileName(id),
                     std::ios::binary | std::ios::ate);
  if (!file)
    return false;
  const auto size = file.tellg();
  if (size < 40 || uint64_t(size) > kNativeTextureMaxBytes) {
    ++stats_.invalid;
    return false;
  }
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  file.seekg(0);
  if (!file.read(reinterpret_cast<char *>(bytes.data()), size) ||
      NativeTextureContentId(bytes) != id ||
      !DecodeNativeTexture(bytes, data)) {
    ++stats_.invalid;
    return false;
  }
  return true;
}
NativeTextureHandle NativeTextureLibrary::Insert(uint64_t id,
                                                 NativeTextureData data) {
  uint64_t size = sizeof(NativeTextureAsset);
  for (const auto &image : data.images)
    size += image.capacity() + sizeof(image);
  if (size > byte_budget_) {
    ++stats_.budget_refusals;
    return {};
  }
  while (size > byte_budget_ - stats_.bytes || entries_.size() >= 8192) {
    auto victim = recent_.rbegin();
    for (; victim != recent_.rend(); ++victim)
      if (entries_.at(*victim).asset.use_count() == 1)
        break;
    if (victim == recent_.rend()) {
      ++stats_.budget_refusals;
      return {};
    }
    auto entry = entries_.find(*victim);
    stats_.bytes -= entry->second.bytes;
    recent_.erase(entry->second.recent);
    entries_.erase(entry);
  }
  auto asset = std::make_shared<const NativeTextureAsset>(
      NativeTextureAsset{id, std::move(data)});
  recent_.push_front(id);
  entries_.emplace(id, Entry{asset, recent_.begin(), size});
  stats_.bytes += size;
  return asset;
}
bool NativeTextureLibrary::Write(const std::filesystem::path &path,
                                 std::span<const uint8_t> bytes) {
  if (directory_.empty())
    return false;
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error)
    return false;
  // Derived files only. Interrupted writes fail size/checksum/identity checks.
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  file.close();
  return bool(file);
}
NativeTextureHandle NativeTextureLibrary::Load(uint64_t id) {
  std::lock_guard lock(mutex_);
  if (auto found = Find(id))
    return found;
  NativeTextureData data;
  if (!Read(id, data))
    return {};
  auto asset = Insert(id, std::move(data));
  if (asset)
    ++stats_.loaded;
  return asset;
}
NativeTextureHandle NativeTextureLibrary::Resolve(NativeTextureData data,
                                                  bool generate_mips) {
  std::vector<uint8_t> bytes;
  if (!EncodeNativeTexture(data, bytes))
    return {};
  std::lock_guard lock(mutex_);
  std::filesystem::path recipe;
  if (generate_mips) {
    if (data.dimension != NativeTextureDimension::Image2D ||
        data.mip_levels != 1 || data.format == NativeTextureFormat::RGBA8 ||
        data.width < 8 || data.height < 8)
      return {};
    recipe = directory_ / "mips-v1" /
             FileName(NativeTextureContentId(bytes)).replace_extension(".ref");
    std::ifstream reference;
    if (!directory_.empty())
      reference.open(recipe, std::ios::binary | std::ios::ate);
    if (reference && reference.tellg() != 16)
      ++stats_.invalid;
    std::array<uint8_t, 16> ref{};
    if (reference && reference.tellg() == 16) {
      reference.seekg(0);
      if (reference.read(reinterpret_cast<char *>(ref.data()), ref.size()) &&
          std::equal(kMipMagic.begin(), kMipMagic.end(), ref.begin())) {
        uint64_t id = 0;
        for (unsigned i = 0; i < 8; ++i)
          id |= uint64_t(ref[8 + i]) << (8 * i);
        if (auto found = Find(id);
            found && MatchesMipSource(data, found->data)) {
          ++stats_.cached_mips;
          return found;
        }
        NativeTextureData cached;
        if (Read(id, cached) && MatchesMipSource(data, cached)) {
          auto found = Insert(id, std::move(cached));
          if (found) {
            ++stats_.loaded;
            ++stats_.cached_mips;
          }
          return found;
        }
      }
      ++stats_.invalid;
    }
    NativeTextureData chain;
    if (!GenerateNativeTextureMips(data, chain))
      return {};
    ++stats_.generated_mips;
    data = std::move(chain);
    if (!EncodeNativeTexture(data, bytes))
      return {};
  }
  const auto id = NativeTextureContentId(bytes);
  auto found = Find(id);
  if (found && found->data != data) {
    ++stats_.invalid;
    return {};
  }
  if (!found) {
    NativeTextureData cached;
    const bool loaded = Read(id, cached);
    if (loaded && cached != data) {
      ++stats_.invalid;
      return {};
    }
    found = Insert(id, loaded ? std::move(cached) : std::move(data));
    if (!found)
      return {};
    if (loaded)
      ++stats_.loaded;
    else {
      ++stats_.cooked;
      if (!Write(directory_ / FileName(id), bytes))
        ++stats_.write_failures;
    }
  }
  if (!recipe.empty()) {
    std::array<uint8_t, 16> ref{};
    std::copy(kMipMagic.begin(), kMipMagic.end(), ref.begin());
    for (unsigned i = 0; i < 8; ++i)
      ref[8 + i] = uint8_t(id >> (8 * i));
    if (!Write(recipe, ref))
      ++stats_.write_failures;
  }
  return found;
}
NativeTextureStats NativeTextureLibrary::Stats() const {
  std::lock_guard lock(mutex_);
  auto result = stats_;
  result.resident = entries_.size();
  return result;
}
} // namespace bd::gpu::scene
