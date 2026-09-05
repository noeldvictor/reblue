#include "gpu/post_parameters.h"
#include "gpu/lens_flare.h"
#include "gpu/lens_flare_uv.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <limits>

int main() {
  using namespace bd::gpu;
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
