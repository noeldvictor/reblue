/**
 * @file    native_alpha_bridge.cpp
 * @brief   Host alpha production with explicit temporary engine getter shadows.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/scene/native_alpha_bridge.h"
#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/frame_stats.h"
#include "gpu/scene/alpha_import.h"
#include <cstring>
#include <mutex>
#include <rex/cvar.h>
#include <rex/ppc/context.h>
#include <stdexcept>

extern "C" void __imp__bdSetRenderState(PPCContext &, uint8_t *);
REXCVAR_DECLARE(bool, bd_native_alpha);
REXCVAR_DECLARE(bool, bd_native_alpha_verify);

namespace bd::gpu::scene {
namespace {
constexpr uint32_t kCache = 0x82DBE1A8;
constexpr uint32_t kDevice = (uint32_t(-32133) << 16) - 31532;
constexpr uint32_t kScale = 0x8200167C;
constexpr uint32_t kDeviceBytes = 12188;
std::mutex alpha_mutex;
AlphaImport imported;
AlphaState intent;
bool initialized = false;
struct Stats {
  uint64_t produced = 0, unchanged = 0, compatibility = 0, refused = 0;
  uint64_t checked = 0, wrong = 0, draws = 0, draw_checks = 0, drift = 0;
  uint64_t imports = 0, legacy_draws = 0, enabled = 0, coverage_requested = 0;
  std::array<uint64_t, 4> setters{};
  std::array<uint64_t, 8> compares{};
  uint32_t frame = 0;
} stats;
void Report() {
  const auto frame = FrameStatFrameCount();
  if (frame - stats.frame < 300)
    return;
  BD_INFO(
      "[native-alpha] produced {} unchanged {} compatibility {} refused {}; "
      "checked {} wrong {}; native draws {} checked {} drift {} legacy draws "
      "{}; "
      "imports {}; setters enable {} ref {} func {} coverage {}; "
      "enabled draw intent {} coverage requested {}; compares GE {} never {} "
      "less {} equal {} LE {} greater {} NE {} always {}; "
      "engine cache/getter, material and replay adapters remain",
      stats.produced, stats.unchanged, stats.compatibility, stats.refused,
      stats.checked, stats.wrong, stats.draws, stats.draw_checks, stats.drift,
      stats.legacy_draws, stats.imports, stats.setters[0], stats.setters[1],
      stats.setters[2], stats.setters[3], stats.enabled,
      stats.coverage_requested, stats.compares[0], stats.compares[1],
      stats.compares[2], stats.compares[3], stats.compares[4],
      stats.compares[5], stats.compares[6], stats.compares[7]);
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
AlphaImport ReadImport() {
  if (!Range(kCache, 340))
    throw std::runtime_error("Alpha engine cache is unavailable");
  AlphaImport result;
  for (size_t i = 0; i < kAlphaOffsets.size(); ++i)
    result.words[i] = bd::mem::load<uint32_t>(kCache + kAlphaOffsets[i]);
  return result;
}
void Bootstrap() {
  if (initialized)
    return;
  imported = ReadImport();
  intent = DecodeAlphaImport(imported);
  initialized = true;
  ++stats.imports;
}
AlphaShadow ReadShadow(uint32_t device) {
  return {bd::mem::load<uint32_t>(device + 10428),
          bd::mem::load<uint32_t>(device + 10372),
          bd::mem::load<uint64_t>(device + 16),
          bd::mem::load<uint64_t>(device + 24)};
}
bool NativeUpdate(PPCContext &ctx, uint8_t *base, size_t index) {
  const uint32_t offset = ctx.r3.u32, value = ctx.r4.u32;
  if (!Range(kCache, 340) || !Range(kDevice, 4) || !Range(kScale, 4) ||
      bd::mem::load<uint32_t>(kScale) !=
          std::bit_cast<uint32_t>(kAlphaImportScale))
    return false;
  const auto device = bd::mem::load<uint32_t>(kDevice);
  if (!Range(device, kDeviceBytes) ||
      bd::mem::load<uint32_t>(device + 56 + offset) != kAlphaSetters[index])
    return false;
  Bootstrap();
  auto shadow = ReadShadow(device);
  const bool changed = bd::mem::load<uint32_t>(kCache + offset) != value;
  if (changed && !PublishAlphaShadow(shadow, offset, value))
    return false;
  if (REXCVAR_GET(bd_native_alpha_verify)) {
    std::array<uint8_t, kDeviceBytes> expected;
    std::memcpy(expected.data(), bd::mem::at<uint8_t>(device), expected.size());
    if (changed)
      WriteAlphaShadow(shadow, offset, [&](uint32_t at, auto v) {
        const auto be = std::byteswap(v);
        std::memcpy(expected.data() + at, &be, sizeof(be));
      });
    __imp__bdSetRenderState(ctx, base);
    ++stats.checked;
    if (std::memcmp(expected.data(), bd::mem::at<uint8_t>(device),
                    expected.size()) ||
        bd::mem::load<uint32_t>(kCache + offset) != value) {
      if (++stats.wrong <= 8)
        BD_WARN("[native-alpha] publication mismatch offset {} value {}",
                offset, value);
    }
  }
  if (changed)
    WriteAlphaShadow(shadow, offset, [&](uint32_t at, auto v) {
      bd::mem::store<decltype(v)>(device + at, v);
    });
  bd::mem::store<uint32_t>(kCache + offset, value);
  imported.words[index] = value;
  intent = DecodeAlphaImport(imported);
  ++stats.produced;
  ++stats.setters[index];
  stats.unchanged += !changed;
  return true;
}
} // namespace
void ResetAlphaImport() {
  std::lock_guard lock(alpha_mutex);
  initialized = false;
}
bool UpdateAlphaImport(PPCContext &ctx, uint8_t *base) {
  const auto index = AlphaImportIndex(ctx.r3.u32);
  if (!index)
    return false;
  if (REXCVAR_GET(bd_native_alpha)) {
    std::lock_guard lock(alpha_mutex);
    if (NativeUpdate(ctx, base, *index)) {
      Report();
      return true;
    }
    if (++stats.refused <= 8)
      BD_WARN("[native-alpha] unsupported device/setter/scale before effects, "
              "offset {}",
              ctx.r3.u32);
  }
  __imp__bdSetRenderState(ctx, base);
  std::lock_guard lock(alpha_mutex);
  ++stats.compatibility;
  initialized = false;
  Report();
  return true;
}
AlphaState CurrentAlphaIntent() {
  std::lock_guard lock(alpha_mutex);
  if (!REXCVAR_GET(bd_native_alpha)) {
    ++stats.legacy_draws;
    initialized = false;
    Report();
    return DecodeAlphaImport(ReadImport());
  }
  Bootstrap();
  ++stats.draws;
  if (intent.enabled) {
    ++stats.enabled;
    ++stats.compares[uint32_t(intent.compare)];
  }
  stats.coverage_requested += intent.alpha_to_coverage;
  if (REXCVAR_GET(bd_native_alpha_verify)) {
    ++stats.draw_checks;
    if (ReadImport() != imported && ++stats.drift <= 8)
      BD_WARN("[native-alpha] untracked engine cache writer; native intent not "
              "silently repaired");
  }
  Report();
  return intent;
}
} // namespace bd::gpu::scene
