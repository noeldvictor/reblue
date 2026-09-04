#include "gpu/scene/native_material_library.h"

#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace bd::gpu::scene;
namespace {
void Require(bool ok, const char *what) {
  if (!ok)
    throw std::runtime_error(what);
}
struct Scratch {
  std::filesystem::path path;
  Scratch() {
    const auto parent = std::filesystem::temp_directory_path();
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned i = 0; i < 100; ++i) {
      auto candidate = parent / ("reblue_native_material_test_" + std::to_string(stamp) +
                                 "_" + std::to_string(i));
      if (std::filesystem::create_directory(candidate)) {
        path = std::move(candidate);
        return;
      }
    }
    throw std::runtime_error("cannot create private material test directory");
  }
  ~Scratch() {
    // Only the directory this test successfully created, never a shared root.
    if (!path.empty()) {
      std::error_code ignored;
      std::filesystem::remove_all(path, ignored);
    }
  }
};
void WriteBytes(const std::filesystem::path &path, std::span<const uint8_t> bytes) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  file.close();
  Require(bool(file), "test file write");
}
void RepairChecksum(std::vector<uint8_t> &file) {
  const uint64_t sum = NativeMaterialContentId(std::span(file).subspan(16));
  for (unsigned i = 0; i < 8; ++i)
    file[8 + i] = uint8_t(sum >> (8 * i));
}
} // namespace

void TestMaterialAssets() {
  NativeMaterialAsset asset;
  auto &m = asset.properties;
  m.modulate_diffuse = true;
  m.has_diffuse_multiplier = m.has_specular_colour = true;
  m.has_reflection_colour = m.has_shininess = true;
  m.diffuse_multiplier = {1, 0.5f, 0.25f};
  m.specular_colour = {0.125f, 0.25f, 0.5f};
  m.reflection_colour = {1, 0, 0.5f, 0.75f};
  m.shininess = 12;
  std::vector<uint8_t> file;
  Require(EncodeNativeMaterial(asset, file), "encode");
  Require(file.size() == kNativeMaterialFileBytes, "fixed format size");
  Require(file[16] == 0 && file[20] == 31 && file[24] == 12, "native fields");
  Require(file[28] == 0 && file[29] == 0 && file[30] == 128 && file[31] == 63,
          "binary32 little endian, independent of host layout");
  NativeMaterialAsset decoded;
  Require(DecodeNativeMaterial(file, decoded) && decoded == asset, "round trip");
  const auto id = NativeMaterialContentId(file);
  Require(id == 0x901af8371ac3c368ull, "portable v1 identity golden");
  for (size_t n = 0; n < file.size(); ++n) {
    Require(!DecodeNativeMaterial(std::span(file).first(n), decoded), "truncated file");
    Require(decoded == asset, "transactional decode");
  }
  auto corrupt = file;
  corrupt.push_back(0);
  Require(!DecodeNativeMaterial(corrupt, decoded), "trailing byte");
  for (size_t n = 0; n < file.size(); ++n) {
    corrupt = file;
    corrupt[n] ^= 1;
    Require(!DecodeNativeMaterial(corrupt, decoded), "single-byte corruption");
  }
  for (auto [offset, value] : {std::pair<size_t, uint8_t>{16, 2}, {20, 128},
                              {25, 1}, {31, 127}, {67, 255}}) {
    corrupt = file;
    corrupt[offset] = value;
    RepairChecksum(corrupt);
    Require(!DecodeNativeMaterial(corrupt, decoded), "invalid rechecksummed field");
  }
  auto invalid = asset;
  invalid.properties.specular_colour[0] = std::numeric_limits<float>::infinity();
  corrupt = file;
  Require(!EncodeNativeMaterial(invalid, corrupt) && corrupt == file,
          "transactional invalid encode");
  invalid = asset;
  invalid.properties.has_reflection_colour = false;
  invalid.properties.reflection_colour[0] = std::numeric_limits<float>::quiet_NaN();
  Require(EncodeNativeMaterial(invalid, corrupt) && DecodeNativeMaterial(corrupt, decoded),
          "unknown fields canonicalize, never retain unconsumed bytes");
  Require(decoded.properties.reflection_colour == std::array<float, 4>{}, "unknown zero");
  const auto unknown_file = corrupt;
  corrupt[52] = 1;
  RepairChecksum(corrupt);
  Require(!DecodeNativeMaterial(corrupt, decoded), "noncanonical unknown value rejected");
  invalid.properties.reflection_colour = {};
  Require(EncodeNativeMaterial(invalid, corrupt) && corrupt == unknown_file,
          "unknown values do not alter identity");
  invalid = asset;
  invalid.properties.reflection_colour[1] = -0.0f;
  Require(EncodeNativeMaterial(invalid, corrupt) && corrupt == file, "negative zero canonical");
  invalid.lighting_model = NativeLightingModel::Cel;
  Require(EncodeNativeMaterial(invalid, corrupt) && DecodeNativeMaterial(corrupt, decoded),
          "cel lighting slot round trip");
  Require(NativeMaterialContentId(corrupt) != id, "lighting participates in identity");
  std::array<float, 4> output[3];
  Require(ComposeNativeMaterialAsset(decoded, {1, 1, 1, 1}, true, output) == 0,
          "unsupported shader must not silently become OriginalLit");
  Require(ComposeNativeMaterialAsset(asset, {1, 1, 1, 1}, true, output) == 7,
          "native asset composition");

  Scratch scratch;
  NativeMaterialHandle pinned;
  {
    NativeMaterialLibrary library(scratch.path, 2);
    pinned = library.Resolve(asset);
    Require(pinned && pinned->id == id && pinned->asset == asset, "cook and pin");
    Require(library.Resolve(asset) == pinned, "dedup ownership");
    Require(library.Load(id) == pinned, "source-free resident lookup");
    const auto stats = library.Stats();
    Require(stats.cooked == 1 && stats.loaded == 0 && stats.resident == 1 &&
            stats.memory_hits == 2 && stats.write_failures == 0, "cold library stats");
  }
  Require(pinned->asset == asset, "draw owns asset beyond library destruction");
  {
    NativeMaterialLibrary restarted(scratch.path, 2);
    auto loaded = restarted.Load(id); // no guest tags, source commands or runtime
    Require(loaded && loaded->asset == pinned->asset, "disk-only load on restart");
    Require(restarted.Stats().loaded == 1 && restarted.Stats().cooked == 0,
            "warm load not mislabeled cook");
    auto second_asset = asset;
    second_asset.properties.shininess = 13;
    auto second = restarted.Resolve(second_asset);
    Require(bool(second), "second pinned material");
    auto third_asset = asset;
    third_asset.properties.shininess = 14;
    Require(!restarted.Resolve(third_asset), "pinned budget refuses growth");
    Require(restarted.Stats().resident == 2 && restarted.Stats().budget_refusals == 1,
            "bounded residency");
    loaded.reset();
    auto third = restarted.Resolve(third_asset);
    Require(third && restarted.Stats().resident == 2, "unreferenced LRU eviction");
    Require(second->asset == second_asset && pinned->asset == asset, "pins survive eviction");
  }
  corrupt = file;
  corrupt.resize(12);
  WriteBytes(scratch.path / NativeMaterialLibrary::FileName(id), corrupt);
  {
    NativeMaterialLibrary damaged(scratch.path);
    Require(!damaged.Load(id), "reject interrupted write");
    Require(bool(damaged.Resolve(asset)), "recook invalid derived file");
    Require(damaged.Stats().cooked == 1 && damaged.Stats().invalid == 2,
            "corruption reported, not a cache hit");
  }
  {
    NativeMaterialLibrary repaired(scratch.path);
    Require(bool(repaired.Load(id)), "repaired file reloads");
  }
  WriteBytes(scratch.path / NativeMaterialLibrary::FileName(id ^ 1), file);
  {
    NativeMaterialLibrary wrong_name(scratch.path);
    Require(!wrong_name.Load(id ^ 1), "valid bytes under wrong identity rejected");
  }
  {
    // A file where the directory should be forces an I/O failure even as admin.
    NativeMaterialLibrary unwritable(scratch.path / NativeMaterialLibrary::FileName(id));
    Require(bool(unwritable.Resolve(asset)), "failed persistence retains usable host data");
    Require(unwritable.Stats().write_failures == 1, "write failure is observable");
    NativeMaterialLibrary zero_budget(scratch.path, 0);
    Require(!zero_budget.Resolve(asset) && zero_budget.Stats().resident == 0,
            "zero capacity must not grow");
  }
  std::cout << "native material format, identity, persistence and ownership passed\n";
}
