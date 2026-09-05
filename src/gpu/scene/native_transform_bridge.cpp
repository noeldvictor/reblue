/**
 * @file    native_transform_bridge.cpp
 * @brief   Replace engine transform execution; track remaining data/ABI
 * imports.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/scene/native_transform_bridge.h"
#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/constant_buffers.h"
#include "gpu/device.h"
#include "gpu/frame_stats.h"
#include "gpu/host_heap.h"
#include "gpu/scene/deferred_shader_bridge.h"
#include "gpu/scene/host_draw.h"
#include "gpu/scene/native_transform.h"
#include <algorithm>
#include <bit>
#include <rex/cvar.h>
#include <rex/ppc/context.h>
#include <stdexcept>

extern "C" void __imp__bdBuildViewMatrix(PPCContext &, uint8_t *);
REXCVAR_DECLARE(bool, bd_native_transforms);
REXCVAR_DECLARE(bool, bd_native_transforms_verify);

namespace bd::gpu::scene {
namespace {
constexpr uint32_t kEngine = (uint32_t(-32034) << 16) - 19936;
constexpr uint32_t kMatrices = kEngine + 54656;
constexpr uint32_t kCallback = kEngine + 54848;
constexpr uint32_t kSuppress = kEngine + 54852;
constexpr uint32_t kDefaultCallback = 0x820F7068;
constexpr uint32_t kDevice = (uint32_t(-32133) << 16) - 31532;
constexpr uint32_t kFirstVector = 20;

struct Stats {
  uint64_t produced = 0, world_only = 0, passes = 0, suppressed = 0;
  uint64_t compatibility = 0, custom = 0, refused = 0;
  uint64_t checked = 0, wrong = 0, cache_wrong = 0, constants_wrong = 0,
           mask_wrong = 0;
  uint32_t frame = 0;
  std::array<uint64_t, 4> refusal_reasons{};
  uint64_t nonfinite = 0;
};
thread_local Stats stats;
thread_local std::optional<RenderTransforms> native_transforms;
void Report() {
  const auto frame = FrameStatFrameCount();
  if (frame - stats.frame < 300)
    return;
  BD_INFO(
      "[native-transforms] produced {} world-only {} passes {} suppressed {}; "
      "compatibility {} custom {} refused {}; checked {} wrong {} "
      "(cache {} constants {} mask {}); refusal reasons memory {} callback {} "
      "alias {} device {}; native nonfinite updates {}; engine inputs/shader "
      "ABI remain",
      stats.produced, stats.world_only, stats.passes, stats.suppressed,
      stats.compatibility, stats.custom, stats.refused, stats.checked,
      stats.wrong, stats.cache_wrong, stats.constants_wrong, stats.mask_wrong,
      stats.refusal_reasons[0], stats.refusal_reasons[1],
      stats.refusal_reasons[2], stats.refusal_reasons[3], stats.nonfinite);
  stats.frame = frame;
}
bool Range(uint64_t address, uint64_t bytes) {
  if (!address || !bytes || address > UINT32_MAX || bytes > UINT32_MAX ||
      address + bytes - 1 > UINT32_MAX ||
      !bd::mem::try_at<uint8_t>(uint32_t(address)))
    return false;
  for (uint64_t page = (address & ~uint64_t(4095)) + 4096;
       page < address + bytes; page += 4096)
    if (!bd::mem::try_at<uint8_t>(uint32_t(page)))
      return false;
  return true;
}
RenderMatrix ReadMatrix(uint32_t address) {
  RenderMatrix matrix;
  for (uint32_t i = 0; i < 16; ++i)
    matrix[i] = bd::mem::load<float>(address + i * 4);
  return matrix;
}
void WriteMatrix(uint32_t address, const RenderMatrix &matrix) {
  for (uint32_t i = 0; i < 16; ++i)
    bd::mem::store<float>(address + i * 4, matrix[i]);
}
struct Publication {
  RenderTransforms transforms;
  std::array<float, 64> constants{};
  std::array<bool, 3> update{};
  uint32_t device = 0, vectors = 0;
  uint64_t dirty = 0;
  bool suppressed = false;
  uint32_t nonfinite_mask = 0;
};
std::optional<Publication> Refuse(size_t reason, uint32_t address = 0) {
  ++stats.refusal_reasons[reason];
  if (++stats.refused <= 8)
    BD_WARN("[native-transforms] refused reason {} address 0x{:08X} before "
            "native effects",
            reason, address);
  return {};
}
std::optional<Publication> Prepare(PPCContext &ctx,
                                   const float *view_override) {
  if (!Range(kMatrices, 197) || !Range(kDevice, 4))
    return Refuse(0);
  if (bd::mem::load<uint32_t>(kCallback) != kDefaultCallback) {
    ++stats.custom;
    return Refuse(1, bd::mem::load<uint32_t>(kCallback));
  }
  const std::array<uint32_t, 3> addresses{ctx.r3.u32, ctx.r4.u32, ctx.r5.u32};
  RenderTransformInputs inputs;
  const std::array<RenderMatrix *, 3> matrices{&inputs.world, &inputs.view,
                                               &inputs.projection};
  Publication result;
  for (uint32_t i = 0; i < 3; ++i) {
    const auto address = addresses[i] ? addresses[i] : kMatrices + i * 64;
    // Cross-aliases into another cache slot have sequential write semantics in
    // the old entry point. Refuse before any effects instead of changing them.
    if (!Range(address, 64))
      return Refuse(0, address);
    if (uint64_t(address) < uint64_t(kMatrices) + 192 &&
        uint64_t(address) + 64 > kMatrices && address != kMatrices + i * 64)
      return Refuse(2, address);
    *matrices[i] = ReadMatrix(address);
    result.update[i] = addresses[i] != 0;
  }
  if (view_override) {
    std::copy_n(view_override, 16, inputs.view.begin());
    result.update[1] = true;
  }
  result.transforms = ComposeRenderTransformValues(inputs);
  const std::array<const RenderMatrix *, 4> values{
      &inputs.world, &inputs.view, &inputs.projection,
      &result.transforms.view_projection};
  for (uint32_t m = 0; m < values.size(); ++m)
    for (float value : *values[m])
      if (!std::isfinite(value))
        result.nonfinite_mask |= 1u << m;
  result.device = bd::mem::load<uint32_t>(kDevice);
  if (!Range(result.device, 0x2720))
    return Refuse(3, result.device);
  result.vectors = result.update[1] || result.update[2] ? 16 : 4;
  result.suppressed = bd::mem::load<uint8_t>(kSuppress) == 1;
  result.dirty = bd::mem::load<uint64_t>(result.device);
  for (uint32_t i = 0; i < 64; ++i)
    result.constants[i] =
        bd::mem::load<float>(result.device + 0x700 + kFirstVector * 16 + i * 4);
  if (!result.suppressed) {
    const std::array<RenderMatrix, 4> packed{
        TransposeRenderMatrix(inputs.world), TransposeRenderMatrix(inputs.view),
        TransposeRenderMatrix(inputs.projection),
        TransposeRenderMatrix(result.transforms.view_projection)};
    for (uint32_t i = 0; i < result.vectors / 4; ++i)
      std::copy(packed[i].begin(), packed[i].end(),
                result.constants.begin() + i * 16);
    result.dirty |= *DeferredConstantMask(kFirstVector, result.vectors);
  }
  return result;
}
void Original(PPCContext &ctx, uint8_t *base, const float *view_override) {
  // Only diagnostic/compatibility execution needs a guest-addressed view copy.
  if (view_override) {
    static thread_local uint32_t scratch = 0;
    if (!scratch)
      scratch = HostHeap::Get().AllocGuest(64, 16);
    if (!scratch)
      throw std::runtime_error("No compatibility view scratch available");
    for (uint32_t i = 0; i < 16; ++i)
      bd::mem::store<float>(scratch + i * 4, view_override[i]);
    ctx.r4.u32 = scratch;
  }
  __imp__bdBuildViewMatrix(ctx, base);
}
void Compare(const Publication &publication) {
  ++stats.checked;
  bool cache_wrong = false, constants_wrong = false;
  const auto &inputs = publication.transforms.inputs;
  const std::array<const RenderMatrix *, 3> matrices{
      &inputs.world, &inputs.view, &inputs.projection};
  for (uint32_t m = 0; m < 3; ++m)
    for (uint32_t i = 0; i < 16; ++i)
      cache_wrong |= bd::mem::load<uint32_t>(kMatrices + m * 64 + i * 4) !=
                     std::bit_cast<uint32_t>((*matrices[m])[i]);
  for (uint32_t i = 0; i < 64; ++i) {
    const float expected = publication.constants[i];
    const float actual = bd::mem::load<float>(publication.device + 0x700 +
                                              kFirstVector * 16 + i * 4);
    if (std::bit_cast<uint32_t>(actual) != std::bit_cast<uint32_t>(expected) &&
        !(std::isnan(actual) && std::isnan(expected)) &&
        (!std::isfinite(actual) || !std::isfinite(expected) ||
         std::abs(actual - expected) > 1e-5f * (1 + std::abs(expected))))
      constants_wrong = true;
  }
  const bool mask_wrong =
      bd::mem::load<uint64_t>(publication.device) != publication.dirty;
  stats.cache_wrong += cache_wrong;
  stats.constants_wrong += constants_wrong;
  stats.mask_wrong += mask_wrong;
  if ((cache_wrong || constants_wrong || mask_wrong) && ++stats.wrong <= 8)
    BD_WARN(
        "[native-transforms] comparison mismatch cache={} constants={} mask={}",
        cache_wrong, constants_wrong, mask_wrong);
}
void Publish(const Publication &publication) {
  native_transforms = publication.transforms;
  if (publication.nonfinite_mask && ++stats.nonfinite <= 8)
    BD_WARN("[native-transforms] preserving nonfinite engine values on host; "
            "mask {} (world=1 view=2 projection=4 derived=8)",
            publication.nonfinite_mask);
  const auto &inputs = publication.transforms.inputs;
  const std::array<const RenderMatrix *, 3> matrices{
      &inputs.world, &inputs.view, &inputs.projection};
  for (uint32_t i = 0; i < 3; ++i)
    if (publication.update[i])
      WriteMatrix(kMatrices + i * 64, *matrices[i]);
  if (!publication.suppressed) {
    for (uint32_t i = 0; i < publication.vectors * 4; ++i)
      bd::mem::store<float>(publication.device + 0x700 + kFirstVector * 16 +
                                i * 4,
                            publication.constants[i]);
    bd::mem::store<uint64_t>(publication.device, publication.dirty);
    NoteGuestConstantWrite();
    Video::MarkVSConstantsDirty();
    NoteConstantsSet(true, kFirstVector, publication.vectors);
  } else {
    ++stats.suppressed;
  }
  ++stats.produced;
  if (publication.vectors == 4)
    ++stats.world_only;
  else
    ++stats.passes;
}
} // namespace

const RenderTransforms *GetNativeRenderTransforms() {
  return native_transforms ? &*native_transforms : nullptr;
}

void UpdateRenderTransforms(PPCContext &ctx, uint8_t *base,
                            const float *view_override) {
  if (REXCVAR_GET(bd_native_transforms)) {
    if (auto publication = Prepare(ctx, view_override)) {
      if (REXCVAR_GET(bd_native_transforms_verify)) {
        Original(ctx, base, view_override);
        Compare(*publication);
      }
      Publish(*publication);
      Report();
      return;
    }
  }
  ++stats.compatibility;
  native_transforms.reset();
  Original(ctx, base, view_override);
  Report();
}
} // namespace bd::gpu::scene
