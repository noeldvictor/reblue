/**
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause License
 */
#include "gpu/upload_page_arena.h"
#include <cstdlib>
#include <iostream>

using namespace bd::gpu;
static void Check(bool value, const char *why) {
  if (!value) {
    std::cerr << why << '\n';
    std::exit(1);
  }
}
struct Page {
  std::vector<uint8_t> bytes;
  unsigned *destroyed;
  Page(uint32_t size, unsigned &count) : bytes(size), destroyed(&count) {}
  ~Page() { ++*destroyed; }
};

int main() {
  Check(ReserveUploadRange(3, 4, 4, 8) == 4, "aligned exact-end allocation");
  Check(!ReserveUploadRange(5, 4, 4, 8), "alignment padding cannot overrun");
  Check(!ReserveUploadRange(0, 9, 1, 8),
        "single request cannot cross its slot");
  Check(!ReserveUploadRange(UINT32_MAX - 1, 4, 4, UINT32_MAX),
        "alignment must not wrap");
  Check(!ReserveUploadRange(UINT32_MAX - 1, UINT32_MAX, 1, UINT32_MAX),
        "size must not wrap");
  Check(!ReserveUploadRange(0, 0, 1, 8) && !ReserveUploadRange(0, 1, 0, 8) &&
            !ReserveUploadRange(0, 1, 3, 8),
        "reject empty and invalid alignments");

  unsigned destroyed = 0, created = 0;
  auto create = [&](uint32_t size) {
    ++created;
    return std::make_unique<Page>(size, destroyed);
  };
  UploadPageArena<Page, 2> arena(16, 64, 96);
  auto a = arena.Allocate(0, 12, 4, create);
  auto b = arena.Allocate(0, 8, 8, create);
  auto c = arena.Allocate(1, 16, 4, create);
  Check(a.resource && b.resource && c.resource && created == 3,
        "page overflow and another in-flight slot allocate separate pages");
  std::fill(a.resource->bytes.begin(), a.resource->bytes.end(), 0xA5);
  std::fill(b.resource->bytes.begin(), b.resource->bytes.end(), 0xB6);
  std::fill(c.resource->bytes.begin(), c.resource->bytes.end(), 0xC7);
  auto tail = arena.Allocate(0, 4, 4, create);
  Check(tail.resource == a.resource && tail.offset == 12,
        "remaining space is non-overlapping and still reusable");
  auto large = arena.Allocate(0, 48, 16, create);
  Check(large.resource && large.offset == 0 && arena.Stats().reserved == 96,
        "large allocation fits a dedicated page at the exact global budget");
  const auto before = arena.Stats();
  Check(!arena.Allocate(1, 16, 4, create).resource && created == 4 &&
            arena.Stats().reserved == before.reserved,
        "budget refusal neither calls factory nor changes residency");
  Check(!arena.Allocate(2, 1, 1, create).resource &&
            !arena.Allocate(0, 65, 1, create).resource,
        "invalid slots and oversized uploads are refused");
  arena.ResetAfterFence(1);
  Check(destroyed == 0 && a.resource->bytes[0] == 0xA5 &&
            b.resource->bytes[0] == 0xB6,
        "other slot fence cannot retire or rewind this slot's data");
  auto again = arena.Allocate(1, 16, 4, create);
  Check(again.resource == c.resource && again.offset == 0,
        "only completed slot storage can rewind");
  unsigned reset_callbacks = 0, retire_callbacks = 0;
  arena.ResetAfterFence(0, [&](Page &page, bool retiring) {
    Check(!page.bytes.empty() && destroyed == 0,
          "binding invalidation precedes page destruction");
    ++reset_callbacks;
    retire_callbacks += retiring;
  });
  Check(
      reset_callbacks == 3 && retire_callbacks == 1,
      "rewound streams are scrubbed, only retired pages forget heap bindings");
  Check(destroyed == 1 && arena.Stats().reserved == 48,
        "large burst page retires only after its own fence");
  Check(arena.Contains([&](const Page &p) {
    return &p == a.resource;
  }) && !arena.Contains([&](const Page &p) { return &p == large.resource; }),
        "only live pages are identified as transient upload storage");
  auto reused = arena.Allocate(0, 12, 4, create);
  Check(reused.resource == a.resource && reused.offset == 0 && created == 4,
        "ordinary page is retained for the next recording cycle");
  arena.ResetAfterFence(0);
  Check(destroyed == 2,
        "unused second page is trimmed after an idle slot cycle");
  arena.ResetAfterFence(0);
  Check(destroyed == 3, "fully idle slot releases ordinary pages");

  UploadPageArena<Page, 1> failure(16, 64, 64);
  auto null_factory = [](uint32_t) -> std::unique_ptr<Page> { return {}; };
  Check(!failure.Allocate(0, 16, 4, null_factory).resource &&
            failure.Stats().reserved == 0 && failure.Stats().created == 0,
        "buffer/map failure is transactional");
  Check(failure.Allocate(0, 16, 4, create).resource != nullptr,
        "creation failure does not poison a later upload");

  // Real-sized loading burst: preserve every earlier copy source while
  // exceeding the old 32 MiB shader slot several times, across both slots.
  UploadPageArena<Page, 2> stress(4u << 20, 64u << 20, 256ull << 20);
  std::vector<UploadPageArena<Page, 2>::Allocation> uploads;
  for (unsigned i = 0; i < 160; ++i) {
    auto upload = stress.Allocate(i & 1u, 1u << 20, 512, create);
    Check(upload.resource != nullptr,
          "160 MiB upload burst fits bounded arena");
    std::fill_n(upload.resource->bytes.data() + upload.offset, upload.size,
                uint8_t(i));
    uploads.push_back(upload);
  }
  for (unsigned i = 0; i < uploads.size(); ++i) {
    const auto &upload = uploads[i];
    const auto *start = upload.resource->bytes.data() + upload.offset;
    Check(std::all_of(start, start + upload.size,
                      [&](uint8_t b) { return b == uint8_t(i); }),
          "later uploads must never wrap over an earlier copy source");
  }
  Check(stress.Stats().peak_frame_bytes == (80ull << 20) &&
            stress.Stats().reserved == (160ull << 20) &&
            stress.Stats().failures == 0,
        "exact stress accounting");
  stress.ResetAfterFence(0);
  stress.ResetAfterFence(0);
  Check(stress.Stats().reserved == (80ull << 20),
        "retire only the completed slot");
  auto maximum = stress.Allocate(0, 64u << 20, 512, create);
  Check(maximum.resource && maximum.size == (64u << 20),
        "one 64 MiB image is not limited by the old 32 MiB shader slot");
  std::cout << "host upload pages: bounds, non-overlap, budgets and fence "
               "reuse passed\n";
}
