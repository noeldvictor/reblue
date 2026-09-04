/**
 * @file    fenced_asset_cache.h
 * @brief   Shared native GPU residency, reclaimed only at a proven fence.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license   BSD 3-Clause License
 */
#pragma once
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>

namespace bd::gpu::scene {
struct FencedAssetStats {
  uint64_t created = 0, reused = 0, retired = 0, refused = 0, failed = 0;
  uint64_t bytes = 0, resident = 0;
};

// Caller serializes all operations with its renderer lock. A handle must live
// through command recording, not GPU execution: unused entries are marked only
// after the current slot's entry drain, and released on its NEXT proven fence.
// Pending entries remain counted against the budget and can be reacquired.
template <typename T> class FencedAssetCache {
public:
  using Handle = std::shared_ptr<const T>;
  explicit FencedAssetCache(uint64_t byte_budget, uint64_t entry_budget = 8192)
      : byte_budget_(byte_budget), entry_budget_(entry_budget) {}

  template <typename Create>
  Handle Acquire(uint64_t id, uint64_t bytes, Create &&create) {
    if (auto it = entries_.find(id); it != entries_.end()) {
      if (it->second.bytes != bytes) {
        ++stats_.refused;
        return {};
      }
      it->second.retire_slot = kLive;
      ++stats_.reused;
      return it->second.handle;
    }
    if (!bytes || bytes > byte_budget_ || bytes > byte_budget_ - stats_.bytes ||
        entries_.size() >= entry_budget_) {
      ++stats_.refused;
      return {};
    }
    Handle handle = std::forward<Create>(create)();
    if (!handle) {
      ++stats_.failed;
      return {};
    }
    entries_.emplace(id, Entry{handle, bytes, kLive});
    stats_.bytes += bytes;
    ++stats_.created;
    return handle;
  }

  // Only at slot-entry, after its submitted work has actually completed.
  // Retire must invalidate/free the descriptor before the last image/view dies.
  template <typename Retire> void AfterFence(uint32_t slot, Retire &&retire) {
    for (auto it = entries_.begin(); it != entries_.end();) {
      auto &entry = it->second;
      if (entry.retire_slot == slot) {
        if (entry.handle.use_count() == 1) {
          retire(*entry.handle);
          stats_.bytes -= entry.bytes;
          ++stats_.retired;
          it = entries_.erase(it);
          continue;
        }
        entry.retire_slot = kLive;
      }
      ++it;
    }
  }

  // After the slot-entry drain, never before it. A later upload into this slot
  // is unmarked until a subsequent call; no unsubmitted upload can be freed.
  void MarkUnused(uint32_t recording_slot) {
    for (auto &[id, entry] : entries_)
      if (entry.retire_slot == kLive && entry.handle.use_count() == 1)
        entry.retire_slot = recording_slot;
  }
  FencedAssetStats Stats() const {
    auto result = stats_;
    result.resident = entries_.size();
    return result;
  }

private:
  static constexpr uint32_t kLive = ~uint32_t{0};
  struct Entry {
    Handle handle;
    uint64_t bytes;
    uint32_t retire_slot;
  };
  uint64_t byte_budget_, entry_budget_;
  std::unordered_map<uint64_t, Entry> entries_;
  FencedAssetStats stats_;
};
} // namespace bd::gpu::scene
