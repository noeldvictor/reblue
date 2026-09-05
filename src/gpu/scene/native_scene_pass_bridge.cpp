/**
 * @file    native_scene_pass_bridge.cpp
 * @brief   Whole scene begin/end ownership, with counted remaining producers.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/scene/native_scene_pass.h"
#include "gpu/scene/native_pass_bridge.h"
#include "gpu/scene/native_view_bridge.h"
#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/device.h"
#include "gpu/frame_stats.h"
#include "gpu/host_resource_heap.h"
#include "gpu/host_targets.h"
#include "gpu/resource_bridge.h"
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc/context.h>
#include <stdexcept>
#include <vector>

REX_EXTERN(__imp__sub_82186BA0);
REX_EXTERN(__imp__sub_82187010);
REX_EXTERN(bdSetRenderState);
REX_EXTERN(sub_82184A88);
REX_EXTERN(bdBuildViewMatrix);
REX_EXTERN(sub_82186840);
REX_EXTERN(sub_821CCF48);
REX_EXTERN(sub_821764F8);
REX_EXTERN(sub_82179868);
REXCVAR_DECLARE(bool, bd_native_scene_passes);
REXCVAR_DECLARE(bool, bd_host_targets);
REXCVAR_DECLARE(i32, bd_render_scale);

namespace bd::gpu::scene {
namespace {
constexpr uint32_t kSettings = (uint32_t(-32035) << 16) - 26552;
constexpr uint32_t kTile = (uint32_t(-32035) << 16) - 26710;
constexpr uint32_t kView = (uint32_t(-32035) << 16) - 26424;
constexpr uint32_t kStack = (uint32_t(-32034) << 16) - 23232;
constexpr uint32_t kEngine = (uint32_t(-32034) << 16) - 19936;
constexpr uint32_t kViewCache = (uint32_t(-32035) << 16) + 32568;
constexpr uint32_t kFrustum = (uint32_t(-32033) << 16) - 30608;
constexpr uint32_t kPrimary = (uint32_t(-32035) << 16) + 24832;
constexpr uint32_t kArray = (uint32_t(-32035) << 16) + 25248;
constexpr uint32_t kCount = (uint32_t(-32137) << 16) + 16748;
constexpr uint32_t kLast = (uint32_t(-32035) << 16) + 28608;
constexpr uint32_t kSecondary = (uint32_t(-32137) << 16) + 28452;
constexpr uint32_t kHdr = (uint32_t(-32136) << 16) + 14888 + 12;
constexpr uint32_t kOne = (uint32_t(-32251) << 16) + 20908;
// Header formats belong only to the existing texture/getter adapter. Roles are
// explicit even for a square scene, no-MSAA scene, or small offscreen scene.
constexpr uint32_t kColorFormat = 0x1A2201BF, kDepthFormat = 0x2D200196;
struct ScenePass {
  uint32_t source = 0;
  GuestTexture *color = nullptr, *depth = nullptr;
  std::size_t nesting = 0;
};
thread_local std::vector<ScenePass> scenes;
struct Stats {
  uint64_t begins = 0, ends = 0, compatibility_begin = 0, compatibility_end = 0;
  uint64_t refused = 0, outputs = 0, null_outputs = 0, checked = 0, wrong = 0;
  uint64_t camera_calls = 0, state308_calls = 0, parameters = 0, empty_clears = 0;
  uint32_t frame = 0;
};
thread_local Stats stats;
void Report() {
  const auto frame = FrameStatFrameCount();
  if (frame - stats.frame < 300)
    return;
  BD_INFO("[native-scene] begins {} ends {} active {}; compatibility begin {} end {} "
          "refused {}; explicit outputs {} null {} empty clears {}; ownership checks {} wrong {}; "
          "view-cache entries {} state-308 adapters {} parameter adapters {}; "
          "post-chain/getter/resource adapters and engine traversal remain",
          stats.begins, stats.ends, scenes.size(), stats.compatibility_begin,
          stats.compatibility_end, stats.refused, stats.outputs, stats.null_outputs,
          stats.empty_clears, stats.checked, stats.wrong, stats.camera_calls,
          stats.state308_calls, stats.parameters);
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
bool Words(uint64_t address, uint64_t bytes) {
  return !(address & 3) && Range(address, bytes);
}
void Check(bool same, const char *message) {
  ++stats.checked;
  if (!same) {
    ++stats.wrong;
    BD_ERROR("[native-scene] {}", message);
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
bool Output(uint32_t container, GuestTexture *&image) {
  if (!Words(container, 8))
    return false;
  const auto address = bd::mem::load<uint32_t>(container + 4);
  image = address ? Texture(address) : nullptr;
  return !address || image;
}
// Reuse the original caller frame for still-recompiled camera-cache execution.
// This is stack ABI storage, not rendering ownership or an engine allocation.
struct CallFrame {
  PPCContext &ctx;
  uint64_t saved;
  explicit CallFrame(PPCContext &context) : ctx(context), saved(ctx.r1.u64) {
    ctx.r1.u32 -= 256;
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
  bdSetRenderState(ctx, base); // native state producers, except counted offset 308
  stats.state308_calls += offset == 308;
}
void Parameters(PPCContext &ctx, uint8_t *base, uint32_t source) {
  ctx.r3.s64 = int32_t(source);
  sub_821764F8(ctx, base); // complete native producer; engine descriptors remain
  ++stats.parameters;
}
bool Begin(PPCContext &ctx, uint8_t *base, uint32_t source) {
  if (!REXCVAR_GET(bd_native_scene_passes) || !REXCVAR_GET(bd_host_targets) ||
      !Words(source, 40) || !Words(kSettings, 4) || !CanEnterNativePass())
    return false;
  const auto settings = bd::mem::load<uint32_t>(kSettings);
  const auto camera = bd::mem::load<uint32_t>(source + 12);
  const auto reference = bd::mem::load<uint32_t>(source + 16);
  // An in-flight legacy handle must unwind through its matching compatibility
  // owner. Native attachments never enter the engine's 16-slot allocation list.
  if (!Words(settings, 7140) || !Words(camera, 300) || !Words(reference, 4) ||
      bd::mem::load<uint32_t>(source + 28) || bd::mem::load<uint32_t>(source + 36) ||
      ctx.r1.u32 < 256 || !Words(uint64_t(ctx.r1.u32) - 256, 328) ||
      !Words(kEngine, 54624) || !Words(kCount, 4) || !Words(kPrimary, 420) ||
      !Words(kLast, 420) || !Words(kSecondary, 160) || !Words(kViewCache, 112) ||
      !Words(kFrustum, 160) || !Words(kView, 4) || !Range(kTile, 1) ||
      !Range(kHdr, 1) || !Words(kOne, 4) ||
      bd::mem::load<uint32_t>(kOne) != 0x3F800000)
    return false;
  const auto count = bd::mem::load<int32_t>(kCount);
  if (count > 0 && !Words(kArray, uint64_t(count) * 420))
    return false;
  GuestTexture *output = nullptr, *depth_output = nullptr;
  if (!Output(bd::mem::load<uint32_t>(source + 4), output) || !output ||
      !Output(bd::mem::load<uint32_t>(source + 8), depth_output))
    return false;
  const auto extent = ScaleSceneExtent({output->width, output->height},
                                       Video::BootSupersampling(), REXCVAR_GET(bd_render_scale));
  if (!extent)
    return false;
  const auto samples = uint32_t(Video::CvarMSAASampleCount());
  auto *color = HostTargetAcquire(HostTargetClass::SceneColor, extent->width,
                                  extent->height, kColorFormat, samples);
  auto *depth = HostTargetAcquire(HostTargetClass::SceneDepth, extent->width,
                                  extent->height, kDepthFormat, samples);
  if (!color || !depth) {
    if (color) ReleaseResourceAdapter(color->selfVa);
    if (depth) ReleaseResourceAdapter(depth->selfVa);
    return false;
  }
  // From here the native pair owns the scope. Never run the original begin a
  // second time after an observable state/resource publication.
  CallFrame frame(ctx);
  bd::mem::store<uint32_t>(source + 20, bd::mem::load<uint32_t>(reference));
  SetState(ctx, base, 60, 1);
  SetState(ctx, base, 56, 0);
  SetState(ctx, base, 72, 6);
  SetState(ctx, base, 76, 7);
  SetState(ctx, base, 40, 1);
  sub_82184A88(ctx, base); // native sampler defaults
  // Getter-only shadows: native allocation does not read an EDRAM/MSAA default.
  bd::mem::store<uint8_t>(kTile, 0);
  if (bd::mem::load<uint32_t>(settings + 7112))
    bd::mem::store<uint32_t>(kStack + 320, 0);
  bd::mem::store<uint32_t>(source + 28, color->selfVa);
  bd::mem::store<uint32_t>(source + 36, depth->selfVa);
  uint32_t result = 0;
  Check(EnterNativePass(color, depth, result), "Native scene could not enter its preflighted pass");
  scenes.push_back({source, color, depth, NativePassDepth()});
  const bool primary = bd::mem::load<uint32_t>(kView) == 0;
  const auto clear = SceneClearColor(bd::mem::load<uint32_t>(camera + 8), primary);
  SetState(ctx, base, 308, 0);
  Video::RequestClear(0x31, clear, 1.0f, 0);
  SetState(ctx, base, 308, 1);
  SetState(ctx, base, 212, SceneColorWriteMask(primary));
  ctx.r3.u64 = 0;
  ctx.r4.u64 = camera + 160;
  ctx.r5.u64 = camera + 224;
  bdBuildViewMatrix(ctx, base); // native view/matrix producer
  ctx.fpscr.disableFlushMode();
  const float x = bd::mem::load<float>(camera + 288);
  const float y = bd::mem::load<float>(camera + 292);
  const float z = bd::mem::load<float>(camera + 296);
  bd::mem::store<float>(kEngine + 54608, x);
  bd::mem::store<float>(kEngine + 54620, 1.0f);
  bd::mem::store<uint32_t>(kEngine + 4, 1);
  bd::mem::store<float>(kEngine + 54616, z);
  bd::mem::store<float>(kEngine + 54612, y);
  ctx.r3.u64 = 0;
  sub_82186840(ctx, base); // native complete camera/frustum-cache producer
  ++stats.camera_calls;
  if (bd::mem::load<uint32_t>(settings + 7136) &&
      bd::mem::load<uint32_t>(kViewCache + 56) &&
      !PublishCachedViewFrustum(ctx, 1)) {
    ctx.fpscr.disableFlushMode();
    for (uint32_t i = 0; i < 13; ++i)
      bd::mem::store<float>(kFrustum + i * 4,
                            bd::mem::load<float>(kViewCache + 60 + i * 4));
    ctx.r3.s64 = int32_t(kFrustum);
    ctx.r4.s64 = int32_t(kFrustum + 64);
    ctx.r5.s64 = int32_t(kFrustum + 80);
    ctx.r6.s64 = int32_t(kFrustum + 96);
    ctx.r7.s64 = int32_t(kFrustum + 112);
    ctx.r8.s64 = int32_t(kFrustum + 128);
    ctx.r9.s64 = int32_t(kFrustum + 144);
    sub_821CCF48(ctx, base); // native six-plane publication
  }
  Parameters(ctx, base, kPrimary);
  for (int32_t i = 0; i < count; ++i)
    Parameters(ctx, base, kArray + uint32_t(i) * 420);
  Parameters(ctx, base, kLast);
  ctx.r3.s64 = int32_t(kSecondary);
  sub_82179868(ctx, base); // preserves the original final result
  ++stats.parameters;
  ++stats.begins;
  return true;
}
bool End(PPCContext &ctx, uint32_t source) {
  if (scenes.empty() || !scenes.back().color)
    return false;
  const auto pass = scenes.back();
  Check(pass.source == source && NativePassDepth() == pass.nesting,
        "Native scene end does not match the active scene scope");
  Check(Words(source, 40) &&
        bd::mem::load<uint32_t>(source + 28) == pass.color->selfVa &&
        bd::mem::load<uint32_t>(source + 36) == pass.depth->selfVa &&
        state().render_target == pass.color && state().depth_stencil == pass.depth,
        "Native scene attachment getter or live binding changed");
  GuestTexture *color_output = nullptr, *depth_output = nullptr;
  Check(Output(bd::mem::load<uint32_t>(source + 4), color_output) &&
        Output(bd::mem::load<uint32_t>(source + 8), depth_output),
        "Native scene output adapter no longer names a live texture");
  // An empty pass still publishes its clear, never the persistent image's old
  // contents. Binding consumes the pair's held clears before any output read.
  if (pass.color->hostClearFlags || pass.depth->hostClearFlags) {
    Check(Video::BindDrawFramebuffer(), "Native scene clear could not bind its attachments");
    ++stats.empty_clears;
  }
  const float exposure = SceneOutputExposure(bd::mem::load<uint8_t>(kHdr) != 0);
  if (depth_output) {
    Check(Video::PublishSceneOutput(pass.depth, depth_output, 1.0f),
          "Native scene depth publication failed");
    ++stats.outputs;
  } else ++stats.null_outputs;
  if (color_output) {
    Check(Video::PublishSceneOutput(pass.color, color_output, exposure),
          "Native scene colour publication failed");
    ++stats.outputs;
  } else ++stats.null_outputs;
  uint32_t result = 0;
  Check(LeaveNativePass(result), "Native scene could not restore its previous pass");
  ReleaseResourceAdapter(pass.color->selfVa);
  bd::mem::store<uint32_t>(source + 28, 0);
  ctx.r3.u64 = ReleaseResourceAdapter(pass.depth->selfVa);
  bd::mem::store<uint32_t>(source + 36, 0);
  ctx.fpscr.disableFlushMode();
  scenes.pop_back();
  ++stats.ends;
  return true;
}
} // namespace
} // namespace bd::gpu::scene

REX_HOOK_RAW(sub_82186BA0) {
  using namespace bd::gpu::scene;
  const auto source = ctx.r3.u32;
  if (!Begin(ctx, base, source)) {
    ++stats.compatibility_begin;
    stats.refused += REXCVAR_GET(bd_native_scene_passes);
    __imp__sub_82186BA0(ctx, base);
    scenes.push_back({source}); // match the original end even if the cvar changes
  }
  Report();
}
REX_HOOK_RAW(sub_82187010) {
  using namespace bd::gpu::scene;
  const auto source = ctx.r3.u32;
  if (!End(ctx, source)) {
    Check(scenes.empty() || scenes.back().source == source,
          "Compatibility scene end does not match its begin");
    ++stats.compatibility_end;
    __imp__sub_82187010(ctx, base);
    if (!scenes.empty()) scenes.pop_back();
  }
  Report();
}
