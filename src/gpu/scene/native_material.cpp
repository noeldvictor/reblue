/**
 * @file    gpu/scene/native_material.cpp
 * @brief   Import fixed material properties without recording shader constants.
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/scene/native_material.h"
#include "gpu/scene/native_shadow.h"
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <rex/runtime.h>
#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/frame_stats.h"
#include "gpu/physical_buffers.h"
#include "gpu/scene/guest_scene.h"

namespace bd::gpu::scene {
namespace {
struct Decoded {
  std::vector<NativeMaterialRange> ranges;
  std::vector<NativeMaterialHandle> materials;
  bool valid = false;
};
// Only this temporary discovery cache indexes by guest address. Material
// ownership and persistent identities live in the independent native library.
thread_local std::unordered_map<uint32_t, Decoded> decoded;
thread_local size_t discovery_bytes = 0;
constexpr size_t kDiscoveryBudget = 8u << 20;
thread_local uint64_t generation = 0;
thread_local uint32_t checked[3]{}, wrong[3]{}, last_report = 0;
thread_local uint32_t composed[3]{};

NativeMaterialLibrary &Library() {
  static NativeMaterialLibrary library([] {
    std::filesystem::path root;
    if (auto *runtime = rex::Runtime::instance())
      root = runtime->cache_root();
    if (root.empty())
      root = std::filesystem::current_path();
    return root / "native_materials" / "v1";
  }());
  return library;
}

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
      if (result.valid) {
        result.materials.reserve(result.ranges.size());
        for (const auto &range : result.ranges)
          result.materials.push_back(Library().Resolve({range.material}));
      }
      return result;
    }
  }
  return result;
}
Decoded *FindCommands(const NodeTag &tag) {
  // Technique 11 selects visual-specific colours on texture tokens. Phase 1
  // rewrites colour/shininess commands. Their native recipes remain pending.
  if (tag.from_list || !tag.ctx_va || !tag.mesh_va || tag.tech == 11 ||
      bd::mem::try_load<uint32_t>(tag.ctx_va + 16, uint32_t(-1)) != 0)
    return {};
  const uint64_t now = PhysicalBufferGeneration();
  if (generation != now) {
    decoded.clear();
    discovery_bytes = 0;
    generation = now;
  }
  const uint32_t tokens = bd::mem::try_load<uint32_t>(tag.mesh_va);
  auto it = decoded.find(tokens);
  if (it == decoded.end()) {
    auto imported = ReadCommands(tokens);
    const size_t bytes = imported.ranges.capacity() * sizeof(NativeMaterialRange) +
                         imported.materials.capacity() * sizeof(NativeMaterialHandle);
    if (bytes > kDiscoveryBudget)
      return {};
    if (decoded.size() >= 4096 || discovery_bytes + bytes > kDiscoveryBudget) {
      decoded.clear();
      discovery_bytes = 0;
    }
    discovery_bytes += bytes;
    it = decoded.emplace(tokens, std::move(imported)).first;
  }
  if (!it->second.valid)
    return {};
  return &it->second;
}

bool Matches(const NativeMaterialRange &range, uint32_t ib, uint32_t vb,
             uint32_t index_va, uint32_t stream_va,
             uint32_t first_index, uint32_t index_count) {
  return range.first_index == first_index && range.index_count == index_count &&
      range.stream == 0 && range.index_record != 0xffff &&
      range.vertex_record != 0xffff &&
      bd::mem::try_load<uint32_t>(ib + range.index_record * 8 + 4) == index_va &&
      bd::mem::try_load<uint32_t>(vb + 4 + range.vertex_record * 12 + 8) == stream_va;
}
} // namespace

NativeMaterialHandle ImportNativeMaterial(
    const NodeTag &tag, uint32_t index_va, uint32_t stream_va,
    uint32_t first_index, uint32_t index_count) {
  const auto *commands = FindCommands(tag);
  if (!commands)
    return {};
  const uint32_t ib = bd::mem::try_load<uint32_t>(tag.mesh_va + 8);
  const uint32_t vb = bd::mem::try_load<uint32_t>(tag.mesh_va + 16);
  if (!ib || !vb)
    return {};
  NativeMaterialHandle found;
  for (size_t i = 0; i < commands->ranges.size(); ++i) {
    if (!Matches(commands->ranges[i], ib, vb, index_va, stream_va, first_index, index_count))
      continue;
    const auto &material = commands->materials[i];
    if (!material || (found && found->id != material->id))
      return {}; // repeated geometry under different materials is ambiguous
    found = material;
  }
  return found;
}

std::optional<bool> ImportMaterialDisablesShadow(
    const NodeTag &tag, uint32_t index_va, uint32_t stream_va,
    uint32_t first_index, uint32_t index_count) {
  const auto *commands = FindCommands(tag);
  if (!commands)
    return {};
  const uint32_t ib = bd::mem::try_load<uint32_t>(tag.mesh_va + 8);
  const uint32_t vb = bd::mem::try_load<uint32_t>(tag.mesh_va + 16);
  const uint32_t graph = bd::mem::try_load<uint32_t>(tag.ctx_va + 4);
  const auto *table_ptr = graph ? bd::mem::try_at<const be_u32>(graph + 8) : nullptr;
  if (!ib || !vb || !table_ptr)
    return {};
  const uint32_t table = uint32_t(*table_ptr);
  std::optional<bool> found;
  for (const auto &range : commands->ranges) {
    if (!Matches(range, ib, vb, index_va, stream_va, first_index, index_count))
      continue;
    bool disables = false;
    if (table && range.control_record != 0xffff) {
      // 0x822813CC: E000 selects a 16-byte control record. sub_8228AB40
      // dispatches sub_8228AAB0 for bit 0; its fourth output byte is the
      // shadow-disable flag (payload bit 3). An absent table is a no-op.
      const uint64_t address = uint64_t(table) + uint64_t(range.control_record) * 16;
      if (address + 4 > std::numeric_limits<uint32_t>::max())
        return {};
      const auto *mask = bd::mem::try_at<const be_u32>(uint32_t(address));
      const auto *flags = bd::mem::try_at<const be_u32>(uint32_t(address + 4));
      if (!mask || !flags)
        return {};
      disables = MaterialControlDisablesShadow(uint32_t(*mask), uint32_t(*flags));
    }
    if (found && *found != disables)
      return {}; // ambiguous geometry under different feature policies
    found = disables;
  }
  return found;
}

uint32_t EvaluateNativeMaterial(const NodeTag &tag,
                               const NativeMaterialAsset &material,
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
  return ComposeNativeMaterialAsset(material, object_colour,
      bd::mem::try_load<uint32_t>(tag.visual_va + 3044) != 0,
      values);
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
    const auto assets = Library().Stats();
    BD_INFO("[native-material-assets] {} cooked, {} loaded, {} resident, {} memory hits; "
            "{} invalid, {} write failures, {} budget refusals; {} discovery bytes",
            assets.cooked, assets.loaded, assets.resident, assets.memory_hits,
            assets.invalid, assets.write_failures, assets.budget_refusals, discovery_bytes);
    last_report = frame;
  }
}
} // namespace bd::gpu::scene
