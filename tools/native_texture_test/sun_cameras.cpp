/**
 * @file    sun_cameras.cpp
 * @brief   Native sun coverage, stabilization and invalid-input regression tests.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/scene/native_sun_camera.h"
#include <cstdio>
#include <cstdlib>
#include <numbers>
using namespace bd::gpu::scene;
#define REQUIRE(value) do { if (!(value)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #value); std::exit(1); } } while (false)
int main() {
  const RenderMatrix identity{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
  const RenderMatrix rh{2,0,0,0, 0,3,0,0, 0,0,-1.001f,-1, 0,0,-1.001f,0};
  const RenderMatrix lh{2,0,0,0, 0,3,0,0, 0,0,1.001f,1, 0,0,-1.001f,0};
  const RenderMatrix infinite{2,0,0,0, 0,3,0,0, 0,0,-1,-1, 0,0,-1,0};
  for (const auto &projection : {rh, lh, infinite, identity}) {
    for (int i = 0; i < 80; ++i) {
      auto view = identity;
      view[12] = float(i*127);
      view[13] = float(i*-13);
      view[14] = float(i*53);
      const auto fit = BuildNativeSunCamera(view, projection, i*0.073, i*0.1, 4096, 500, 500);
      REQUIRE(fit);
      REQUIRE(fit->world_texel > 0 && fit->depth_range > 1000);
      for (const auto &point : fit->receivers) {
        const auto clip = TransformSunPoint(point, 1, fit->view_projection);
        REQUIRE(std::abs(clip[0]) <= 1.0001 && std::abs(clip[1]) <= 1.0001);
        REQUIRE(clip[2] >= -0.0001 && clip[2] <= 1.0001 && clip[3] == 1);
        for (const auto &plane : fit->frustum.planes)
          REQUIRE(point[0]*plane[0]+point[1]*plane[1]+point[2]*plane[2]+plane[3] < 0.01);
      }
      // Light eye is on the near plane; positive camera-forward is outside.
      const auto eye = TransformSunPoint(fit->eye, 1, fit->view_projection);
      for (size_t c = 0; c < 3; ++c) {
        // A tiny orthographic box 10k units from origin magnifies float matrix
        // publication error. Bound it by the actual summed term magnitudes,
        // rather than pretending world-to-clip floats retain double precision.
        double magnitude = std::abs(fit->view_projection[12+c]);
        for (size_t k = 0; k < 3; ++k)
          magnitude += std::abs(fit->eye[k]*fit->view_projection[k*4+c]);
        REQUIRE(std::abs(eye[c]) <= 4*std::numeric_limits<float>::epsilon()*magnitude + 1e-7);
      }
    }
  }
  auto off_center = rh;
  off_center[8] = 0.3f;
  REQUIRE(BuildNativeSunCamera(identity, off_center, 0.7, 0.5, 64, 100, 100));
  REQUIRE(BuildNativeSunCamera(identity, rh, std::numbers::pi/2, 0, 4096, 500, 500));
  const auto first = *BuildNativeSunCamera(identity, rh, 0, 0, 4096, 500, 500);
  REQUIRE(IntersectsSunVolume(first.frustum, first.focus, 1));
  auto outside = first.focus;
  outside[0] = first.eye[0] + first.half_extent + 2;
  REQUIRE(!IntersectsSunVolume(first.frustum, outside, 1));
  REQUIRE(IntersectsSunVolume(first.frustum, outside, 3));
  REQUIRE(!IntersectsSunVolume(first.frustum, {NAN, 0, 0}, 1));
  REQUIRE(!IntersectsSunVolume(first.frustum, first.focus, -1));
  REQUIRE(!IntersectsSunVolume(first.frustum, first.focus, INFINITY));
  auto translated = identity;
  translated[12] = float(first.world_texel*0.2);
  const auto moved = *BuildNativeSunCamera(translated, rh, 0, 0, 4096, 500, 500);
  REQUIRE(first.view_projection == moved.view_projection);
  REQUIRE(!BuildNativeSunCamera(identity, {}, 0, 0, 4096, 500, 500));
  REQUIRE(!BuildNativeSunCamera({}, rh, 0, 0, 4096, 500, 500));
  REQUIRE(!BuildNativeSunCamera(identity, rh, NAN, 0, 4096, 500, 500));
  REQUIRE(!BuildNativeSunCamera(identity, rh, 0, INFINITY, 4096, 500, 500));
  REQUIRE(!BuildNativeSunCamera(identity, rh, 0, 0, 0, 500, 500));
  REQUIRE(!BuildNativeSunCamera(identity, rh, 0, 0, 4096, 0, 500));
  REQUIRE(!BuildNativeSunCamera(identity, rh, 0, 0, 4096, 500, -1));
  REQUIRE(!BuildNativeSunCamera(identity, rh, 0, 0, 4096, 500, INFINITY));
  return 0;
}
