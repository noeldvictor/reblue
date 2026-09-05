/**
 * @file    lens_flare.h
 * @brief   Native lens-flare recipe and sprite production, without render ABI state.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include "gpu/scene/native_transform.h"
#include <algorithm>
#include <cstdint>

namespace bd::gpu {
struct LensFlareSprite {
  std::array<float, 4> rect{}; // normalized top-left x/y and width/height
  std::array<float, 4> color{}; // tint RGB and intensity, not coverage alpha
  uint32_t texture = 0;
  uint32_t padding[3]{};
};
static_assert(sizeof(LensFlareSprite) == 48);
struct LensFlareParameters {
  std::array<LensFlareSprite, 15> sprites{};
  uint32_t count = 0;
};
struct LensFlareRecipe {
  uint32_t texture;
  float position, size;
  std::array<float, 4> color;
};
// Authored optical ghosts from the complete sub_82218140 producer. Stable
// native data: neither an engine pointer nor a shader-register template.
inline constexpr std::array<LensFlareRecipe, 15> kLensFlareRecipe{{
    {2, 1.2f, 2, {.7f, 1, .8f, .6f}},
    {0, 1, 1, {1, 1, 1, 1}},
    {0, .6f, .25f, {1, .5f, .9f, .5f}},
    {1, .58f, 1, {.9f, 1, .95f, .4f}},
    {0, .56f, .4f, {0, 0, 6, .4f}},
    {0, .2f, .3f, {1, .5f, 0, .8f}},
    {0, 0, .05f, {.3f, 1, .3f, .8f}},
    {0, -.3f, .2f, {.3f, 1, .3f, .6f}},
    {0, -.5f, .6f, {1, .5f, .1f, .6f}},
    {0, -.55f, .4f, {1, .5f, .1f, .6f}},
    {1, -.57f, 1, {1, .4f, 0, .5f}},
    {2, -.78f, .4f, {.3f, 0, 1, .8f}},
    {1, -.8f, .6f, {.3f, 1, .3f, .5f}},
    {1, -1, 1.4f, {1, 1, .7f, .4f}},
    {3, -1.2f, 1.2f, {1, 1, 1, .6f}},
}};

inline std::array<float, 2> ProjectLensFlare(
    const std::array<float, 3> &point, const scene::RenderMatrix &view,
    const scene::RenderMatrix &projection) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
  const auto transform = [](const std::array<float, 3> &p,
                            const scene::RenderMatrix &m) {
    std::array<float, 3> out;
    for (size_t i = 0; i < 3; ++i) {
      float v = p[2] * m[8 + i] + m[12 + i];
      v = p[1] * m[4 + i] + v;
      out[i] = p[0] * m[i] + v;
    }
    return out;
  };
  // Preserve the authored lens convention: each transform treats xyz as a
  // point; the final screen divisor is projected z, not projected w.
  const auto clip = transform(transform(point, view), projection);
  const float reciprocal = 1.0f / clip[2];
  return {clip[0] * reciprocal, -(clip[1] * reciprocal)};
}

inline LensFlareParameters MakeLensFlareParameters(
    bool visible, const std::array<float, 2> &position, float strength,
    float radius_scale, const std::array<float, 3> &tint, float visibility,
    float edge_attenuation) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
  LensFlareParameters result;
  if (!visible)
    return result;
  const float x = position[0], y = position[1];
  float radius = std::sqrt(float(std::fma(double(y), double(y), double(x * x))));
  if (radius > 1.0f) radius = 1.0f;
  const float edge = float(std::pow(double(radius), 1.5));
  const float power = (float(-std::fma(double(edge), double(edge_attenuation), -double(1.2f))) *
                       strength) * visibility;
  constexpr std::array<float, 4> diameters{128, 128, 64, 256};
  for (size_t i = 0; i < result.sprites.size(); ++i) {
    const auto &recipe = kLensFlareRecipe[i];
    auto &sprite = result.sprites[i];
    float diameter = diameters[recipe.texture] * recipe.size;
    if (i == 1) diameter *= radius_scale * ((1.4142f - radius) + .5f);
    else if (i == 14) diameter = ((1.4142f - radius) + 1.0f) * diameter * 3.0f;
    else if (i >= 8) diameter = ((1.4142f - radius) + 1.0f) * diameter * 1.75f;
    const float cx = float(std::fma(double(recipe.position), double(x), 1.0)) * 640.0f;
    const float cy = float(std::fma(double(recipe.position), double(y), 1.0)) * 360.0f;
    sprite.rect = {(cx - diameter * .5f) / 1280.0f,
                   (cy - diameter * .5f) / 720.0f,
                   diameter / 1280.0f, diameter / 720.0f};
    sprite.color = recipe.color;
    if (i == 1)
      for (size_t lane = 0; lane < 3; ++lane) sprite.color[lane] *= tint[lane];
    sprite.color[3] *= power;
    sprite.texture = recipe.texture;
  }
  result.count = uint32_t(result.sprites.size());
  return result;
}
} // namespace bd::gpu
