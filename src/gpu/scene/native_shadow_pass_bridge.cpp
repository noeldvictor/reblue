/**
 * @file    native_shadow_pass_bridge.cpp
 * @brief   Sun-shadow attachment lifecycle with explicit native ownership.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/scene/native_pass_bridge.h"
#include "gpu/scene/native_sun_camera_bridge.h"
#include "gpu/scene/native_view_bridge.h"
#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/device.h"
#include "gpu/frame_stats.h"
#include "gpu/host_resource_heap.h"
#include "gpu/host_targets.h"
#include "gpu/resource_bridge.h"
#include "gpu/settings.h"
#include <algorithm>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc/context.h>
#include <stdexcept>
#include <vector>

REX_EXTERN(__imp__sub_82187168);
REX_EXTERN(__imp__sub_82187330);
REX_EXTERN(__imp__sub_82287788);
REX_EXTERN(bdSetRenderState);
REX_EXTERN(sub_82184A88);
REX_EXTERN(bdBuildViewMatrix);
REX_EXTERN(sub_82283068);
REX_EXTERN(sub_821752E8);
REX_EXTERN(sub_82186840);
REXCVAR_DECLARE(bool, bd_native_shadow_passes);
REXCVAR_DECLARE(bool, bd_host_targets);
REXCVAR_DECLARE(bool, bd_shadows);
REXCVAR_DECLARE(bool, bd_shadow_fit_diag);

namespace bd::gpu::scene {
namespace {
constexpr uint32_t kSettings = (uint32_t(-32035) << 16) - 26552;
constexpr uint32_t kPrimary = (uint32_t(-32035) << 16) + 24832;
constexpr uint32_t kEngine = (uint32_t(-32034) << 16) - 19936;
constexpr uint32_t kPassMode = (uint32_t(-32036) << 16) - 5536;
constexpr uint32_t kOne = (uint32_t(-32251) << 16) + 20908;
constexpr uint32_t kDepthFormat = 0x2D200196; // resource-header adapter only
struct ShadowPass {
  uint32_t source = 0;
  GuestTexture *depth = nullptr, *output = nullptr;
  std::size_t nesting = 0;
};
thread_local std::vector<ShadowPass> shadows;
struct Stats {
  uint64_t begins = 0, ends = 0, compatibility_begin = 0, compatibility_end = 0;
  uint64_t refused = 0, outputs = 0, null_outputs = 0, empty_clears = 0;
  uint64_t checked = 0, wrong = 0, camera_snapshots = 0, light_fits = 0;
  uint64_t object_culls = 0, object_visible = 0, object_comparisons = 0, object_changed = 0;
  uint32_t frame = 0;
};
thread_local Stats stats;
void Report() {
  const auto frame = FrameStatFrameCount();
  if (frame - stats.frame < 300)
    return;
  BD_INFO("[native-shadow-pass] begins {} ends {} active {}; compatibility begin {} end {} "
          "refused {}; explicit outputs {} null {} empty clears {}; ownership checks {} wrong {}; "
          "engine camera snapshots {} light fits {}; secondary shadow lifecycle, "
          "caster scheduling and sampling/getter/resource adapters remain",
          stats.begins, stats.ends, shadows.size(), stats.compatibility_begin,
          stats.compatibility_end, stats.refused, stats.outputs, stats.null_outputs,
          stats.empty_clears, stats.checked, stats.wrong, stats.camera_snapshots,
          stats.light_fits);
  stats.frame = frame;
  BD_INFO("[native-sun-cull] objects {} visible {} original comparisons {} changed {}",
          stats.object_culls, stats.object_visible, stats.object_comparisons, stats.object_changed);
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
bool Words(uint64_t address, uint64_t bytes) {
  return !(address & 3) && Range(address, bytes);
}
void Check(bool same, const char *message) {
  ++stats.checked;
  if (!same) {
    ++stats.wrong;
    BD_ERROR("[native-shadow-pass] {}", message);
    throw std::runtime_error(message);
  }
}
GuestTexture *Texture(uint32_t address) {
  ResourceType type;
  if (!HostResourceHeap::GetType(address, &type) ||
      (type != ResourceType::Texture && type != ResourceType::RenderTarget &&
       type != ResourceType::DepthStencil))
    return nullptr;
  auto *image = HostResourceHeap::FromGuest<GuestTexture>(address);
  return image && image->texture ? image : nullptr;
}
struct CallFrame {
  PPCContext &ctx;
  uint64_t saved;
  explicit CallFrame(PPCContext &context) : ctx(context), saved(ctx.r1.u64) {
    ctx.r1.u32 -= 128;
    bd::mem::store<uint32_t>(ctx.r1.u32, uint32_t(saved));
    ctx.fpscr.disableFlushMode();
  }
  ~CallFrame() {
    ctx.r1.u64 = saved;
    ctx.fpscr.disableFlushMode();
  }
};
void SetState(PPCContext &ctx, uint8_t *base, uint32_t offset, uint32_t value) {
  ctx.r3.u64 = offset;
  ctx.r4.u64 = value;
  bdSetRenderState(ctx, base); // already-native raster/blend/alpha producers
}
bool Begin(PPCContext &ctx, uint8_t *base, uint32_t source) {
  if (!REXCVAR_GET(bd_native_shadow_passes) || !REXCVAR_GET(bd_host_targets) ||
      !Words(source, 40) || !Words(kSettings, 4) || !Words(kPrimary, 420) ||
      !Words(kEngine, 54624) || !Range(kPassMode, 1) || !Words(kOne, 4) ||
      bd::mem::load<uint32_t>(kOne) != 0x3F800000 || !CanEnterNativePass())
    return false;
  const auto settings = bd::mem::load<uint32_t>(kSettings);
  const auto camera = bd::mem::load<uint32_t>(source + 12);
  if (!Words(settings, 7084) || !Words(camera, 312) ||
      bd::mem::load<uint32_t>(source + 36) || ctx.r1.u32 < 2048 ||
      (ctx.r1.u32 & 15) || !Words(uint64_t(ctx.r1.u32) - 2048, 2120))
    return false;
  const auto output_address = bd::mem::load<uint32_t>(kPrimary + 12);
  auto *output = output_address ? Texture(output_address) : nullptr;
  if (output_address && (!output ||
      output->sampleCount != plume::RenderSampleCount::COUNT_1 ||
      output->viewDimension == plume::RenderTextureViewDimension::TEXTURE_CUBE))
    return false;
  // Same owner setting as the sampleable texture's creation hook. Neither role
  // selection nor empty-pass behaviour is inferred from square dimensions.
  const auto dimension = REXCVAR_GET(bd_shadows)
      ? uint32_t(Settings::Get().ShadowDimension()) : 64u;
  if (!dimension)
    return false;
  auto *depth = HostTargetAcquire(HostTargetClass::Shadow, dimension, dimension,
                                  kDepthFormat, 1);
  if (!depth)
    return false;
  if (output && depth->layers != output->layers) {
    ReleaseResourceAdapter(depth->selfVa);
    return false; // unsupported layer policy, before observable pass publication
  }
  // From this point the native scope must unwind natively, even if a cvar
  // changes. Never execute the original lifecycle after publishing attachments.
  CallFrame frame(ctx);
  SetState(ctx, base, 60, 0);
  SetState(ctx, base, 72, 6);
  SetState(ctx, base, 76, 7);
  SetState(ctx, base, 40, 1);
  sub_82184A88(ctx, base);
  bd::mem::store<uint32_t>(source + 36, depth->selfVa);
  uint32_t result = 0;
  Check(EnterNativePass(nullptr, depth, result), "Native shadow could not enter its preflighted pass");
  if (output) RetainResourceAdapter(output->selfVa);
  shadows.push_back({source, depth, output, NativePassDepth()});
  Video::RequestClear(0x30, 0xFFFFFFFF, 1.0f, 0);
  SetState(ctx, base, 212, 0);
  ctx.r3.u64 = 0;
  ctx.r4.u64 = camera + 160;
  ctx.r5.u64 = camera + 224;
  bdBuildViewMatrix(ctx, base);
  if (!ProduceNativeSunCamera(kPrimary, camera + 300, dimension)) {
    sub_82283068(ctx, base); // explicitly counted unsupported/correctness path
    ++stats.camera_snapshots;
    ctx.r3.s64 = int32_t(kPrimary);
    ctx.r4.u64 = camera + 288;
    ctx.r5.u64 = camera + 300;
    sub_821752E8(ctx, base);
    ++stats.light_fits;
  }
  ctx.r3.u64 = 0;
  ctx.r4.s64 = int32_t(kPrimary + 68);
  ctx.r5.s64 = int32_t(kPrimary + 132);
  bdBuildViewMatrix(ctx, base);
  ctx.fpscr.disableFlushMode();
  for (uint32_t i = 0; i < 3; ++i)
    bd::mem::store<float>(kEngine + 54608 + i * 4,
                          bd::mem::load<float>(kPrimary + 28 + i * 4));
  bd::mem::store<uint32_t>(kEngine + 4, 1);
  bd::mem::store<float>(kEngine + 54620, 1.0f);
  ctx.r3.u64 = 1;
  sub_82186840(ctx, base); // native complete frustum/cache producer; preserve r3
  if (const auto sun = GetNativeSunCamera())
    Check(PublishNativeViewVolume(1, sun->frustum), "Native sun volume publication failed");
  bd::mem::store<uint8_t>(kPassMode,
      uint8_t(std::min(bd::mem::load<uint32_t>(settings + 7080), 2u)));
  ++stats.begins;
  return true;
}
bool End(PPCContext &ctx, uint8_t *base, uint32_t source) {
  if (shadows.empty() || !shadows.back().depth)
    return false;
  const auto pass = shadows.back();
  Check(pass.source == source && NativePassDepth() == pass.nesting,
        "Native shadow end does not match the active scope");
  Check(Words(source, 40) &&
        bd::mem::load<uint32_t>(source + 36) == pass.depth->selfVa &&
        !state().render_target && state().depth_stencil == pass.depth,
        "Native shadow attachment getter or live binding changed");
  Check(bd::mem::load<uint32_t>(kPrimary + 12) ==
            (pass.output ? pass.output->selfVa : 0),
        "Native shadow output association changed during its pass");
  // Clear the owned attachment even with no casters: empty means far, not a
  // previous frame, last-drawn surface or a square-texture resolve heuristic.
  if (pass.depth->hostClearFlags) {
    Check(Video::BindDrawFramebuffer(), "Native shadow clear could not bind its attachment");
    ++stats.empty_clears;
  }
  if (pass.output) {
    Check(Video::PublishSceneOutput(pass.depth, pass.output, 1.0f, false),
          "Native shadow depth publication failed");
    ++stats.outputs;
  } else ++stats.null_outputs;
  CallFrame frame(ctx);
  SetState(ctx, base, 212, 7);
  uint32_t result = 0;
  Check(LeaveNativePass(result), "Native shadow could not restore its previous pass");
  ctx.r3.u64 = ReleaseResourceAdapter(pass.depth->selfVa);
  bd::mem::store<uint32_t>(source + 36, 0);
  if (pass.output) ReleaseResourceAdapter(pass.output->selfVa);
  bd::mem::store<uint8_t>(kPassMode, 0);
  shadows.pop_back();
  ++stats.ends;
  return true;
}
} // namespace
} // namespace bd::gpu::scene

REX_HOOK_RAW(sub_82187168) {
  using namespace bd::gpu::scene;
  const auto source = ctx.r3.u32;
  if (!Begin(ctx, base, source)) {
    InvalidateNativeSunCamera();
    ++stats.compatibility_begin;
    stats.refused += REXCVAR_GET(bd_native_shadow_passes);
    __imp__sub_82187168(ctx, base);
    shadows.push_back({source});
  }
  Report();
}
REX_HOOK_RAW(sub_82187330) {
  using namespace bd::gpu::scene;
  const auto source = ctx.r3.u32;
  if (!End(ctx, base, source)) {
    Check(shadows.empty() || shadows.back().source == source,
          "Compatibility shadow end does not match its begin");
    ++stats.compatibility_end;
    __imp__sub_82187330(ctx, base);
    if (!shadows.empty()) shadows.pop_back();
  }
  Report();
}

REX_HOOK_RAW(sub_82287788) {
  using namespace bd::gpu::scene;
  // View 1's original branch uses fixed view-depth and radial clip cutoffs,
  // not the published six planes. Those are not the native camera's volume.
  // Restrict its replacement to the explicit owned sun scope, not target size.
  const auto sun = !shadows.empty() && shadows.back().depth &&
      NativePassDepth() == shadows.back().nesting ? GetNativeSunCamera() : std::nullopt;
  if (!sun || !Words(ctx.r3.u32, 12)) {
    __imp__sub_82287788(ctx, base);
    return;
  }
  SunVector center;
  for (uint32_t i = 0; i < 3; ++i) center[i] = bd::mem::load<float>(ctx.r3.u32 + i*4);
  const auto radius = ctx.f1.f64;
  const bool visible = IntersectsSunVolume(sun->frustum, center, radius);
  ++stats.object_culls;
  stats.object_visible += visible;
  if (REXCVAR_GET(bd_shadow_fit_diag)) {
    __imp__sub_82287788(ctx, base);
    ++stats.object_comparisons;
    if (bool(ctx.r3.u32) != visible && ++stats.object_changed <= 8)
      BD_INFO("[native-sun-cull] original {} native {} center ({:.3f} {:.3f} {:.3f}) radius {:.3f}",
              ctx.r3.u32, visible, center[0], center[1], center[2], radius);
  }
  ctx.r3.u64 = visible;
}
