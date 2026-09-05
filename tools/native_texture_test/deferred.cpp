/**
 * @file    deferred.cpp
 * @brief   Host deferred-work contracts, without game memory or a GPU.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/scene/deferred_entry_bridge.h"
#include "gpu/scene/deferred_work.h"
#include <array>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <limits>
#include <random>

using namespace bd::gpu::scene;
int main() {
  std::array items{DeferredSortItem{2, 0}, DeferredSortItem{-1, 1},
                   DeferredSortItem{2, 2}, DeferredSortItem{7, 3}};
  assert(OrderDeferredWork(items));
  assert(items[0].payload == 3 && items[1].payload == 0 &&
         items[2].payload == 2 && items[3].payload == 1);
  assert(OrderDeferredWork({}));
  for (float bad : {std::numeric_limits<float>::quiet_NaN(),
                    std::numeric_limits<float>::infinity(),
                    -std::numeric_limits<float>::infinity()}) {
    auto invalid = items;
    invalid[1].depth = bad;
    assert(!OrderDeferredWork(invalid));
    for (size_t i = 0; i < items.size(); ++i)
      assert(invalid[i].payload == items[i].payload);
  }
  std::vector<DeferredSortItem> many;
  std::mt19937 random(17);
  for (uint32_t i = 0; i < 5140; ++i)
    many.push_back({float(int(random() % 200) - 100), i});
  assert(OrderDeferredWork(many));
  std::vector<bool> seen(many.size());
  for (size_t i = 0; i < many.size(); ++i) {
    assert(!seen[many[i].payload]);
    seen[many[i].payload] = true;
    if (i) {
      assert(many[i - 1].depth >= many[i].depth);
      if (many[i - 1].depth == many[i].depth)
        assert(many[i - 1].payload < many[i].payload);
    }
  }
  DeferredBatchPlan plan;
  const std::array<uint32_t, 3> sizes{816, 832, 912};
  assert(PlanDeferredBatch({3000, 100, 8, 4}, sizes, plan));
  assert((plan.offsets == std::vector<uint32_t>{100, 916, 1748}));
  assert(plan.bytes_used == 2660 && plan.items_used == 7);
  const auto saved = plan;
  for (DeferredArenaState invalid : {DeferredArenaState{2659, 100, 8, 4},
                                     {3000, 100, 6, 4},
                                     {99, 100, 8, 4},
                                     {3000, 100, 3, 4},
                                     {3000, 101, 8, 4}}) {
    assert(!PlanDeferredBatch(invalid, sizes, plan));
    assert(plan.offsets == saved.offsets &&
           plan.bytes_used == saved.bytes_used &&
           plan.items_used == saved.items_used);
  }
  assert(PlanDeferredBatch({2660, 100, 7, 4}, sizes, plan)); // exact fit
  for (uint32_t bad : {0u, 3u, 0xfffffffcu}) {
    assert(!PlanDeferredBatch({3000, 100, 8, 4}, std::span(&bad, 1), plan));
    assert(plan.offsets == saved.offsets &&
           plan.bytes_used == saved.bytes_used);
  }
  uint32_t four = 4;
  assert(!PlanDeferredBatch({UINT32_MAX, UINT32_MAX - 3, 8, 4},
                            std::span(&four, 1), plan));
  assert(PlanDeferredBatch({0, 0, 0, 0}, {}, plan));
  assert(plan.offsets.empty() && !plan.bytes_used && !plan.items_used);

  std::vector<uint8_t> image(816 + 9 * 4, 0x35);
  image[289] = 9;
  std::array<uint8_t, 64> matrix;
  for (size_t i = 0; i < matrix.size(); ++i)
    matrix[i] = uint8_t(i);
  auto relocated = image;
  assert(ValidDeferredEntryImage(image));
  assert(RelocateDeferredEntry(image, matrix, 0x50000, 0x12345678, relocated));
  assert(std::equal(matrix.begin(), matrix.end(), relocated.begin() + 16));
  assert(relocated[264] == 0 && relocated[265] == 5 && relocated[266] == 1 &&
         relocated[267] == 0x84);
  assert(relocated[268] == 0x12 && relocated[269] == 0x34 &&
         relocated[270] == 0x56 && relocated[271] == 0x78);
  for (size_t i = 0; i < image.size(); ++i)
    if (!(i >= 16 && i < 80) && !(i >= 264 && i < 272))
      assert(relocated[i] == image[i]);
  const auto before = relocated;
  for (uint32_t bad : {0u, 1u, 0xffffff00u}) {
    assert(!RelocateDeferredEntry(image, matrix, bad, 0, relocated));
    assert(relocated == before);
  }
  image[289] = 10; // declared bones exceed payload
  assert(!ValidDeferredEntryImage(image));
  assert(!RelocateDeferredEntry(image, matrix, 0x50000, 0, relocated));
  assert(relocated == before);
  image.resize(816);
  image[289] = 0xff; // negative bone count has no palette payload
  assert(ValidDeferredEntryImage(image));
  image.resize(815);
  assert(!ValidDeferredEntryImage(image));
}
