/**
 * @file    deferred.cpp
 * @brief   Host deferred-work contracts, without game memory or a GPU.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/scene/deferred_depth.h"
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

void TestDepth() {
  const DeferredMatrix identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  DeferredDepthRecipe bounds;
  bounds.centre = {2, 3, -10};
  bounds.radius = 2;
  assert(EvaluateDeferredDepth(bounds, identity, identity) == 8);
  auto world = identity;
  world[14] = -5;
  assert(EvaluateDeferredDepth(bounds, world, identity) == 13);
  auto view = identity;
  view[14] = 7;
  assert(EvaluateDeferredDepth(bounds, world, view) == 6);
  // Camera rotation uses X, not the previously captured Z key.
  view = {0, 0, -1, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1};
  assert(EvaluateDeferredDepth(bounds, identity, view) == 0);
  // Preserve the producer's unscaled far extent; no invented max-axis scale.
  world = identity;
  world[0] = 2;
  world[5] = 3;
  world[10] = 4;
  assert(EvaluateDeferredDepth(bounds, world, identity) == 38);
  bounds.radius = -2;
  assert(EvaluateDeferredDepth(bounds, identity, identity) == 12);
  bounds.radius = 2;

  // Compare arbitrary affine transforms against independent double-precision
  // point->world->view math, not a copy of the optimized Z-column expression.
  std::mt19937 random(123);
  auto value = [&] { return (int(random() % 2001) - 1000) / 100.0f; };
  for (int sample = 0; sample < 1000; ++sample) {
    world = identity;
    view = identity;
    for (size_t row = 0; row < 4; ++row)
      for (size_t col = 0; col < 3; ++col) {
        world[row * 4 + col] = value();
        view[row * 4 + col] = value();
      }
    for (auto &axis : bounds.centre)
      axis = value();
    bounds.radius = value();
    std::array<double, 4> point{bounds.centre[0], bounds.centre[1],
                                bounds.centre[2], 1};
    std::array<double, 4> transformed{};
    for (size_t col = 0; col < 4; ++col)
      for (size_t row = 0; row < 4; ++row)
        transformed[col] += point[row] * world[row * 4 + col];
    double reference = -bounds.radius;
    for (size_t row = 0; row < 4; ++row)
      reference -= transformed[row] * view[row * 4 + 2];
    const auto actual = EvaluateDeferredDepth(bounds, world, view);
    assert(actual && std::fabs(*actual - reference) <
                         0.001 + std::fabs(reference) * 1e-5);
  }

  bounds = {};
  for (float bad : {std::numeric_limits<float>::quiet_NaN(),
                    std::numeric_limits<float>::infinity(),
                    -std::numeric_limits<float>::infinity()}) {
    bounds.radius = bad;
    assert(!EvaluateDeferredDepth(bounds, identity, identity));
    bounds.radius = 0;
    for (size_t i = 0; i < 3; ++i) {
      bounds.centre[i] = bad;
      assert(!EvaluateDeferredDepth(bounds, identity, identity));
      bounds.centre[i] = 0;
    }
    for (size_t i = 0; i < 16; ++i) {
      world = identity;
      world[i] = bad;
      assert(!EvaluateDeferredDepth(bounds, world, identity));
      assert(!EvaluateDeferredDepth(bounds, identity, world));
    }
  }
  world = identity;
  world[10] = std::numeric_limits<float>::max();
  bounds.centre[2] = 2;
  assert(!EvaluateDeferredDepth(bounds, world,
                                identity)); // finite inputs overflow
  bounds.kind = static_cast<DeferredDepthRecipe::Kind>(99);
  assert(!EvaluateDeferredDepth(bounds, identity, identity));
  bounds.kind = DeferredDepthRecipe::Kind::Fixed;
  bounds.fixed_depth = -17;
  world.fill(std::numeric_limits<float>::quiet_NaN());
  assert(EvaluateDeferredDepth(bounds, world, world) ==
         -17); // no matrix dependency
  bounds.fixed_depth = std::numeric_limits<float>::infinity();
  assert(!EvaluateDeferredDepth(bounds, identity, identity));

  // Live movement must actually change back-to-front submission order.
  bounds = {};
  bounds.centre = {0, 0, -10};
  std::array work{
      DeferredSortItem{*EvaluateDeferredDepth(bounds, identity, identity), 0},
      DeferredSortItem{15, 1}};
  assert(OrderDeferredWork(work) && work[0].payload == 1);
  world = identity;
  world[14] = -20;
  work[1].depth = *EvaluateDeferredDepth(bounds, world, identity);
  assert(OrderDeferredWork(work) && work[0].payload == 0);
}

int main() {
  TestDepth();
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
