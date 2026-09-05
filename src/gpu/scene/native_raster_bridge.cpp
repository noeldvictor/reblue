/**
 * @file    native_raster_bridge.cpp
 * @brief   Native raster production with explicit engine getter shadows.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/scene/native_raster_bridge.h"
#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/frame_stats.h"
#include "gpu/scene/raster_import.h"
#include <cstring>
#include <mutex>
#include <rex/cvar.h>
#include <rex/ppc/context.h>
#include <stdexcept>

extern "C" void __imp__bdSetRenderState(PPCContext &, uint8_t *);
REXCVAR_DECLARE(bool, bd_native_raster);
REXCVAR_DECLARE(bool, bd_native_raster_verify);

namespace bd::gpu::scene {
namespace {
constexpr uint32_t kCache = 0x82DBE1A8;
constexpr uint32_t kDevice = (uint32_t(-32133) << 16) - 31532;
constexpr uint32_t kDeviceBytes = 12188;
std::mutex raster_mutex;
RasterImport imported;
RasterState intent;
bool initialized = false;
struct Stats {
  uint64_t produced = 0, unchanged = 0, compatibility = 0, refused = 0;
  uint64_t checked = 0, wrong = 0, draws = 0, draw_checks = 0, drift = 0;
  uint64_t imports = 0, legacy_draws = 0;
  std::array<uint64_t, 15> setters{};
  std::array<uint64_t, 97> other_states{};
  uint32_t frame = 0;
} stats;
void Report() {
  const auto frame = FrameStatFrameCount();
  if (frame - stats.frame < 300)
    return;
  BD_INFO(
      "[native-raster] produced {} unchanged {} compatibility {} refused {}; "
      "checked {} wrong {}; native draws {} checked {} drift {} legacy draws "
      "{}; "
      "imports {}; coverage depth {} {} {} fill {} cull {} stencil {} {} {} {} "
      "{} {} {} {} {} color {}; "
      "blend/other state and engine cache adapters remain",
      stats.produced, stats.unchanged, stats.compatibility, stats.refused,
      stats.checked, stats.wrong, stats.draws, stats.draw_checks, stats.drift,
      stats.legacy_draws, stats.imports, stats.setters[0], stats.setters[1],
      stats.setters[2], stats.setters[3], stats.setters[4], stats.setters[5],
      stats.setters[6], stats.setters[7], stats.setters[8], stats.setters[9],
      stats.setters[10], stats.setters[11], stats.setters[12],
      stats.setters[13], stats.setters[14]);
  stats.frame = frame;
}
bool Range(uint64_t address, uint64_t bytes) {
  if (!address || !bytes || address > UINT32_MAX ||
      address + bytes - 1 > UINT32_MAX ||
      !bd::mem::try_at<uint8_t>(uint32_t(address)))
    return false;
  for (uint64_t page = (address & ~uint64_t(4095)) + 4096;
       page < address + bytes; page += 4096)
    if (!bd::mem::try_at<uint8_t>(uint32_t(page)))
      return false;
  return true;
}
RasterImport ReadImport() {
  if (!Range(kCache, 216))
    throw std::runtime_error("Raster engine cache is unavailable");
  RasterImport result;
  for (size_t i = 0; i < kRasterOffsets.size(); ++i)
    result.words[i] = bd::mem::load<uint32_t>(kCache + kRasterOffsets[i]);
  return result;
}
void Bootstrap() {
  if (initialized)
    return;
  imported = ReadImport();
  intent = DecodeRasterImport(imported);
  initialized = true;
  ++stats.imports;
}
RasterShadow ReadShadow(uint32_t device) {
  RasterShadow s;
  s.depth_control = bd::mem::load<uint32_t>(device + 10420);
  s.raster_control = bd::mem::load<uint32_t>(device + 10440);
  s.color_mask = bd::mem::load<uint32_t>(device + 10332);
  s.stencil_bytes = bd::mem::load<uint32_t>(device + 10368);
  s.depth_enable = bd::mem::load<uint32_t>(device + 11604);
  s.stencil_enable = bd::mem::load<uint32_t>(device + 11608);
  s.color_enable = bd::mem::load<uint32_t>(device + 11588);
  s.depth_gate = bd::mem::load<uint32_t>(device + 12184);
  s.color_gate = bd::mem::load<uint32_t>(device + 12168);
  s.dirty16 = bd::mem::load<uint64_t>(device + 16);
  s.dirty24 = bd::mem::load<uint64_t>(device + 24);
  return s;
}
// Only the exact setter's locations are written. A disabled hardware gate
// still preserves engine intent in the named native state and getter shadow.
template <class Store>
void WriteShadow(const RasterShadow &s, uint32_t offset, Store store) {
  if (offset == 52 || offset == 56)
    store(10440, s.raster_control);
  else if (offset == 132 || offset == 136 || offset == 140)
    store(10368, s.stencil_bytes);
  else if (offset == 212)
    store(10332, s.color_mask);
  else
    store(10420, s.depth_control);
  if (offset == 40)
    store(11604, s.depth_enable);
  if (offset == 108)
    store(11608, s.stencil_enable);
  if (offset == 212)
    store(11588, s.color_enable);
  if (offset == 132 || offset == 136 || offset == 140 || offset == 212)
    store(24, s.dirty24);
  else
    store(16, s.dirty16);
}
bool NativeUpdate(PPCContext &ctx, uint8_t *base, size_t index) {
  const uint32_t offset = ctx.r3.u32, value = ctx.r4.u32;
  if (!Range(kCache, 216) || !Range(kDevice, 4))
    return false;
  const auto device = bd::mem::load<uint32_t>(kDevice);
  if (!Range(device, kDeviceBytes) ||
      bd::mem::load<uint32_t>(device + 56 + offset) != kRasterSetters[index])
    return false;
  Bootstrap();
  auto shadow = ReadShadow(device);
  const bool changed = bd::mem::load<uint32_t>(kCache + offset) != value;
  if (changed && !PublishRasterShadow(shadow, offset, value))
    return false;
  if (REXCVAR_GET(bd_native_raster_verify)) {
    std::array<uint8_t, kDeviceBytes> expected;
    std::memcpy(expected.data(), bd::mem::at<uint8_t>(device), expected.size());
    if (changed)
      WriteShadow(shadow, offset, [&](uint32_t at, auto v) {
        const auto be = std::byteswap(v);
        std::memcpy(expected.data() + at, &be, sizeof(be));
      });
    __imp__bdSetRenderState(ctx, base);
    ++stats.checked;
    if (std::memcmp(expected.data(), bd::mem::at<uint8_t>(device),
                    expected.size()) ||
        bd::mem::load<uint32_t>(kCache + offset) != value) {
      if (++stats.wrong <= 8)
        BD_WARN("[native-raster] publication mismatch offset {} value {}",
                offset, value);
    }
  }
  if (changed)
    WriteShadow(shadow, offset, [&](uint32_t at, auto v) {
      bd::mem::store<decltype(v)>(device + at, v);
    });
  bd::mem::store<uint32_t>(kCache + offset, value);
  imported.words[index] = value;
  intent = DecodeRasterImport(imported);
  ++stats.produced;
  ++stats.setters[index];
  stats.unchanged += !changed;
  return true;
}
} // namespace

void ResetRasterImport() {
  std::lock_guard lock(raster_mutex);
  initialized = false;
}
void UpdateRasterImport(PPCContext &ctx, uint8_t *base) {
  const uint32_t offset = ctx.r3.u32, value = ctx.r4.u32;
  const auto index = RasterImportIndex(offset);
  if (REXCVAR_GET(bd_native_raster) && index) {
    std::lock_guard lock(raster_mutex);
    if (NativeUpdate(ctx, base, *index)) {
      Report();
      return;
    }
    if (++stats.refused <= 8)
      BD_WARN(
          "[native-raster] unsupported device/setter before effects, offset {}",
          ctx.r3.u32);
  }
  __imp__bdSetRenderState(ctx, base);
  std::lock_guard lock(raster_mutex);
  ++stats.compatibility;
  if (!index && offset % 4 == 0 && offset / 4 < stats.other_states.size() &&
      ++stats.other_states[offset / 4] == 1)
    BD_INFO("[native-raster] remaining engine state offset {} first value {}",
            offset, value);
  // A replaced/disabled raster setter can still update intent. Other-state
  // compatibility calls do not force per-draw raster-cache imports.
  if (index)
    initialized = false;
  Report();
}
RasterState CurrentRasterIntent() {
  std::lock_guard lock(raster_mutex);
  if (!REXCVAR_GET(bd_native_raster)) {
    ++stats.legacy_draws;
    Report();
    return DecodeRasterImport(ReadImport());
  }
  Bootstrap();
  ++stats.draws;
  if (REXCVAR_GET(bd_native_raster_verify)) {
    ++stats.draw_checks;
    if (ReadImport() != imported && ++stats.drift <= 8)
      BD_WARN("[native-raster] untracked cache writer at draw; native state "
              "not silently repaired");
  }
  Report();
  return intent;
}
} // namespace bd::gpu::scene
