/**
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause License
 */
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace bd::gpu {
// Checked in wide arithmetic. Failure never changes a caller's cursor.
inline std::optional<uint32_t> ReserveUploadRange(uint32_t cursor,
                                                  uint32_t size,
                                                  uint32_t alignment,
                                                  uint32_t capacity) {
  if (!size || !alignment || (alignment & (alignment - 1)))
    return {};
  const uint64_t offset =
      (uint64_t(cursor) + alignment - 1) & ~(uint64_t(alignment) - 1);
  if (offset + size > capacity)
    return {};
  return uint32_t(offset);
}

// Resource-independent host upload residency. A page never moves or rewinds
// while its slot records/is in flight. Only the renderer's completed-fence
// boundary may call ResetAfterFence. Callers serialize allocation and reset.
template <class Resource, size_t SlotCount> class UploadPageArena {
public:
  struct Allocation {
    Resource *resource = nullptr;
    uint32_t offset = 0, size = 0;
  };
  struct Statistics {
    uint64_t reserved = 0, peak_reserved = 0, peak_frame_bytes = 0;
    uint64_t created = 0, retired = 0, allocations = 0, failures = 0;
  };
  UploadPageArena(uint32_t page_bytes, uint32_t max_allocation, uint64_t budget)
      : page_bytes_(page_bytes), max_allocation_(max_allocation),
        budget_(budget) {}

  template <class Factory>
  Allocation Allocate(size_t slot, uint32_t size, uint32_t alignment,
                      Factory create) {
    if (slot >= SlotCount || size > max_allocation_ ||
        !ReserveUploadRange(0, size, alignment, max_allocation_)) {
      ++stats_.failures;
      return {};
    }
    auto &frame = frames_[slot];
    for (auto &page : frame.pages) {
      if (auto offset =
              ReserveUploadRange(page.cursor, size, alignment, page.capacity))
        return Commit(frame, page, *offset, size);
    }
    const uint32_t capacity = std::max(page_bytes_, size);
    if (capacity > budget_ || stats_.reserved > budget_ - capacity) {
      ++stats_.failures;
      return {};
    }
    auto resource = create(capacity);
    if (!resource) {
      ++stats_.failures;
      return {};
    }
    frame.pages.push_back({std::move(resource), capacity});
    stats_.reserved += capacity;
    stats_.peak_reserved = std::max(stats_.peak_reserved, stats_.reserved);
    ++stats_.created;
    return Commit(frame, frame.pages.back(), 0, size);
  }

  void ResetAfterFence(size_t slot) {
    ResetAfterFence(slot, [](Resource &, bool) {});
  }
  template <class BeforeReuse>
  void ResetAfterFence(size_t slot, BeforeReuse before_reuse) {
    if (slot >= SlotCount)
      return;
    auto &frame = frames_[slot];
    std::erase_if(frame.pages, [&](const Page &page) {
      // Large loading bursts do not permanently raise idle residency. Ordinary
      // pages get one idle slot cycle before release; one-off large pages go
      // immediately after the fence covering their last copy.
      const bool retiring = !page.used || page.capacity > page_bytes_;
      before_reuse(*page.resource, retiring);
      if (!retiring)
        return false;
      stats_.reserved -= page.capacity;
      ++stats_.retired;
      return true;
    });
    for (auto &page : frame.pages) {
      page.cursor = 0;
      page.used = false;
    }
    frame.bytes = 0;
  }
  const Statistics &Stats() const { return stats_; }
  template <class Predicate> bool Contains(Predicate predicate) const {
    for (const auto &frame : frames_)
      for (const auto &page : frame.pages)
        if (predicate(*page.resource))
          return true;
    return false;
  }

private:
  struct Page {
    std::unique_ptr<Resource> resource;
    uint32_t capacity = 0, cursor = 0;
    bool used = false;
  };
  struct Frame {
    std::vector<Page> pages;
    uint64_t bytes = 0;
  };
  Allocation Commit(Frame &frame, Page &page, uint32_t offset, uint32_t size) {
    frame.bytes += uint64_t(offset) + size - page.cursor;
    page.cursor = offset + size;
    page.used = true;
    ++stats_.allocations;
    stats_.peak_frame_bytes = std::max(stats_.peak_frame_bytes, frame.bytes);
    return {page.resource.get(), offset, size};
  }
  std::array<Frame, SlotCount> frames_;
  uint32_t page_bytes_, max_allocation_;
  uint64_t budget_;
  Statistics stats_;
};
} // namespace bd::gpu
