/**
 * @file    deferred_depth_import.cpp
 * @brief   Import native bounds/policy once; use current transforms on replay.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/scene/deferred_depth_import.h"
#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/frame_stats.h"
#include "gpu/scene/deferred_entry_bridge.h"
#include <cstring>
#include <unordered_map>

namespace bd::gpu::scene {
namespace {
constexpr uint32_t kViewMatrix =
    (uint32_t(-32034) << 16) - 19936 + 65536 - 10816;
constexpr uint32_t kFixedDepth = (uint32_t(-32251) << 16) + 20912;
constexpr size_t kMaxDepthImports = 5140;
thread_local std::unordered_map<uint32_t, DeferredDepthRecipe> imports;
struct Stats {
  uint64_t imported = 0, fixed = 0, produced = 0, compatibility = 0;
  uint64_t replayed = 0, changed = 0;
  uint64_t checked = 0, wrong = 0, refused = 0;
  uint32_t frame = 0;
};
thread_local Stats stats;
void Report(bool refused = false) {
  if (refused && ++stats.refused <= 8)
    BD_WARN("[native-depth] unknown policy or invalid depth inputs; refusing "
            "native result");
  const auto frame = FrameStatFrameCount();
  if (frame - stats.frame >= 300) {
    BD_INFO(
        "[native-depth] imported {} fixed {}; produced {} compatibility {}; "
        "replayed {} changed {}; checked {} "
        "wrong {}; refused {} (cumulative, engine transform import remains)",
        stats.imported, stats.fixed, stats.produced, stats.compatibility,
        stats.replayed, stats.changed, stats.checked, stats.wrong,
        stats.refused);
    stats.frame = frame;
  }
}
std::optional<float> ReadFloat(uint64_t address) {
  if (address > UINT32_MAX - 3)
    return {};
  const auto *value = bd::mem::try_at<const bd::be<float>>(uint32_t(address));
  if (!value || !std::isfinite(float(*value)))
    return {};
  return float(*value);
}
bool ReadMatrix(uint32_t address, DeferredMatrix &matrix) {
  if (!address)
    return false;
  for (size_t i = 0; i < matrix.size(); ++i) {
    const auto value = ReadFloat(uint64_t(address) + i * 4);
    if (!value)
      return false;
    matrix[i] = *value;
  }
  return true;
}
float ImageFloat(const uint8_t *bytes) {
  uint32_t word;
  std::memcpy(&word, bytes, 4);
  word = __builtin_bswap32(word);
  float value;
  std::memcpy(&value, &word, 4);
  return value;
}
} // namespace

void ResetDeferredDepthImports() { imports.clear(); }

std::optional<float> ImportDeferredDepth(uint32_t entry, uint32_t matrix,
                                         uint32_t mesh, bool fixed) {
  imports.erase(entry); // a reused slot must never retain its previous policy
  DeferredDepthRecipe recipe;
  DeferredMatrix world{}, view{};
  bool valid = entry != 0 && !(entry & 3) && entry <= UINT32_MAX - 279 &&
               imports.size() < kMaxDepthImports &&
               bd::mem::try_at<bd::be<float>>(entry + 276);
  if (fixed || !mesh) {
    recipe.kind = DeferredDepthRecipe::Kind::Fixed;
    const auto value = ReadFloat(kFixedDepth);
    valid &= value.has_value();
    recipe.fixed_depth = value.value_or(0);
  } else {
    for (size_t i = 0; i < 3; ++i) {
      const auto value = ReadFloat(uint64_t(mesh) + 20 + i * 4);
      valid &= value.has_value();
      recipe.centre[i] = value.value_or(0);
    }
    const auto radius = ReadFloat(uint64_t(mesh) + 32);
    valid &= radius.has_value();
    recipe.radius = radius.value_or(0);
    valid &= ReadMatrix(matrix, world) && ReadMatrix(kViewMatrix, view);
  }
  const auto depth =
      valid ? EvaluateDeferredDepth(recipe, world, view) : std::nullopt;
  if (!depth) {
    Report(true);
    return {};
  }
  imports.emplace(entry, recipe);
  ++stats.imported;
  stats.fixed += recipe.kind == DeferredDepthRecipe::Kind::Fixed;
  Report();
  return depth;
}

std::optional<DeferredDepthRecipe> CapturedDeferredDepth(uint32_t entry) {
  const auto it = imports.find(entry);
  return it != imports.end() ? std::optional(it->second) : std::nullopt;
}

void VerifyDeferredDepth(uint32_t entry, float expected) {
  const auto actual = ReadFloat(uint64_t(entry) + 276);
  ++stats.checked;
  if (!actual ||
      std::fabs(*actual - expected) > 1e-5f * (1 + std::fabs(expected))) {
    if (++stats.wrong <= 8)
      BD_WARN("[native-depth] mismatch entry {:08x}: host {} engine {}", entry,
              expected, actual.value_or(NAN));
  }
  Report();
}

bool PublishDeferredDepth(uint32_t entry, float depth) {
  if (!entry || (entry & 3) || entry > UINT32_MAX - 279 ||
      !std::isfinite(depth) || !bd::mem::try_store<float>(entry + 276, depth)) {
    imports.erase(entry);
    Report(true);
    return false;
  }
  ++stats.produced;
  Report();
  return true;
}

void RecordDeferredDepthFallback() {
  ++stats.compatibility;
  Report();
}

bool CopyDeferredMatrix(uint32_t address, std::array<uint8_t, 64> &image) {
  if (!address || (address & 3) || address > UINT32_MAX - 63)
    return false;
  const auto *start = bd::mem::try_at<const uint8_t>(address);
  // A 64-byte aligned-float image can touch at most two pages.
  if (!start || !bd::mem::try_at<const uint8_t>(address + 63))
    return false;
  std::memcpy(image.data(), start, image.size());
  return true;
}

bool ComposeDeferredDepths(std::span<const DeferredEntryRecipe> entries,
                           std::span<const uint8_t, 64> matrix,
                           std::vector<float> &depths) {
  DeferredMatrix world{}, view{};
  bool needs_transform = false;
  for (const auto &entry : entries) {
    if (!entry.depth) {
      Report(true);
      return false;
    }
    needs_transform |=
        entry.depth->kind == DeferredDepthRecipe::Kind::BoundsFarExtent;
  }
  if (needs_transform) {
    for (size_t i = 0; i < world.size(); ++i)
      world[i] = ImageFloat(matrix.data() + i * 4);
    if (!ReadMatrix(kViewMatrix, view)) {
      Report(true);
      return false;
    }
  }
  if (!EvaluateDeferredEntryDepths(entries, world, view, depths)) {
    Report(true);
    return false;
  }
  return true;
}

void RecordDeferredDepthReplay(std::span<const DeferredEntryRecipe> entries,
                               std::span<const float> depths) {
  for (size_t i = 0; i < entries.size(); ++i) {
    ++stats.replayed;
    const auto &image = entries[i].compatibility_image;
    stats.changed +=
        image.size() >= 280 && ImageFloat(image.data() + 276) != depths[i];
  }
  Report();
}
} // namespace bd::gpu::scene
