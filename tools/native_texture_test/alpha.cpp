/**
 * @file    alpha.cpp
 * @brief   Shared native alpha predicate, pipeline and engine shadow tests.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "gpu/scene/alpha_import.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <random>

using namespace bd::gpu::scene;
int main() {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  for (uint32_t function = 0; function < 8; ++function) {
    AlphaImport imported{{1, 128, function, 1}};
    auto state = DecodeAlphaImport(imported);
    const float cutoff = state.threshold;
    for (float value : {-inf, 0.0f, std::nextafter(cutoff, 0.0f), cutoff,
                        std::nextafter(cutoff, 1.0f), 1.0f, inf, nan}) {
      const bool reference[]{
          false, value<cutoff, value == cutoff, value <= cutoff, value> cutoff,
          value != cutoff, value >= cutoff, true};
      assert(AlphaPass(state, value) == reference[function]);
      assert(BD_AlphaPass(uint32_t(state.compare), value, cutoff) ==
             reference[function]);
    }
    struct Pipeline {
      uint32_t specConstants = SPEC_CONSTANT_CEL | SPEC_CONSTANT_INSTANCED;
      bool enableAlphaToCoverage = false;
      int shader = 7;
    } pipeline;
    bool dirty = false;
    ApplyAlphaState(state, pipeline, dirty, true);
    assert(dirty && pipeline.enableAlphaToCoverage && pipeline.shader == 7);
    assert(BD_AlphaMode(pipeline.specConstants) == uint32_t(state.compare));
    assert((pipeline.specConstants & SPEC_CONSTANT_CEL) != 0);
    assert((pipeline.specConstants & SPEC_CONSTANT_INSTANCED) != 0);
    dirty = false;
    ApplyAlphaState(state, pipeline, dirty, true);
    assert(!dirty);
    ApplyAlphaState(state, pipeline, dirty, false);
    assert(dirty && !pipeline.enableAlphaToCoverage);
    ApplyAlphaState(state, pipeline, dirty, false, true);
    assert(!(pipeline.specConstants & SPEC_CONSTANT_ALPHA_TEST));
    assert(!(pipeline.specConstants & SPEC_CONSTANT_ALPHA_COMPARE_MASK));
    ApplyAlphaState(state, pipeline, dirty, false);
    assert(pipeline.specConstants & SPEC_CONSTANT_ALPHA_TEST);
    state.enabled = false;
    assert(AlphaPass(state, nan));
    ApplyAlphaState(state, pipeline, dirty, false);
    assert(!(pipeline.specConstants & SPEC_CONSTANT_ALPHA_COMPARE_MASK));
    assert(dirty); // Other producers' marks are preserved.
  }
  assert(DecodeAlphaImport({{1, 255, 6, 0}}).threshold == 1.0f);
  assert(DecodeAlphaImport({{1, 0, 6, 0}}).threshold == 0.0f);
  assert(!DecodeAlphaImport({{2, 255, 6, 2}}).enabled);
  assert(!DecodeAlphaImport({{2, 255, 6, 2}}).alpha_to_coverage);
  std::mt19937 random(0xA1FA);
  for (size_t i = 0; i < kAlphaOffsets.size(); ++i) {
    const auto offset = kAlphaOffsets[i];
    assert(AlphaImportIndex(offset) == i);
    for (int n = 0; n < 4000; ++n) {
      const uint32_t value = n < 256 ? uint32_t(n) : uint32_t(random());
      AlphaShadow before{uint32_t(random()), uint32_t(random()),
                         (uint64_t(random()) << 32) | random(),
                         (uint64_t(random()) << 32) | random()};
      auto expected = before;
      if (offset == 100) {
        const float ref =
            float(double(float(value)) * double(kAlphaImportScale));
        expected.reference_bits = std::bit_cast<uint32_t>(ref);
        expected.dirty24 |= 256;
      } else {
        const unsigned start = offset == 96 ? 3 : offset == 336 ? 4 : 0;
        const unsigned count = offset == 104 ? 3 : 1;
        for (unsigned b = 0; b < count; ++b) {
          expected.control &= ~(1u << (start + b));
          expected.control |= ((value >> b) & 1) << (start + b);
        }
        expected.dirty16 |= 512;
        if (offset == 96)
          expected.dirty16 |= uint64_t(1) << 50;
      }
      auto actual = before;
      assert(PublishAlphaShadow(actual, offset, value));
      assert(actual == expected);
      auto published = before;
      WriteAlphaShadow(actual, offset, [&](uint32_t at, auto word) {
        if (at == 10372)
          published.reference_bits = uint32_t(word);
        else if (at == 10428)
          published.control = uint32_t(word);
        else if (at == 16)
          published.dirty16 = word;
        else if (at == 24)
          published.dirty24 = word;
        else
          assert(false);
      });
      assert(published == actual);
      assert((actual.control & ~31u) == (before.control & ~31u));
    }
  }
  AlphaShadow shadow;
  const auto before = shadow;
  for (auto offset : {0u, 95u, 97u, 308u, 340u, UINT32_MAX}) {
    assert(!PublishAlphaShadow(shadow, offset, 1));
    assert(shadow == before);
  }
  std::cout << "Native alpha: all eight shader predicates and 16000 setter "
               "publications passed\n";
}
