/**
 * @file    deferred_work.h
 * @brief   Host deferred-work ordering and transactional arena planning.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace bd::gpu::scene {

// Payload indices refer to the caller's host records, never guest addresses.
struct DeferredSortItem {
  float depth = 0;
  uint32_t payload = 0;
};

// Back-to-front, with deterministic submission order for equal depths. Reject
// invalid keys before changing the caller's order (NaN is not a comparator).
inline bool OrderDeferredWork(std::span<DeferredSortItem> items) {
  for (const auto &item : items)
    if (!std::isfinite(item.depth))
      return false;
  std::sort(items.begin(), items.end(), [](const auto &a, const auto &b) {
    return a.depth != b.depth ? a.depth > b.depth : a.payload < b.payload;
  });
  return true;
}

struct DeferredArenaState {
  uint32_t byte_capacity = 0, bytes_used = 0;
  uint32_t item_capacity = 0, items_used = 0;
};
struct DeferredBatchPlan {
  std::vector<uint32_t> offsets;
  uint32_t bytes_used = 0, items_used = 0;
};

// Offsets, not pointers. Failure leaves the output and arena unchanged, so a
// compound scene submission can reserve all its deferred work before drawing.
inline bool PlanDeferredBatch(const DeferredArenaState &arena,
                              std::span<const uint32_t> sizes,
                              DeferredBatchPlan &out) {
  if ((arena.bytes_used & 3u) || arena.bytes_used > arena.byte_capacity ||
      arena.items_used > arena.item_capacity ||
      sizes.size() > arena.item_capacity - arena.items_used)
    return false;
  uint64_t used = arena.bytes_used;
  for (const uint32_t size : sizes) {
    if (!size || (size & 3u))
      return false;
    used += size;
    if (used > arena.byte_capacity)
      return false;
  }
  DeferredBatchPlan next;
  next.offsets.reserve(sizes.size());
  next.bytes_used = arena.bytes_used;
  next.items_used = arena.items_used + uint32_t(sizes.size());
  for (const uint32_t size : sizes) {
    next.offsets.push_back(next.bytes_used);
    next.bytes_used += size;
  }
  out = std::move(next);
  return true;
}
} // namespace bd::gpu::scene
