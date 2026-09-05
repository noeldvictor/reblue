// Independent packed-shader register transcription and native attachment routing.
#include "gpu/post_grade.h"
#include "gpu/post_passes.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <limits>
using namespace bd::gpu;

GradeColor Reference(GradeColor color, const GradeParameters &p) {
  std::array<float, 3> r1{color.r, color.b, color.g};
  const auto f = [](uint32_t bits) { return std::bit_cast<float>(bits); };
  const auto power = [](float value, float exponent) {
    const float logarithm = std::clamp(std::log2(std::abs(value)),
        std::numeric_limits<float>::lowest(), std::numeric_limits<float>::max());
    return std::exp2(logarithm * exponent);
  };
  const std::array<float, 3> weights{f(0x3D93A92A), f(0x3E59999A), f(0x3F372474)};
  if (p.discolor) {
    const float light = power((r1[1]*weights[0] + r1[0]*weights[1]) + r1[2]*weights[2], f(0x3F8CCCCD));
    std::array<float, 3> r0{light*f(0x3F47AE14)-r1[2], light*f(0x3F3851EC)-r1[1],
                           light*f(0x3F666666)-r1[0]};
    r1 = {r0[2]*p.discolor_strength+r1[0], r0[1]*p.discolor_strength+r1[1],
          r0[0]*p.discolor_strength+r1[2]};
  }
  if (p.correction) {
    const std::array<float, 3> powered{power(r1[0],p.gamma), power(r1[1],p.gamma), power(r1[2],p.gamma)};
    const float light = (powered[1]*weights[0] + powered[0]*weights[1]) + powered[2]*weights[2];
    std::array<float, 3> r0{((powered[1]-light)*p.saturation+light)*p.gain.b+p.bias.b,
                           ((powered[2]-light)*p.saturation+light)*p.gain.g+p.bias.g,
                           ((powered[0]-light)*p.saturation+light)*p.gain.r+p.bias.r};
    r1 = {(p.target.r-r0[2])*p.blend+r0[2], (p.target.b-r0[0])*p.blend+r0[0],
          (p.target.g-r0[1])*p.blend+r0[1]};
  }
  return {r1[0],r1[2],r1[1]};
}

int main() {
  assert(!GradeParameters{}.Active());
  assert(!GradeStrengthEnabled(1, .01f));
  assert(GradeStrengthEnabled(1, std::nextafter(.01f, 1.0f)));
  assert(!GradeStrengthEnabled(1, -1));
  assert(!GradeStrengthEnabled(2, 1));
  assert(GradeStrengthEnabled(3, 1));
  assert(!GradeStrengthEnabled(1, std::numeric_limits<float>::quiet_NaN()));
  GradeParameters p;
  for (uint32_t mask = 0; mask < 4; ++mask) {
    p.discolor = (mask & 1) != 0;
    p.correction = (mask & 2) != 0;
    for (float gamma : {0.0f, .5f, 1.0f, 2.2f}) {
      p.gamma = gamma;
      for (float strength : {0.0f, .4f, 1.0f, 1.5f}) {
        p.discolor_strength = strength;
        p.saturation = 1.0f - strength;
        p.gain = {1.2f, .8f, 1.5f}; p.bias = {-.1f, .1f, -.2f};
        p.target = {.3f, .2f, .1f}; p.blend = strength * .5f;
        for (int i = 0; i < 257; ++i) {
          const GradeColor color{i == 0 ? 0 : float(i-1)/128-1,
              i == 0 ? 0 : float((i*37)%257)/128, i == 0 ? 0 : float((i*19)%257)/256};
          auto actual = p.discolor ? GradeDiscolor(color, p.discolor_strength) : color;
          if (p.correction) actual = GradeCorrect(actual, p.gamma, p.saturation, p.gain, p.bias, p.target, p.blend);
          const auto expected = Reference(color, p);
          assert(std::abs(actual.r-expected.r) < 2e-5f);
          assert(std::abs(actual.g-expected.g) < 2e-5f);
          assert(std::abs(actual.b-expected.b) < 2e-5f);
        }
      }
    }
  }
  for (uint32_t frame : {0u, 1u, 2u, 3u, 100u, UINT32_MAX}) {
    GradeParameters left, right;
    AnimateGradeGrain(left, frame, false);
    AnimateGradeGrain(right, frame, true);
    assert(left.phase_x >= 0 && left.phase_x < .5f);
    assert(left.phase_y >= 0 && left.phase_y < .5f);
    assert(left.phase_x == right.phase_x && left.phase_y == right.phase_y);
    assert(left.grain_image == frame%3 && right.grain_image == left.grain_image+3);
  }
  for (uint32_t mask = 0; mask < 8; ++mask) {
    const auto plan = MakePostPasses(mask & 1, mask & 2, mask & 4);
    assert(plan.count == std::popcount(mask));
    assert(plan.scratch_count == std::min(plan.count, 2u));
    uint32_t source = plan.composite_output, count = 0;
    for (uint32_t effect = 0; effect < 3; ++effect) {
      if (!(mask & (1u << effect))) continue;
      const auto &step = plan.steps[count++];
      assert(step.effect == PostEffect(effect));
      assert(step.input == source && step.input < plan.scratch_count);
      assert(step.input != step.output);
      assert(step.output == PostPasses::kOutput || step.output < plan.scratch_count);
      source = step.output;
    }
    assert(source == PostPasses::kOutput);
  }
}
