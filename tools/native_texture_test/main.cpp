#include "gpu/scene/native_texture_library.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace bd::gpu::scene;
namespace {
void Check(bool good, const char *what) {
  if (!good)
    throw std::runtime_error(what);
}
NativeTextureData
Example(NativeTextureFormat format = NativeTextureFormat::BC1) {
  NativeTextureData data;
  data.format = format;
  data.width = data.height = 8;
  data.images.emplace_back(NativeTextureImageBytes(format, 8, 8, 1), 255);
  return data;
}
void Write(const std::filesystem::path &path, std::span<const uint8_t> bytes) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  file.close();
  Check(bool(file), "write test fixture");
}
void Checksum(std::vector<uint8_t> &bytes) {
  auto hash = NativeTextureContentId(std::span(bytes).subspan(16));
  for (unsigned i = 0; i < 8; ++i)
    bytes[8 + i] = uint8_t(hash >> (8 * i));
}
struct Scratch {
  std::filesystem::path path;
  Scratch() {
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned i = 0; i < 100; ++i) {
      auto candidate = std::filesystem::temp_directory_path() /
                       ("reblue_native_texture_test_" + std::to_string(stamp) +
                        "_" + std::to_string(i));
      if (std::filesystem::create_directory(candidate)) {
        path = std::move(candidate);
        return;
      }
    }
    throw std::runtime_error("cannot create scratch directory");
  }
  ~Scratch() {
    // Only this test's successfully created directory; no user assets.
    if (!path.empty()) {
      std::error_code ignored;
      std::filesystem::remove_all(path, ignored);
    }
  }
};
} // namespace
int main(int argc, char **argv) {
  try {
    if (argc == 3 && std::string(argv[1]) == "--fixture") {
      std::vector<uint8_t> file;
      Check(EncodeNativeTexture(Example(), file), "fixture encoding");
      Write(argv[2], file);
      return 0;
    }
    Check(argc == 1, "unknown arguments");
    for (auto format : {NativeTextureFormat::BC1, NativeTextureFormat::BC2,
                        NativeTextureFormat::BC3, NativeTextureFormat::RGBA8}) {
      auto data = Example(format);
      std::vector<uint8_t> file;
      Check(EncodeNativeTexture(data, file), "encode supported format");
      Check(file[16] == uint8_t(format) && file[24] == 8 && file[25] == 0,
            "native little endian fields");
      NativeTextureData decoded;
      Check(DecodeNativeTexture(file, decoded) && decoded == data,
            "round trip");
      for (size_t n = 0; n < file.size(); ++n) {
        Check(!DecodeNativeTexture(std::span(file).first(n), decoded),
              "truncated file");
        Check(decoded == data, "transactional decode");
        auto bad = file;
        bad[n] ^= 1;
        Check(!DecodeNativeTexture(bad, decoded), "corrupted byte");
      }
      auto bad = file;
      bad.push_back(0);
      Checksum(bad);
      Check(!DecodeNativeTexture(bad, decoded),
            "trailing byte with repaired checksum");
      for (auto offset : {16, 20, 24, 36}) {
        bad = file;
        for (int j = 0; j < 4; ++j)
          bad[offset + j] = 255;
        Checksum(bad);
        Check(!DecodeNativeTexture(bad, decoded),
              "oversized or unsupported geometry");
      }
      NativeTextureData chain;
      if (format != NativeTextureFormat::RGBA8) {
        Check(GenerateNativeTextureMips(data, chain), "native mip generation");
        Check(chain.mip_levels == 2 && chain.images[0] == data.images[0],
              "base unchanged, 4x4 tail");
        Check(EncodeNativeTexture(chain, bad) &&
                  DecodeNativeTexture(bad, decoded) && decoded == chain,
              "mip chain round trip");
      } else
        Check(!GenerateNativeTextureMips(data, chain),
              "unsupported mip format refused");
    }
    // Padding must neither leak into the asset nor participate in identity.
    std::vector<uint8_t> padded(3 * 768, 0xee), tight{7};
    for (uint32_t z = 0; z < 3; ++z)
      for (uint32_t y = 0; y < 2; ++y)
        for (uint32_t x = 0; x < 16; ++x)
          padded[z * 768 + y * 256 + x] = uint8_t(z * 32 + y * 16 + x);
    Check(ImportNativeTextureImage(NativeTextureFormat::BC1, 8, 8, 3, padded,
                                   256, 768, tight),
          "padded volume import");
    Check(tight.size() == 96, "tight size");
    for (size_t i = 0; i < tight.size(); ++i)
      Check(tight[i] == i, "row/slice ordering");
    const auto before = tight;
    Check(!ImportNativeTextureImage(NativeTextureFormat::BC1, 8, 8, 3,
                                    std::span(padded).first(1800), 256, 768,
                                    tight) &&
              tight == before,
          "short padded source");
    Check(!ImportNativeTextureImage(NativeTextureFormat::BC1, 8, 8, 3, padded,
                                    8, 768, tight),
          "short row");
    Check(!ImportNativeTextureImage(NativeTextureFormat::BC1, 8, 8, 3, padded,
                                    256, 300, tight),
          "short slice");
    auto volume = Example();
    volume.dimension = NativeTextureDimension::Volume;
    volume.depth = 3;
    volume.images[0] = tight;
    std::vector<uint8_t> file;
    NativeTextureData decoded;
    Check(EncodeNativeTexture(volume, file) &&
              DecodeNativeTexture(file, decoded) && decoded == volume,
          "volume round trip");
    auto cube = Example();
    cube.dimension = NativeTextureDimension::Cube;
    cube.images.resize(6, cube.images[0]);
    for (size_t i = 0; i < 6; ++i)
      cube.images[i][0] = uint8_t(i);
    Check(EncodeNativeTexture(cube, file) &&
              DecodeNativeTexture(file, decoded) && decoded == cube,
          "all cube faces round trip");
    const auto cube_id = NativeTextureContentId(file);
    cube.images[5][0] ^= 1;
    Check(EncodeNativeTexture(cube, file) &&
              NativeTextureContentId(file) != cube_id,
          "last face contributes to ID");
    cube.images.pop_back();
    Check(!ValidateNativeTexture(cube), "missing face refused");

    Scratch scratch;
    auto base = Example();
    NativeTextureHandle pin;
    {
      NativeTextureLibrary library(scratch.path);
      pin = library.Resolve(base, true);
      Check(pin && pin->data.mip_levels == 2, "cook and own native mip asset");
      Check(library.Resolve(base, true) == pin, "recipe dedup");
      const auto stats = library.Stats();
      Check(stats.cooked == 1 && stats.generated_mips == 1 &&
                stats.cached_mips == 1 && stats.write_failures == 0,
            "cold recipe stats");
    }
    {
      NativeTextureLibrary restarted(scratch.path);
      auto warm = restarted.Resolve(base, true);
      Check(warm && warm->id == pin->id && warm->data == pin->data,
            "warm recipe stable identity");
      Check(restarted.Stats().generated_mips == 0 &&
                restarted.Stats().cached_mips == 1 &&
                restarted.Stats().loaded == 1,
            "restart bypasses generation");
      Check(restarted.Load(pin->id) == warm, "load by ID with no source");
      auto changed = base;
      changed.images[0][0] ^= 1;
      auto other = restarted.Resolve(changed, true);
      Check(other && other->id != warm->id, "changed base invalidates recipe");
    }
    {
      NativeTextureLibrary limited(scratch.path / "limited", 200);
      auto one = limited.Resolve(base);
      Check(bool(one), "fits one small asset");
      auto changed = base;
      changed.images[0][0] ^= 1;
      Check(!limited.Resolve(changed) && limited.Stats().budget_refusals == 1,
            "pins enforce byte budget");
      one.reset();
      Check(bool(limited.Resolve(changed)) && limited.Stats().resident == 1,
            "unpinned asset eviction");
      Check(pin->data.mip_levels == 2, "asset survives old library lifetime");
    }
    Write(scratch.path / NativeTextureLibrary::FileName(pin->id),
          std::array<uint8_t, 3>{1, 2, 3});
    {
      NativeTextureLibrary damaged(scratch.path);
      Check(!damaged.Load(pin->id), "corrupted native file rejected");
      auto repaired = damaged.Resolve(base, true);
      Check(repaired && repaired->id == pin->id &&
                damaged.Stats().generated_mips == 1,
            "broken recipe output recooked");
    }
    std::cout << "native texture format, padding, mips, persistence and "
                 "ownership passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
