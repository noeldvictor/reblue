/**
 * @file    native_view_bridge.cpp
 * @brief   Complete camera/frustum-cache execution on the host.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/scene/native_view_bridge.h"
#include "gpu/scene/native_view.h"
#include "gpu/scene/native_transform_bridge.h"
#include "gpu/scene/host_frustum_bridge.h"
#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/frame_stats.h"
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc/context.h>
#include <stdexcept>

REX_EXTERN(__imp__sub_82186840);
REX_EXTERN(__imp__sub_827355C0);
REXCVAR_DECLARE(bool, bd_native_views);
REXCVAR_DECLARE(bool, bd_native_views_verify);
REXCVAR_DECLARE(bool, bd_host_frustum);

namespace bd::gpu::scene {
namespace {
constexpr uint32_t kSettings = (uint32_t(-32035) << 16) - 26552;
constexpr uint32_t kCache = (uint32_t(-32035) << 16) + 32568;
constexpr uint32_t kShape = (uint32_t(-32033) << 16) - 30608;
constexpr uint32_t kMatrices = (uint32_t(-32034) << 16) - 19936 + 54720;
constexpr uint32_t kClipPoints = (uint32_t(-32136) << 16) - 3056;
constexpr uint32_t kViewCount = 7; // engine adapter extent, not native capacity
thread_local NativeViewCache views;
struct Stats {
  uint64_t produced = 0, hits = 0, rebuilt = 0, resets = 0, selected = 0;
  uint64_t compatibility = 0, refused = 0, checked = 0, wrong = 0;
  uint64_t native_matrices = 0, imported_matrices = 0, bootstrap = 0, clip_checks = 0;
  uint32_t frame = 0;
};
thread_local Stats stats;
void Report() {
  const auto frame = FrameStatFrameCount();
  if (frame - stats.frame < 300)
    return;
  BD_INFO("[native-views] produced {} hits {} rebuilt {} resets {} selected {}; "
          "compatibility {} refused {}; checked {} wrong {}; native matrices {} "
          "matrix imports {} cache bootstraps {} clip convention checks {}; "
          "engine camera sources, invalidation/settings and getters remain",
          stats.produced, stats.hits, stats.rebuilt, stats.resets, stats.selected,
          stats.compatibility, stats.refused, stats.checked, stats.wrong,
          stats.native_matrices, stats.imported_matrices, stats.bootstrap, stats.clip_checks);
  stats.frame = frame;
}
bool Words(uint64_t address, uint64_t bytes) {
  if (!address || (address & 3) || address + bytes - 1 > UINT32_MAX ||
      !bd::mem::try_at<uint8_t>(uint32_t(address)))
    return false;
  for (auto page = (address & ~uint64_t(4095)) + 4096; page < address + bytes; page += 4096)
    if (!bd::mem::try_at<uint8_t>(uint32_t(page)))
      return false;
  return true;
}
bool Close(float a, float b) {
  return a == b || (std::isnan(a) && std::isnan(b)) ||
         (std::isfinite(a) && std::isfinite(b) &&
          std::abs(a - b) <= 1e-5f * (1 + std::abs(b)));
}
std::array<float, 13> Pack(const FrustumShape &shape) {
  return {shape.origin[0], shape.origin[1], shape.origin[2],
          shape.orientation[0], shape.orientation[1], shape.orientation[2], shape.orientation[3],
          shape.right, shape.left, shape.top, shape.bottom, shape.near_distance, shape.far_distance};
}
FrustumShape ReadShape(uint32_t address) {
  std::array<float, 13> words;
  for (uint32_t i = 0; i < words.size(); ++i)
    words[i] = bd::mem::load<float>(address + i * 4);
  return {{words[0], words[1], words[2]}, {words[3], words[4], words[5], words[6]},
          words[7], words[8], words[9], words[10], words[11], words[12]};
}
void WriteShape(uint32_t address, const FrustumShape &shape) {
  const auto words = Pack(shape);
  for (uint32_t i = 0; i < words.size(); ++i)
    bd::mem::store<float>(address + i * 4, words[i]);
}
void Publish(const FrustumShape &shape, const RenderFrustum &frustum) {
  WriteShape(kShape, shape); // getter ABI only; native consumers own the values
  for (uint32_t i = 0; i < 6; ++i)
    for (uint32_t c = 0; c < 4; ++c)
      bd::mem::store<float>(kShape + 64 + i * 16 + c * 4, frustum.planes[i][c]);
  PublishNativeSceneFrustum(frustum);
}
void Check(bool same, const char *what) {
  if (!same) {
    ++stats.wrong;
    BD_ERROR("[native-views] {} mismatch", what);
    if (const auto *transforms = GetNativeRenderTransforms())
      for (const auto *matrix : {&transforms->inputs.view, &transforms->inputs.projection})
        for (uint32_t row = 0; row < 4; ++row)
          BD_ERROR("[native-views] input row {} {} {} {}", (*matrix)[row*4],
                   (*matrix)[row*4+1], (*matrix)[row*4+2], (*matrix)[row*4+3]);
    throw std::runtime_error("Native view differs from original");
  }
}
void CompareWords(uint32_t address, const auto &expected, const char *what) {
  for (uint32_t i = 0; i < expected.size(); ++i) {
    const float actual = bd::mem::load<float>(address + i * 4);
    if (!Close(actual, expected[i]))
      BD_ERROR("[native-views] {} component {} native {} original {}",
               what, i, expected[i], actual);
    Check(Close(actual, expected[i]), what);
  }
}
bool Produce(PPCContext &ctx, uint8_t *base) {
  const uint32_t view = ctx.r3.u32;
  if (!REXCVAR_GET(bd_native_views) || !REXCVAR_GET(bd_host_frustum))
    return false;
  if (view >= kViewCount || !Words(kSettings, 4) || !Words(kCache, kViewCount * 56) ||
      !Words(kShape, 160) || !Words(kMatrices, 128) ||
      ctx.r1.u32 < 1024 || (ctx.r1.u32 & 15) ||
      !Words(ctx.r1.u32 - 1024, 1096)) {
    ++stats.refused;
    return false;
  }
  const uint32_t settings = bd::mem::load<uint32_t>(kSettings);
  if (!Words(settings, 7132) ||
      bd::mem::try_load<uint32_t>((uint32_t(-32247) << 16) - 4536) !=
          std::bit_cast<uint32_t>(-1.1f)) {
    ++stats.refused;
    return false;
  }
  const uint32_t slot = kCache + view * 56;
  const bool rebuild = bd::mem::load<uint32_t>(settings + 7128) ||
                       !bd::mem::load<uint32_t>(slot);
  FrustumShape shape;
  if (rebuild) {
    ctx.fpscr.enableFlushMode();
    RenderTransformInputs inputs;
    if (const auto *transforms = GetNativeRenderTransforms()) {
      inputs = transforms->inputs;
      ++stats.native_matrices;
    } else {
      ctx.fpscr.disableFlushMode();
      for (uint32_t i = 0; i < 16; ++i) {
        inputs.view[i] = bd::mem::load<float>(kMatrices + i * 4);
        inputs.projection[i] = bd::mem::load<float>(kMatrices + 64 + i * 4);
      }
      ++stats.imported_matrices;
    }
    if (REXCVAR_GET(bd_native_views_verify)) {
      Check(Words(kClipPoints, 96), "clip table range");
      for (uint32_t i = 0; i < 6; ++i)
        CompareWords(kClipPoints + i * 16, kViewClipPoints[i], "clip convention");
      ++stats.clip_checks;
    }
    ctx.fpscr.enableFlushMode();
    shape = BuildViewFrustumShape(inputs.view, inputs.projection);
    ++stats.rebuilt;
  } else {
    if (const auto *cached = views.Get(view))
      shape = *cached;
    else {
      ctx.fpscr.disableFlushMode();
      shape = ReadShape(slot + 4); // explicit startup/enable compatibility import
      ++stats.bootstrap;
    }
    ++stats.hits;
  }
  ctx.fpscr.enableFlushMode();
  const auto frustum = BuildFrustumPlanes(shape);
  if (REXCVAR_GET(bd_native_views_verify)) {
    __imp__sub_82186840(ctx, base); // exactly once, before native getter writes
    ++stats.checked;
    Check(ctx.r3.s64 == int64_t(int32_t(kShape)), "return");
    CompareWords(kShape, Pack(shape), "shape");
    CompareWords(slot + 4, Pack(shape), "cache");
    Check(bd::mem::load<uint32_t>(slot) != 0, "valid cache");
    for (uint32_t i = 0; i < 6; ++i)
      CompareWords(kShape + 64 + i * 16, frustum.planes[i], "plane");
  }
  Publish(shape, frustum);
  views.Publish(view, shape);
  if (rebuild) {
    bd::mem::store<uint32_t>(slot, 1);
    WriteShape(slot + 4, shape);
    ctx.fpscr.disableFlushMode();
  } else {
    ctx.fpscr.enableFlushMode();
  }
  ctx.r3.s64 = int32_t(kShape);
  ++stats.produced;
  return true;
}
} // namespace

bool PublishCachedViewFrustum(PPCContext &ctx, uint32_t view) {
  if (!REXCVAR_GET(bd_native_views) || !REXCVAR_GET(bd_host_frustum))
    return false;
  const auto *shape = views.Get(view);
  if (!shape)
    return false;
  ctx.fpscr.enableFlushMode();
  const auto *volume = views.Volume(view);
  Publish(*shape, volume ? *volume : BuildFrustumPlanes(*shape));
  ++stats.selected;
  return true;
}
bool PublishNativeViewVolume(uint32_t view, const RenderFrustum &volume) {
  if (!Words(kShape, 160) || !views.Get(view)) return false;
  views.PublishVolume(view, volume);
  Publish(*views.Get(view), volume); // slope shape stays a getter-only adapter
  return true;
}
} // namespace bd::gpu::scene

REX_HOOK_RAW(sub_82186840) {
  using namespace bd::gpu::scene;
  if (!Produce(ctx, base)) {
    ++stats.compatibility;
    views.Clear();
    __imp__sub_82186840(ctx, base);
  }
  Report();
}
REX_HOOK_RAW(sub_827355C0) {
  using namespace bd::gpu::scene;
  views.Clear();
  if (REXCVAR_GET(bd_native_views) && Words(kCache, kViewCount * 56)) {
    for (uint32_t i = 0; i < kViewCount; ++i)
      bd::mem::store<uint32_t>(kCache + i * 56, 0);
    ++stats.resets;
  } else {
    __imp__sub_827355C0(ctx, base);
  }
}
