/**
 * @file    native_shadow.h
 * @brief   Current object/pass shadow inputs, independent of shader registers.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include <cstdint>
#include <optional>

namespace bd::gpu::scene {
struct NodeTag;

// The import boundary still supplies pass availability and the object's
// visibility stamp. The renderer consumes values, never a cached bool register
// or the outcome of another object's draw.
struct NativeShadowInputs {
  bool pass_enabled = false;
  bool receiver_filter_enabled = false;
  bool receiver_visible = false;
};

constexpr bool ReceivesNativeShadow(const NativeShadowInputs &inputs,
                                    bool material_disables_shadow) {
  return inputs.pass_enabled &&
         (!inputs.receiver_filter_enabled ||
          (inputs.receiver_visible && !material_disables_shadow));
}

constexpr bool ShadowStampMatches(uint16_t stamp, uint32_t frame) {
  return stamp < 0x8000 && uint32_t(stamp) == frame;
}

constexpr bool MaterialControlDisablesShadow(uint32_t present, uint32_t flags) {
  return (present & 1u) && (flags & 8u);
}

// Temporary engine-data adapter. Called BEFORE the node interpreter can
// mutate its deferred state; missing/unsupported inputs are not false values.
std::optional<NativeShadowInputs> ImportNodeShadowInputs(const NodeTag &tag);
} // namespace bd::gpu::scene
