/**
 * @file    deferred_list.cpp
 * @brief   Bounded host allocation/ordering for the remaining scene bridge.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/scene/deferred_list.h"
#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/frame_stats.h"
#include "gpu/scene/deferred_entry_bridge.h"
#include "gpu/scene/deferred_work.h"
#include <cstring>

namespace bd::gpu::scene {
namespace {
// bdInitInputSystem initializes a 4 MiB pool and a 20560-byte pointer array.
// sub_8227DB50 previously checked only the byte pool, not the pointer array.
constexpr uint32_t kList = 0x82DBA8F8u;
constexpr uint32_t kPoolBytes = 4 * 1024 * 1024;
constexpr uint32_t kMaxEntries = 20560 / 4;

struct Stats {
  uint64_t allocated = 0, batches = 0, ordered = 0, items = 0, refused = 0;
  uint32_t frame = 0;
};
thread_local Stats stats;
void Report(bool refused = false) {
  if (refused && ++stats.refused <= 8)
    BD_WARN("[host-deferred] refused invalid or exhausted scene-entry bridge");
  const auto frame = FrameStatFrameCount();
  if (frame - stats.frame >= 300) {
    BD_INFO(
        "[host-deferred] allocated {} entries in {} host batches; ordered {} "
        "lists / {} items; {} refused (cumulative, guest consumer remains)",
        stats.allocated, stats.batches, stats.ordered, stats.items,
        stats.refused);
    stats.frame = frame;
  }
}

// Check every touched page and the full arithmetic extent before any writes.
uint8_t *Range(uint64_t address, uint64_t bytes) {
  if (!address || !bytes || address > UINT32_MAX || bytes > UINT32_MAX ||
      address + bytes - 1 > UINT32_MAX)
    return nullptr;
  auto *ptr = bd::mem::try_at<uint8_t>(uint32_t(address));
  if (!ptr)
    return nullptr;
  for (uint64_t page = (address & ~uint64_t(4095)) + 4096;
       page < address + bytes; page += 4096)
    if (!bd::mem::try_at<uint8_t>(uint32_t(page)))
      return nullptr;
  return ptr;
}
void StoreWord(uint8_t *dst, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(dst, &value, 4);
}

struct BridgePlan {
  DeferredBatchPlan batch;
  uint32_t pool = 0, array = 0;
  uint8_t *state = nullptr, *data = nullptr, *slots = nullptr;
};
bool Prepare(std::span<const uint32_t> sizes, BridgePlan &out) {
  auto *state = Range(kList, 24);
  if (!state || sizes.empty())
    return false;
  const uint32_t pool = bd::mem::try_load<uint32_t>(kList + 8);
  const uint32_t cursor = bd::mem::try_load<uint32_t>(kList + 4);
  const uint32_t array = bd::mem::try_load<uint32_t>(kList + 12);
  const uint32_t slot_cursor = bd::mem::try_load<uint32_t>(kList + 16);
  const uint32_t count = bd::mem::try_load<uint32_t>(kList + 20);
  if (!pool || !array || (pool & 3) || (cursor & 3) || (array & 3) ||
      cursor < pool || uint64_t(pool) + kPoolBytes > UINT32_MAX ||
      uint64_t(array) + kMaxEntries * 4 > UINT32_MAX ||
      uint64_t(array) + uint64_t(count) * 4 != slot_cursor)
    return false;
  for (uint32_t size : sizes)
    if (size < kDeferredEntryBytes || size > kDeferredMaxEntryBytes)
      return false;
  BridgePlan plan;
  if (!PlanDeferredBatch({kPoolBytes, cursor - pool, kMaxEntries, count}, sizes,
                         plan.batch))
    return false;
  plan.pool = pool;
  plan.array = array;
  plan.state = state;
  plan.data = Range(cursor, plan.batch.bytes_used - (cursor - pool));
  plan.slots = Range(slot_cursor, uint64_t(sizes.size()) * 4);
  if (!plan.data || !plan.slots)
    return false;
  out = std::move(plan);
  return true;
}
void Publish(const BridgePlan &plan) {
  for (size_t i = 0; i < plan.batch.offsets.size(); ++i)
    StoreWord(plan.slots + i * 4, plan.pool + plan.batch.offsets[i]);
  StoreWord(plan.state, plan.pool + plan.batch.offsets.back());
  StoreWord(plan.state + 4, plan.pool + plan.batch.bytes_used);
  StoreWord(plan.state + 16, plan.array + plan.batch.items_used * 4);
  StoreWord(plan.state + 20, plan.batch.items_used);
  stats.allocated += plan.batch.offsets.size();
  ++stats.batches;
  Report();
}
bool ImageSizes(std::span<const std::vector<uint8_t>> images,
                std::vector<uint32_t> &sizes) {
  sizes.clear();
  if (images.empty() || images.size() > 64)
    return false;
  for (const auto &image : images) {
    if (!ValidDeferredEntryImage(image))
      return false;
    sizes.push_back(uint32_t(image.size()));
  }
  return true;
}
} // namespace

uint32_t AllocateDeferredEntry(uint32_t bytes) {
  BridgePlan plan;
  if (!Prepare(std::span(&bytes, 1), plan)) {
    Report(true);
    return 0;
  }
  Publish(plan);
  return plan.pool + plan.batch.offsets.front();
}

bool CanAppendDeferredEntries(std::span<const std::vector<uint8_t>> images) {
  thread_local std::vector<uint32_t> sizes;
  BridgePlan plan;
  return ImageSizes(images, sizes) && Prepare(sizes, plan);
}

bool AppendDeferredEntries(std::span<const std::vector<uint8_t>> images,
                           std::span<const uint8_t, 64> matrix,
                           uint32_t palette) {
  thread_local std::vector<uint32_t> sizes;
  BridgePlan plan;
  if (!ImageSizes(images, sizes) || !Prepare(sizes, plan)) {
    Report(true);
    return false;
  }
  // The draw thread owns this list. Populate the entire reserved batch before
  // exposing its slots/count; no partial list remains on validation failure.
  for (size_t i = 0; i < images.size(); ++i) {
    auto *dst = plan.data + (plan.batch.offsets[i] - plan.batch.offsets.front());
    // bdSceneNodeDrawSingle stores entry+388 into the callback pointer (+264).
    // This is a self-relative reference, not the original pooled address.
    // All validation below was already performed by ImageSizes/Prepare.
    if (!RelocateDeferredEntry(images[i], matrix,
                               plan.pool + plan.batch.offsets[i], palette,
                               std::span(dst, images[i].size())))
      return false; // never publish a failed batch
  }
  Publish(plan);
  return true;
}

bool OrderDeferredEntries(uint32_t array, int32_t first, int32_t last) {
  if (first >= last)
    return true;
  if (first < 0 || last >= int32_t(kMaxEntries) || (array & 3)) {
    Report(true);
    return false;
  }
  const uint32_t count = uint32_t(last - first + 1);
  auto *slots =
      Range(uint64_t(array) + uint32_t(first) * 4, uint64_t(count) * 4);
  if (!slots) {
    Report(true);
    return false;
  }
  thread_local std::vector<DeferredSortItem> order;
  thread_local std::vector<uint32_t> entries;
  order.clear();
  entries.clear();
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t entry;
    std::memcpy(&entry, slots + i * 4, 4);
    entry = __builtin_bswap32(entry);
    const auto *key = Range(uint64_t(entry) + 276, 4);
    if (!entry || (entry & 3) || !key) {
      Report(true);
      return false;
    }
    uint32_t bits;
    std::memcpy(&bits, key, 4);
    bits = __builtin_bswap32(bits);
    float depth;
    std::memcpy(&depth, &bits, 4);
    order.push_back({depth, i});
    entries.push_back(entry);
  }
  if (!OrderDeferredWork(order)) {
    Report(true);
    return false; // preserve submission order for invalid input, never NaN sort
  }
  for (uint32_t i = 0; i < count; ++i)
    StoreWord(slots + i * 4, entries[order[i].payload]);
  ++stats.ordered;
  stats.items += count;
  Report();
  return true;
}
} // namespace bd::gpu::scene
