#include "gpu/post_parameters.h"
#include "gpu/lens_flare.h"
#include "gpu/lens_flare_uv.h"
#include "gpu/post_adjustments.h"
#include "gpu/post_scanline.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <limits>
#include <bit>

int main() {
  using namespace bd::gpu;
  assert(!ScanlineParameters{}.enabled);
  assert(ScanlinePhase(1, false, 0, 32767) == 0);
  assert(ScanlinePhase(1, true, 32767, 32767) == 32767.0f / 65536);
  assert(ScanlinePhase(2, true, 32767, 1) == 1.0f / 65536);
  assert(ScanlinePhase(4, true, 16383, 32767) > 0);
  assert(ScanlinePhase(4, true, 16384, 32767) == 0);
  assert(ScanlinePhase(-1, true, 32767, 1) == ScanlinePhase(1, true, 32767, 1));
  assert(ScanlinePhase(std::numeric_limits<float>::infinity(), true, 0, 1) == 0);
  assert(ScanlinePhase(std::numeric_limits<float>::quiet_NaN(), true, 0, 1) == 0);
  uint32_t rolls = 0;
  float previous_phase = -1;
  uint32_t changes = 0;
  for (uint32_t frame = 0; frame < 10000; ++frame) {
    const auto phase = ScanlineFramePhase(4, true, frame);
    assert(phase == ScanlineFramePhase(4, true, frame)); // both eyes/scopes agree
    assert(phase >= 0 && phase < .5f);
    rolls += phase != 0;
    changes += phase != previous_phase;
    previous_phase = phase;
  }
  assert(rolls > 4800 && rolls < 5200);
  assert(changes > 6000);
  // Independent shader-register transcription: signed exp2(log2(abs(sin))*N).
  // Compare actual sample-coordinate offsets over flat and native eye extents.
  for (float height : {720.0f, 1080.0f, 1584.0f}) {
    for (float strength : {-1.0f, .25f, 1.0f, 2.0f}) {
      for (float phase : {0.0f, .125f, .499f}) {
        for (int row = 0; row < 128; ++row) {
          const float y = (float(row) + .5f) / 128;
          float cycle = (((((y + phase) * height) * strength) * .1f) * phase) *
              std::bit_cast<float>(0x3E22F983u) + .5f;
          cycle = (cycle - std::floor(cycle)) * std::bit_cast<float>(0x40C90FDBu) +
              std::bit_cast<float>(0xC0490FDBu);
          const float wave = std::sin(cycle);
          for (float exponent : {235.0f, 159.0f, 33.0f, 87.0f}) {
            const float reference = wave == 0 ? 0 :
                (((wave > 0 ? 1.0f : -1.0f) * std::exp2(std::log2(std::abs(wave)) * exponent)) *
                 strength) * .01f;
            assert(std::abs(ScanlineOffset(ScanlineWave(y, height, strength, phase),
                                           strength, exponent) - reference) < 2e-7f);
          }
        }
      }
    }
  }
  assert(ScanlineWave(.5f, 1584, 1, 0) == 0);
  assert(ScanlineWave(.5f, 1584, 0, .2f) == 0);
  assert(ScanlineOffset(0, 1, 33) == 0);
  assert(ScanlineOffset(1, 1, 33) == .01f);
  assert(ScanlineOffset(-1, 1, 235) == -.01f);
  PostAdjustments adjustments;
  assert(!adjustments.Active());
  adjustments.fisheye_enabled = true;
  assert(adjustments.Active());
  adjustments.fisheye_enabled = false;
  adjustments.reverse_enabled = true;
  assert(adjustments.Active());
  // Independent scalar transcription of the original shader's literal lanes.
  // It used fixed 720/1280 in its radius; output geometry now supplies aspect.
  const auto literal = [](uint32_t bits) { return std::bit_cast<float>(bits); };
  const auto original_scale = [&](float radius, float strength) {
    if (strength < 0) {
      const float distance = radius * literal(0x3FB504E6);
      return ((distance * distance / radius) * strength) * .5f;
    }
    float angle = radius * literal(0x3F350387) + .5f;
    angle -= std::floor(angle);
    angle = angle * literal(0x40C90FDB) + literal(0xC0490FDB);
    return ((-2.0f / radius) * std::sin(angle) * strength) * .1f;
  };
  for (float strength : {-2.0f, -.75f, .25f, 1.0f}) {
    assert(FisheyeOffsetScale(0, strength) == 0);
    for (float aspect : {.5625f, 1.0f, 1.1f}) {
      for (int i = 1; i <= 64; ++i) {
        const float delta = float(i) / 128;
        const float radius = std::sqrt(delta * delta + delta * delta * aspect * aspect);
        assert(std::abs(FisheyeOffsetScale(radius, strength) - original_scale(radius, strength)) < 3e-6f);
      }
    }
  }
  assert(FisheyeOffsetScale(.5f, 0) == 0);
  assert(FisheyeOffsetScale(.3f, -.75f) < 0);
  // Equal pixel distances from the center produce equal radial distortion.
  const float pixel_delta = 100.0f;
  const float horizontal = pixel_delta / 1440;
  const float vertical = (pixel_delta / 1584) * (1584.0f / 1440);
  assert(std::abs(horizontal - vertical) < 1e-7f);
  for (float color : {0.0f, .25f, .5f, .75f, 1.0f}) {
    assert(ReverseColor(color, 0, 1) == color);
    assert(ReverseColor(color, 1, 1) == 1 - color);
    assert(ReverseColor(color, .5f, 1) == .5f);
    assert(ReverseColor(color, 1, .5f) == .5f - color);
  }
  // All ten original fan vertices (center, perimeter, repeated first edge).
  constexpr std::array<std::array<float, 4>, 10> optical_samples{{
      {.5f,.5f,0,1}, {.5f,0,0,0}, {1,0,1,0}, {1,.5f,1,1}, {1,1,1,0},
      {.5f,1,0,0}, {0,1,1,0}, {0,.5f,1,1}, {0,0,1,0}, {.5f,0,0,0}}};
  for (const auto &sample : optical_samples) {
    assert(LensFlareU(sample[0]) == sample[2]);
    assert(LensFlareV(sample[1]) == sample[3]);
  }
  assert(LensFlareU(.25f) == .5f && LensFlareU(.75f) == .5f);
  assert(LensFlareV(.25f) == .5f && LensFlareV(.75f) == .5f);
  const auto bloom = MakeBloomParameters(0.25f, 10, true, 0);
  assert(bloom.threshold == 0.25f && bloom.intensity == 10);
  assert((bloom.scene_weight == std::array<float, 4>{4, 4, 4, 4}));
  assert((bloom.bloom_weight == std::array<float, 4>{1, 1, 1, 0}));
  const auto off = MakeBloomParameters(0.5f, 3, false, 1);
  assert((off.bloom_weight == std::array<float, 4>{0, 0, 0, 0}));
  const auto dual = MakeBloomParameters(0.25f, 10, true, 1);
  assert((dual.bloom_weight == std::array<float, 4>{2, 2, 2, 0}));
  assert(dual.directional.enabled && dual.directional.iterations == 0);
  assert(!bloom.directional.enabled && !off.directional.enabled);
  assert(!MakeBloomParameters(0.25f, 10, false, 1).directional.enabled);
  assert(!MakeBloomParameters(0.25f, 10, true, 2).directional.enabled);
  assert(MakeBloomParameters(0.25f, 10, true, 2).bloom_weight == bloom.bloom_weight);
  const scene::RenderMatrix identity{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
  assert((ProjectLensFlare({2, 3, 4}, identity, identity) == std::array<float, 2>{.5f, -.75f}));
  auto lens_view = identity;
  lens_view[12] = -2;
  auto lens_projection = identity;
  lens_projection[0] = 2;
  assert((ProjectLensFlare({4, 3, 4}, lens_view, lens_projection) == std::array<float, 2>{1, -.75f}));
  const auto inactive_flare = MakeLensFlareParameters(false, {0, 0}, 1, 1, {1,1,1}, 1, 1);
  assert(inactive_flare.count == 0);
  const auto flare = MakeLensFlareParameters(true, {0, 0}, 1, 1, {.2f,.4f,.6f}, 1, 1);
  assert(flare.count == 15);
  assert((flare.sprites[0].rect == std::array<float, 4>{.45f, (360-64)/720.0f, .1f, 128/720.0f}));
  assert(flare.sprites[0].color[3] == .6f * 1.2f);
  assert(flare.sprites[1].color[0] == .2f && flare.sprites[1].color[2] == .6f);
  assert(flare.sprites[1].rect[2] == (128.0f * (1.4142f + .5f)) / 1280.0f);
  assert(flare.sprites[14].rect[2] == ((1.4142f + 1) * (256 * 1.2f) * 3) / 1280.0f);
  assert(flare.sprites[4].color[2] == 6); // authored HDR tint, not silently clamped
  const auto obscured = MakeLensFlareParameters(true, {0, 0}, 1, 1, {1,1,1}, 0, 1);
  for (const auto &sprite : obscured.sprites) assert(sprite.color[3] == 0);
  const auto edge_flare = MakeLensFlareParameters(true, {3, 4}, 1, 1, {1,1,1}, 1, 1);
  assert(edge_flare.sprites[1].color[3] == (1.2f - 1.0f));
  for (size_t i = 0; i < flare.sprites.size(); ++i) {
    assert(flare.sprites[i].texture == kLensFlareRecipe[i].texture);
    assert(flare.sprites[i].texture < 4);
    for (float value : flare.sprites[i].rect) assert(std::isfinite(value));
  }
  auto view = identity;
  view[14] = -5;
  auto projection = identity;
  projection[10] = 0.5f;
  projection[11] = 1;
  projection[15] = 0;
  auto p = MakeDofParameters(1.4f, 60, 20, 1, {0,0,9}, view, projection);
  assert(p.aperture == 1.4f);
  assert(std::abs(p.blur_scale - 0.075f) < 1e-7f);
  assert(p.authored_range == 20 * 0.010001f);
  assert(p.focus_depth == 0.5f);
  const auto half = MakeDofParameters(1.4f, 60, 20, 0.25, {0,0,9}, view, projection);
  assert(half.blur_scale == p.blur_scale * 0.5f);
  const auto zero = MakeDofParameters(1.4f, 60, 20, 0, {0,0,9}, view, projection);
  assert(zero.blur_scale == 0);
  // Translation and off-axis terms are respected, not a camera-distance guess.
  projection[2] = 2;
  projection[6] = 3;
  p = MakeDofParameters(1, 60, 20, 1, {2,3,9}, view, projection);
  assert(p.focus_depth == 3.75f);
  const auto invalid = MakeDofParameters(1, 60, 20, 1, {0,0,5}, view, projection);
  assert(std::isnan(invalid.focus_depth));
  DofPreparation ticket;
  assert(!ticket.Prepare(0, 2, 3));
  assert(!ticket.Prepare(1, 0, 3));
  assert(!ticket.Consume(1, 2, 3));
  assert(ticket.Prepare(1, 2, 3));
  assert(!ticket.Prepare(2, 3, 3));
  assert(!ticket.Consume(2, 2, 3));
  assert(!ticket.Consume(1, 3, 3));
  assert(!ticket.Consume(1, 2, 4));
  assert(ticket.Active());
  assert(ticket.Consume(1, 2, 3));
  assert(!ticket.Active());
  assert(!ticket.Consume(1, 2, 3));
  assert(ticket.Prepare(2, 3, 4));
  assert(ticket.Consume(2, 3, 4));
}
