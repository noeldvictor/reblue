/**
 * @file    blend.cpp
 * @brief   Native blend intent and exact temporary setter-publication tests.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "gpu/scene/blend_import.h"
#include <cassert>
#include <iostream>
#include <random>

using namespace bd::gpu::scene;
namespace {
// Independent bit-by-bit expression of the SDK alpha folding instructions.
uint32_t ReferenceFold(uint32_t word) {
  uint32_t result = word & 0xffff;
  for (unsigned bit = 0; bit < 16; ++bit)
    if (word & (1u << bit)) {
      if (bit != 4 && bit != 12)
        result |= 1u << (bit + 16);
      if (bit == 4 || bit == 12)
        result |= 1u << (bit + 12);
    }
  return result;
}
BlendShadow Reference(BlendShadow s, size_t index, uint32_t value) {
  uint32_t result;
  if (index < 2) {
    const auto shift = index == 0 ? 31 : 30;
    s.flags = (s.flags & ~(1u << shift)) | ((value & 1) << shift);
    const bool separate =
        index == 1 ? value != 0 : (s.flags & 0x40000000u) != 0;
    result = separate ? s.requested : ReferenceFold(s.requested);
    if (index == 0 ? value == 0 : (s.flags & 0x80000000u) == 0)
      result = 0x10001;
  } else {
    constexpr unsigned starts[]{0, 8, 5, 16, 24, 21};
    unsigned start = starts[index - 2];
    unsigned count = index == 4 || index == 7 ? 3 : 5;
    for (unsigned bit = 0; bit < count; ++bit) {
      s.requested &= ~(1u << (start + bit));
      if (value & (1u << bit))
        s.requested |= 1u << (start + bit);
    }
    if (!(s.flags >> 31) || (index >= 5 && !(s.flags & 0x40000000u)))
      return s;
    result = (s.flags & 0x40000000u) ? s.requested : ReferenceFold(s.requested);
  }
  s.effective.fill(result);
  s.dirty16 |= 1024 | 4 | 2 | 1;
  return s;
}
} // namespace
int main() {
  struct Pipeline : BlendState {
    int shader = 99, target = 17;
    bool depth = true, multiview = true;
  } pipeline;
  BlendState native;
  bool dirty = false;
  ApplyBlendState(native, pipeline, dirty);
  assert(!dirty);
  native.alphaBlendEnable = true;
  native.srcBlend = plume::RenderBlend::SRC_ALPHA;
  native.destBlend = plume::RenderBlend::INV_SRC_ALPHA;
  native.blendOpAlpha = plume::RenderBlendOperation::MAX;
  ApplyBlendState(native, pipeline, dirty);
  assert(dirty && static_cast<BlendState &>(pipeline) == native);
  assert(pipeline.shader == 99 && pipeline.target == 17 && pipeline.depth &&
         pipeline.multiview);
  ApplyBlendState(native, pipeline, dirty);
  assert(dirty); // Never clear another producer's dirty mark.
  pipeline.srcBlend = plume::RenderBlend::ZERO; // A replay changed bound state.
  dirty = false;
  ApplyBlendState(native, pipeline, dirty);
  assert(dirty && pipeline.srcBlend == native.srcBlend);
  assert(DecodeBlendImport(0x10001, 0) == BlendState{});
  assert(!SupportedBlendWord(12)); // Constant factors remain unsupported.
  assert(!SupportedBlendWord(5 << 5));
  assert(SupportedBlendWord(0x10001));
  assert(ImportBlendFactor(16) == plume::RenderBlend::SRC_ALPHA_SAT);
  assert(ImportBlendFactor(31) == plume::RenderBlend::ZERO);
  assert(ImportBlendOperation(4) == plume::RenderBlendOperation::REV_SUBTRACT);
  assert(ImportBlendOperation(7) == plume::RenderBlendOperation::ADD);
  for (uint32_t rgb = 0; rgb < 65536; ++rgb)
    assert(FoldBlendAlpha(rgb) == ReferenceFold(rgb));
  std::mt19937 random(0xB1E0D);
  for (size_t index = 0; index < kBlendOffsets.size(); ++index) {
    assert(BlendImportIndex(kBlendOffsets[index]) == index);
    for (int n = 0; n < 4000; ++n) {
      BlendShadow before{uint32_t(random()), uint32_t(random())};
      for (auto &word : before.effective)
        word = random();
      before.dirty16 = (uint64_t(random()) << 32) | random();
      // Exercise canonical booleans, noncanonical nonzero and arbitrary bits.
      const uint32_t value = n % 4 == 0   ? 0
                             : n % 4 == 1 ? 1
                             : n % 4 == 2 ? 2
                                          : random();
      auto actual = before;
      assert(PublishBlendShadow(actual, kBlendOffsets[index], value));
      assert(actual == Reference(before, index, value));
      assert((actual.flags & 0x3fffffffu) == (before.flags & 0x3fffffffu));
      // Decode every lane independently; no guest state enters the pipeline
      // copy.
      auto decoded = DecodeBlendImport(actual.effective[0], actual.flags);
      assert(decoded.alphaBlendEnable == ((actual.flags >> 31) != 0));
      assert(decoded.srcBlend == ImportBlendFactor(actual.effective[0] & 31));
      assert(decoded.destBlend ==
             ImportBlendFactor((actual.effective[0] >> 8) & 31));
      assert(decoded.blendOp ==
             ImportBlendOperation((actual.effective[0] >> 5) & 7));
      assert(decoded.srcBlendAlpha ==
             ImportBlendFactor((actual.effective[0] >> 16) & 31));
      assert(decoded.destBlendAlpha ==
             ImportBlendFactor((actual.effective[0] >> 24) & 31));
      assert(decoded.blendOpAlpha ==
             ImportBlendOperation((actual.effective[0] >> 21) & 7));
      auto published = before;
      WriteBlendChanges(before, actual, [&](uint32_t offset, auto value) {
        if (offset == 11576)
          published.requested = uint32_t(value);
        else if (offset == 11580)
          published.flags = uint32_t(value);
        else if (offset == 16)
          published.dirty16 = value;
        else {
          size_t i = 0;
          while (i < 4 && kBlendWordOffsets[i] != offset)
            ++i;
          assert(i < 4);
          published.effective[i] = uint32_t(value);
        }
      });
      assert(published == actual);
    }
  }
  BlendShadow sequence;
  PublishBlendShadow(sequence, 72, 6);
  PublishBlendShadow(sequence, 76, 7);
  PublishBlendShadow(sequence, 84, 1);
  assert(sequence.effective[0] == 0x10001 && sequence.dirty16 == 0);
  PublishBlendShadow(sequence, 60, 1);
  assert(sequence.effective[0] == 0x07060706);
  PublishBlendShadow(sequence, 64, 1);
  assert(sequence.effective[0] == 0x00010706);
  PublishBlendShadow(sequence, 60, 0);
  assert(sequence.effective[0] == 0x10001);
  PublishBlendShadow(sequence, 60, 1);
  assert(sequence.effective[0] == 0x00010706);
  auto unchanged = sequence;
  for (auto offset : {0u, 59u, 61u, 68u, 96u, UINT32_MAX}) {
    assert(!PublishBlendShadow(sequence, offset, 0));
    assert(sequence == unchanged);
  }
  std::cout << "Native blend intent, 65536 alpha folds and 32000 setter "
               "publications passed\n";
}
