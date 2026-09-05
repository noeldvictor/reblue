#include "gpu/post_parameters.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <limits>

int main() {
  using namespace bd::gpu;
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
