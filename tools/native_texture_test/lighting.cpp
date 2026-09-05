/**
 * @file    lighting.cpp
 * @brief   Host lighting and its temporary staging boundary without the SDK.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/scene/lighting_shader_bridge.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <limits>

using namespace bd::gpu::scene;
int main() {
  NativeLightingInputs inputs;
  inputs.ambient = {0.1f, 0.2f, 0.3f, 1};
  inputs.camera_position = {19, 149, 35, 1};
  inputs.color_scale = {0.4f, 0.5f, 0.6f, 1};
  inputs.shadow_threshold = 0.5f;
  inputs.scene_origin = {2, 3, 4};
  inputs.scene_range = 8;
  const auto first = ComposeNativeLighting(inputs);
  assert((first.shadow_sampling == LightingVector{0, 0.5f, 0, 0}));
  assert((first.scene_sampling == LightingVector{2, 3, 4, 0.125f}));
  inputs.sample_extent = LightingExtent{960, 540};
  const auto large = ComposeNativeLighting(inputs);
  inputs.sample_extent = LightingExtent{32, 36};
  const auto small = ComposeNativeLighting(inputs);
  assert((large.shadow_sampling == LightingVector{0, 0.5f, 480, 270}));
  assert((small.shadow_sampling == LightingVector{0, 0.5f, 16, 18}));
  // An absent source resets dimensions, never inherits a previous pass.
  inputs.sample_extent.reset();
  assert(ComposeNativeLighting(inputs).shadow_sampling == first.shadow_sampling);
  inputs.sample_extent = LightingExtent{1, 3};
  assert(ComposeNativeLighting(inputs).shadow_sampling[3] == 1.5f);
  for (int count : {-1, 0, 1, 2, 3}) {
    inputs.light_count = count;
    inputs.receiver_filter = 2;
    inputs.secondary_shadow = 3;
    inputs.shadow_mode = 4;
    inputs.specular = 5;
    for (bool enabled : {false, true}) {
      inputs.receivers_enabled = enabled;
      const auto pass = ComposeNativeLighting(inputs);
      const auto packed = PackLightingStaging(pass);
      assert(packed[340 / 4] == (enabled ? 5u : 0u));
      assert(packed[348 / 4] == (enabled ? 2u : 0u));
      assert(packed[352 / 4] == 3 && packed[360 / 4] == 4);
      assert(packed[364 / 4] == uint32_t(count > 0));
      assert(packed[368 / 4] == uint32_t(count > 1));
      assert(packed[408 / 4] == 13u + (enabled ? 2u : 0u) +
                                     (count > 0) + (count > 1));
      for (size_t i = 0; i < 4; ++i) {
        assert(packed[i] == std::bit_cast<uint32_t>(inputs.ambient[i]));
        assert(packed[20 + i] == packed[i]);
        assert(packed[4 + i] == std::bit_cast<uint32_t>(inputs.camera_position[i]));
        assert(packed[24 + i] == packed[4 + i]);
        assert(packed[28 + i] == std::bit_cast<uint32_t>(inputs.color_scale[i]));
        assert(packed[56 + i] == std::bit_cast<uint32_t>(pass.shadow_sampling[i]));
        assert(packed[72 + i] == std::bit_cast<uint32_t>(pass.scene_sampling[i]));
        assert(packed[93 + i] == 1);
      }
      for (size_t i = 0; i < packed.size(); ++i) {
        const bool written = i < 8 || (i >= 20 && i < 32) ||
            (i >= 56 && i < 60) || (i >= 72 && i < 76) || i == 85 ||
            i == 87 || i == 88 || (i >= 90 && i <= 96) || i >= 99;
        if (!written)
          assert(packed[i] == 0);
      }
      assert(packed[99] == std::bit_cast<uint32_t>(1.0f));
      assert(packed[100] == packed[99] && packed[101] == packed[99]);
    }
  }
  // Runtime imports can contain nonfinite loading values; retain their
  // arithmetic semantics instead of silently substituting a previous pass.
  inputs.scene_range = 0;
  assert(std::isinf(ComposeNativeLighting(inputs).scene_sampling[3]));
  inputs.shadow_bias = std::numeric_limits<float>::quiet_NaN();
  assert(std::isnan(ComposeNativeLighting(inputs).shadow_sampling[0]));
}
