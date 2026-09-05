/**
 * @file    native_scene_texture_bridge.cpp
 * @brief   Host current/next selection and material scene-image publication.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/scene/native_scene_texture_bridge.h"
#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/device.h"
#include "gpu/frame.h"
#include "gpu/frame_stats.h"
#include "gpu/native_texture_mirror.h"
#include "gpu/scene/host_draw.h"
#include <mutex>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc/context.h>

extern "C" void __imp__bdGetCurrentRenderTarget(PPCContext &, uint8_t *);
extern "C" void __imp__bdGetNextRenderTarget(PPCContext &, uint8_t *);
extern "C" void __imp__sub_8221E618(PPCContext &, uint8_t *);
REXCVAR_DECLARE(bool, bd_native_scene_textures);
REXCVAR_DECLARE(bool, bd_native_scene_textures_verify);

namespace bd::gpu::scene {
namespace {
constexpr uint32_t kDevice = (uint32_t(-32133) << 16) - 31532;
struct Stats {
  uint64_t current = 0, next = 0, produced = 0, compatibility = 0, refused = 0;
  uint64_t checked = 0, wrong = 0, binding_checks = 0, binding_wrong = 0;
  uint64_t native = 0, dynamic = 0, null = 0;
  uint32_t frame = 0;
};
std::mutex stats_mutex;
Stats stats;
void Report() { // stats_mutex held, never across resource lookup/publication
  const uint32_t frame = FrameStatFrameCount();
  if (frame - stats.frame < 300)
    return;
  BD_INFO("[native-scene-textures] current {} next {} pairs {} compatibility {} "
          "refused {}; selections checked {} wrong {} bindings checked {} wrong {}; "
          "native {} dynamic {} null no-ops {}; scene table/resource adapters remain",
          stats.current, stats.next, stats.produced, stats.compatibility, stats.refused,
          stats.checked, stats.wrong, stats.binding_checks, stats.binding_wrong,
          stats.native, stats.dynamic, stats.null);
  stats.frame = frame;
}
std::optional<uint32_t> ReadWord(uint64_t address) {
  if (!address || address > UINT32_MAX - 3)
    return {};
  const auto *word = bd::mem::try_at<const be_u32>(uint32_t(address));
  return word ? std::optional(uint32_t(*word)) : std::nullopt;
}
SceneTextureInput CaptureInput(GuestTexture *texture, uint32_t address = 0) {
  return {CaptureNativeTexture(texture), texture, address};
}
bool SameInput(const SceneTextureInput &expected, GuestTexture *actual) {
  return expected.native.primary ? expected.native == CaptureNativeTexture(actual)
                                 : expected.bridge == actual;
}
void Select(SceneTextureRole role, PPCContext &ctx, uint8_t *base) {
  const auto original = role == SceneTextureRole::Current
      ? __imp__bdGetCurrentRenderTarget : __imp__bdGetNextRenderTarget;
  const bool enabled = REXCVAR_GET(bd_native_scene_textures);
  const auto selected = enabled ? ReadSceneTextureSelection(role, ReadWord) : std::nullopt;
  if (!selected) {
    original(ctx, base);
    std::lock_guard lock(stats_mutex);
    ++stats.compatibility;
    stats.refused += enabled;
    Report();
    return;
  }
  const bool verify = REXCVAR_GET(bd_native_scene_textures_verify);
  if (verify)
    original(ctx, base);
  std::lock_guard lock(stats_mutex);
  if (verify) {
    ++stats.checked;
    if (ctx.r3.u32 != *selected && ++stats.wrong <= 8)
      BD_WARN("[native-scene-textures] role {} actual {:08X} expected {:08X}",
              uint32_t(role), ctx.r3.u32, *selected);
  }
  ctx.r3.u64 = *selected;
  if (role == SceneTextureRole::Current)
    ++stats.current;
  else
    ++stats.next;
  Report();
}
} // namespace

std::optional<SceneTextureSelections> ReadNativeSceneTextureSources() {
  return ReadSceneTextureSources(ReadWord);
}

std::optional<SceneTextureInputs> PrepareNativeSceneTextures() {
  const auto selections = ReadNativeSceneTextureSources();
  if (!selections)
    return {};
  SceneTextureInputs result;
  for (size_t i = 0; i < result.size(); ++i) {
    const auto &selection = (*selections)[i];
    auto *texture = selection.image ? ResolveGuestTexture(selection.image) : nullptr;
    if (selection.image && !texture)
      return {}; // preflight both before any binding; original owns debug fallback
    result[i] = CaptureInput(texture, selection.image);
    result[i].selection = selection;
  }
  return result;
}

void PublishNativeSceneTextures(PPCContext &ctx, uint8_t *base) {
  const bool enabled = REXCVAR_GET(bd_native_scene_textures);
  const auto inputs = enabled ? PrepareNativeSceneTextures() : std::nullopt;
  const auto device = ReadWord(kDevice);
  if (!inputs || !device) {
    __imp__sub_8221E618(ctx, base);
    std::lock_guard lock(stats_mutex);
    ++stats.compatibility;
    stats.refused += enabled;
    Report();
    return;
  }
  const bool verify = REXCVAR_GET(bd_native_scene_textures_verify);
  bool wrong = false;
  if (verify) {
    SceneTextureInputs expected = *inputs;
    {
      auto &s = state();
      std::lock_guard lock(s.mutex);
      for (size_t i = 0; i < expected.size(); ++i)
        if (!expected[i].source_address)
          expected[i] = CaptureInput(s.textures[kSceneTextureSlots[i]]);
    }
    __imp__sub_8221E618(ctx, base);
    {
      auto &s = state();
      std::lock_guard lock(s.mutex);
      for (size_t i = 0; i < expected.size(); ++i)
        wrong |= !SameInput(expected[i], s.textures[kSceneTextureSlots[i]]);
    }
  }
  for (size_t i = 0; i < inputs->size(); ++i)
    if ((*inputs)[i].source_address) {
      Video::SetTexture(kSceneTextureSlots[i], (*inputs)[i].bridge);
      // Video first records the ordinary write. Claim this semantic role only
      // after the native producer has actually published its non-null input.
      NoteSceneTextureInput(SceneTextureRole(i), (*inputs)[i]);
    }
  // The original callback is void; its final SetTexture leaves the device in r3.
  ctx.r3.u64 = *device;
  std::lock_guard lock(stats_mutex);
  ++stats.produced;
  if (verify) {
    ++stats.binding_checks;
    if (wrong && ++stats.binding_wrong <= 8)
      BD_WARN("[native-scene-textures] current/next binding publication mismatch");
  }
  for (const auto &input : *inputs) {
    stats.null += !input.source_address;
    stats.native += bool(input.native.primary);
    stats.dynamic += input.bridge && !input.native.primary;
  }
  Report();
}
} // namespace bd::gpu::scene

REX_HOOK_RAW(bdGetCurrentRenderTarget) {
  bd::gpu::scene::Select(bd::gpu::scene::SceneTextureRole::Current, ctx, base);
}
REX_HOOK_RAW(bdGetNextRenderTarget) {
  bd::gpu::scene::Select(bd::gpu::scene::SceneTextureRole::Next, ctx, base);
}
REX_HOOK_RAW(sub_8221E618) {
  bd::gpu::scene::PublishNativeSceneTextures(ctx, base);
}
