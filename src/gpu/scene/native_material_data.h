/**
 * @file    gpu/scene/native_material_data.h
 * @brief   Asset-level material properties decoded from model commands.
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once
#include "gpu/scene/native_skin.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace bd::gpu::scene {

// Named asset properties, not a captured shader register file. Unknown fields
// stay unknown: an omitted command inherits state and is not a white default.
struct NativeMaterialProperties {
  std::array<float, 3> diffuse_multiplier{};
  std::array<float, 3> specular_colour{};
  std::array<float, 4> reflection_colour{};
  bool modulate_diffuse = false;
  uint8_t shininess = 0;
  bool has_diffuse_multiplier = false;
  bool has_specular_colour = false;
  bool has_reflection_colour = false;
  bool has_shininess = false;
  bool operator==(const NativeMaterialProperties &) const = default;
};

struct NativeMaterialRange {
  NativeMaterialProperties material;
  // Unknown until a bone-index command; an explicit empty binding is unskinned.
  std::optional<NativeSkinBinding> skin;
  uint32_t index_count = 0;
  uint32_t first_index = 0;
  uint16_t index_record = 0xffff;
  uint16_t vertex_record = 0xffff;
  uint16_t stream = 0;
  // Import-only index into the model's control table, not a shader bool value.
  uint16_t control_record = 0xffff;
};

// Operand framing is shared by the bounded guest reader and offline decoder.
// -1 is an unsupported opcode. 0x00ff ends the stream, only at an opcode boundary.
int MeshCommandOperands(uint16_t command);

// Input words are host endian. The native material program here covers scene
// phase 0; the adapter must not apply it to phase 1's shader/colour overrides.
// Failure is transactional, including truncated operands and missing terminator.
bool DecodeMeshMaterials(std::span<const uint16_t> commands,
                         std::vector<NativeMaterialRange> &out);

// Compose only fully known values; no staging globals or sibling draw state.
// Specular power is written by the game only when the visual permits it.
uint32_t ComposeNativeMaterial(const NativeMaterialProperties &material,
                               const std::array<float, 4> &object_colour,
                               bool writes_shininess,
                               std::array<float, 4> &diffuse,
                               std::array<float, 4> &specular,
                               std::array<float, 4> &reflection);
constexpr uint32_t kNativeDiffuse = 1;
constexpr uint32_t kNativeSpecular = 2;
constexpr uint32_t kNativeReflection = 4;

} // namespace bd::gpu::scene
