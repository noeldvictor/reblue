// Standalone desktop material cooker/validator; needs neither game runtime nor GPU.
#include "gpu/scene/native_material_library.h"
#include <charconv>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace bd::gpu::scene;
int main(int argc, char **argv) {
  try {
    if (argc != 3 && argc != 4) {
      std::cerr << "native_material_cook --verify <material-directory>\n"
                   "native_material_cook --commands-be <model-commands.bin> <material-directory>\n";
      return 1;
    }
    if (argc == 3 && std::string(argv[1]) == "--verify") {
      NativeMaterialLibrary library(argv[2]);
      size_t count = 0, fields[3]{};
      for (const auto &entry : std::filesystem::directory_iterator(argv[2])) {
        if (!entry.is_regular_file() || entry.path().extension() != ".bdmat")
          continue;
        const auto name = entry.path().stem().string();
        NativeMaterialId id = 0;
        const auto parsed = std::from_chars(name.data(), name.data() + name.size(), id, 16);
        if (name.size() != 16 || parsed.ec != std::errc{} ||
            parsed.ptr != name.data() + name.size())
          throw std::runtime_error("invalid material filename: " + entry.path().string());
        auto material = library.Load(id);
        if (!material)
          throw std::runtime_error("invalid material: " + entry.path().string());
        std::array<float, 4> values[3];
        const auto mask = ComposeNativeMaterialAsset(material->asset, {1, 1, 1, 1}, true, values);
        for (size_t i = 0; i < 3; ++i)
          fields[i] += bool(mask & (1u << i));
        ++count;
      }
      if (!count)
        throw std::runtime_error("no native materials to verify");
      std::cout << count << " materials loaded by ID without guest/runtime/GPU; composable "
                << "diffuse/specular/reflection " << fields[0] << '/' << fields[1] << '/'
                << fields[2] << '\n';
      return 0;
    }
    if (argc != 4 || std::string(argv[1]) != "--commands-be")
      throw std::runtime_error("unknown command or wrong argument count");
    std::ifstream input(argv[2], std::ios::binary | std::ios::ate);
    if (!input)
      throw std::runtime_error("cannot open model commands");
    const auto size = input.tellg();
    if (size <= 0 || size > 131072 || size % 2)
      throw std::runtime_error("model commands must be <= 65536 big-endian words");
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char *>(bytes.data()), size))
      throw std::runtime_error("cannot read model commands");
    std::vector<uint16_t> words;
    for (size_t i = 0; i < bytes.size(); i += 2)
      words.push_back((uint16_t(bytes[i]) << 8) | bytes[i + 1]);
    std::vector<NativeMaterialRange> ranges;
    if (!DecodeMeshMaterials(words, ranges) || ranges.empty())
      throw std::runtime_error("unsupported, incomplete or empty model commands");
    NativeMaterialLibrary library(argv[3]);
    for (size_t i = 0; i < ranges.size(); ++i) {
      auto material = library.Resolve({ranges[i].material});
      if (!material)
        throw std::runtime_error("cannot cook material range " + std::to_string(i));
      std::cout << "range " << i << " -> " << NativeMaterialLibrary::FileName(material->id).string()
                << '\n';
    }
    if (library.Stats().write_failures)
      throw std::runtime_error("material file write failed");
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
