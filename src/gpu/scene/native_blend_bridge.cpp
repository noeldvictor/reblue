/**
 * @file    native_blend_bridge.cpp
 * @brief   Native blend production with explicit temporary engine shadows.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/scene/native_blend_bridge.h"
#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/frame_stats.h"
#include "gpu/scene/blend_import.h"
#include <bit>
#include <cstring>
#include <mutex>
#include <rex/cvar.h>
#include <rex/ppc/context.h>
#include <stdexcept>

extern "C" void __imp__bdSetRenderState(PPCContext &, uint8_t *);
REXCVAR_DECLARE(bool, bd_native_blend);
REXCVAR_DECLARE(bool, bd_native_blend_verify);

namespace bd::gpu::scene {
namespace {
constexpr uint32_t kCache = 0x82DBE1A8;
constexpr uint32_t kDevice = (uint32_t(-32133) << 16) - 31532;
constexpr uint32_t kDeviceBytes = 12188;
std::mutex blend_mutex;
uint32_t initialized_device = 0;
BlendShadow imported;
BlendState intent;
struct Stats {
  uint64_t produced = 0, unchanged = 0, compatibility = 0, refused = 0;
  uint64_t checked = 0, wrong = 0, draws = 0, draw_checks = 0;
  uint64_t update_checks = 0, drift = 0, imports = 0, legacy_draws = 0;
  uint64_t unmapped = 0;
  std::array<uint64_t, 8> setters{};
  uint32_t frame = 0;
} stats;
void Report() {
  const auto frame = FrameStatFrameCount();
  if (frame - stats.frame < 300)
    return;
  BD_INFO(
      "[native-blend] produced {} unchanged {} compatibility {} refused {}; "
      "checked {} wrong {}; native draws {} checked {} update checks {} "
      "drift {} legacy draws {}; imports {} unmapped {}; coverage "
      "enable {} separate {} RGB {} {} {} alpha {} {} {}; "
      "engine cache/getter, alpha-test, constant and material adapters remain",
      stats.produced, stats.unchanged, stats.compatibility, stats.refused,
      stats.checked, stats.wrong, stats.draws, stats.draw_checks,
      stats.update_checks, stats.drift, stats.legacy_draws, stats.imports,
      stats.unmapped, stats.setters[0], stats.setters[1], stats.setters[2],
      stats.setters[3], stats.setters[4], stats.setters[5], stats.setters[6],
      stats.setters[7]);
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
BlendShadow ReadShadow(uint32_t device) {
  BlendShadow s;
  s.requested = bd::mem::load<uint32_t>(device + 11576);
  s.flags = bd::mem::load<uint32_t>(device + 11580);
  for (size_t i = 0; i < s.effective.size(); ++i)
    s.effective[i] = bd::mem::load<uint32_t>(device + kBlendWordOffsets[i]);
  s.dirty16 = bd::mem::load<uint64_t>(device + 16);
  return s;
}
void DecodeIntent() {
  intent = DecodeBlendImport(imported.effective[0], imported.flags);
  if (!SupportedBlendWord(imported.effective[0]) && ++stats.unmapped <= 8)
    BD_WARN("[native-blend] unsupported factor/operation {:08X}; existing "
            "ZERO/ADD fallback, not qualified constant blending",
            imported.effective[0]);
}
void Bootstrap(uint32_t device) {
  if (initialized_device == device && device)
    return;
  if (!Range(device, kDeviceBytes))
    throw std::runtime_error("Blend engine device is unavailable");
  imported = ReadShadow(device);
  DecodeIntent();
  initialized_device = device;
  ++stats.imports;
}
void CheckTrackedShadow(uint32_t device) {
  const auto actual = ReadShadow(device);
  // Other SDK states legitimately modify lower flag bits and dirty marks.
  if ((actual.requested != imported.requested ||
       actual.effective != imported.effective ||
       ((actual.flags ^ imported.flags) & 0xc0000000u)) &&
      ++stats.drift <= 8)
    BD_WARN("[native-blend] untracked blend writer; native state not silently "
            "repaired");
}
bool NativeUpdate(PPCContext &ctx, uint8_t *base, size_t index) {
  const uint32_t offset = ctx.r3.u32, value = ctx.r4.u32;
  if (!Range(kCache, 96) || !Range(kDevice, 4))
    return false;
  const auto device = bd::mem::load<uint32_t>(kDevice);
  if (!Range(device, kDeviceBytes) ||
      bd::mem::load<uint32_t>(device + 56 + offset) != kBlendSetters[index])
    return false;
  Bootstrap(device);
  const bool verify = REXCVAR_GET(bd_native_blend_verify);
  if (verify) {
    ++stats.update_checks;
    CheckTrackedShadow(device);
  }
  auto before = imported;
  // Read only neighbouring engine-owned bits here; the actual blend inputs
  // are retained host production, not a refresh from mutable guest registers.
  before.flags = (before.flags & 0xc0000000u) |
                 (bd::mem::load<uint32_t>(device + 11580) & 0x3fffffffu);
  before.dirty16 = bd::mem::load<uint64_t>(device + 16);
  auto after = before;
  const bool changed = bd::mem::load<uint32_t>(kCache + offset) != value;
  if (changed && !PublishBlendShadow(after, offset, value))
    return false;
  if (verify) {
    std::array<uint8_t, kDeviceBytes> expected;
    std::memcpy(expected.data(), bd::mem::at<uint8_t>(device), expected.size());
    WriteBlendChanges(before, after, [&](uint32_t at, auto v) {
      const auto be = std::byteswap(v);
      std::memcpy(expected.data() + at, &be, sizeof(be));
    });
    __imp__bdSetRenderState(ctx, base);
    ++stats.checked;
    if (std::memcmp(expected.data(), bd::mem::at<uint8_t>(device),
                    expected.size()) ||
        bd::mem::load<uint32_t>(kCache + offset) != value) {
      if (++stats.wrong <= 8)
        BD_WARN("[native-blend] publication mismatch offset {} value {}",
                offset, value);
    }
  }
  WriteBlendChanges(before, after, [&](uint32_t at, auto v) {
    bd::mem::store<decltype(v)>(device + at, v);
  });
  bd::mem::store<uint32_t>(kCache + offset, value);
  imported = after;
  if (changed)
    DecodeIntent();
  ++stats.produced;
  ++stats.setters[index];
  stats.unchanged += !changed;
  return true;
}
} // namespace
void ResetBlendImport() {
  std::lock_guard lock(blend_mutex);
  initialized_device = 0;
}
bool UpdateBlendImport(PPCContext &ctx, uint8_t *base) {
  const auto index = BlendImportIndex(ctx.r3.u32);
  if (!index)
    return false;
  if (REXCVAR_GET(bd_native_blend)) {
    std::lock_guard lock(blend_mutex);
    if (NativeUpdate(ctx, base, *index)) {
      Report();
      return true;
    }
    if (++stats.refused <= 8)
      BD_WARN(
          "[native-blend] unsupported device/setter before effects, offset {}",
          ctx.r3.u32);
  }
  __imp__bdSetRenderState(ctx, base);
  std::lock_guard lock(blend_mutex);
  ++stats.compatibility;
  initialized_device = 0;
  Report();
  return true;
}
BlendState CurrentBlendIntent(uint32_t device) {
  std::lock_guard lock(blend_mutex);
  if (!REXCVAR_GET(bd_native_blend)) {
    ++stats.legacy_draws;
    Bootstrap(device);
    const auto shadow = ReadShadow(device);
    initialized_device = 0; // Runtime re-enable must import this legacy result.
    Report();
    return DecodeBlendImport(shadow.effective[0], shadow.flags);
  }
  Bootstrap(device);
  ++stats.draws;
  if (REXCVAR_GET(bd_native_blend_verify)) {
    ++stats.draw_checks;
    CheckTrackedShadow(device);
  }
  Report();
  return intent;
}
} // namespace bd::gpu::scene
