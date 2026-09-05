/**
 * @file    native_dof_bridge.cpp
 * @brief   Replace complete DoF preparation and quad submission bodies.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/post_chain.h"
#include "gpu/post_parameters.h"
#include "gpu/scene/native_transform_bridge.h"
#include "gpu/host_resource_heap.h"
#include "gpu/resources.h"
#include "gpu/settings.h"
#include "gpu/frame_stats.h"
#include "core/memory_helpers.h"
#include "core/logging.h"
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc/context.h>
#include <rex/system/xthread.h>
#include <stdexcept>

REX_EXTERN(__imp__sub_82217108);
// Historical symbol: 0x82217DE8 is the DoF composite, not a stencil draw.
// Exact source binds depth/scene/five blur levels and draws a fullscreen quad.
REX_EXTERN(__imp__bdShadowStencilDrawIndexed);
REXCVAR_DECLARE(bool, bd_native_dof);
REXCVAR_DECLARE(bool, bd_native_dof_verify);

namespace bd::gpu {
namespace {
constexpr uint32_t kThread = (uint32_t(-32035) << 16) - 26664;
constexpr uint32_t kFocus = (uint32_t(-32034) << 16) - 31876;
constexpr uint32_t kDepth = (uint32_t(-32136) << 16) + 14888 + 16;
thread_local DofPreparation preparation;
struct Stats {
  uint64_t native = 0, consumed = 0, original_prepare = 0, original_draw = 0;
  uint64_t refused = 0, verified = 0, mismatched = 0, nonfinite = 0;
  uint32_t frame = 0;
};
thread_local Stats stats;
void Report(const DofParameters *parameters = nullptr) {
  const auto frame = FrameStatFrameCount();
  if (frame - stats.frame < 300)
    return;
  BD_INFO("[native-dof] prepared {} consumed {} active {}; original prepare {} draw {} "
          "refused {}; parameter checks {} wrong {} nonfinite {}; "
          "engine properties/source handles, outer scheduler and bloom adapters remain",
          stats.native, stats.consumed, preparation.Active(), stats.original_prepare,
          stats.original_draw, stats.refused, stats.verified, stats.mismatched, stats.nonfinite);
  if (parameters)
    BD_INFO("[native-dof] aperture {:.6g} blur {:.6g} range {:.6g} focus {:.9g}",
            parameters->aperture, parameters->blur_scale, parameters->authored_range,
            parameters->focus_depth);
  stats.frame = frame;
}
bool Words(uint64_t address, uint64_t bytes) {
  if (!address || (address & 3) || !bytes || address + bytes - 1 > UINT32_MAX ||
      !bd::mem::try_at<uint8_t>(uint32_t(address)))
    return false;
  for (uint64_t page = (address & ~uint64_t(4095)) + 4096;
       page < address + bytes; page += 4096)
    if (!bd::mem::try_at<uint8_t>(uint32_t(page)))
      return false;
  return true;
}
GuestTexture *Texture(uint32_t container) {
  if (!Words(container, 4))
    return nullptr;
  const auto address = bd::mem::load<uint32_t>(container);
  ResourceType type;
  if (!HostResourceHeap::GetType(address, &type) ||
      (type != ResourceType::Texture && type != ResourceType::RenderTarget &&
       type != ResourceType::DepthStencil))
    return nullptr;
  return HostResourceHeap::FromGuest<GuestTexture>(address);
}
bool Same(float a, float b) {
  return a == b || (std::isnan(a) && std::isnan(b)) ||
         (std::isfinite(a) && std::isfinite(b) &&
          std::abs(a - b) <= 2e-6f * std::max(1.0f, std::abs(b)));
}
bool ReadParameters(uint32_t owner, DofParameters &parameters,
                    uint32_t &bank, bool &local_focus) {
  const auto *transforms = scene::GetNativeRenderTransforms();
  if (!Words(owner, 3028) || !Words(kThread, 4) || !Words(kFocus, 28) ||
      !transforms || !rex::system::XThread::GetCurrentThread())
    return false;
  bank = rex::system::XThread::GetCurrentThreadId() ==
                 bd::mem::load<uint32_t>(kThread) ? 0 : 1;
  const auto scalar = [&](uint32_t offset) {
    return bd::mem::load<float>(owner + offset + bank * 4);
  };
  local_focus = bd::mem::load<uint32_t>(owner + 2980 + bank * 4) == 1;
  const auto point = (local_focus ? owner + 2988 : kFocus) + 4 + bank * 12;
  const std::array<float, 3> focus{bd::mem::load<float>(point),
      bd::mem::load<float>(point + 4), bd::mem::load<float>(point + 8)};
  parameters = MakeDofParameters(scalar(2944), scalar(2956), scalar(2968),
      Settings::Get().DOFStrength(), focus, transforms->inputs.view,
      transforms->inputs.projection);
  return true;
}
void Verify(uint32_t owner, const DofParameters &parameters) {
  const auto descriptor = bd::mem::load<uint32_t>(owner + 2912);
  if (!Words(descriptor, 16))
    throw std::runtime_error("DoF verification descriptor is unavailable");
  const uint64_t address = uint64_t(bd::mem::load<uint32_t>(descriptor + 12)) +
      uint64_t(bd::mem::load<uint32_t>(owner + 2920)) * 16;
  if (!Words(address, 16))
    throw std::runtime_error("DoF verification publication is unavailable");
  const std::array<float, 4> native{parameters.aperture, parameters.blur_scale,
                                 parameters.authored_range, parameters.focus_depth};
  ++stats.verified;
  for (uint32_t lane = 0; lane < 4; ++lane) {
    const auto original = bd::mem::load<float>(uint32_t(address) + lane * 4);
    if (!Same(native[lane], original)) {
      ++stats.mismatched;
      BD_ERROR("[native-dof] parameter lane {} native {:.9g} original {:.9g}",
               lane, native[lane], original);
      throw std::runtime_error("Native DoF parameter mismatch");
    }
  }
}
} // namespace

bool ReadDofProducerParameters(u32 owner, DofParameters &parameters) {
  uint32_t bank = 0;
  bool local_focus = false;
  return ReadParameters(owner, parameters, bank, local_focus);
}
void PublishDofProducerProperties(u32 owner) {
  const auto bank = rex::system::XThread::GetCurrentThreadId() ==
      bd::mem::load<uint32_t>(kThread) ? 0u : 1u;
  bd::mem::store<uint32_t>(owner + 2932 + bank * 4, 2);
  const auto divisor = owner + 3020 + bank * 4;
  const float resolution = bd::mem::load<float>(divisor);
  if (resolution < 2.0f || resolution > 16.0f)
    bd::mem::store<float>(divisor, std::clamp(resolution, 2.0f, 16.0f));
  if (bd::mem::load<uint32_t>(owner + 2980 + bank * 4) == 1)
    for (uint32_t offset = 4; offset <= 24; offset += 4)
      bd::mem::store<uint32_t>(kFocus + offset,
          bd::mem::load<uint32_t>(owner + 2988 + offset));
}

REX_HOOK_RAW(sub_82217108) {
  const auto owner = ctx.r3.u32, source = ctx.r4.u32;
  DofParameters parameters;
  uint32_t bank = 0;
  bool local_focus = false;
  const bool enabled = REXCVAR_GET(bd_native_dof);
  const bool inputs = enabled && ReadParameters(owner, parameters, bank, local_focus);
  if (preparation.Active())
    throw std::runtime_error("DoF preparation was not consumed before another producer");
  if (inputs && REXCVAR_GET(bd_native_dof_verify)) {
    ++stats.original_prepare;
    __imp__sub_82217108(ctx, base);
    Verify(owner, parameters);
    Report(&parameters);
    return;
  }
  if (inputs && HostPostPrepareDof(Texture(source), Texture(kDepth), parameters)) {
    // Preserve authored-property getter semantics, not intermediate blur
    // resources or a register publication. The original forces mode 2 and
    // clamps the resolution divisor before building its obsolete pyramid.
    PublishDofProducerProperties(owner);
    if (!preparation.Prepare(owner, source, FrameStatFrameCount()))
      throw std::runtime_error("Native DoF could not publish its preparation owner");
    ++stats.native;
    stats.nonfinite += !std::isfinite(parameters.focus_depth);
    Report(&parameters);
    return;
  }
  stats.refused += enabled;
  ++stats.original_prepare;
  __imp__sub_82217108(ctx, base);
  Report();
}

REX_HOOK_RAW(bdShadowStencilDrawIndexed) {
  if (preparation.Active()) {
    if (!preparation.Consume(ctx.r3.u32, ctx.r4.u32, FrameStatFrameCount()))
      throw std::runtime_error("Native DoF draw has a different preparation owner");
    // The host atlas is already produced. Its combined composite will read
    // the retained scene, so no intermediate target, binding loop, quad or
    // output resolve is needed at this entry.
    ++stats.consumed;
    ctx.r3.u64 = 1;
  } else {
    ++stats.original_draw;
    __imp__bdShadowStencilDrawIndexed(ctx, base);
  }
  Report();
}
} // namespace bd::gpu
