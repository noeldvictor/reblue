#ifdef NDEBUG
#undef NDEBUG
#endif
#include "gpu/scene/sampler_import.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <random>

using namespace bd::gpu::scene;
// Independent PPC mask/rotate oracle, matching the translated instructions.
// Do not call production bit-pack helpers here: full-width values and unrelated
// neighbors expose truncation, rotate and combined-anisotropy mistakes.
uint32_t Rlwinm(uint32_t value, int rotate, int begin, int end) {
  uint32_t mask = 0;
  for (int bit = 0; bit < 32; ++bit)
    if (begin <= end ? bit >= begin && bit <= end : bit >= begin || bit <= end)
      mask |= uint32_t(1) << (31 - bit);
  return std::rotl(value, rotate) & mask;
}
uint32_t Rlwimi(uint32_t dst, uint32_t src, int rotate, int begin, int end) {
  return (dst & ~Rlwinm(UINT32_MAX, 0, begin, end)) | Rlwinm(src, rotate, begin, end);
}
SamplerShadow Original(SamplerShadow s, uint32_t slot, SamplerField field, uint32_t value) {
  switch (field) {
  case SamplerField::AddressU: s.fetch[0] = Rlwimi(s.fetch[0], value, 10, 19, 21); break;
  case SamplerField::AddressV: s.fetch[0] = Rlwimi(s.fetch[0], value, 13, 16, 18); break;
  case SamplerField::AddressW: s.fetch[0] = Rlwimi(s.fetch[0], value, 16, 13, 15); break;
  case SamplerField::BorderColor: {
    const auto count = uint32_t(std::countl_zero(value));
    s.fetch[5] = Rlwinm(s.fetch[5], 0, 0, 29) | (Rlwinm(count, 27, 31, 31) ^ 1);
    break;
  }
  case SamplerField::MipFilter: s.fetch[3] = Rlwimi(s.fetch[3], value, 23, 7, 8); break;
  case SamplerField::MinFilter:
  case SamplerField::MagFilter: {
    const bool min = field == SamplerField::MinFilter;
    auto r10 = Rlwinm(value, 30, 2, 31);
    auto r7 = s.anisotropy_lookup;
    auto r9 = s.fetch[4];
    auto r6 = Rlwinm(r9, min ? 22 : 21, 31, 31);
    r9 = Rlwimi(r9, r10, min ? 11 : 10, min ? 20 : 21, min ? 20 : 21);
    r6 = (r6 | r10) - 1u;
    r7 &= ~r6;
    r7 = Rlwinm(r7, min ? 4 : 6, 0, min ? 27 : 25);
    r10 |= r7 | value;
    auto r31 = Rlwimi(s.fetch[3], r10, min ? 21 : 19, min ? 9 : 11, min ? 10 : 12);
    r31 = Rlwimi(r31, r10, min ? 21 : 19, 4, 6);
    s.fetch[3] = r31;
    r10 = r31;
    r7 = r10;
    r10 = Rlwimi(r10, r7, 31, 13, 31);
    r10 = Rlwimi(r10, r7, 31, 1, 11);
    r7 = Rlwinm(r10, 13, 20, 31);
    r10 = Rlwinm(s.z_filter, 30, 2, 31) - 1u;
    r7 &= r10;
    r10 = s.z_filter & ~r10;
    r10 += r7;
    s.fetch[4] = Rlwimi(r10, r9, 0, 0, 29);
    break;
  }
  }
  s.dirty |= (uint64_t(1) << 63) >> (slot + 20);
  return s;
}
int main() {
  for (uint32_t offset = 0; offset < 100; ++offset) {
    const auto field = SamplerImportField(offset);
    assert(bool(field) == (offset <= 24 && offset % 4 == 0));
    if (field) assert(SamplerOffset(*field) == offset);
  }
  assert(!SamplerImportField(UINT32_MAX));
  SamplerShadow invalid;
  const auto untouched = invalid;
  assert(!PublishSamplerShadow(invalid, 32, SamplerField::AddressU, 1));
  assert(!PublishSamplerShadow(invalid, 0, SamplerField(7), 1));
  assert(invalid == untouched);
  std::mt19937 random(0x82184a88);
  for (uint32_t field = 0; field < 7; ++field) {
    for (uint32_t iteration = 0; iteration < 16000; ++iteration) {
      SamplerShadow before;
      for (auto &word : before.fetch) word = random();
      before.dirty = (uint64_t(random()) << 32) | random();
      before.anisotropy_lookup = random();
      before.z_filter = uint8_t(iteration);
      const auto slot = iteration % 32;
      const uint32_t value = iteration < 32 ? iteration : uint32_t(random());
      auto actual = before;
      assert(PublishSamplerShadow(actual, slot, SamplerField(field), value));
      assert(actual == Original(before, slot, SamplerField(field), value));
      // The memory writer may touch only the original setter's bytes.
      std::array<uint8_t, 1800> memory{};
      for (auto &byte : memory) byte = uint8_t(random());
      auto expected = memory;
      auto put = [&](uint32_t at, auto v) { std::memcpy(expected.data() + at, &v, sizeof(v)); };
      const auto at = 1024 + slot * 24;
      if (field < 3) put(at, actual.fetch[0]);
      else if (field == 3) put(at + 20, actual.fetch[5]);
      else {
        put(at + 12, actual.fetch[3]);
        if (field != 6) put(at + 16, actual.fetch[4]);
      }
      put(16, actual.dirty);
      WriteSamplerShadow(actual, slot, SamplerField(field), [&](uint32_t at, auto v) {
        std::memcpy(memory.data() + at, &v, sizeof(v));
      });
      assert(memory == expected);
    }
  }
  // All 64 meaningful settings combinations plus unknown/signed bit patterns.
  constexpr std::array<uint32_t, 6> settings{0, 1, 2, 3, 4, UINT32_MAX};
  auto original_setting = [](uint32_t value) {
    return value == 1 ? 0u : value == 2 ? 4u : value == 3 ? 2u : 1u;
  };
  for (auto min : settings) for (auto mag : settings) for (auto mip : settings) {
    const auto plan = SceneSamplerDefaults(min, mag, mip);
    std::array<std::array<uint32_t, 20>, 32> cache;
    for (auto &slot : cache) for (auto &word : slot) word = random();
    const auto inherited = cache;
    uint32_t changes = 0;
    // A second reset must do no work and must not clear an already dirty flag.
    uint32_t dirty = 123;
    for (uint32_t pass = 0; pass < 2; ++pass) {
      const auto prior_changes = changes;
      for (uint32_t slot = 0; slot < 5; ++slot) {
        constexpr std::array order{SamplerField::MinFilter, SamplerField::MagFilter,
            SamplerField::MipFilter, SamplerField::AddressU, SamplerField::AddressV};
        for (uint32_t i = 0; i < 5; ++i) {
          const auto command = plan[slot * 5 + i];
          assert(command.slot == slot && command.field == order[i]);
          const uint32_t setting = i == 0 ? min : i == 1 ? mag : mip;
          assert(command.value == (i >= 3 ? 0u : slot ? 1u : original_setting(setting)));
          auto &cached = cache[slot][uint32_t(command.field)];
          if (cached != command.value) { cached = command.value; dirty = 1; ++changes; }
        }
      }
      if (pass) assert(changes == prior_changes && dirty == 1);
    }
    for (uint32_t slot = 0; slot < 32; ++slot)
      for (uint32_t field = 0; field < 20; ++field)
        if (slot >= 5 || field == 2 || field == 3 || field > 6)
          assert(cache[slot][field] == inherited[slot][field]);
  }
  std::cout << "Sampler publication: 112000 PPC comparisons, byte masks and scene defaults passed\n";
}
