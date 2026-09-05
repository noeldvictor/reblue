/**
 * @file    gpu/scene/native_material_data.cpp
 * @brief   Decode model material commands once, outside draw submission.
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/scene/native_material_data.h"

#include <cmath>
#include <utility>

namespace bd::gpu::scene {

int MeshCommandOperands(uint16_t command) {
  if (command == 0 || command == 0xff)
    return 0;
  switch (command & 0xf000) {
  case 0x0000:
    switch (command & 0x0f00) {
    case 0x0200: return command & 0xff; // bone indices
    case 0x0100: case 0x0300: case 0x0400: case 0x0500:
    case 0x0600: case 0x0700: case 0x0800: case 0x0900: return 0;
    default: return -1;
    }
  case 0x1000: case 0x2000: case 0x3000: return 2; // strip range
  case 0x4000: return 1; // vertex/declaration record
  case 0x5000: case 0x6000: case 0xe000: return 0;
  case 0x9000:
    switch (command & 0x0f00) {
    case 0x0000: case 0x0300: return 1; // RGB8
    case 0x0400: return 2; // RGBA8, split across words
    default: return -1;
    }
  default: return -1;
  }
}

bool DecodeMeshMaterials(std::span<const uint16_t> commands,
                         std::vector<NativeMaterialRange> &out) {
  if (commands.size() > 65536)
    return false;
  std::vector<NativeMaterialRange> ranges;
  NativeMaterialRange current;
  constexpr float byte_scale = 1.0f / 255.0f;
  for (size_t cursor = 0; cursor < commands.size();) {
    const uint16_t command = commands[cursor++];
    const int operands = MeshCommandOperands(command);
    if (operands < 0 || size_t(operands) > commands.size() - cursor)
      return false;
    if (command == 0xff) {
      out = std::move(ranges);
      return true;
    }
    const uint16_t kind = command & 0xf000;
    auto &m = current.material;
    if (kind >= 0x1000 && kind <= 0x3000) {
      // bdSceneNodeDrawIndexed adds 2 to the first operand and submits a
      // triangle strip; the second operand is StartIndex, not a byte offset.
      current.index_count = uint32_t(commands[cursor]) + 2;
      current.first_index = commands[cursor + 1];
      ranges.push_back(current);
    } else if (kind == 0x4000) {
      current.stream = command & 0x0fff;
      current.vertex_record = commands[cursor];
    } else if (kind == 0x5000) {
      current.index_record = command & 0x0fff;
    } else if (kind == 0xe000) {
      current.control_record = command & 0x0fff;
    } else if ((command & 0xff00) == 0x0100) {
      m.modulate_diffuse = (command & 0xff) == 0;
    } else if ((command & 0xff00) == 0x0400) {
      const uint8_t shininess = command & 0xff;
      // The interpreter skips a repeated power command, even if an RGB
      // command has changed the specular colour in between.
      if (!m.has_shininess || m.shininess != shininess) {
        m.shininess = shininess;
        m.has_shininess = true;
        if (!m.shininess) {
          m.specular_colour = {};
          m.has_specular_colour = true;
        }
      }
    } else if (kind == 0x9000) {
      const uint16_t rgb = commands[cursor];
      const std::array<float, 3> colour = {
          float(command & 0xff) * byte_scale,
          float(rgb >> 8) * byte_scale, float(rgb & 0xff) * byte_scale};
      switch (command & 0x0f00) {
      case 0x0000:
        m.diffuse_multiplier = colour;
        m.has_diffuse_multiplier = true;
        break;
      case 0x0300:
        m.specular_colour = colour;
        m.has_specular_colour = true;
        break;
      case 0x0400: {
        const uint16_t rg = commands[cursor + 1];
        // loc_822814BC: low byte of first word is R, high byte of second
        // is G, low byte of second is B, high byte of first is A.
        m.reflection_colour = {float(rgb & 0xff) * byte_scale,
                               float(rg >> 8) * byte_scale,
                               float(rg & 0xff) * byte_scale,
                               float(rgb >> 8) * byte_scale};
        m.has_reflection_colour = true;
        break;
      }
      }
    }
    cursor += size_t(operands);
  }
  return false;
}

uint32_t ComposeNativeMaterial(const NativeMaterialProperties &m,
                               const std::array<float, 4> &object_colour,
                               bool writes_shininess,
                               std::array<float, 4> &diffuse,
                               std::array<float, 4> &specular,
                               std::array<float, 4> &reflection) {
  uint32_t mask = 0;
  bool finite = true;
  for (float c : object_colour)
    finite &= std::isfinite(c);
  if (finite && (!m.modulate_diffuse || m.has_diffuse_multiplier)) {
    diffuse = object_colour;
    if (m.modulate_diffuse)
      for (size_t i = 0; i < 3; ++i)
        diffuse[i] *= m.diffuse_multiplier[i];
    mask |= kNativeDiffuse;
  }
  if (writes_shininess && m.has_shininess && m.has_specular_colour) {
    for (size_t i = 0; i < 3; ++i)
      specular[i] = m.specular_colour[i];
    specular[3] = float(m.shininess);
    mask |= kNativeSpecular;
  }
  if (m.has_reflection_colour) {
    reflection = m.reflection_colour;
    mask |= kNativeReflection;
  }
  return mask;
}

} // namespace bd::gpu::scene
