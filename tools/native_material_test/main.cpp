#include "gpu/scene/native_material_data.h"
#include "gpu/scene/native_shadow.h"
#include <cmath>
#include <iostream>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

using namespace bd::gpu::scene;
void TestMaterialAssets();
void TestNativeSkinBindings();
void Check(bool good) {
  if (!good)
    throw std::runtime_error("native material check failed");
}
int main(int argc, char **argv) {
  // 0xff inside bone/colour operands must not terminate the stream. Two
  // strips share geometry records but carry different material properties.
  const std::vector<uint16_t> words = {
      0, 0x0202, 0xff, 0x9000, 0x4000, 7, 0x5002,
      0x0100, 0x9080, 0x4020, 0x040c, 0x9326, 0x1f0d,
      0x9400, 0x8040, 0x2010, 0x1000, 8, 4,
      0x0400, 0x0101, 0x2000, 5, 14, 0xff};
  if (argc == 3 && std::string(argv[1]) == "--commands-fixture") {
    std::ofstream output(argv[2], std::ios::binary | std::ios::trunc);
    for (uint16_t word : words) {
      const char bytes[]{char(word >> 8), char(word & 0xff)};
      output.write(bytes, 2);
    }
    output.close();
    return output ? 0 : 1;
  }
  if (argc != 1)
    return 1;
  std::vector<NativeMaterialRange> ranges;
  Check(DecodeMeshMaterials(words, ranges));
  Check(ranges.size() == 2);
  Check(ranges[0].index_count == 10 && ranges[0].first_index == 4);
  Check(ranges[0].vertex_record == 7 && ranges[0].index_record == 2);
  std::array<float, 4> diffuse{}, specular{}, reflection{};
  const std::array<float, 4> base{0.5f, 0.25f, 0.125f, 0.75f};
  auto mask = ComposeNativeMaterial(ranges[0].material, base, true,
                                     diffuse, specular, reflection);
  Check(mask == 7);
  Check(std::fabs(diffuse[0] - 0.5f * (128.0f / 255)) < 1e-7f);
  Check(diffuse[3] == base[3]); // modulation never scales alpha
  Check(specular[3] == 12 && std::fabs(specular[0] - 38.0f / 255) < 1e-7f);
  Check(std::fabs(reflection[0] - 64.0f / 255) < 1e-7f);
  Check(std::fabs(reflection[1] - 32.0f / 255) < 1e-7f);
  Check(std::fabs(reflection[2] - 16.0f / 255) < 1e-7f);
  Check(std::fabs(reflection[3] - 128.0f / 255) < 1e-7f);
  mask = ComposeNativeMaterial(ranges[1].material, base, true,
                                diffuse, specular, reflection);
  Check(diffuse == base && specular == std::array<float, 4>{});
  Check((ComposeNativeMaterial(ranges[0].material, base, false,
                               diffuse, specular, reflection) & kNativeSpecular) == 0);
  NativeMaterialProperties unknown;
  unknown.modulate_diffuse = true;
  Check(ComposeNativeMaterial(unknown, base, true, diffuse, specular, reflection) == 0);
  auto invalid_base = base;
  invalid_base[1] = std::numeric_limits<float>::quiet_NaN();
  Check(!(ComposeNativeMaterial(ranges[0].material, invalid_base, true,
                                diffuse, specular, reflection) & kNativeDiffuse));
  for (size_t n = 0; n < words.size(); ++n) {
    std::vector<NativeMaterialRange> before = ranges;
    Check(!DecodeMeshMaterials(std::span(words).first(n), ranges));
    Check(ranges.size() == before.size() && ranges[0].material == before[0].material);
  }
  const uint16_t unsupported[]{0x7000, 0xff};
  Check(!DecodeMeshMaterials(unsupported, ranges));
  Check(MeshCommandOperands(0x9400) == 2);
  Check(MeshCommandOperands(0x9000) == 1);
  Check(MeshCommandOperands(0x02ff) == 255);
  const uint16_t repeated_power[]{0x0400, 0x93ff, 0xffff, 0x0400,
                                  0x1000, 1, 0, 0xff};
  Check(DecodeMeshMaterials(repeated_power, ranges));
  Check(ranges[0].material.specular_colour == std::array<float, 3>{1, 1, 1});
  TestMaterialAssets();
  TestNativeSkinBindings();
  const uint16_t controls[]{0xe003, 0x1000, 1, 0, 0xe005, 0x1000, 1, 3, 0xff};
  Check(DecodeMeshMaterials(controls, ranges));
  Check(ranges.size() == 2 && ranges[0].control_record == 3 && ranges[1].control_record == 5);
  for (unsigned bits = 0; bits < 16; ++bits) {
    NativeShadowInputs inputs{bool(bits & 1), bool(bits & 2), bool(bits & 4)};
    const bool disabled = bits & 8;
    // Truth table: pass-off always rejects; filter-off ignores object/material;
    // filter-on requires a visible receiver and an enabled material.
    Check(ReceivesNativeShadow(inputs, disabled) == bool((0x22a2u >> bits) & 1u));
  }
  Check(ShadowStampMatches(17, 17) && !ShadowStampMatches(17, 18));
  Check(!ShadowStampMatches(0xffff, 0xffff) && !ShadowStampMatches(0x8000, 0x8000));
  Check(!ShadowStampMatches(1, 0x10001)); // never truncate the frame counter
  Check(ShadowStampMatches(0, 0) && ShadowStampMatches(0x7fff, 0x7fff));
  Check(!MaterialControlDisablesShadow(0, 8));
  Check(!MaterialControlDisablesShadow(2, 8));
  Check(!MaterialControlDisablesShadow(1, 7));
  Check(MaterialControlDisablesShadow(1, 8) && MaterialControlDisablesShadow(3, 15));
  std::cout << "native material decoding and composition passed\n";
}
