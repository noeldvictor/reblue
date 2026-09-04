/**
 * @file    gpu/scene/native_material.cpp
 * @brief   Import fixed material properties without recording shader constants.
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/scene/native_material.h"
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>
#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/frame_stats.h"
#include "gpu/physical_buffers.h"
#include "gpu/scene/guest_scene.h"

namespace bd::gpu::scene {
namespace {
struct Decoded {
  std::vector<NativeMaterialRange> ranges;
  bool valid = false;
};
// A discovery cache, not the final persistent asset loader. Only the boundary
// indexes by guest address; NativeMaterialProperties contains none.
thread_local std::unordered_map<uint32_t, Decoded> decoded;
thread_local uint64_t generation = 0;
thread_local uint32_t checked[3]{}, wrong[3]{}, last_report = 0;
thread_local uint32_t composed[3]{};

Decoded ReadCommands(uint32_t source) {
  Decoded result;
  std::vector<uint16_t> words;
  if (!source)
    return result;
  while (words.size() < 65536) {
    auto read = [&]() {
      const uint64_t address = uint64_t(source) + words.size() * 2;
      if (address > std::numeric_limits<uint32_t>::max())
        return false;
      const auto *word = bd::mem::try_at<const be_u16>(uint32_t(address));
      if (!word)
        return false;
      words.push_back(uint16_t(*word));
      return true;
    };
    if (!read())
      return result;
    const uint16_t command = words.back();
    const int operands = MeshCommandOperands(command);
    if (operands < 0 || words.size() + size_t(operands) > 65536)
      return result;
    for (int i = 0; i < operands; ++i)
      if (!read())
        return result;
    if (command == 0xff) {
      result.valid = DecodeMeshMaterials(words, result.ranges);
      return result;
    }
  }
  return result;
}
} // namespace

std::optional<NativeMaterialProperties> ImportNativeMaterial(
    const NodeTag &tag, uint32_t index_va, uint32_t stream_va,
    uint32_t first_index, uint32_t index_count) {
  // Technique 11 selects visual-specific colours on texture tokens. Phase 1
  // rewrites colour/shininess commands. Their native recipes remain pending.
  if (tag.from_list || !tag.ctx_va || !tag.mesh_va || tag.tech == 11 ||
      bd::mem::try_load<uint32_t>(tag.ctx_va + 16, uint32_t(-1)) != 0)
    return {};
  const uint64_t now = PhysicalBufferGeneration();
  if (generation != now) {
    decoded.clear();
    generation = now;
  }
  const uint32_t tokens = bd::mem::try_load<uint32_t>(tag.mesh_va);
  auto it = decoded.find(tokens);
  if (it == decoded.end()) {
    if (decoded.size() >= 4096)
      decoded.clear();
    it = decoded.emplace(tokens, ReadCommands(tokens)).first;
  }
  if (!it->second.valid)
    return {};
  const uint32_t ib = bd::mem::try_load<uint32_t>(tag.mesh_va + 8);
  const uint32_t vb = bd::mem::try_load<uint32_t>(tag.mesh_va + 16);
  if (!ib || !vb)
    return {};
  std::optional<NativeMaterialProperties> found;
  for (const auto &range : it->second.ranges) {
    if (range.first_index != first_index || range.index_count != index_count ||
        range.stream != 0 || range.index_record == 0xffff ||
        range.vertex_record == 0xffff)
      continue;
    if (bd::mem::try_load<uint32_t>(ib + range.index_record * 8 + 4) != index_va ||
        bd::mem::try_load<uint32_t>(vb + 4 + range.vertex_record * 12 + 8) != stream_va)
      continue;
    if (found && *found != range.material)
      return {}; // repeated geometry under different materials is ambiguous
    found = range.material;
  }
  return found;
}

uint32_t EvaluateNativeMaterial(const NodeTag &tag,
                               const NativeMaterialProperties &material,
                               std::array<float, 4> values[3]) {
  if (!tag.visual_va || tag.from_list || !tag.ctx_va || tag.tech == 11 ||
      bd::mem::try_load<uint32_t>(tag.ctx_va + 16, uint32_t(-1)) != 0)
    return 0;
  std::array<float, 4> object_colour;
  for (uint32_t i = 0; i < 4; ++i) {
    const auto *component = bd::mem::try_at<const be_f32>(
        tag.visual_va + kVisualMaterialColor + i * 4);
    if (!component)
      return 0;
    object_colour[i] = float(*component);
  }
  return ComposeNativeMaterial(material, object_colour,
      bd::mem::try_load<uint32_t>(tag.visual_va + 3044) != 0,
      values[0], values[1], values[2]);
}

void NativeMaterialCheck(uint32_t mask, const std::array<float, 4> values[3],
                         const uint8_t *pixel_constants) {
  for (uint32_t field = 0; field < 3; ++field) {
    if (!(mask & (1u << field)))
      continue;
    ++checked[field];
    bool equal = true;
    for (uint32_t i = 0; i < 4; ++i) {
      float actual;
      std::memcpy(&actual, pixel_constants + (field + 3) * 16 + i * 4, 4);
      equal &= std::isfinite(actual) &&
          std::fabs(actual - values[field][i]) <= 1e-6f * (1 + std::fabs(actual));
    }
    if (!equal)
      ++wrong[field];
  }
  NativeMaterialNoteReplay(0);
}

void NativeMaterialNoteReplay(uint32_t mask) {
  for (uint32_t field = 0; field < 3; ++field)
    if (mask & (1u << field))
      ++composed[field];
  const uint32_t frame = FrameStatFrameCount();
  if (frame - last_report >= 300) {
    BD_INFO("[native-material] source checks (wrong/checked): diffuse {}/{}, "
            "specular {}/{}, reflection {}/{}; {} decoded streams; "
            "replay fields composed {}/{}/{}",
            wrong[0], checked[0], wrong[1], checked[1], wrong[2], checked[2],
            decoded.size(), composed[0], composed[1], composed[2]);
    last_report = frame;
  }
}
} // namespace bd::gpu::scene
