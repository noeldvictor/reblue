/**
 * @file    host_frustum_bridge.cpp
 * @brief   Complete frustum plane producer and native current-view ownership.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/scene/host_frustum_bridge.h"
#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/frame_stats.h"
#include <bit>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc/context.h>
#include <stdexcept>

extern "C" void __imp__sub_821CCF48(PPCContext &, uint8_t *);
REXCVAR_DECLARE(bool, bd_host_frustum);
REXCVAR_DECLARE(bool, bd_host_frustum_verify);

namespace bd::gpu::scene {
namespace {
constexpr uint32_t kScenePlanes = (uint32_t(-32033) << 16) - 30608 + 64;
thread_local FrameFrustum scene_frustum;
struct Stats {
  uint64_t built = 0, scene = 0, compatibility = 0, refused = 0;
  uint64_t checked = 0, wrong = 0, reads = 0, missing = 0, shadows = 0;
  uint64_t exceptional = 0;
  uint32_t frame = 0;
};
thread_local Stats stats;
void Report() {
  const auto frame = FrameStatFrameCount();
  if (frame - stats.frame < 300)
    return;
  BD_INFO("[host-frustum] built {} scene {}; compatibility {} refused {}; "
          "checked {} wrong {}; native walks {} missing {} shadow checks {}; "
          "exceptional inputs {}; "
          "engine camera/projection/cache and other-view tables remain",
          stats.built, stats.scene, stats.compatibility, stats.refused,
          stats.checked, stats.wrong, stats.reads, stats.missing, stats.shadows,
          stats.exceptional);
  stats.frame = frame;
}
bool Range(uint64_t address, uint64_t bytes) {
  if (!address || address + bytes - 1 > UINT32_MAX ||
      !bd::mem::try_at<uint8_t>(uint32_t(address)))
    return false;
  for (uint64_t page = (address & ~uint64_t(4095)) + 4096;
       page < address + bytes; page += 4096)
    if (!bd::mem::try_at<uint8_t>(uint32_t(page)))
      return false;
  return true;
}
bool Overlap(uint64_t a, uint64_t size, uint64_t b, uint64_t other_size) {
  return a < b + other_size && b < a + size;
}
float LoadFloat(uint32_t address) {
  return std::bit_cast<float>(bd::mem::load<uint32_t>(address));
}
bool Close(float a, float b) {
  return a == b || (std::isnan(a) && std::isnan(b)) ||
         (std::isfinite(a) && std::isfinite(b) &&
                   std::abs(a - b) <= 1e-5f * (1 + std::abs(b)));
}
void Checked(bool same, const char *kind) {
  if (same)
    return;
  ++stats.wrong;
  BD_ERROR("[host-frustum] {} mismatch", kind);
  throw std::runtime_error("Native frustum differs from original");
}
void Produce(PPCContext &ctx, uint8_t *base) {
  const uint32_t source = ctx.r3.u32;
  const uint64_t incoming_r3 = ctx.r3.u64;
  // STVX rounds addresses down. Distinct inputs may therefore alias a plane;
  // all inputs are snapshotted before the six ordered output stores.
  const std::array<uint32_t, 6> outputs{
      ctx.r4.u32 & ~15u, ctx.r5.u32 & ~15u, ctx.r6.u32 & ~15u,
      ctx.r7.u32 & ~15u, ctx.r8.u32 & ~15u, ctx.r9.u32 & ~15u};
  bool canonical = true;
  for (size_t i = 0; i < outputs.size(); ++i) {
    canonical &= outputs[i] == kScenePlanes + i * 16;
    if (Overlap(outputs[i], 16, kScenePlanes, 96))
      scene_frustum.Invalidate();
  }
  // Refuse aliases of the original callee's scratch and argument spills
  // before effects. Ordinary caller-local inputs (e.g. +832) are supported.
  const uint64_t stack = ctx.r1.u32;
  bool supported = REXCVAR_GET(bd_host_frustum) && stack >= 208 &&
                   stack + 72 <= UINT32_MAX && Range(source, 52) &&
                   !Overlap(source, 52, stack - 208, 280);
  for (auto output : outputs)
    supported &= Range(output, 16) &&
                 !Overlap(output, 16, stack - 208, 280);
  // Constants belong to the temporary engine import contract, not the
  // native math. Detect a patched engine convention before using it.
  supported &= bd::mem::try_load<uint32_t>((uint32_t(-32247) << 16) - 5572) ==
                   std::bit_cast<uint32_t>(-1.0f) &&
               bd::mem::try_load<uint32_t>((uint32_t(-32251) << 16) + 20908) ==
                   std::bit_cast<uint32_t>(1.0f) &&
               bd::mem::try_load<uint32_t>((uint32_t(-32251) << 16) + 21040) == 0;
  FrustumShape shape;
  if (supported) {
    for (size_t i = 0; i < 3; ++i)
      shape.origin[i] = LoadFloat(source + uint32_t(i * 4));
    for (size_t i = 0; i < 4; ++i)
      shape.orientation[i] = LoadFloat(source + 12 + uint32_t(i * 4));
    shape.right = LoadFloat(source + 28);
    shape.left = LoadFloat(source + 32);
    shape.top = LoadFloat(source + 36);
    shape.bottom = LoadFloat(source + 40);
    shape.near_distance = LoadFloat(source + 44);
    shape.far_distance = LoadFloat(source + 48);
    bool exceptional = false;
    for (uint32_t i = 0; i < 13; ++i)
      exceptional |= !std::isfinite(LoadFloat(source + i * 4));
    stats.exceptional += exceptional;
  }
  RenderFrustum result;
  if (supported) {
    ctx.fpscr.enableFlushMode();
    result = BuildFrustumPlanes(shape);
  }
  if (!supported) {
    ++stats.compatibility;
    stats.refused += REXCVAR_GET(bd_host_frustum);
    if (stats.compatibility <= 8)
      BD_INFO("[host-frustum] compatibility source {:08X} stack {:08X} "
              "outputs {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
              source, uint32_t(stack), outputs[0], outputs[1], outputs[2],
              outputs[3], outputs[4], outputs[5]);
    __imp__sub_821CCF48(ctx, base);
    return;
  }
  if (REXCVAR_GET(bd_host_frustum_verify)) {
    __imp__sub_821CCF48(ctx, base); // exactly once; native writes cannot hide a bug
    ++stats.checked;
    Checked(ctx.r3.u64 == incoming_r3, "return");
    for (size_t i = 0; i < outputs.size(); ++i) {
      size_t last = i;
      for (size_t j = i + 1; j < outputs.size(); ++j)
        if (outputs[j] == outputs[i])
          last = j;
      for (uint32_t k = 0; k < 4; ++k) {
        const float actual = LoadFloat(outputs[i] + k * 4);
        if (!Close(result.planes[last][k], actual))
          BD_ERROR("[host-frustum] plane {} component {} native {} original {}",
                   last, k, result.planes[last][k], actual);
        Checked(Close(result.planes[last][k], actual), "plane publication");
      }
    }
  } else {
    for (size_t i = 0; i < outputs.size(); ++i)
      for (uint32_t k = 0; k < 4; ++k)
        bd::mem::store<uint32_t>(outputs[i] + k * 4,
                                std::bit_cast<uint32_t>(result.planes[i][k]));
  }
  ++stats.built;
  if (canonical) {
    scene_frustum.Publish(FrameStatFrameCount(), result);
    ++stats.scene;
  }
}
} // namespace

bool GetNativeSceneFrustum(RenderFrustum &frustum) {
  const auto *current = REXCVAR_GET(bd_host_frustum)
                            ? scene_frustum.Get(FrameStatFrameCount()) : nullptr;
  if (!current) {
    ++stats.missing;
    return false;
  }
  if (REXCVAR_GET(bd_host_frustum_verify)) {
    // Diagnostic only: detect an unconverted writer between construction and
    // consumption. Normal culling does not read/import these getter shadows.
    for (uint32_t i = 0; i < 6; ++i)
      for (uint32_t k = 0; k < 4; ++k)
        Checked(Close(current->planes[i][k], LoadFloat(kScenePlanes + i * 16 + k * 4)),
                "current scene shadow");
    ++stats.shadows;
  }
  frustum = *current;
  ++stats.reads;
  return true;
}
} // namespace bd::gpu::scene

REX_HOOK_RAW(sub_821CCF48) {
  bd::gpu::scene::Produce(ctx, base);
  bd::gpu::scene::Report();
}
