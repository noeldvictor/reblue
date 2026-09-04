#include "gpu/scene/native_texture_library.h"
#include <charconv>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
using namespace bd::gpu::scene;
int main(int argc, char **argv) {
  try {
    if (argc == 3 && std::string(argv[1]) == "--verify") {
      NativeTextureLibrary library(argv[2]);
      uint64_t count = 0, levels = 0, cubes = 0, volumes = 0, bytes = 0;
      for (const auto &entry : std::filesystem::directory_iterator(argv[2])) {
        if (!entry.is_regular_file() || entry.path().extension() != ".bdtex")
          continue;
        const auto name = entry.path().stem().string();
        uint64_t id = 0;
        const auto parsed =
            std::from_chars(name.data(), name.data() + name.size(), id, 16);
        if (name.size() != 16 || parsed.ec != std::errc{} ||
            parsed.ptr != name.data() + name.size())
          throw std::runtime_error("invalid texture filename");
        auto asset = library.Load(id);
        if (!asset)
          throw std::runtime_error("invalid texture: " + entry.path().string());
        ++count;
        levels += asset->data.images.size();
        bytes += entry.file_size();
        cubes += asset->data.dimension == NativeTextureDimension::Cube;
        volumes += asset->data.dimension == NativeTextureDimension::Volume;
      }
      if (!count)
        throw std::runtime_error("no textures to verify");
      std::cout << count << " textures, " << levels << " images, " << cubes
                << " cubes, " << volumes << " volumes, " << bytes
                << " file bytes; loaded by ID without guest/runtime/GPU\n";
      return 0;
    }
    if (argc != 4 || std::string(argv[1]) != "--mips")
      throw std::runtime_error("native_texture_cook --verify <directory> OR "
                               "--mips <input.bdtex> <directory>");
    std::ifstream input(argv[2], std::ios::binary | std::ios::ate);
    if (!input || input.tellg() < 40 ||
        uint64_t(input.tellg()) > kNativeTextureMaxBytes)
      throw std::runtime_error("invalid input texture size");
    std::vector<uint8_t> bytes(static_cast<size_t>(input.tellg()));
    input.seekg(0);
    NativeTextureData data;
    if (!input.read(reinterpret_cast<char *>(bytes.data()), bytes.size()) ||
        !DecodeNativeTexture(bytes, data))
      throw std::runtime_error("invalid input texture");
    if (data.dimension != NativeTextureDimension::Image2D)
      throw std::runtime_error("mip cooker needs 2D input");
    data.images.resize(1);
    data.mip_levels = 1;
    NativeTextureLibrary library(argv[3]);
    auto asset = library.Resolve(std::move(data), true);
    if (!asset || library.Stats().write_failures)
      throw std::runtime_error("mip cooking failed");
    std::cout << NativeTextureLibrary::FileName(asset->id).string() << " : "
              << asset->data.mip_levels << " levels; "
              << library.Stats().generated_mips << " generated / "
              << library.Stats().cached_mips << " cached\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
