/**
 * @file    native_lighting_bridge.cpp
 * @brief   Host lighting production; engine sources/staging remain adapters.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/scene/native_lighting_bridge.h"
#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/frame_stats.h"
#include "gpu/native_texture_mirror.h"
#include "gpu/resources.h"
#include "gpu/scene/lighting_shader_bridge.h"
#include "gpu/scene/node_tag.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc/context.h>
#include <unordered_set>

extern "C" void __imp__sub_82174CE8(PPCContext &, uint8_t *);
extern "C" void __imp__sub_821982C8(PPCContext &, uint8_t *);
REXCVAR_DECLARE(bool, bd_native_lighting);
REXCVAR_DECLARE(bool, bd_native_lighting_verify);

namespace bd::gpu::scene {
namespace {
constexpr uint32_t Address(int high, int low) {
  return (uint32_t(high) << 16) + uint32_t(low);
}
constexpr uint32_t kStaging = Address(-32034, -32552);
constexpr uint32_t kFlags = Address(-32133, -31628);
constexpr uint32_t kCamera = Address(-32034, -19936) + 54608;
constexpr uint32_t kScene = Address(-32137, 28048) + 152;
constexpr uint32_t kTextureList = Address(-32035, -26540);
constexpr uint32_t kSlot = Address(-32035, -26424);
constexpr uint32_t kKernelScale = Address(-32250, 3208);
std::mutex lighting_mutex;
std::optional<NativeLightingPass> current;
struct Stats {
  uint64_t produced = 0, compatibility = 0, refused = 0, resets = 0;
  uint64_t checked = 0, wrong = 0, draw_checks = 0, draw_wrong = 0;
  uint64_t replayed = 0, missing_extent = 0;
  uint32_t frame = 0;
} stats;
void Report() {
  const uint32_t frame = FrameStatFrameCount();
  if (frame - stats.frame < 300)
    return;
  BD_INFO("[native-lighting] produced {} compatibility {} refused {} external "
          "resets {}; checked {} wrong {}; shadow-input checks {} wrong {} "
          "replayed {}; missing extent {}; engine source/staging adapters remain",
          stats.produced, stats.compatibility, stats.refused, stats.resets,
          stats.checked, stats.wrong, stats.draw_checks, stats.draw_wrong,
          stats.replayed, stats.missing_extent);
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
LightingVector ReadVector(uint32_t address) {
  LightingVector result;
  for (uint32_t i = 0; i < 4; ++i)
    result[i] = bd::mem::load<float>(address + i * 4);
  return result;
}
std::optional<NativeLightingPass> Prepare(uint32_t source, bool enabled) {
  if (!Range(source, 68) || !Range(kStaging, 412) || !Range(kFlags, 20) ||
      !Range(kCamera, 16) || !Range(kScene, 20) ||
      !Range(kTextureList, 4) || !Range(kSlot, 4) || !Range(kKernelScale, 4))
    return {};
  // The original resets staging before reading the lighting descriptor. A
  // descriptor alias into that block has sequential semantics, not a snapshot.
  if (uint64_t(source) < uint64_t(kStaging) + 412 &&
      uint64_t(source) + 68 > kStaging)
    return {};
  NativeLightingInputs inputs;
  inputs.receivers_enabled = enabled;
  inputs.receiver_filter = bd::mem::load<uint8_t>(source + 13);
  inputs.secondary_shadow = bd::mem::load<uint8_t>(source + 16);
  inputs.shadow_mode = bd::mem::load<uint8_t>(source + 17);
  inputs.specular = bd::mem::load<uint8_t>(source + 18);
  inputs.light_count = bd::mem::load<int32_t>(source + 24);
  inputs.ambient = ReadVector(source + 28);
  inputs.camera_position = ReadVector(kCamera);
  inputs.color_scale = ReadVector(source + 44);
  inputs.shadow_bias = bd::mem::load<float>(source + 60);
  inputs.shadow_threshold = bd::mem::load<float>(source + 64);
  inputs.shadow_kernel_scale = bd::mem::load<float>(kKernelScale);
  for (uint32_t i = 0; i < 3; ++i)
    inputs.scene_origin[i] = bd::mem::load<float>(kScene + i * 4);
  inputs.scene_range = bd::mem::load<float>(kScene + 16);

  // sub_82174CE8 selects the current slot's texture wrapper in a linked list;
  // sub_82182180/1D0 then query level-zero dimensions twice apiece. Read the
  // host resource metadata directly, without SDK descriptor or guest calls.
  const uint32_t slot = bd::mem::load<uint32_t>(kSlot);
  uint32_t entry = bd::mem::load<uint32_t>(kTextureList);
  std::unordered_set<uint32_t> visited;
  while (entry) {
    if (visited.size() == 4096 || !visited.insert(entry).second || !Range(entry, 32))
      return {};
    if (bd::mem::load<uint32_t>(entry) == slot) {
      const uint32_t texture_va = bd::mem::load<uint32_t>(entry + 12);
      LightingExtent extent;
      if (texture_va) {
        const auto *texture = ResolveGuestTexture(texture_va);
        if (!texture || texture->width > INT32_MAX || texture->height > INT32_MAX)
          return {};
        extent = {std::max(1u, texture->width), std::max(1u, texture->height)};
      }
      inputs.sample_extent = extent;
      break;
    }
    entry = bd::mem::load<uint32_t>(entry + 28);
  }
  return ComposeNativeLighting(inputs);
}
bool SameWord(uint32_t actual, uint32_t expected, bool floating) {
  return actual == expected ||
      (floating && std::isnan(std::bit_cast<float>(actual)) &&
       std::isnan(std::bit_cast<float>(expected)));
}
std::array<uint32_t, 5> Flags(const NativeLightingPass &pass) {
  return {~0u, pass.inputs.shadow_mode, pass.inputs.secondary_shadow,
           pass.inputs.receiver_filter, pass.inputs.specular};
}
void Compare(const NativeLightingPass &pass, const LightingStagingImage &image) {
  ++stats.checked;
  bool wrong = false;
  for (uint32_t i = 0; i < image.size(); ++i) {
    const auto actual = bd::mem::load<uint32_t>(kStaging + i * 4);
    if (!SameWord(actual, image[i], i < 76 || (i >= 99 && i <= 101))) {
      if (!wrong && stats.wrong < 8)
        BD_WARN("[native-lighting] staging mismatch offset {} actual {:08X} "
                "expected {:08X}", i * 4, actual, image[i]);
      wrong = true;
    }
  }
  const auto flags = Flags(pass);
  for (uint32_t i = 0; i < flags.size(); ++i)
    wrong |= bd::mem::load<uint32_t>(kFlags + i * 4) != flags[i];
  stats.wrong += wrong;
}
} // namespace

void UpdateNativeLighting(PPCContext &ctx, uint8_t *base) {
  // Do not hold this mutex across original execution: the original calls the
  // independently callable reset hook, which invalidates the native record.
  const auto pass = REXCVAR_GET(bd_native_lighting)
      ? Prepare(ctx.r3.u32, ctx.r4.u32 != 0) : std::nullopt;
  if (!pass) {
    {
      std::lock_guard lock(lighting_mutex);
      current.reset();
      ++stats.compatibility;
      stats.refused += REXCVAR_GET(bd_native_lighting);
    }
    __imp__sub_82174CE8(ctx, base);
    std::lock_guard lock(lighting_mutex);
    Report();
    return;
  }
  const auto image = PackLightingStaging(*pass);
  const bool verify = REXCVAR_GET(bd_native_lighting_verify);
  if (verify)
    __imp__sub_82174CE8(ctx, base);
  std::lock_guard lock(lighting_mutex);
  if (!stats.produced)
    BD_INFO("[native-lighting] source kernel extent scale {}",
            pass->inputs.shadow_kernel_scale);
  if (verify)
    Compare(*pass, image);
  // Compatibility mirror only: the native record owns these values. Remaining
  // material mutation/flush consumers still require this big-endian image.
  for (uint32_t i = 0; i < image.size(); ++i)
    bd::mem::store<uint32_t>(kStaging + i * 4, image[i]);
  const auto flags = Flags(*pass);
  for (uint32_t i = 0; i < flags.size(); ++i)
    bd::mem::store<uint32_t>(kFlags + i * 4, flags[i]);
  current = *pass;
  ++stats.produced;
  stats.missing_extent += !pass->inputs.sample_extent;
  ctx.r3.u64 = 1;
  Report();
}
void InvalidateNativeLighting() {
  std::lock_guard lock(lighting_mutex);
  current.reset();
  ++stats.resets;
}
std::optional<LightingVector> NativeNodeShadowSampling(const NodeTag &tag) {
  if (!REXCVAR_GET(bd_native_lighting) || !tag.valid || tag.from_list ||
      !tag.ctx_va || bd::mem::try_load<uint32_t>(tag.ctx_va + 16, ~0u) != 0)
    return {};
  std::lock_guard lock(lighting_mutex);
  return current ? std::optional(current->shadow_sampling) : std::nullopt;
}
void CheckNativeShadowSampling(const LightingVector &expected,
                                const uint8_t *pixel_constants) {
  std::lock_guard lock(lighting_mutex);
  ++stats.draw_checks;
  if (std::memcmp(expected.data(), pixel_constants + 9 * 16, 16) != 0 &&
      ++stats.draw_wrong <= 8) {
    LightingVector actual;
    std::memcpy(actual.data(), pixel_constants + 9 * 16, 16);
    BD_WARN("[native-lighting] draw shadow input ({:.5g} {:.5g} {:.5g} {:.5g}) "
            "expected ({:.5g} {:.5g} {:.5g} {:.5g})", actual[0], actual[1],
            actual[2], actual[3], expected[0], expected[1], expected[2], expected[3]);
  }
}
void NoteNativeShadowSamplingReplay() {
  std::lock_guard lock(lighting_mutex);
  ++stats.replayed;
}
} // namespace bd::gpu::scene

REX_HOOK_RAW(sub_82174CE8) {
  bd::gpu::scene::UpdateNativeLighting(ctx, base);
}
REX_HOOK_RAW(sub_821982C8) {
  bd::gpu::scene::InvalidateNativeLighting();
  __imp__sub_821982C8(ctx, base);
}
