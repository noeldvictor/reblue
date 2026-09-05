/**
 * @file    passes.cpp
 * @brief   Native pass nesting, live restoration, dimensions and ownership.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/scene/native_pass.h"
#include <memory>
#include <stdexcept>
using namespace bd::gpu::scene;
namespace {
void Require(bool value) {
  if (!value)
    throw std::runtime_error("native pass check failed");
}
}
int main() {
  NativePassStack<int> stack;
  Require(stack.SetRootContent({1920, 1080}));
  Require(!stack.Leave() && !stack.Previous() && !stack.Depth());
  Require(stack.Content() == PassExtent{1920, 1080});
  stack.Enter({10, 11}, PassExtent{1440, 808});
  Require(stack.Depth() == 1 && stack.Content() == PassExtent{1440, 808});
  Require(!stack.SetRootContent({1, 1}));
  stack.Enter({20, 21}, std::nullopt); // shadow/depth-only: logical extent inherited
  Require(stack.Content() == PassExtent{1440, 808});
  // An independent entry point can replace the live targets before a nested
  // push. Restore these actual inputs, not the earlier scope's selected pair.
  stack.Enter({30, 31}, PassExtent{320, 180});
  auto saved = stack.Leave();
  Require(saved && saved->attachments == PassAttachments<int>{30, 31});
  Require(stack.Content() == PassExtent{1440, 808});
  saved = stack.Leave();
  Require(saved && saved->attachments == PassAttachments<int>{20, 21});
  saved = stack.Leave();
  Require(saved && saved->attachments == PassAttachments<int>{10, 11});
  Require(stack.Content() == PassExtent{1920, 1080} && !stack.Depth());
  // Entirely null passes and explicit zero extents are distinct from inheritance.
  stack.Enter({}, PassExtent{0, 0});
  stack.Enter({}, std::nullopt);
  Require(stack.Content() == PassExtent{});
  Require(stack.Leave()->attachments == PassAttachments<int>{});
  Require(stack.Leave()->content == PassExtent{1920, 1080});
  // The console's seven-entry limit belongs only to its temporary adapter.
  for (int i = 0; i < 24; ++i)
    stack.Enter({i, i + 100}, PassExtent{float(i + 1), float(i + 2)});
  for (int i = 23; i >= 0; --i)
    Require(stack.Leave()->attachments == PassAttachments<int>{i, i + 100});
  Require(!stack.Leave());
  NativePassStack<std::shared_ptr<int>> owned;
  auto image = std::make_shared<int>(42);
  std::weak_ptr<int> lifetime = image;
  owned.Enter({image, image}, std::nullopt);
  image.reset();
  Require(!lifetime.expired());
  {
    const auto restore = owned.Leave();
    Require(restore && *restore->attachments.color == 42);
    Require(!lifetime.expired());
  }
  Require(lifetime.expired());
  return 0;
}
