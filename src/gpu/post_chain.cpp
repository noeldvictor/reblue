/**
 * @file    gpu/post_chain.cpp
 * @brief   The host-owned post chain. See post_chain.h.
 *
 * Blue Dragon's post chain (recorded with bd_dump_post_draws, 2026-09-02, a
 * field frame at 1280x720) is fifteen full-screen quads, each rendered into
 * the EDRAM tile and resolved out again:
 *
 *   quoter    x5   1280 -> 640 -> 320 -> 160 -> 80 -> 80   (bilinear copies)
 *   ms_weight x5   13-tap weighted blur of each level, in place
 *   dof            full-res composite: depth (slot 0), scene (slot 1), the
 *                  five blurred levels (slots 2-6)
 *   brightpass     1280 -> 320, threshold/intensity in c27
 *   ms_weight x2   13-tap blur of the mask, twice
 *   ms_tex         full-res composite: scene (slot 0) + mask (slot 1)
 *
 * The host does all of it in nine passes, none through the tile and none
 * resolved: at the dof draw one dual-filter downsample per level into the
 * very texture objects slots 2-6 name, and at the ms_tex draw the bright
 * mask, two blur directions, and one composite that is bd_pe_ps_dof and
 * bd_pe_ps_ms_tex folded together, written straight into the frame. Every
 * guest post draw is dropped. The guest's parameters are read from its live
 * constant block at the draw it would have made (c27 at dof, c13/c14 at
 * ms_tex).
 *
 * The first host chain (same day, earlier) kept the guest's two composites
 * and only produced their inputs: the Quest measured 38.0 ms against 37.5,
 * because the small passes cost what the guest's small passes cost. Fewer
 * passes is the lever, not moving them.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/post_chain.h"

#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/backend.h"
#include "gpu/constant_buffers.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/draw_queue.h"
#include "gpu/frame.h"
#include "gpu/frame_stats.h"
#include "gpu/resources.h"

#if defined(REBLUE_D3D12)
#include "src/gpu/shaders/hlsl/post_blur_ps.hlsl.dxil.h"
#include "src/gpu/shaders/hlsl/post_bright_ps.hlsl.dxil.h"
#include "src/gpu/shaders/hlsl/post_composite_ps.hlsl.dxil.h"
#include "src/gpu/shaders/hlsl/post_down_ps.hlsl.dxil.h"
#include "src/gpu/shaders/hlsl/post_dual_down_ps.hlsl.dxil.h"
#else
#include "src/gpu/shaders/hlsl/post_blur_ps.hlsl.spirv.h"
#include "src/gpu/shaders/hlsl/post_bright_ps.hlsl.spirv.h"
#include "src/gpu/shaders/hlsl/post_composite_ps.hlsl.spirv.h"
#include "src/gpu/shaders/hlsl/post_down_ps.hlsl.spirv.h"
#include "src/gpu/shaders/hlsl/post_dual_down_ps.hlsl.spirv.h"
#endif

REXCVAR_DECLARE(bool, bd_host_post);
REXCVAR_DECLARE(bool, bd_host_post_composite);
REXCVAR_DECLARE(i32, bd_host_post_debug);
REXCVAR_DECLARE(f64, bd_host_post_blur);

namespace bd::gpu {
namespace {

// The guest's post pixel shaders, by XenosRecomp cache hash
// (tools/shader_cache/shader_hashes.csv).
constexpr u64 kQuoter = 0x77344D98A7F5B956ull;
constexpr u64 kMsWeight = 0x1E2676BD7DBBE4F7ull;
constexpr u64 kBrightPass = 0xFFDBD782126EB6E8ull;
constexpr u64 kDof = 0xF6FF1BED057E0FC4ull;
constexpr u64 kMsTex = 0x620B403BCBBF1B98ull;

enum class Shader : u32 {
  Down = 0,
  Blur = 1,
  Bright = 2,
  DualDown = 3,
  Composite = 4,
  Count
};

// Same layout as the copy passes' push block (copy_common.hlsli).
struct PostPush {
  u32 src;
  u32 src2;
  float param0;
  float param1;
};

// The composite's parameter block; layout documented in post_composite_ps.
struct CompositeConstants {
  float dof[4];
  float w0[4];
  float w1[4];
  u32 indices0[4];
  u32 indices1[4];
};

struct Scratch {
  std::unique_ptr<plume::RenderTexture> texture;
  std::unique_ptr<plume::RenderFramebuffer> framebuffer;
  u32 width = 0;
  u32 height = 0;
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  u32 slot = kInvalidDescriptorIndex;
  plume::RenderTextureLayout layout = plume::RenderTextureLayout::UNKNOWN;
  // Two per size: a blur is two passes and the second reads the first.
  u32 role = 0;
};

// What the dof draw bound, kept for the composite at the ms_tex draw.
struct DofInputs {
  GuestTexture *depth = nullptr;
  GuestTexture *levels[5] = {};
  float scene_scale = 1.0f; // what the composite multiplies the scene tap by
  float params[4] = {0, 0, 0, 0}; // c27.x, .y, .z, .w
  bool valid = false;
};

struct Chain {
  // The bloom mask texture the host wrote last, for HostPostWillOverwrite.
  GuestTexture *bloom_mask = nullptr;
  std::unique_ptr<plume::RenderShader> shaders[u32(Shader::Count)];
  std::unordered_map<u64, std::unique_ptr<plume::RenderPipeline>> pipelines;
  std::vector<std::unique_ptr<Scratch>> scratch;
  DofInputs dof;
  bool failed = false;
  u32 dof_frames = 0;
  u32 bloom_frames = 0;
  u32 composite_frames = 0;
  u32 skipped = 0;
};

Chain &chain() {
  static Chain c;
  return c;
}

bool EnsureShaders(VideoState &s, Chain &c) {
  if (c.shaders[0])
    return true;
  if (!s.device)
    return false;
  c.shaders[u32(Shader::Down)] = s.device->createShader(
      REBLUE_SHADER_BLOB(post_down_ps), "main", kHostShaderFormat);
  c.shaders[u32(Shader::Blur)] = s.device->createShader(
      REBLUE_SHADER_BLOB(post_blur_ps), "main", kHostShaderFormat);
  c.shaders[u32(Shader::Bright)] = s.device->createShader(
      REBLUE_SHADER_BLOB(post_bright_ps), "main", kHostShaderFormat);
  c.shaders[u32(Shader::DualDown)] = s.device->createShader(
      REBLUE_SHADER_BLOB(post_dual_down_ps), "main", kHostShaderFormat);
  c.shaders[u32(Shader::Composite)] = s.device->createShader(
      REBLUE_SHADER_BLOB(post_composite_ps), "main", kHostShaderFormat);
  for (auto &sh : c.shaders) {
    if (!sh) {
      BD_ERROR("[post] host post shaders failed to create; chain disabled");
      c.failed = true;
      return false;
    }
  }
  return true;
}

plume::RenderPipeline *Pipeline(VideoState &s, Chain &c, Shader which,
                                plume::RenderFormat format) {
  const u64 key = (u64(which) << 32) | u64(format);
  auto it = c.pipelines.find(key);
  if (it != c.pipelines.end())
    return it->second.get();
  if (!EnsureShaders(s, c) || !s.copy_vs || !s.pipeline_layout)
    return nullptr;
  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = s.pipeline_layout.get();
  desc.vertexShader = s.copy_vs.get();
  desc.pixelShader = c.shaders[u32(which)].get();
  desc.depthFunction = plume::RenderComparisonFunction::ALWAYS;
  desc.depthEnabled = false;
  desc.depthWriteEnabled = false;
  desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  desc.cullMode = plume::RenderCullMode::NONE;
  desc.renderTargetCount = 1;
  desc.renderTargetFormat[0] = format;
  desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
  desc.depthTargetFormat = plume::RenderFormat::UNKNOWN;
  auto pipe = CreateHostGraphicsPipeline(s.device.get(), desc, "host-post");
  if (!pipe) {
    BD_ERROR("[post] pipeline {} for format {} failed; chain disabled",
             u32(which), u32(format));
    c.failed = true;
    return nullptr;
  }
  plume::RenderPipeline *raw = pipe.get();
  c.pipelines.emplace(key, std::move(pipe));
  return raw;
}

Scratch *GetScratch(VideoState &s, Chain &c, u32 width, u32 height,
                    plume::RenderFormat format, u32 role) {
  for (auto &sc : c.scratch)
    if (sc->width == width && sc->height == height && sc->format == format &&
        sc->role == role)
      return sc.get();
  auto sc = std::make_unique<Scratch>();
  sc->width = width;
  sc->height = height;
  sc->format = format;
  sc->role = role;
  sc->texture = CreateHostTexture(
      s.device.get(), plume::RenderTextureDesc::ColorTarget(width, height, format),
      "host-post-scratch");
  if (!sc->texture) {
    BD_ERROR("[post] scratch {}x{} failed; chain disabled", width, height);
    c.failed = true;
    return nullptr;
  }
  const plume::RenderTexture *attachments[1] = {sc->texture.get()};
  sc->framebuffer = s.device->createFramebuffer(
      plume::RenderFramebufferDesc(attachments, 1));
  if (!sc->framebuffer) {
    BD_ERROR("[post] scratch framebuffer failed; chain disabled");
    c.failed = true;
    return nullptr;
  }
  sc->slot = Video::AllocateBindlessTextureSlot();
  if (sc->slot == kInvalidDescriptorIndex) {
    BD_ERROR("[post] no bindless slot for scratch; chain disabled");
    c.failed = true;
    return nullptr;
  }
  SetBindlessTextureLocked(s, sc->slot, sc->texture.get(), nullptr);
  Scratch *raw = sc.get();
  c.scratch.emplace_back(std::move(sc));
  BD_INFO("[post] scratch {}x{} role {} slot {}", width, height, role, raw->slot);
  return raw;
}

void Transition(VideoState &s, plume::RenderTexture *texture,
                plume::RenderTextureLayout &current,
                plume::RenderTextureLayout wanted) {
  if (current == wanted)
    return;
  const plume::RenderTextureBarrier b(texture, wanted);
  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, &b, 1);
  NoteBarrierCall(1, BarrierSite::Resolve);
  current = wanted;
}

// One full-screen pass into fb at width x height.
void Pass(VideoState &s, plume::RenderPipeline *pipeline,
          plume::RenderFramebuffer *fb, u32 width, u32 height,
          const PostPush &push) {
  auto *cmd = s.command_list;
  cmd->setFramebuffer(fb);
  cmd->setPipeline(pipeline);
  cmd->setViewports(plume::RenderViewport(0.0f, 0.0f, float(width), float(height)));
  cmd->setScissors(plume::RenderRect(0, 0, i32(width), i32(height)));
  cmd->setGraphicsPushConstants(kCopyPushConstantRangeIndex, &push,
                                kCopyPushConstantByteOffset, sizeof(push));
  cmd->drawInstanced(3, 1, 0, 0);
  cmd->setFramebuffer(nullptr);
}

// The surface that holds a guest texture's content: a deferred resolve leaves
// it in the source surface until something forces the copy.
GuestTexture *Content(GuestTexture *t) {
  if (!t)
    return nullptr;
  if (t->sourceSurface && t->sourceSurface != t && t->sourceSurface->texture)
    return t->sourceSurface;
  return t;
}

bool Readable(GuestTexture *t) {
  return t && t->texture && t->descriptorIndex != kInvalidDescriptorIndex &&
         t->layers == 1;
}

// Content, transitioned for sampling; null when it cannot be sampled.
GuestTexture *Source(VideoState &s, GuestTexture *t) {
  GuestTexture *src = Content(t);
  if (!Readable(src))
    return nullptr;
  Transition(s, src->texture, src->layout, plume::RenderTextureLayout::SHADER_READ);
  return src;
}

// The factor a reader of Source(t) applies: a scaled resolve that aliases
// (the HDR scene at x0.25) hands out the unscaled surface.
float SourceScale(const GuestTexture *t) {
  if (t && t->sourceSurface && t->sourceSurface != t &&
      t->sourceSurface->texture && t->resolveScale != 1.0f)
    return t->resolveScale;
  return 1.0f;
}

// A guest texture about to be written by the host: drop any resolve link
// pointing into it (the host content replaces what the copy would bring) and
// make it a colour attachment.
plume::RenderFramebuffer *BeginGuestTarget(VideoState &s, GuestTexture *dst) {
  if (!dst || !dst->texture || dst->layers != 1)
    return nullptr;
  DetachSourceSurfaceLocked(s, dst);
  plume::RenderFramebuffer *fb = GetFramebuffer(s, dst, nullptr);
  if (!fb)
    return nullptr;
  Transition(s, dst->texture, dst->layout, plume::RenderTextureLayout::COLOR_WRITE);
  return fb;
}

void EndGuestTarget(VideoState &s, GuestTexture *dst) {
  Transition(s, dst->texture, dst->layout, plume::RenderTextureLayout::SHADER_READ);
}

// After host passes the guest draw's framebuffer, viewport and pipeline have
// to come back; the draw path flushes them again when the flags say so.
void RestoreGuestDraw(VideoState &s) {
  plume::RenderFramebuffer *fb =
      GetFramebuffer(s, s.render_target, s.depth_stencil);
  if (fb)
    s.command_list->setFramebuffer(fb);
  s.dirtyStates.viewport = true;
  s.dirtyStates.scissorRect = true;
  s.dirtyStates.pipelineState = true;
  Video::FlushViewport();
}

float GuestPixelConstant(u32 device_guest, u32 reg, u32 lane) {
  const auto *device = bd::mem::at<const D3DDevice>(device_guest);
  if (!device || reg >= 256 || lane >= 4)
    return 0.0f;
  return float(device->psFloatConstants[reg][lane]);
}

// The dof draw samples depth in slot 0, the scene in slot 1 and five blurred
// levels in slots 2-6 (1/2, 1/4, 1/8, 1/16, 1/16). One dual-filter pass per
// level, each from the previous.
bool BuildDofPyramid(VideoState &s, Chain &c, u32 device_guest) {
  c.dof = DofInputs{};
  GuestTexture *scene = Source(s, s.textures[1]);
  if (!scene)
    return false;
  auto *dual = Pipeline(s, c, Shader::DualDown, scene->format);
  if (!dual)
    return false;
  u32 prev_slot = scene->descriptorIndex;
  // Param1 of the first level: the scene's resolve scale when the scene is an
  // alias of the unscaled surface. 0 means 1 to the shader.
  float level_scale = SourceScale(s.textures[1]);
  c.dof.scene_scale = level_scale;
  u32 filled = 0;
  for (u32 slot = 2; slot <= 6; ++slot) {
    GuestTexture *dst = s.textures[slot];
    if (!dst || !dst->texture || dst->layers != 1 || dst->width == 0)
      break;
    plume::RenderFramebuffer *fb = BeginGuestTarget(s, dst);
    if (!fb)
      break;
    auto *pipe = dst->format == scene->format
                     ? dual
                     : Pipeline(s, c, Shader::DualDown, dst->format);
    if (!pipe)
      break;
    // A level the size of its predecessor (the guest's 80x45 pair) just
    // blurs it again. The kernel is twice its nominal width: the guest's
    // first level is what its depth-of-field lerps toward below level one,
    // and at the nominal width the distance stayed sharp (desktop captures,
    // 2026-09-02).
    Pass(s, pipe, fb, dst->width, dst->height,
         PostPush{prev_slot, 0, float(REXCVAR_GET(bd_host_post_blur)),
                  level_scale});
    EndGuestTarget(s, dst);
    c.dof.levels[filled++] = dst;
    prev_slot = dst->descriptorIndex;
    level_scale = 1.0f; // the levels hold scaled content
  }
  if (filled == 0)
    return false;
  // A missing tail repeats the last level, so the composite always has five.
  for (u32 i = filled; i < 5; ++i)
    c.dof.levels[i] = c.dof.levels[filled - 1];
  c.dof.depth = s.textures[0];
  for (u32 i = 0; i < 4; ++i)
    c.dof.params[i] = GuestPixelConstant(device_guest, 27, i);
  c.dof.valid = Readable(Content(c.dof.depth));
  if (c.dof_frames++ < 3)
    BD_INFO("[post] dof pyramid: {} levels from the {}x{} scene, depth {}, "
            "params ({:.3g}, {:.3g}, {:.3g}, {:.3g})",
            filled, scene->width, scene->height, c.dof.valid ? "yes" : "no",
            c.dof.params[0], c.dof.params[1], c.dof.params[2], c.dof.params[3]);
  return true;
}

// The bloom mask into the slot-1 texture the guest's ms_tex samples (and the
// host composite reads): bright pass at the mask's size, two blur passes.
GuestTexture *BuildBloomMask(VideoState &s, Chain &c, GuestTexture *scene,
                             u32 device_guest) {
  GuestTexture *dst = s.textures[1];
  if (!scene || !dst || !dst->texture || dst->layers != 1 || dst->width == 0 ||
      dst->width > scene->width)
    return nullptr;
  const float threshold = GuestPixelConstant(device_guest, 27, 0);
  const float intensity = GuestPixelConstant(device_guest, 27, 1);
  const u32 w = dst->width;
  const u32 h = dst->height;
  const plume::RenderFormat fmt = dst->format;
  auto *bright = Pipeline(s, c, Shader::Bright, fmt);
  auto *blur = Pipeline(s, c, Shader::Blur, fmt);
  Scratch *a = GetScratch(s, c, w, h, fmt, 0);
  Scratch *b = GetScratch(s, c, w, h, fmt, 1);
  if (!bright || !blur || !a || !b)
    return nullptr;
  const u32 ratio = scene->width >= w * 2 ? scene->width / w : 1;
  Transition(s, a->texture.get(), a->layout, plume::RenderTextureLayout::COLOR_WRITE);
  Pass(s, bright, a->framebuffer.get(), w, h,
       PostPush{scene->descriptorIndex, ratio, threshold, intensity});
  Transition(s, a->texture.get(), a->layout, plume::RenderTextureLayout::SHADER_READ);
  Transition(s, b->texture.get(), b->layout, plume::RenderTextureLayout::COLOR_WRITE);
  Pass(s, blur, b->framebuffer.get(), w, h, PostPush{a->slot, 0, 1.0f, 0.0f});
  Transition(s, b->texture.get(), b->layout, plume::RenderTextureLayout::SHADER_READ);
  plume::RenderFramebuffer *fb = BeginGuestTarget(s, dst);
  if (!fb)
    return nullptr;
  Pass(s, blur, fb, w, h, PostPush{b->slot, 0, 0.0f, 1.0f});
  EndGuestTarget(s, dst);
  if (c.bloom_frames++ < 3)
    BD_INFO("[post] bloom mask {}x{} from {}x{} scene, threshold {:.3g} "
            "intensity {:.3g}",
            w, h, scene->width, scene->height, threshold, intensity);
  return dst;
}

// One pass for both guest composites, into the frame the ms_tex draw would
// have written. Needs the dof draw's inputs from earlier this frame.
bool HostComposite(VideoState &s, Chain &c, GuestTexture *scene,
                   GuestTexture *bloom, u32 device_guest) {
#if defined(REBLUE_D3D12)
  (void)s; (void)c; (void)scene; (void)bloom; (void)device_guest;
  return false; // the parameter block rides the Vulkan dynamic UBO binding
#else
  if (!REXCVAR_GET(bd_host_post_composite) || !c.dof.valid || !scene || !bloom)
    return false;
  GuestTexture *rt = s.render_target;
  if (!rt || !rt->texture || rt->layers != 1)
    return false;
  GuestTexture *depth = Source(s, c.dof.depth);
  if (!depth)
    return false;
  CompositeConstants k{};
  k.dof[0] = c.dof.params[0];
  k.dof[1] = c.dof.params[1];
  k.dof[2] = c.dof.params[3];
  k.dof[3] = SourceScale(s.textures[0]); // the scene tap's factor, 0 = 1
  for (u32 i = 0; i < 4; ++i) {
    k.w0[i] = GuestPixelConstant(device_guest, 13, i);
    k.w1[i] = GuestPixelConstant(device_guest, 14, i);
  }
  k.indices0[0] = depth->descriptorIndex;
  for (u32 i = 0; i < 5; ++i) {
    GuestTexture *level = Content(c.dof.levels[i]);
    if (!Readable(level))
      return false;
    Transition(s, level->texture, level->layout, plume::RenderTextureLayout::SHADER_READ);
    const u32 idx = level->descriptorIndex;
    if (i < 3)
      k.indices0[1 + i] = idx;
    else
      k.indices1[i - 3] = idx;
  }
  auto alloc = UploadHostConstants(&k, sizeof(k));
  if (!alloc.memory)
    return false;
  auto *pipe = Pipeline(s, c, Shader::Composite, rt->format);
  if (!pipe)
    return false;
  Transition(s, bloom->texture, bloom->layout, plume::RenderTextureLayout::SHADER_READ);
  plume::RenderFramebuffer *fb = GetFramebuffer(s, rt, nullptr);
  if (!fb)
    return false;
  const u32 offsets[3] = {s.constant_dyn_offsets[0], alloc.dynamicOffset,
                          s.constant_dyn_offsets[2]};
  s.command_list->setGraphicsDescriptorSetDynamic(
      s.constant_descriptor_set.get(), kConstantDescriptorSetIndex, offsets, 3);
  Pass(s, pipe, fb, rt->width, rt->height,
       PostPush{scene->descriptorIndex, bloom->descriptorIndex,
                float(REXCVAR_GET(bd_host_post_debug)), 0.0f});
  if (c.composite_frames++ < 3)
    BD_INFO("[post] composite into {}x{}: dof ({:.3g}, {:.3g}, focus {:.3g}) "
            "w0 {:.3g} w1 {:.3g}",
            rt->width, rt->height, k.dof[0], k.dof[1], k.dof[2], k.w0[0],
            k.w1[0]);
  return true;
#endif
}

} // namespace

bool HostPostProducerSkip(VideoState &s, u64 ps_hash) {
  if (!REXCVAR_GET(bd_host_post) || !s.render_target)
    return false;
  Chain &c = chain();
  if (c.failed)
    return false;
  switch (ps_hash) {
  case kQuoter:
  case kMsWeight:
  case kBrightPass:
    // A producer: its target is a pyramid level, never the frame. Anything
    // drawn with these shaders into a full-screen target is not the chain
    // and goes through.
    if (FullscreenChainClassLocked(s, s.render_target))
      return false;
    ++c.skipped;
    return true;
  default:
    return false;
  }
}

bool HostPostActive() {
  if (!REXCVAR_GET(bd_host_post))
    return false;
  Chain &c = chain();
  return !c.failed && c.composite_frames > 0;
}

bool HostPostWillOverwrite(const GuestTexture *dst) {
  if (!dst || !REXCVAR_GET(bd_host_post))
    return false;
  Chain &c = chain();
  if (c.failed || !c.dof.valid)
    return false;
  for (const GuestTexture *level : c.dof.levels)
    if (level == dst)
      return true;
  return c.bloom_mask == dst;
}

bool HostPostOverwritesTarget(VideoState &s, u64 ps_hash) {
  if (!REXCVAR_GET(bd_host_post) || !REXCVAR_GET(bd_host_post_composite))
    return false;
  Chain &c = chain();
  return !c.failed && ps_hash == kMsTex && c.dof.valid;
}

bool HostPostIntercept(VideoState &s, u64 ps_hash, u32 device_guest) {
  if (!REXCVAR_GET(bd_host_post) || !s.command_list || !s.render_target)
    return false;
  Chain &c = chain();
  if (c.failed)
    return false;
  switch (ps_hash) {
  case kDof: {
    if (s.plume_framebuffer_bound)
      DrawQueueFlush(s.command_list);
    const bool built = BuildDofPyramid(s, c, device_guest);
    RestoreGuestDraw(s);
    // With the host composite the guest's dof draw is not needed; without
    // it (no depth, D3D12) the guest composites over the host levels.
    return built && REXCVAR_GET(bd_host_post_composite) && c.dof.valid;
  }
  case kMsTex: {
    if (s.plume_framebuffer_bound)
      DrawQueueFlush(s.command_list);
    GuestTexture *scene = Source(s, s.textures[0]);
    // The bloom mask reads the first dof level rather than the scene: half
    // the texels, and already scaled when the scene is a scaled alias.
    GuestTexture *bloom_src =
        c.dof.valid && c.dof.levels[0] ? Content(c.dof.levels[0]) : nullptr;
    if (!Readable(bloom_src) || SourceScale(s.textures[0]) == 1.0f)
      bloom_src = scene;
    GuestTexture *bloom = BuildBloomMask(s, c, bloom_src, device_guest);
    c.bloom_mask = bloom;
    const bool composed = HostComposite(s, c, scene, bloom, device_guest);
    c.dof.valid = false;
    RestoreGuestDraw(s);
    return composed;
  }
  default:
    return false;
  }
}

} // namespace bd::gpu
