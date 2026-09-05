#ifdef NDEBUG
#undef NDEBUG
#endif
#include "gpu/scene/raster_import.h"
#include <cassert>
#include <iostream>
#include <random>

using namespace bd::gpu::scene;
int main() {
  using F = plume::RenderComparisonFunction;
  using S = plume::RenderStencilOp;
  for (size_t i = 0; i < kRasterOffsets.size(); ++i) {
    assert(RasterImportIndex(kRasterOffsets[i]) == i);
    assert(kRasterSetters[i] != 0);
  }
  for (uint32_t offset = 0; offset < 400; ++offset) {
    RasterShadow shadow;
    const auto before = shadow;
    if (!RasterImportIndex(offset)) {
      assert(!PublishRasterShadow(shadow, offset, UINT32_MAX));
      assert(shadow == before);
    }
  }
  RasterImport raw;
  auto decoded = DecodeRasterImport(raw);
  assert(!decoded.zEnable && decoded.zWriteEnable &&
         decoded.zFunc == F::LESS_EQUAL);
  assert(decoded.stencilMask == 255 && decoded.stencilWriteMask == 255);
  assert(decoded.colorWriteEnable == 0 && decoded.stencilFunc == F::NEVER);
  raw.words = {1, 7, 0, 37, 6, 1, 1, 0, 3, 6, 5, 0x1234, 0x1234, 0x100, 0xf7};
  decoded = DecodeRasterImport(raw);
  assert(decoded.zEnable && !decoded.zWriteEnable &&
         decoded.zFunc == F::ALWAYS);
  assert(decoded.fillMode == plume::RenderFillMode::WIREFRAME);
  assert(decoded.cullMode == plume::RenderCullMode::BACK);
  assert(decoded.stencilEnable && decoded.stencilTwoSided);
  assert(decoded.stencilFail == S::KEEP &&
         decoded.stencilZFail == S::INCREMENT_AND_CLAMP);
  assert(decoded.stencilPass == S::INCREMENT_AND_WRAP &&
         decoded.stencilFunc == F::NOT_EQUAL);
  assert(decoded.stencilRef == 0x34 && decoded.stencilMask == 0x34 &&
         decoded.stencilWriteMask == 0);
  assert(decoded.colorWriteEnable == 7);
  assert(ImportRasterCompare(UINT32_MAX) == F::LESS_EQUAL);
  assert(ImportRasterStencil(UINT32_MAX) == S::KEEP);

  // The native API does not confuse valid NEVER / zero masks with import
  // sentinels, and applying it does not overwrite unrelated pipeline fields.
  struct Pipeline : RasterState {
    uint32_t shader = 123;
    bool multiview = true;
  } pipeline;
  RasterState native;
  native.zFunc = F::NEVER;
  native.zWriteEnable = false;
  native.stencilMask = native.stencilWriteMask = 0;
  bool dirty = false;
  ApplyRasterState(native, pipeline, dirty);
  assert(dirty && pipeline.zFunc == F::NEVER && !pipeline.zWriteEnable);
  assert(!pipeline.stencilMask && !pipeline.stencilWriteMask);
  assert(pipeline.shader == 123 && pipeline.multiview);
  dirty = false;
  ApplyRasterState(native, pipeline, dirty);
  assert(!dirty);
  dirty = true;
  ApplyRasterState(native, pipeline, dirty);
  assert(dirty);
  // Replay may bind another pipeline; applying live intent restores only its
  // raster fields and does not make that replay the source of future draws.
  pipeline.zEnable = false;
  ApplyRasterState(native, pipeline, dirty);
  assert(pipeline.zEnable && native.zEnable);

  std::mt19937 random(0x822870d8);
  constexpr uint64_t depth_dirty = (uint64_t(1) << 49) | 2048;
  // Independent bit-field reference for every publication, both engine gates,
  // and randomized neighbors/dirty bits. Covers all 32 input bits.
  for (uint32_t offset : kRasterOffsets) {
    for (uint32_t iteration = 0; iteration < 2000; ++iteration) {
      RasterShadow actual;
      actual.depth_control = random();
      actual.raster_control = random();
      actual.color_mask = random();
      actual.stencil_bytes = random();
      actual.depth_enable = random();
      actual.stencil_enable = random();
      actual.color_enable = random();
      actual.depth_gate = iteration & 1;
      actual.color_gate = (iteration >> 1) & 1;
      actual.dirty16 = (uint64_t(random()) << 32) | random();
      actual.dirty24 = (uint64_t(random()) << 32) | random();
      auto expected = actual;
      uint32_t v = random();
      if (iteration < 32)
        v = uint32_t(1) << iteration;
      auto bits = [&](uint32_t &word, uint32_t mask, unsigned shift) {
        word = (word & ~(mask << shift)) | ((v & mask) << shift);
      };
      switch (offset) {
      case 40:
        expected.depth_enable = v;
        if (!expected.depth_gate)
          v = 0;
        bits(expected.depth_control, 1, 1);
        expected.dirty16 |= depth_dirty;
        break;
      case 44:
        bits(expected.depth_control, 7, 4);
        expected.dirty16 |= depth_dirty;
        break;
      case 48:
        bits(expected.depth_control, 1, 2);
        expected.dirty16 |= 2048;
        break;
      case 52:
        bits(expected.raster_control, 255, 3);
        expected.dirty16 |= 64;
        break;
      case 56:
        bits(expected.raster_control, 7, 0);
        expected.dirty16 |= 64;
        break;
      case 108:
        expected.stencil_enable = v;
        if (!expected.depth_gate)
          v = 0;
        bits(expected.depth_control, 1, 0);
        expected.dirty16 |= depth_dirty;
        break;
      case 112:
        bits(expected.depth_control, 1, 7);
        expected.dirty16 |= depth_dirty;
        break;
      case 116:
        bits(expected.depth_control, 7, 11);
        expected.dirty16 |= depth_dirty;
        break;
      case 120:
        bits(expected.depth_control, 7, 17);
        expected.dirty16 |= depth_dirty;
        break;
      case 124:
        bits(expected.depth_control, 7, 14);
        expected.dirty16 |= 2048;
        break;
      case 128:
        bits(expected.depth_control, 7, 8);
        expected.dirty16 |= 2048;
        break;
      case 132:
        bits(expected.stencil_bytes, 255, 0);
        expected.dirty24 |= 512;
        break;
      case 136:
        bits(expected.stencil_bytes, 255, 8);
        expected.dirty24 |= 512;
        break;
      case 140:
        bits(expected.stencil_bytes, 255, 16);
        expected.dirty24 |= 512;
        break;
      case 212:
        expected.color_enable = v;
        if (!expected.color_gate)
          v = 0;
        bits(expected.color_mask, 15, 0);
        expected.dirty24 |= 262144;
        break;
      }
      // Publication receives the original value, even if the reference gated
      // it.
      uint32_t original = offset == 40    ? expected.depth_enable
                          : offset == 108 ? expected.stencil_enable
                          : offset == 212 ? expected.color_enable
                                          : v;
      assert(PublishRasterShadow(actual, offset, original));
      assert(actual == expected);
    }
  }
  std::cout
      << "Native raster intent and 30000 compatibility publications passed\n";
}
