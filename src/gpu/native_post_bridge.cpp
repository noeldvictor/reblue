/**
 * @file    native_post_bridge.cpp
 * @brief   Native DoF/bloom scheduling and explicit completed post output.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/post_chain.h"
#include "gpu/post_parameters.h"
#include "gpu/device.h"
#include "gpu/host_targets.h"
#include "gpu/host_resource_heap.h"
#include "gpu/resource_bridge.h"
#include "gpu/frame_stats.h"
#include "core/memory_helpers.h"
#include "core/logging.h"
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc/context.h>
#include <rex/system/xthread.h>
#include <stdexcept>

REX_EXTERN(__imp__sub_8221B1D8);
REX_EXTERN(sub_8221E758);
REX_EXTERN(sub_8221E700);
REX_EXTERN(bdSetRenderState);
REXCVAR_DECLARE(bool, bd_native_post);
REXCVAR_DECLARE(bool, bd_native_post_verify);
REXCVAR_DECLARE(bool, bd_native_dof);
REXCVAR_DECLARE(bool, bd_native_dof_verify);
REXCVAR_DECLARE(bool, bd_host_targets);

namespace bd::gpu {
namespace {
constexpr uint32_t kThread = (uint32_t(-32035) << 16) - 26664;
constexpr uint32_t kDepth = (uint32_t(-32136) << 16) + 14888 + 16;
constexpr uint32_t kState = (uint32_t(-32036) << 16) - 7768;
struct Stats {
  uint64_t native = 0, original = 0, comparisons = 0, wrong = 0;
  uint64_t settings = 0, memory = 0, effects = 0, inputs = 0;
  uint64_t tail_calls = 0, state_calls = 0;
  uint32_t frame = 0;
};
thread_local Stats stats;
thread_local const BloomParameters *comparison = nullptr;
thread_local uint32_t comparison_count = 0;
void Report() {
  const auto frame = FrameStatFrameCount();
  if (frame - stats.frame < 300)
    return;
  BD_INFO("[native-post] frames {} original {} parameter checks {} wrong {}; "
          "refusals settings {} memory {} effects {} inputs {}; tail effects {} state-308 {}; "
          "authored properties, image/output getters, other effects and UI remain",
          stats.native, stats.original, stats.comparisons, stats.wrong,
          stats.settings, stats.memory, stats.effects, stats.inputs,
          stats.tail_calls, stats.state_calls);
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
  const auto handle = bd::mem::load<uint32_t>(container);
  ResourceType type;
  if (!HostResourceHeap::GetType(handle, &type) ||
      (type != ResourceType::Texture && type != ResourceType::RenderTarget &&
       type != ResourceType::DepthStencil))
    return nullptr;
  return HostResourceHeap::FromGuest<GuestTexture>(handle);
}
struct Tail {
  bool glare = false;
  std::array<bool, 3> adjustments{};
  uint32_t saved_state = 0;
};
bool ReadPlan(uint32_t owner, DofParameters &dof, BloomParameters &bloom,
              bool &has_dof, Tail &tail) {
  if (!REXCVAR_GET(bd_native_post) || !REXCVAR_GET(bd_host_targets) ||
      !REXCVAR_GET(bd_native_dof) || REXCVAR_GET(bd_native_dof_verify)) {
    ++stats.settings;
    return false;
  }
  if (!Words(owner, 12756) || !Words(kThread, 4) || !Words(kState + 308, 4) ||
      !rex::system::XThread::GetCurrentThread()) {
    ++stats.memory;
    return false;
  }
  const auto bank = rex::system::XThread::GetCurrentThreadId() ==
      bd::mem::load<uint32_t>(kThread) ? 0u : 1u;
  const auto flag = [&](uint32_t offset) {
    return bd::mem::load<uint8_t>(owner + offset + bank) == 1;
  };
  // These filters can alter the scene between DoF and bloom, or consume the
  // original composite's retained entry array. Keep their entire old scope
  // counted until each producer and its inputs are converted; never skip it.
  for (const auto offset : {16u, 48u, 56u, 80u}) {
    if (flag(offset)) {
      if (stats.effects < 8)
        BD_INFO("[native-post] compatibility effect flag {} bank {}", offset, bank);
      ++stats.effects;
      return false;
    }
  }
  tail.glare = flag(32);
  tail.saved_state = bd::mem::load<uint32_t>(kState + 308);
  constexpr std::array<uint32_t, 3> flags{40, 64, 72};
  constexpr std::array<uint32_t, 3> strengths{12724, 12740, 12748};
  for (size_t i = 0; i < flags.size(); ++i) {
    if (!flag(flags[i]))
      continue;
    const auto property = bd::mem::load<uint32_t>(owner + strengths[i]);
    if (!Words(property, 12)) {
      ++stats.memory;
      return false;
    }
    tail.adjustments[i] = std::abs(bd::mem::load<float>(property + 4 + bank * 4)) > 0.0001f;
  }
  const bool composed = flag(8); // the same buffered bool gates both stages
  // sub_8221B1D8: buffered bool descriptors have their payload at +4;
  // the bright-pass object is +84, with scalar payloads at +652/+664.
  has_dof = flag(24);
  dof = {1, 0, 0, 0.5f}; // authored DoF-off, not an override of enabled DoF
  if (has_dof && !ReadDofProducerParameters(owner + 3440, dof)) {
    ++stats.inputs;
    return false;
  }
  const auto mode = bd::mem::load<int32_t>(owner + 12648 + bank * 4);
  bloom = MakeBloomParameters(bd::mem::load<float>(owner + 736 + bank * 4),
      bd::mem::load<float>(owner + 748 + bank * 4), composed, mode);
  // Comparison currently samples the existing folded composite's two inputs.
  // The original mode-1 third mask needs separate GPU qualification.
  if (mode == 1 && composed) {
    ++stats.effects;
    return false;
  }
  return true;
}
void RunTail(PPCContext &ctx, uint8_t *base, uint32_t owner, uint32_t source,
             const Tail &tail) {
  // These whole filter bodies are still compatibility producers. Keep exact
  // ordering/state and count execution; never call the original bloom scope.
  struct CallFrame {
    PPCContext &ctx;
    uint64_t stack;
    ~CallFrame() { ctx.r1.u64 = stack; ctx.fpscr.disableFlushMode(); }
  } frame{ctx, ctx.r1.u64};
  ctx.r1.u32 -= 256;
  bd::mem::store<uint32_t>(ctx.r1.u32, uint32_t(frame.stack));
  const auto state = [&](uint32_t value) {
    ctx.r3.u64 = 308;
    ctx.r4.u64 = value;
    bdSetRenderState(ctx, base);
    ++stats.state_calls;
  };
  const auto filter = [&](uint32_t offset, bool two_images) {
    ctx.r3.u64 = owner + offset;
    ctx.r4.u64 = source;
    ctx.r5.u64 = source;
    if (two_images) sub_8221E700(ctx, base);
    else sub_8221E758(ctx, base);
    ++stats.tail_calls;
  };
  if (tail.glare) {
    state(tail.saved_state);
    filter(8660, false);
  }
  state(0);
  if (tail.adjustments[0]) filter(6472, false);
  if (tail.adjustments[1]) filter(9500, true);
  if (tail.adjustments[2]) filter(10172, true);
  state(tail.saved_state);
}
} // namespace

void VerifyNativePostParameters(const BloomParameters &parameters) {
  if (!comparison)
    return;
  ++comparison_count;
  ++stats.comparisons;
  const auto same = [](float a, float b) {
    return a == b || (std::isfinite(a) && std::isfinite(b) &&
        std::abs(a - b) <= 2e-6f * std::max(1.0f, std::abs(b)));
  };
  bool match = same(parameters.threshold, comparison->threshold) &&
               same(parameters.intensity, comparison->intensity);
  for (size_t lane = 0; lane < 4; ++lane)
    match &= same(parameters.scene_weight[lane], comparison->scene_weight[lane]) &&
             same(parameters.bloom_weight[lane], comparison->bloom_weight[lane]);
  if (!match) {
    ++stats.wrong;
    BD_ERROR("[native-post] bloom native ({}, {}, w {} / {}) original ({}, {}, w {} / {})",
        comparison->threshold, comparison->intensity, comparison->scene_weight[0],
        comparison->bloom_weight[0], parameters.threshold, parameters.intensity,
        parameters.scene_weight[0], parameters.bloom_weight[0]);
    throw std::runtime_error("Native post parameter comparison failed");
  }
}

REX_HOOK_RAW(sub_8221B1D8) {
  if (ctx.r5.s32 != 3) // the original has no render work in other phases
    return;
  const auto owner = ctx.r3.u32, source = ctx.r4.u32;
  DofParameters dof;
  BloomParameters bloom;
  bool has_dof = false;
  Tail tail;
  if (ctx.r1.u32 >= 256 && Words(uint64_t(ctx.r1.u32) - 256, 328) &&
      ReadPlan(owner, dof, bloom, has_dof, tail)) {
    if (REXCVAR_GET(bd_native_post_verify)) {
      if (comparison)
        throw std::runtime_error("Nested native post comparison");
      comparison = &bloom;
      comparison_count = 0;
      ++stats.original;
      __imp__sub_8221B1D8(ctx, base);
      comparison = nullptr;
      // Startup may not yet have a readable depth/scene and therefore no host
      // composite. Report actual comparisons, not one check per entry.
      Report();
      return;
    }
    auto *scene = Texture(source);
    auto *depth = Texture(kDepth);
    auto *target = scene && scene->texture && depth && depth->texture
        ? HostTargetAcquire(HostTargetClass::PostColor, scene->width, scene->height,
                            0x1A2201BF, 1) : nullptr;
    if (target) {
      if (HostPostRender(scene, depth, target, dof, bloom)) {
        if (has_dof)
          PublishDofProducerProperties(owner + 3440);
        if (!Video::PublishSceneOutput(target, scene, 1.0f))
          throw std::runtime_error("Native post output publication failed");
        ReleaseResourceAdapter(target->selfVa);
        RunTail(ctx, base, owner, source, tail);
        ++stats.native;
        Report();
        return;
      }
      ReleaseResourceAdapter(target->selfVa);
    }
    ++stats.inputs;
  }
  ++stats.original;
  __imp__sub_8221B1D8(ctx, base);
  Report();
}
} // namespace bd::gpu
