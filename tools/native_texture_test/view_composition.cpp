/**
 * @file    view_composition.cpp
 * @brief   Explicit camera ownership and one composition per submission.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#include "xr/view_composition_scope.h"
#include <stdexcept>

namespace {
void Require(bool condition) {
  if (!condition) throw std::runtime_error("view composition scope check failed");
}
}
int main() {
  using bd::xr::ViewCompositionScope;
  ViewCompositionScope scope(100, 200);
  unsigned calls = 0;
  auto compose = [&](float *out) {
    ++calls;
    for (unsigned i = 0; i < 16; ++i) out[i] = float(calls * 20 + i);
    return true;
  };
  // World-only, identity/post reset, light camera, wrong projection and
  // disabled/cinema cannot submit an anchor or advance smoothing.
  for (const auto pair : {std::array<uint64_t, 2>{0, 0}, {300, 400},
                          {100, 400}, {300, 200}, {100, 0}})
    Require(!scope.Resolve(pair[0], pair[1], true, compose));
  Require(!scope.Resolve(100, 200, false, compose));
  Require(calls == 0);
  const auto *first = scope.Resolve(100, 200, true, compose);
  Require(first && (*first)[15] == 35 && calls == 1);
  Require(scope.Resolve(100, 200, true, compose) == first && calls == 1);
  Require(!scope.Resolve(300, 400, true, compose) && calls == 1);
  Require(!scope.Resolve(100, 200, false, compose) && calls == 1);
  // A nested/subsequent owner has an independent result. The outer result
  // remains intact when that owner is destroyed.
  {
    ViewCompositionScope nested(100, 200);
    const auto *second = nested.Resolve(100, 200, true, compose);
    Require(second && (*second)[15] == 55 && calls == 2);
  }
  Require(scope.Resolve(100, 200, true, compose) == first && calls == 2);
  Require((*first)[15] == 35);
  ViewCompositionScope invalid(0, 200);
  Require(!invalid.Resolve(0, 200, true, compose) && calls == 2);
  ViewCompositionScope retry(100, 200);
  Require(!retry.Resolve(100, 200, true, [](float *out) { out[0] = -1; return false; }));
  const auto *recovered = retry.Resolve(100, 200, true, compose);
  Require(recovered && (*recovered)[0] == 60 && calls == 3);
}
