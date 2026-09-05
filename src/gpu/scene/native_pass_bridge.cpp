/**
 * @file    native_pass_bridge.cpp
 * @brief   Replace complete engine pass push/pop execution with native scopes.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/scene/native_pass.h"
#include "gpu/scene/native_pass_bridge.h"
#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/device.h"
#include "gpu/format.h"
#include "gpu/frame_stats.h"
#include "gpu/host_resource_heap.h"
#include "gpu/pass_bindings.h"
#include "gpu/resource_bridge.h"
#include <algorithm>
#include <array>
#include <stdexcept>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc/context.h>

extern "C" void __imp__bdSurfaceSetMSAA(PPCContext &, uint8_t *);
extern "C" void __imp__bdDestroySurface(PPCContext &, uint8_t *);
REXCVAR_DECLARE(bool, bd_native_passes);

namespace bd::gpu::scene {
namespace {
constexpr uint32_t kStack = (uint32_t(-32034) << 16) - 23232;
constexpr uint32_t kDevice = (uint32_t(-32133) << 16) - 31532;
constexpr std::array<uint32_t, 5> kSavedOffsets{8, 40, 72, 104, 136};
struct EnginePassShadow {
  std::array<uint32_t, 5> references{};
};
// The engine's pass traversal is render-thread serialized. Resource registry
// lookups and releases are never made under the video mutex.
thread_local NativePassStack<GuestTexture *> passes;
thread_local std::vector<EnginePassShadow> shadows;
struct Stats {
  uint64_t pushes = 0, pops = 0, depth_only = 0, null_passes = 0;
  uint64_t compatibility = 0, refused = 0, overflow = 0, empty = 0;
  uint64_t checked = 0, wrong = 0;
  std::array<uint64_t, 5> refusal_reasons{};
  uint32_t peak = 0, frame = 0;
};
thread_local Stats stats;
void Report() {
  const auto frame = FrameStatFrameCount();
  if (frame - stats.frame < 300)
    return;
  BD_INFO("[native-passes] pushes {} pops {} depth-only {} null {} peak {}; "
          "compatibility {} refused {} overflow {} empty {}; shadows checked {} "
          "wrong {}; refusals memory {} nesting {} inputs {} mrt {} live-target {}; "
          "engine traversal/allocation/resolve adapters remain",
          stats.pushes, stats.pops, stats.depth_only, stats.null_passes, stats.peak,
          stats.compatibility, stats.refused, stats.overflow, stats.empty,
          stats.checked, stats.wrong, stats.refusal_reasons[0],
          stats.refusal_reasons[1], stats.refusal_reasons[2],
          stats.refusal_reasons[3], stats.refusal_reasons[4]);
  stats.frame = frame;
}
bool Range(uint64_t address, uint64_t bytes) {
  if (!address || !bytes || address + bytes - 1 > UINT32_MAX ||
      !bd::mem::try_at<uint8_t>(uint32_t(address)))
    return false;
  for (uint64_t page = (address & ~uint64_t(4095)) + 4096;
       page < address + bytes; page += 4096)
    if (!bd::mem::try_at<uint8_t>(uint32_t(page)))
      return false;
  return true;
}
D3DDevice *Device() {
  if (!Range(kStack, 328) || !Range(kDevice, 4))
    return nullptr;
  const auto address = bd::mem::load<uint32_t>(kDevice);
  return Range(address, sizeof(D3DDevice)) ? bd::mem::at<D3DDevice>(address) : nullptr;
}
// Match the existing target adapter's accepted types. Unknown sentinel handles
// may survive only in getter shadows, never as native attachment identities.
GuestTexture *Attachment(uint32_t address, bool depth) {
  ResourceType type;
  if (!HostResourceHeap::GetType(address, &type) ||
      (type != ResourceType::RenderTarget && type != ResourceType::DepthStencil &&
       type != ResourceType::Texture && type != ResourceType::VolumeTexture))
    return nullptr;
  auto *surface = HostResourceHeap::FromGuest<GuestTexture>(address);
  if (depth && surface && surface->type != ResourceType::DepthStencil &&
      !(surface->type == ResourceType::Texture && IsDepthFormat(surface->format)))
    return nullptr;
  return surface;
}
EnginePassShadow ReadDeviceShadow(const D3DDevice &device) {
  EnginePassShadow result;
  for (size_t i = 0; i < 4; ++i)
    result.references[i] = device.renderTargetShadow[i];
  result.references[4] = device.depthStencilShadow;
  return result;
}
void PublishTargets(D3DDevice &device, PassAttachments<GuestTexture *> targets,
                    const EnginePassShadow &shadow) {
  for (size_t i = 0; i < 4; ++i)
    device.renderTargetShadow[i] = shadow.references[i];
  device.depthStencilShadow = shadow.references[4];
  BindColorAttachment(targets.color);
  BindDepthAttachment(targets.depth);
  Video::SetDefaultViewport(&device, targets.depth ? targets.depth : targets.color);
}
void CheckShadow(uint32_t depth) {
  bool same = passes.Depth() == shadows.size();
  for (size_t i = 0; i < shadows.size(); ++i)
    for (size_t j = 0; j < kSavedOffsets.size(); ++j)
      same &= bd::mem::load<uint32_t>(kStack + kSavedOffsets[j] + uint32_t(i) * 4) ==
              shadows[i].references[j];
  same &= depth == passes.Depth();
  ++stats.checked;
  if (!same) {
    ++stats.wrong;
    // A foreign stack writer cannot silently replace native ownership, nor can
    // we run the original pop and subsequently release these references twice.
    throw std::runtime_error("Native pass stack getter shadow changed unexpectedly");
  }
}
bool Push(GuestTexture *color, GuestTexture *depth_image, uint32_t &result) {
  const auto refuse = [](size_t reason) {
    ++stats.refusal_reasons[reason];
    return false;
  };
  auto *device = Device();
  if (!device)
    return refuse(0);
  const auto depth = bd::mem::load<uint32_t>(kStack + 4);
  if (depth >= 7 && depth <= INT32_MAX) {
    ++stats.overflow; // exact engine no-op, before resource effects
    return true;
  }
  if (!REXCVAR_GET(bd_native_passes) && !passes.Depth())
    return false;
  if (depth != passes.Depth())
    return refuse(1); // an already active compatibility scope must unwind first
  const auto previous = ReadDeviceShadow(*device);
  for (size_t i = 1; i < 4; ++i)
    if (previous.references[i])
      return refuse(3); // native multiple-colour-attachment rendering remains work
  PassAttachments<GuestTexture *> saved{
      Attachment(previous.references[0], false), Attachment(previous.references[4], true)};
  // Other render entry points can change targets between nested scopes. Save
  // the live host state, checking the adapter; never reuse a preceding scope.
  if (saved.color != state().render_target || saved.depth != state().depth_stencil)
    return refuse(4);
  CheckShadow(depth);
  if (!depth)
    passes.SetRootContent({bd::mem::load<float>(kStack + 168),
                           bd::mem::load<float>(kStack + 172)});
  shadows.push_back(previous);
  passes.Enter(saved, color ? std::optional(PassExtent{float(color->width), float(color->height)})
                            : std::nullopt);
  for (size_t i = 0; i < previous.references.size(); ++i) {
    RetainResourceAdapter(previous.references[i]);
    bd::mem::store<uint32_t>(kStack + kSavedOffsets[i] + depth * 4,
                            previous.references[i]);
  }
  EnginePassShadow selected;
  selected.references[0] = color ? color->selfVa : 0;
  selected.references[4] = depth_image ? depth_image->selfVa : 0;
  PublishTargets(*device, {color, depth_image}, selected);
  const auto next_depth = uint32_t(passes.Depth());
  bd::mem::store<uint32_t>(kStack + 4, next_depth);
  bd::mem::store<float>(kStack + 168 + next_depth * 8, passes.Content().width);
  bd::mem::store<float>(kStack + 172 + next_depth * 8, passes.Content().height);
  // GetDesc leaves the colour argument in r3; the null-colour path leaves the device.
  result = color ? selected.references[0] : bd::mem::load<uint32_t>(kDevice);
  ++stats.pushes;
  stats.depth_only += !color && depth_image;
  stats.null_passes += !color && !depth_image;
  stats.peak = std::max(stats.peak, next_depth);
  return true;
}
bool Pop(uint32_t &result) {
  auto *device = Device();
  if (!device)
    return false;
  const auto depth = bd::mem::load<uint32_t>(kStack + 4);
  if (!depth && !passes.Depth()) {
    ++stats.empty;
    return true; // preserves r3 on the engine's empty-stack no-op
  }
  if (!passes.Depth() || depth > passes.Depth())
    return false; // compatibility scopes above native scopes unwind separately
  CheckShadow(depth);
  const auto saved = *passes.Previous();
  const auto previous = shadows.back();
  const auto next_depth = depth - 1;
  bd::mem::store<uint32_t>(kStack + 4, next_depth);
  PublishTargets(*device, saved.attachments, previous);
  for (size_t i = 0; i < previous.references.size(); ++i) {
    const auto address = previous.references[i];
    const auto remaining = address ? ReleaseResourceAdapter(address) : 0u;
    bd::mem::store<uint32_t>(kStack + kSavedOffsets[i] + next_depth * 4, 0u);
    if (i == 4)
      result = remaining;
  }
  passes.Leave();
  shadows.pop_back();
  ++stats.pops;
  return true;
}
} // namespace

std::size_t NativePassDepth() { return passes.Depth(); }
bool CanEnterNativePass() {
  auto *device = Device();
  if (!device || (!REXCVAR_GET(bd_native_passes) && !passes.Depth()))
    return false;
  const auto depth = bd::mem::load<uint32_t>(kStack + 4);
  if (depth >= 7 || depth != passes.Depth())
    return false;
  const auto previous = ReadDeviceShadow(*device);
  for (size_t i = 1; i < 4; ++i)
    if (previous.references[i])
      return false;
  if (Attachment(previous.references[0], false) != state().render_target ||
      Attachment(previous.references[4], true) != state().depth_stencil)
    return false;
  CheckShadow(depth);
  return true;
}
bool EnterNativePass(GuestTexture *color, GuestTexture *depth, uint32_t &result) {
  if (!CanEnterNativePass())
    return false;
  const bool entered = Push(color, depth, result);
  Report();
  return entered;
}
bool LeaveNativePass(uint32_t &result) {
  if (!passes.Depth())
    return false;
  const bool left = Pop(result);
  Report();
  return left;
}
} // namespace bd::gpu::scene

REX_HOOK_RAW(bdSurfaceSetMSAA) {
  using namespace bd::gpu::scene;
  auto *color = Attachment(ctx.r3.u32, false);
  auto *depth = Attachment(ctx.r7.u32, true);
  uint32_t result = ctx.r3.u32;
  // Preserve the adapter's overflow no-op even for otherwise invalid inputs.
  const auto *device = Device();
  const auto level = device ? bd::mem::load<uint32_t>(kStack + 4) : 0u;
  const bool overflow = device && level >= 7 && level <= INT32_MAX;
  const bool inputs = (!ctx.r3.u32 || color) && (!ctx.r7.u32 || depth);
  if (!overflow && !inputs)
    ++stats.refusal_reasons[2];
  if ((!overflow && !inputs) || !Push(color, depth, result)) {
    ++stats.compatibility;
    stats.refused += REXCVAR_GET(bd_native_passes);
    __imp__bdSurfaceSetMSAA(ctx, base);
  } else if (!overflow) {
    ctx.r3.u64 = result;
  }
  Report();
}
REX_HOOK_RAW(bdDestroySurface) {
  using namespace bd::gpu::scene;
  uint32_t result = ctx.r3.u32;
  const bool was_empty = !passes.Depth();
  if (!Pop(result)) {
    ++stats.compatibility;
    stats.refused += REXCVAR_GET(bd_native_passes);
    __imp__bdDestroySurface(ctx, base);
  } else if (!was_empty) {
    ctx.r3.u64 = result;
  }
  Report();
}
