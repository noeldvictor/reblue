/**
 * @file    native_post_bridge.cpp
 * @brief   Native DoF/bloom/flare scheduling and explicit completed post output.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/post_chain.h"
#include "gpu/post_parameters.h"
#include "gpu/post_sequence.h"
#include "gpu/lens_flare.h"
#include "gpu/post_adjustments.h"
#include "gpu/post_scanline.h"
#include "gpu/post_grade.h"
#include "gpu/post_heat.h"
#include "gpu/scene/native_transform_bridge.h"
#include "gpu/native_texture_mirror.h"
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
#include <vector>

REX_EXTERN(__imp__sub_8221B1D8);
REX_EXTERN(__imp__bdEffectSlotArrayApply);
REX_EXTERN(__imp__sub_8221E298);
REX_EXTERN(__imp__sub_822187D8);
REX_EXTERN(__imp__sub_82218E88);
REX_EXTERN(__imp__sub_82219008);
REX_EXTERN(__imp__sub_82219960);
REX_EXTERN(__imp__sub_82216AE8);
REXCVAR_DECLARE(bool, bd_native_post);
REXCVAR_DECLARE(bool, bd_native_post_verify);
REXCVAR_DECLARE(bool, bd_native_dof);
REXCVAR_DECLARE(bool, bd_native_dof_verify);
REXCVAR_DECLARE(bool, bd_host_targets);
REXCVAR_DECLARE(bool, bd_native_lensflare_preview);
REXCVAR_DECLARE(int32_t, bd_native_post_adjustment_preview);
REXCVAR_DECLARE(bool, bd_native_scanline_preview);
REXCVAR_DECLARE(bool, bd_ntsc_filter);
REXCVAR_DECLARE(int32_t, bd_native_grade_preview);
REXCVAR_DECLARE(bool, bd_native_heat_preview);
REXCVAR_DECLARE(int32_t, bd_native_bloom_preview);

namespace bd::gpu {
namespace {
constexpr uint32_t kThread = (uint32_t(-32035) << 16) - 26664;
constexpr uint32_t kDepth = (uint32_t(-32136) << 16) + 14888 + 16;
constexpr uint32_t kLensSource = (uint32_t(-32137) << 16) + 30160;
constexpr uint32_t kLensAttenuation = (uint32_t(-32137) << 16) + 12124;
constexpr uint32_t kPostOwner = (uint32_t(-32137) << 16) + 31212;
constexpr uint32_t kPhase = (uint32_t(-32137) << 16) + 16476;
struct Stats {
  uint64_t native = 0, original = 0, comparisons = 0, wrong = 0;
  uint64_t settings = 0, memory = 0, effects = 0, inputs = 0;
  uint64_t flare_frames = 0, flare_sprites = 0, flare_inactive = 0;
  uint64_t flare_checks = 0;
  uint64_t flare_refusals = 0;
  uint64_t fisheye_frames = 0, reverse_frames = 0, adjustment_checks = 0;
  uint64_t scanline_frames = 0, scanline_checks = 0, scanline_noisy_frames = 0;
  uint64_t grade_scopes = 0, grade_frames = 0, grain_frames = 0, grade_checks = 0;
  uint64_t heat_frames = 0, heat_checks = 0;
  uint64_t directional_bloom_frames = 0, directional_bloom_iterations = 0;
  uint64_t sequences = 0, sequence_roots = 0, sequence_empty = 0;
  uint64_t sequence_original = 0, sequence_refused = 0, sequence_max = 0;
  uint32_t frame = 0;
};
thread_local Stats stats;
thread_local const BloomParameters *comparison = nullptr;
thread_local uint32_t comparison_count = 0;
bool RefuseInput(const char *reason, uint32_t owner) {
  if (stats.inputs++ < 8)
    BD_WARN("[native-post] input refusal {} owner {:08X} frame {}",
            reason, owner, FrameStatFrameCount());
  return false;
}
void Report() {
  const auto frame = FrameStatFrameCount();
  if (frame - stats.frame < 300)
    return;
  BD_INFO("[native-post] frames {} original {} parameter checks {} wrong {}; "
          "refusals settings {} memory {} effects {} inputs {}; "
          "authored properties, image/output getters, other effects and UI remain",
          stats.native, stats.original, stats.comparisons, stats.wrong,
          stats.settings, stats.memory, stats.effects, stats.inputs);
  BD_INFO("[native-post] lens flare visible {} inactive {} native sprites {} parameter checks {} refusals {}; "
          "light/visibility and optical-image adapters remain; preview {}",
          stats.flare_frames, stats.flare_inactive, stats.flare_sprites, stats.flare_checks, stats.flare_refusals,
          REXCVAR_GET(bd_native_lensflare_preview));
  BD_INFO("[native-post] native fisheye {} reverse {} adjustment parameter checks {}; preview {}",
          stats.fisheye_frames, stats.reverse_frames, stats.adjustment_checks,
          REXCVAR_GET(bd_native_post_adjustment_preview));
  BD_INFO("[native-post] native scanline {} noisy {} authored strength checks {}; preview {}; "
          "no compatibility tail or state setters on the native path",
          stats.scanline_frames, stats.scanline_noisy_frames, stats.scanline_checks,
          REXCVAR_GET(bd_native_scanline_preview));
  BD_INFO("[native-post] native grading scopes {} active {} grain {} parameter checks {}; preview {}; "
          "no packed texture-list binding or gameplay RNG on the native path",
          stats.grade_scopes, stats.grade_frames, stats.grain_frames, stats.grade_checks,
          REXCVAR_GET(bd_native_grade_preview));
  stats.frame = frame;
  BD_INFO("[native-post] native heat shimmer {} authored checks {}; preview {}; "
          "depth-aware scene distortion fused before unwarped bloom; no guest phase writes",
          stats.heat_frames, stats.heat_checks, REXCVAR_GET(bd_native_heat_preview));
  BD_INFO("[native-post] native directional bloom {} iterations {}; preview {}; "
          "independent horizontal/vertical masks, no guest mask cache or resolves",
          stats.directional_bloom_frames, stats.directional_bloom_iterations,
          REXCVAR_GET(bd_native_bloom_preview));
  BD_INFO("[native-post] effect sequences {} roots {} empty {} max roots {}; "
          "original sequences {} refused {}; direct depth, no global depth publication",
          stats.sequences, stats.sequence_roots, stats.sequence_empty, stats.sequence_max,
          stats.sequence_original, stats.sequence_refused);
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
struct PostEffects {
  HeatShimmerParameters heat;
  GuestTexture *heat_image = nullptr;
  PostAdjustments adjustments;
  ScanlineParameters scanline;
  GradeParameters grade;
  bool grade_scope = false;
  GuestTexture *grain_image = nullptr;
  LensFlareParameters flare;
  std::array<GuestTexture *, 4> flare_images{};
};
struct PostPlan {
  uint32_t owner = 0; // authored-property/getter adapter, not the GPU stage identity
  DofParameters dof;
  BloomParameters bloom;
  bool has_dof = false;
  PostEffects tail;
};
struct PostTargets {
  std::array<GuestTexture *, 2> images{};
  ~PostTargets() {
    for (auto *image : images)
      if (image) ReleaseResourceAdapter(image->selfVa);
  }
};
thread_local const PostEffects *lens_comparison = nullptr;
thread_local uint32_t lens_comparison_owner = 0, lens_comparison_index = 0;
thread_local uint32_t adjustment_comparison_owner = 0, adjustment_comparison_mask = 0;
bool ReadLensFlare(uint32_t owner, uint32_t bank, bool enabled, PostEffects &tail) {
  const auto refuse = [&](const char *reason, uint32_t detail = 0) {
    if (stats.flare_refusals++ < 8)
      BD_WARN("[native-post] lens input refusal {} detail {} owner {:08X} bank {}",
              reason, detail, owner, bank);
    return false;
  };
  const bool preview = REXCVAR_GET(bd_native_lensflare_preview);
  if (!enabled && !preview) return true;
  if (!Words(kLensSource, 96) || !Words(kLensAttenuation, 4)) return refuse("source memory");
  const bool visible = preview || bd::mem::load<uint8_t>(kLensSource + 92) != 0;
  if (!visible) return true;
  const auto *transforms = scene::GetNativeRenderTransforms();
  if (!transforms) return refuse("native transforms");
  const std::array<float, 3> point{
      bd::mem::load<float>(kLensSource + 80), bd::mem::load<float>(kLensSource + 84),
      bd::mem::load<float>(kLensSource + 88)};
  auto position = ProjectLensFlare(point, transforms->inputs.view, transforms->inputs.projection);
  const auto scalar = [&](uint32_t offset) { return bd::mem::load<float>(owner + offset + bank * 4); };
  float visibility = 1;
  if (bd::mem::load<uint32_t>(owner + 672 + bank * 4) != 0) {
    visibility = 0;
    if (bd::mem::load<uint8_t>(kLensSource + 64) != 0) {
      const auto index = bd::mem::load<uint32_t>(kLensSource + 60);
      if (index > 1) return refuse("query index", index); // sub_82179440 creates two queries
      visibility = float(bd::mem::load<uint32_t>(kLensSource + 16 + index * 28)) / 16384.0f;
    }
  }
  std::array<float, 3> tint;
  for (uint32_t i = 0; i < 3; ++i)
    tint[i] = bd::mem::load<float>(owner + 644 + bank * 12 + i * 4);
  // Preview is a labelled, opt-in GPU coverage probe, not authored visibility
  // qualification. It changes native inputs only and never writes engine data.
  if (preview) position = {.25f, -.15f};
  tail.flare = MakeLensFlareParameters(true, position, preview ? .3f : scalar(632),
      preview ? 1.0f : scalar(620), tint, preview ? 1.0f : visibility,
      bd::mem::load<float>(kLensAttenuation));
  for (uint32_t i = 0; i < 4; ++i) {
    // Optical assets are engine-owned texture headers, not HostResourceHeap
    // allocations. Resolve their eagerly imported/cooked native mirrors;
    // never call a texture setter or infer a draw's retained slot.
    tail.flare_images[i] = ResolveGuestTexture(bd::mem::load<uint32_t>(owner + 712 + i * 32));
    if (!tail.flare_images[i]) return refuse("optical image", i);
  }
  return true;
}
bool ReadPlan(uint32_t owner, DofParameters &dof, BloomParameters &bloom,
              bool &has_dof, PostEffects &tail) {
  if (!REXCVAR_GET(bd_native_post) || !REXCVAR_GET(bd_host_targets) ||
      !REXCVAR_GET(bd_native_dof) || REXCVAR_GET(bd_native_dof_verify)) {
    ++stats.settings;
    return false;
  }
  if (!Words(owner, 12756) || !Words(kThread, 4) ||
      !rex::system::XThread::GetCurrentThread()) {
    ++stats.memory;
    return false;
  }
  const auto bank = rex::system::XThread::GetCurrentThreadId() ==
      bd::mem::load<uint32_t>(kThread) ? 0u : 1u;
  const auto flag = [&](uint32_t offset) {
    return bd::mem::load<uint8_t>(owner + offset + bank) == 1;
  };
  // Heat acts after DoF and bloom-mask preparation, before their composition.
  // Native coordinate selection distorts only the scene, never the bloom mask.
  tail.heat.enabled = flag(16);
  const auto heat_preview = REXCVAR_GET(bd_native_heat_preview);
  if (heat_preview && REXCVAR_GET(bd_native_post_verify))
    throw std::runtime_error("Synthetic heat cannot qualify authored parameters");
  if (tail.heat.enabled || heat_preview) {
    auto &heat = tail.heat;
    heat.enabled = true;
    heat.amplitude_x = bd::mem::load<float>(owner + 2712 + 684 + bank * 4);
    heat.amplitude_y = bd::mem::load<float>(owner + 2712 + 696 + bank * 4);
    heat.noise_scale = bd::mem::load<float>(owner + 2712 + 708 + bank * 4);
    heat.depth_power = bd::mem::load<float>(owner + 2712 + 720 + bank * 4);
    heat.phase = HeatShimmerFramePhase(FrameStatFrameCount());
    if (heat_preview) {
      heat.amplitude_x = heat.amplitude_y = .03f;
      heat.noise_scale = 1;
      heat.depth_power = 1;
    }
    tail.heat_image = ResolveGuestTexture(bd::mem::load<uint32_t>(owner + 2712 + 616));
    if (!tail.heat_image) return RefuseInput("heat image", owner);
  }
  tail.grade_scope = bd::mem::load<uint8_t>(owner + 48 + bank) != 0 ||
      bd::mem::load<uint8_t>(owner + 56 + bank) != 0 ||
      bd::mem::load<uint8_t>(owner + 80 + bank) != 0;
  auto &grade = tail.grade;
  if (tail.grade_scope) {
    // bdGetDoubleBufferPtr ignores its object argument and reads the shared
    // post owner. Resolve those authored flags explicitly, not via a callback.
    if (!Words(kPostOwner, 4)) { ++stats.memory; return false; }
    const auto flags_owner = bd::mem::load<uint32_t>(kPostOwner);
    if (!Words(flags_owner, 84)) { ++stats.memory; return false; }
    grade.discolor_strength = bd::mem::load<float>(owner + 7132 + 652 + bank * 4);
    grade.grain_strength = bd::mem::load<float>(owner + 7792 + 652 + bank * 4);
    grade.discolor = GradeStrengthEnabled(bd::mem::load<uint8_t>(flags_owner + 48 + bank),
                                         grade.discolor_strength);
    grade.grain = GradeStrengthEnabled(bd::mem::load<uint8_t>(flags_owner + 56 + bank),
                                      grade.grain_strength);
    grade.correction = bd::mem::load<uint8_t>(flags_owner + 80 + bank) != 0;
    const uint32_t color = owner + 10844;
    const auto rgb = [&](uint32_t offset) {
      return GradeColor{bd::mem::load<float>(color + offset),
          bd::mem::load<float>(color + offset + 4), bd::mem::load<float>(color + offset + 8)};
    };
    grade.gain = rgb(644 + bank * 12);
    grade.bias = rgb(672 + bank * 12);
    grade.target = rgb(700 + bank * 16);
    grade.gamma = bd::mem::load<float>(color + 620 + bank * 4);
    grade.saturation = bd::mem::load<float>(color + 632 + bank * 4);
    grade.blend = bd::mem::load<float>(color + 712 + bank * 16);
    AnimateGradeGrain(grade, FrameStatFrameCount(),
        bd::mem::load<uint32_t>(owner + 7792 + 664 + bank * 4) != 0);
  }
  const auto grade_preview = REXCVAR_GET(bd_native_grade_preview);
  if (grade_preview != 0) {
    if (grade_preview < 1 || grade_preview > 3 || REXCVAR_GET(bd_native_post_verify))
      throw std::runtime_error("Native grade preview is 1..3 and cannot qualify authored parameters");
    grade.discolor = grade.correction = true;
    grade.grain = grade_preview >= 2;
    grade.discolor_strength = .6f;
    grade.grain_strength = .15f;
    grade.gain = {1.1f, .95f, .8f};
    grade.bias = {.02f, .01f, 0};
    grade.target = {.1f, .15f, .2f};
    grade.gamma = 1.1f;
    grade.saturation = .7f;
    grade.blend = .1f;
    AnimateGradeGrain(grade, FrameStatFrameCount(), grade_preview == 3);
  }
  if (grade.grain) {
    tail.grain_image = ResolveGuestTexture(bd::mem::load<uint32_t>(
        owner + 7792 + 672 + grade.grain_image * 32));
    if (!tail.grain_image) return RefuseInput("grain image", owner);
  }
  if (!ReadLensFlare(owner + 8660, bank, flag(32), tail)) {
    return RefuseInput("lens flare", owner);
  }
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
    const bool enabled = std::abs(bd::mem::load<float>(property + 4 + bank * 4)) > 0.0001f;
    if (i == 0) {
      tail.adjustments.fisheye_enabled = enabled;
      if (enabled) tail.adjustments.fisheye = bd::mem::load<float>(owner + 6472 + 652 + bank * 4);
    } else if (i == 1) {
      tail.adjustments.reverse_enabled = enabled;
      if (enabled) {
        tail.adjustments.reverse_strength = bd::mem::load<float>(owner + 9500 + 620 + bank * 4);
        tail.adjustments.reverse_pivot = bd::mem::load<float>(owner + 9500 + 632 + bank * 4);
      }
    } else if (enabled) {
      tail.scanline = {bd::mem::load<float>(owner + 10172 + 620 + bank * 4),
          ScanlineFramePhase(bd::mem::load<float>(owner + 10172 + 632 + bank * 4),
              REXCVAR_GET(bd_ntsc_filter), FrameStatFrameCount()), true};
    }
  }
  if (REXCVAR_GET(bd_native_scanline_preview)) {
    if (REXCVAR_GET(bd_native_post_verify))
      throw std::runtime_error("Synthetic scanlines cannot qualify authored parameters");
    tail.scanline = {1.0f, ScanlineFramePhase(4.0f, REXCVAR_GET(bd_ntsc_filter),
                                            FrameStatFrameCount()), true};
  }
  const auto preview = REXCVAR_GET(bd_native_post_adjustment_preview);
  if (preview != 0) {
    if (preview != 1 && preview != 2)
      throw std::runtime_error("Native post adjustment preview must be 0, 1 or 2");
    if (REXCVAR_GET(bd_native_post_verify))
      throw std::runtime_error("Synthetic adjustments cannot qualify authored parameters");
    // Only native inputs change. Do not rewrite authored properties or flags.
    tail.adjustments = {preview == 1 ? 0.75f : -0.75f, 1.0f, 1.0f, true, true};
  }
  const bool composed = flag(8); // the same buffered bool gates both stages
  // sub_8221B1D8: buffered bool descriptors have their payload at +4;
  // the bright-pass object is +84, with scalar payloads at +652/+664.
  has_dof = flag(24);
  dof = {1, 0, 0, 0.5f}; // authored DoF-off, not an override of enabled DoF
  if (has_dof && !ReadDofProducerParameters(owner + 3440, dof)) {
    return RefuseInput("DoF properties/transforms", owner);
  }
  const auto mode = bd::mem::load<int32_t>(owner + 12648 + bank * 4);
  bloom = MakeBloomParameters(bd::mem::load<float>(owner + 736 + bank * 4),
      bd::mem::load<float>(owner + 748 + bank * 4), composed, mode);
  if (bloom.directional.enabled) {
    // sub_8221B1D8 and the property serializer use these buffered scalar
    // descriptors. Import intent only, never the mutable blur object or cache.
    bloom.directional.sigma = bd::mem::load<float>(owner + 12612 + bank * 4);
    bloom.directional.gain = bd::mem::load<float>(owner + 12624 + bank * 4);
    bloom.directional.iterations = uint32_t(std::max(0,
        bd::mem::load<int32_t>(owner + 12636 + bank * 4)));
  }
  const auto bloom_preview = REXCVAR_GET(bd_native_bloom_preview);
  if (bloom_preview != 0) {
    if (bloom_preview < 1 || bloom_preview > 2 || REXCVAR_GET(bd_native_post_verify))
      throw std::runtime_error("Native bloom preview is 1..2 and cannot qualify authored parameters");
    // Native-only coverage: paired masks, or the zero-iteration shared bright
    // image. Never modify authored properties to force an event.
    bloom = MakeBloomParameters(.04f, 8, true, 1);
    bloom.directional = {3, 1, bloom_preview == 1 ? 2u : 0u, true};
  }
  return true;
}
bool RenderPostPlan(const PostPlan &plan, GuestTexture *scene, GuestTexture *depth,
                    GuestTexture *target) {
  const auto &tail = plan.tail;
  if (HostPostRender(scene, depth, target, plan.dof, plan.bloom, tail.flare, tail.flare_images,
                     tail.adjustments, tail.scanline, tail.grade, tail.grain_image,
                     tail.heat, tail.heat_image)) {
    if (plan.has_dof)
      PublishDofProducerProperties(plan.owner + 3440);
    if (!Video::PublishSceneOutput(target, scene, 1.0f))
      throw std::runtime_error("Native post output publication failed");
    stats.flare_frames += tail.flare.count != 0;
    stats.flare_inactive += tail.flare.count == 0;
    stats.flare_sprites += tail.flare.count;
    stats.fisheye_frames += tail.adjustments.fisheye_enabled;
    stats.reverse_frames += tail.adjustments.reverse_enabled;
    stats.scanline_frames += tail.scanline.enabled;
    stats.scanline_noisy_frames += tail.scanline.enabled && tail.scanline.phase != 0;
    stats.grade_scopes += tail.grade_scope;
    stats.grade_frames += tail.grade.Active();
    stats.grain_frames += tail.grade.grain;
    stats.heat_frames += tail.heat.enabled;
    stats.directional_bloom_frames += plan.bloom.directional.enabled;
    if (plan.bloom.directional.enabled)
      stats.directional_bloom_iterations += plan.bloom.directional.iterations;
    ++stats.native;
    return true;
  }
  return false;
}
bool AcquirePostTargets(GuestTexture *scene, GuestTexture *depth, uint32_t count,
                        PostTargets &targets) {
  if (!scene || !scene->texture || !depth || !depth->texture || !count || count > 2)
    return false;
  constexpr std::array roles{HostTargetClass::PostColor, HostTargetClass::PostColorAlternate};
  for (uint32_t i = 0; i < count; ++i) {
    targets.images[i] = HostTargetAcquire(roles[i], scene->width, scene->height, 0x1A2201BF, 1);
    if (!targets.images[i]) return false;
  }
  return true;
}
bool RunEffectSequence(uint32_t list, uint32_t color_container, uint32_t depth_container) {
  if (!REXCVAR_GET(bd_native_post) || REXCVAR_GET(bd_native_post_verify) ||
      !REXCVAR_GET(bd_host_targets) || !REXCVAR_GET(bd_native_dof) ||
      REXCVAR_GET(bd_native_dof_verify))
    return false;
  const auto refuse = [&](const char *reason, uint32_t detail = 0) {
    if (stats.sequence_refused++ < 8)
      BD_WARN("[native-post] sequence refusal {} detail {:08X} frame {}",
              reason, detail, FrameStatFrameCount());
    return false;
  };
  if (!Words(list, 48) || !Words(kPhase, 4)) return refuse("list memory", list);
  if (!bd::mem::load<uint8_t>(list + 12)) {
    ++stats.sequence_empty;
    return true;
  }
  // The original leaves its temporary depth getter empty. Do not suppress
  // release/cleanup of a pre-existing compatibility scope or nested publication.
  if (bd::mem::load<uint32_t>(list + 16)) return refuse("active depth getter", list);
  const auto signed_count = bd::mem::load<int32_t>(list + 8);
  const auto sequence = MakePostSequence(uint32_t(std::max(0, signed_count)));
  if (!sequence) return refuse("capacity", uint32_t(signed_count));
  if (!sequence->count) {
    ++stats.sequence_empty;
    return true;
  }
  const auto array = bd::mem::load<uint32_t>(list);
  if (!Words(array, uint64_t(sequence->count) * 4)) return refuse("entry memory", array);
  const auto phase = bd::mem::load<int32_t>(kPhase);
  std::vector<PostPlan> plans(sequence->count);
  // Snapshot and validate the complete ordered list before executing any root.
  // Unknown callbacks cannot be skipped or replayed after a partial native list.
  for (uint32_t i = 0; i < sequence->count; ++i) {
    auto &plan = plans[i];
    plan.owner = bd::mem::load<uint32_t>(array + i * 4);
    if (!Words(plan.owner, 4)) return refuse("owner memory", plan.owner);
    const auto vtable = bd::mem::load<uint32_t>(plan.owner);
    if (!Words(vtable, 4)) return refuse("callback memory", vtable);
    const auto callback = bd::mem::load<uint32_t>(vtable);
    if (callback != 0x8221B1D8u) return refuse("unsupported callback", callback);
    if (phase == 3 && !ReadPlan(plan.owner, plan.dof, plan.bloom, plan.has_dof, plan.tail))
      return refuse("authored plan", plan.owner);
  }
  if (phase != 3) { // all validated whole-post callbacks are no-ops in other phases
    ++stats.sequence_empty;
    return true;
  }
  auto *scene = Texture(color_container);
  auto *depth = Texture(depth_container);
  PostTargets targets;
  if (!AcquirePostTargets(scene, depth, sequence->target_count, targets))
    return refuse("image targets");
  for (uint32_t i = 0; i < sequence->count; ++i) {
    auto &plan = plans[i];
    // A preceding local-focus root publishes the shared authored focus getter.
    // Later roots must observe that ordered update, not their preflight value.
    if (i && plan.has_dof && !ReadDofProducerParameters(plan.owner + 3440, plan.dof))
      throw std::runtime_error("Native effect sequence lost its preflighted focus input");
    if (!RenderPostPlan(plan, scene, depth, targets.images[sequence->Output(i)])) {
      if (i) throw std::runtime_error("Native effect sequence refused after a completed root");
      return refuse("first root image preflight");
    }
  }
  ++stats.sequences;
  stats.sequence_roots += sequence->count;
  stats.sequence_max = std::max(stats.sequence_max, uint64_t(sequence->count));
  return true;
}
void VerifyAdjustmentPublication(uint32_t owner, uint32_t descriptor_offset,
                                  uint32_t register_offset,
                                  const std::array<float, 4> &expected, uint32_t lanes,
                                  uint32_t first_lane = 0) {
  const auto descriptor = bd::mem::load<uint32_t>(owner + descriptor_offset);
  if (!Words(descriptor, 16)) throw std::runtime_error("Missing adjustment descriptor");
  const uint64_t address = uint64_t(bd::mem::load<uint32_t>(descriptor + 12)) +
      uint64_t(bd::mem::load<uint32_t>(owner + register_offset)) * 16 + first_lane * 4;
  if (!Words(address, lanes * 4)) throw std::runtime_error("Missing adjustment publication");
  for (uint32_t lane = 0; lane < lanes; ++lane) {
    const auto actual = bd::mem::load<float>(uint32_t(address) + lane * 4);
    if (actual != expected[lane] && !(std::isnan(actual) && std::isnan(expected[lane]))) {
      ++stats.wrong;
      throw std::runtime_error("Native post adjustment parameter mismatch");
    }
  }
}
} // namespace

REX_HOOK_RAW(sub_822187D8) {
  const auto owner = ctx.r3.u32;
  __imp__sub_822187D8(ctx, base);
  // This original producer is also used by discolor; scope by exact object.
  if (lens_comparison && owner == adjustment_comparison_owner + 6472) {
    if (!lens_comparison->adjustments.fisheye_enabled || (adjustment_comparison_mask & 1))
      throw std::runtime_error("Unexpected original fisheye producer");
    VerifyAdjustmentPublication(owner, 632, 640, {lens_comparison->adjustments.fisheye, 0}, 1);
    ++stats.adjustment_checks;
    adjustment_comparison_mask |= 1;
  }
}
REX_HOOK_RAW(sub_82218E88) {
  const auto owner = ctx.r3.u32;
  __imp__sub_82218E88(ctx, base);
  if (lens_comparison && owner == adjustment_comparison_owner + 9500) {
    if (!lens_comparison->adjustments.reverse_enabled || (adjustment_comparison_mask & 2))
      throw std::runtime_error("Unexpected original reverse producer");
    VerifyAdjustmentPublication(owner, 656, 664,
        {lens_comparison->adjustments.reverse_strength, lens_comparison->adjustments.reverse_pivot}, 2);
    ++stats.adjustment_checks;
    adjustment_comparison_mask |= 2;
  }
}

REX_HOOK_RAW(sub_82219008) {
  const auto owner = ctx.r3.u32;
  __imp__sub_82219008(ctx, base);
  if (lens_comparison && owner == adjustment_comparison_owner + 10172) {
    if (!lens_comparison->scanline.enabled || (adjustment_comparison_mask & 4))
      throw std::runtime_error("Unexpected original scanline producer");
    // Only authored strength is compared. Native dimensions come from the
    // image and native phase deliberately does not consume gameplay rand().
    VerifyAdjustmentPublication(owner, 656, 664, {lens_comparison->scanline.strength, 0}, 1, 2);
    ++stats.scanline_checks;
    adjustment_comparison_mask |= 4;
  }
}

REX_HOOK_RAW(sub_82216AE8) {
  const auto owner = ctx.r3.u32;
  __imp__sub_82216AE8(ctx, base);
  if (lens_comparison && owner == adjustment_comparison_owner + 2712) {
    const auto &heat = lens_comparison->heat;
    if (!heat.enabled || (adjustment_comparison_mask & 16))
      throw std::runtime_error("Unexpected original heat-shimmer producer");
    VerifyAdjustmentPublication(owner, 664, 672,
        {heat.amplitude_x, heat.amplitude_y, heat.noise_scale, heat.depth_power}, 4);
    ++stats.heat_checks;
    adjustment_comparison_mask |= 16;
  }
}

REX_HOOK_RAW(sub_82219960) {
  const auto owner = ctx.r3.u32;
  __imp__sub_82219960(ctx, base);
  if (lens_comparison && owner == adjustment_comparison_owner + 11648) {
    if (!lens_comparison->grade_scope || (adjustment_comparison_mask & 8))
      throw std::runtime_error("Unexpected original packed-grade producer");
    const auto &g = lens_comparison->grade;
    VerifyAdjustmentPublication(owner, 632, 640, {g.discolor_strength}, 1);
    VerifyAdjustmentPublication(owner, 652, 660, {g.grain_strength}, 1);
    VerifyAdjustmentPublication(owner, 672, 680, {g.gain.r, g.gain.g, g.gain.b, g.gamma}, 4);
    VerifyAdjustmentPublication(owner, 692, 700, {g.bias.r, g.bias.g, g.bias.b, g.saturation}, 4);
    VerifyAdjustmentPublication(owner, 712, 720, {g.target.r, g.target.g, g.target.b, g.blend}, 4);
    const std::array<bool, 3> enabled{g.discolor, g.grain, g.correction};
    for (uint32_t i = 0; i < enabled.size(); ++i) {
      const auto descriptor = bd::mem::load<uint32_t>(owner + 744 + i * 16);
      if (!Words(descriptor, 16)) throw std::runtime_error("Missing grade flag descriptor");
      const uint64_t address = uint64_t(bd::mem::load<uint32_t>(descriptor + 12)) +
          uint64_t(bd::mem::load<uint32_t>(owner + 752 + i * 16)) * 4;
      if (!Words(address, 4) || (bd::mem::load<uint32_t>(uint32_t(address)) != 0) != enabled[i]) {
        ++stats.wrong;
        throw std::runtime_error("Native grade activation mismatch");
      }
    }
    ++stats.grade_checks;
    adjustment_comparison_mask |= 8;
  }
}

REX_HOOK_RAW(sub_8221E298) {
  if (lens_comparison) {
    if (lens_comparison_index >= lens_comparison->flare.count)
      throw std::runtime_error("Unexpected original lens-flare sprite");
    const auto &sprite = lens_comparison->flare.sprites[lens_comparison_index++];
    const std::array<float, 4> rectangle{float(ctx.f1.f64) / 1280.0f,
        float(ctx.f2.f64) / 720.0f, float(ctx.f3.f64) / 1280.0f, float(ctx.f4.f64) / 720.0f};
    const auto descriptor = bd::mem::load<uint32_t>(lens_comparison_owner + 696);
    if (!Words(descriptor, 16)) throw std::runtime_error("Missing flare comparison descriptor");
    const uint64_t color_address = uint64_t(bd::mem::load<uint32_t>(descriptor + 12)) +
        uint64_t(bd::mem::load<uint32_t>(lens_comparison_owner + 704)) * 16;
    if (!Words(color_address, 16)) throw std::runtime_error("Missing flare comparison values");
    for (uint32_t lane = 0; lane < 4; ++lane) {
      const float color = bd::mem::load<float>(uint32_t(color_address) + lane * 4);
      const auto same = [](float a, float b) {
        return a == b || (std::isfinite(a) && std::isfinite(b) &&
            std::abs(a - b) <= 2e-6f * std::max(1.0f, std::abs(b)));
      };
      if (!same(rectangle[lane], sprite.rect[lane]) || !same(color, sprite.color[lane])) {
        ++stats.wrong;
        BD_ERROR("[native-post] lens sprite {} lane {} rectangle native {} original {}; color {} / {}",
            lens_comparison_index - 1, lane, sprite.rect[lane], rectangle[lane], sprite.color[lane], color);
        throw std::runtime_error("Native lens-flare parameter mismatch");
      }
    }
    ++stats.flare_checks;
  }
  __imp__sub_8221E298(ctx, base);
}

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
  PostPlan plan;
  plan.owner = owner;
  auto &dof = plan.dof;
  auto &bloom = plan.bloom;
  auto &has_dof = plan.has_dof;
  auto &tail = plan.tail;
  if (ReadPlan(owner, dof, bloom, has_dof, tail)) {
    if (REXCVAR_GET(bd_native_post_verify)) {
      if (comparison)
        throw std::runtime_error("Nested native post comparison");
      comparison = &bloom;
      if (REXCVAR_GET(bd_native_lensflare_preview))
        throw std::runtime_error("Synthetic flare preview cannot qualify original parameters");
      lens_comparison = &tail;
      lens_comparison_owner = owner + 8660;
      lens_comparison_index = 0;
      adjustment_comparison_owner = owner;
      adjustment_comparison_mask = 0;
      comparison_count = 0;
      ++stats.original;
      __imp__sub_8221B1D8(ctx, base);
      if (lens_comparison_index != tail.flare.count)
        throw std::runtime_error("Original lens-flare sprite count differs");
      const auto expected_adjustments = (tail.adjustments.fisheye_enabled ? 1u : 0u) |
                                        (tail.adjustments.reverse_enabled ? 2u : 0u) |
                                        (tail.scanline.enabled ? 4u : 0u) |
                                        (tail.grade_scope ? 8u : 0u) |
                                        (tail.heat.enabled ? 16u : 0u);
      if (adjustment_comparison_mask != expected_adjustments)
        throw std::runtime_error("Original adjustment producer count differs");
      lens_comparison = nullptr;
      comparison = nullptr;
      // Report actual bloom/sprite comparisons, not one check per root entry.
      Report();
      return;
    }
    auto *scene = Texture(source);
    auto *depth = Texture(kDepth);
    PostTargets targets;
    if (AcquirePostTargets(scene, depth, 1, targets) &&
        RenderPostPlan(plan, scene, depth, targets.images[0])) {
      Report();
      return;
    }
    if (stats.inputs < 8)
      BD_WARN("[native-post] render refusal frame {} scene {:08X} depth {:08X} target {}",
              FrameStatFrameCount(), scene ? scene->selfVa : 0, depth ? depth->selfVa : 0,
              targets.images[0] != nullptr);
    ++stats.inputs;
  }
  ++stats.original;
  __imp__sub_8221B1D8(ctx, base);
  Report();
}
REX_HOOK_RAW(bdEffectSlotArrayApply) {
  if (!RunEffectSequence(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32)) {
    ++stats.sequence_original;
    __imp__bdEffectSlotArrayApply(ctx, base);
  }
  // The original has no defined result; bdRenderViewSubmit overwrites r3
  // immediately. No guest stack frame or global depth getter is needed here.
  Report();
}
} // namespace bd::gpu
