#include "gpu/scene/fenced_asset_cache.h"
#include <iostream>
#include <stdexcept>

using bd::gpu::scene::FencedAssetCache;
namespace {
void Check(bool good, const char *what) {
  if (!good)
    throw std::runtime_error(what);
}
struct Image {
  int *destroyed;
  bool *descriptor_live;
  ~Image() {
    // Real native GPU release nulls the descriptor before destroying the view
    // or image. No fence callback means this assertion fails too.
    if (*descriptor_live)
      std::terminate();
    ++*destroyed;
  }
};
} // namespace
int main() {
  try {
    int destroyed = 0, uploads = 0, retired = 0;
    bool descriptor_live = false;
    FencedAssetCache<Image> cache(100);
    auto create = [&] {
      ++uploads;
      descriptor_live = true;
      return std::shared_ptr<const Image>(
          new Image{&destroyed, &descriptor_live});
    };
    auto retire = [&](const Image &image) {
      Check(image.descriptor_live == &descriptor_live,
            "retire correct binding");
      descriptor_live = false;
      ++retired;
    };
    auto first = cache.Acquire(7, 80, create);
    auto second = cache.Acquire(7, 80, create);
    Check(first == second && uploads == 1, "one upload across two owners");
    first.reset();
    cache.MarkUnused(0);
    cache.AfterFence(0, retire);
    Check(retired == 0 && descriptor_live,
          "one wrapper free preserves other owner");
    auto scene = second;
    second.reset();
    cache.MarkUnused(1);
    cache.AfterFence(1, retire);
    Check(retired == 0 && descriptor_live,
          "native scene outlives all wrappers");
    scene.reset();
    cache.MarkUnused(0);
    Check(cache.Stats().bytes == 80, "pending bytes still budgeted");
    Check(!cache.Acquire(8, 40, create),
          "pending image cannot finance over-budget upload");
    cache.AfterFence(1, retire);
    Check(retired == 0 && descriptor_live, "wrong fence does not free image");
    auto reacquired = cache.Acquire(7, 80, create);
    Check(uploads == 1, "reacquire pending image without upload");
    cache.AfterFence(0, retire);
    Check(retired == 0 && descriptor_live, "reacquisition cancels retirement");
    // Releasing after this slot's drain only schedules its NEXT fence.
    reacquired.reset();
    cache.MarkUnused(0);
    Check(retired == 0 && destroyed == 0,
          "mark never frees unsubmitted references");
    cache.AfterFence(1, retire);
    cache.AfterFence(0, retire);
    Check(retired == 1 && destroyed == 1 && !descriptor_live,
          "last pin plus matching fence retires once");
    Check(cache.Stats().bytes == 0 && cache.Stats().resident == 0,
          "reclaim budget after fence");
    cache.AfterFence(0, retire);
    Check(retired == 1, "repeat fence cannot double free descriptor");
    Check(!cache.Acquire(9, 101, create) && uploads == 1,
          "oversize refused before upload");
    Check(!cache.Acquire(9, 0, create), "zero-size refusal");
    Check(!cache.Acquire(9, 80, [] { return std::shared_ptr<const Image>{}; }),
          "failed upload transactional");
    Check(cache.Stats().bytes == 0 && cache.Stats().failed == 1,
          "failure consumes no budget");
    auto replacement = cache.Acquire(9, 80, create);
    Check(replacement && uploads == 2,
          "budget reused after completed retirement");
    Check(!cache.Acquire(9, 81, create), "key with mismatched size refused");
    replacement.reset();
    cache.MarkUnused(1);
    cache.AfterFence(1, retire);
    Check(destroyed == 2 && retired == 2, "replacement retired");

    // An external weak observer can pin an entry after it was marked. The
    // matching fence must notice that pin, without needing an Acquire call.
    auto observed = cache.Acquire(10, 80, create);
    std::weak_ptr<const Image> observer = observed;
    observed.reset();
    cache.MarkUnused(0);
    auto rescued = observer.lock();
    cache.AfterFence(0, retire);
    Check(destroyed == 2 && descriptor_live,
          "late observer pin survives fence");
    rescued.reset();
    cache.MarkUnused(1);
    cache.AfterFence(0, retire);
    Check(destroyed == 2, "rescued entry requires newly marked fence");
    cache.AfterFence(1, retire);
    Check(destroyed == 3 && observer.expired(),
          "late observer eventually released");

    FencedAssetCache<int> one_entry(100, 1);
    auto a =
        one_entry.Acquire(1, 1, [] { return std::make_shared<const int>(1); });
    Check(
        !one_entry.Acquire(2, 1, [] { return std::make_shared<const int>(2); }),
        "entry budget enforced");
    a.reset();
    one_entry.MarkUnused(0);
    one_entry.AfterFence(0, [](const int &) {});
    Check(one_entry.Acquire(
              2, 1, [] { return std::make_shared<const int>(2); }) != nullptr,
          "entry budget reclaimed");
    std::cout << "shared GPU ownership, descriptor order, fence epochs and "
                 "pending budgets passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
