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
 * The normal Vulkan path replaces complete DoF preparation/submission with
 * explicit scene/depth images, native parameters and five rectangles in one
 * host-owned blur atlas. No DoF level allocation, quad or intermediate resolve
 * executes. Native post scheduling also bypasses the bloom allocation/blur
 * loops and ms_tex submission, with typed weights and an explicit completed
 * output. Lens-flare recipes are native, with fifteen optical sprites in one
 * instanced screen-blend draw. Authored properties, image/getter adapters, other effect bodies and
 * unsupported filter combinations remain. Diagnostic/compatibility paths
 * retain the old draw intercept and per-level textures explicitly.
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
#include "gpu/post_parameters.h"
#include "gpu/lens_flare.h"
#include "gpu/post_adjustments.h"
#include "gpu/post_scanline.h"
#include "gpu/post_grade.h"
#include "gpu/post_heat.h"
#include "gpu/post_passes.h"
#include "gpu/sampler_cache.h"

#include <bit>
#include <cstring>
#include <memory>
#include <stdexcept>
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
#include "gpu/host_targets.h"
#include "gpu/resources.h"
#include "gpu/scene/native_texture_gpu.h"

#if defined(REBLUE_D3D12)
#include "src/gpu/shaders/hlsl/post_blur_ps.hlsl.dxil.h"
#include "src/gpu/shaders/hlsl/post_bright_ps.hlsl.dxil.h"
#include "src/gpu/shaders/hlsl/post_composite_ps.hlsl.dxil.h"
#include "src/gpu/shaders/hlsl/post_down_ps.hlsl.dxil.h"
#include "src/gpu/shaders/hlsl/post_dual_down_ps.hlsl.dxil.h"
#include "src/gpu/shaders/hlsl/post_pyramid_ps.hlsl.dxil.h"
#include "src/gpu/shaders/hlsl/lens_flare_vs.hlsl.dxil.h"
#include "src/gpu/shaders/hlsl/lens_flare_ps.hlsl.dxil.h"
#include "src/gpu/shaders/hlsl/post_adjust_ps.hlsl.dxil.h"
#include "src/gpu/shaders/hlsl/post_scanline_ps.hlsl.dxil.h"
#include "src/gpu/shaders/hlsl/post_grade_ps.hlsl.dxil.h"
#else
#include "src/gpu/shaders/hlsl/post_blur_ps.hlsl.spirv.h"
#include "src/gpu/shaders/hlsl/post_bright_ps.hlsl.spirv.h"
#include "src/gpu/shaders/hlsl/post_composite_ps.hlsl.spirv.h"
#include "src/gpu/shaders/hlsl/post_down_ps.hlsl.spirv.h"
#include "src/gpu/shaders/hlsl/post_dual_down_ps.hlsl.spirv.h"
#include "src/gpu/shaders/hlsl/post_pyramid_ps.hlsl.spirv.h"
#include "src/gpu/shaders/hlsl/lens_flare_vs.hlsl.spirv.h"
#include "src/gpu/shaders/hlsl/lens_flare_ps.hlsl.spirv.h"
#include "src/gpu/shaders/hlsl/post_adjust_ps.hlsl.spirv.h"
#include "src/gpu/shaders/hlsl/post_scanline_ps.hlsl.spirv.h"
#include "src/gpu/shaders/hlsl/post_grade_ps.hlsl.spirv.h"
#endif

REXCVAR_DECLARE(bool, bd_host_post);
REXCVAR_DECLARE(bool, bd_host_post_composite);
REXCVAR_DECLARE(i32, bd_host_post_debug);
REXCVAR_DECLARE(f64, bd_host_post_blur);
REXCVAR_DECLARE(bool, bd_host_post_bloom_fold);
REXCVAR_DECLARE(bool, bd_host_post_debug_depth);
REXCVAR_DECLARE(bool, bd_host_post_atlas);

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
  Pyramid = 5,
  LensFlare = 6,
  Adjust = 7,
  Scanline = 8,
  Grade = 9,
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
  // Bloom folded into the composite (bd_host_post_bloom_fold): threshold,
  // intensity, fold flag (nonzero: take the bright pass of dof level 2 in
  // the composite instead of sampling a bloom mask texture), atlas flag
  // (nonzero: the levels are rects of the atlas at indices1[2]).
  float bloom[4];
  // The five levels' rects in the atlas, as UV (x, y, w, h).
  float rects[5][4];
  float heat[4]; // amplitudes xy, noise scale, depth exponent
  float heat_animation[4]; // render-frame phase
  u32 heat_image[4]; // enabled, native image, explicit sampler, reserved
};
static_assert(sizeof(CompositeConstants) == 224);
static_assert(offsetof(CompositeConstants, heat) == 176);
static_assert(offsetof(CompositeConstants, heat_animation) == 192);
static_assert(offsetof(CompositeConstants, heat_image) == 208);

struct Scratch {
  std::unique_ptr<plume::RenderTexture> texture;
  std::unique_ptr<plume::RenderFramebuffer> framebuffer;
  u32 width = 0;
  u32 height = 0;
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  u32 layers = 1; // 2 under multiview: the passes render both eyes at once
  u32 slot = kInvalidDescriptorIndex;
  plume::RenderTextureLayout layout = plume::RenderTextureLayout::UNKNOWN;
  // Two per size: a blur is two passes and the second reads the first.
  u32 role = 0;
};

// What the dof draw bound, kept for the composite at the ms_tex draw.
struct DofInputs {
  GuestTexture *depth = nullptr;
  GuestTexture *levels[5] = {};
  // The scene as the dof draw saw it: the first resolve's content (the
  // surface when that resolve aliased, the texture otherwise) and its
  // scale. The composite reads this, not the ms_tex draw's slot 0: between
  // the two the guest resolves its (dropped) dof output into the same
  // texture at scale 1, which under the host chain is a seeded, unscaled
  // copy of the scene - the frame came out four times too bright reading it.
  GuestTexture *scene_src = nullptr;
  GuestTexture *scene_tex = nullptr;
  // The level atlas (bd_host_post_atlas): one pass, the levels side by side.
  Scratch *atlas = nullptr;
  float rects[5][4] = {};
  float scene_scale = 1.0f; // what the composite multiplies the scene tap by
  DofParameters parameters;
  bool valid = false;
};

struct Chain {
  // The bloom mask texture the host wrote last, for HostPostWillOverwrite.
  GuestTexture *bloom_mask = nullptr;
  std::unique_ptr<plume::RenderShader> shaders[u32(Shader::Count)];
  std::unique_ptr<plume::RenderShader> flare_vs;
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
  c.shaders[u32(Shader::Pyramid)] = s.device->createShader(
      REBLUE_SHADER_BLOB(post_pyramid_ps), "main", kHostShaderFormat);
  c.shaders[u32(Shader::LensFlare)] = s.device->createShader(
      REBLUE_SHADER_BLOB(lens_flare_ps), "main", kHostShaderFormat);
  c.flare_vs = s.device->createShader(
      REBLUE_SHADER_BLOB(lens_flare_vs), "main", kHostShaderFormat);
  c.shaders[u32(Shader::Adjust)] = s.device->createShader(
      REBLUE_SHADER_BLOB(post_adjust_ps), "main", kHostShaderFormat);
  c.shaders[u32(Shader::Scanline)] = s.device->createShader(
      REBLUE_SHADER_BLOB(post_scanline_ps), "main", kHostShaderFormat);
  c.shaders[u32(Shader::Grade)] = s.device->createShader(
      REBLUE_SHADER_BLOB(post_grade_ps), "main", kHostShaderFormat);
  if (!c.flare_vs) {
    c.failed = true;
    return false;
  }
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
                                plume::RenderFormat format, bool layered) {
  const u64 key = (u64(which) << 32) | u64(format) | (layered ? (u64(1) << 63) : 0);
  auto it = c.pipelines.find(key);
  if (it != c.pipelines.end())
    return it->second.get();
  if (!EnsureShaders(s, c) || !s.copy_vs || !s.pipeline_layout)
    return nullptr;
  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = s.pipeline_layout.get();
  desc.vertexShader = which == Shader::LensFlare ? c.flare_vs.get() : s.copy_vs.get();
  desc.pixelShader = c.shaders[u32(which)].get();
  desc.depthFunction = plume::RenderComparisonFunction::ALWAYS;
  desc.depthEnabled = false;
  desc.depthWriteEnabled = false;
  desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  desc.cullMode = plume::RenderCullMode::NONE;
  desc.renderTargetCount = 1;
  desc.renderTargetFormat[0] = format;
  desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
  if (which == Shader::LensFlare) {
    auto &blend = desc.renderTargetBlend[0];
    blend.blendEnabled = true;
    blend.srcBlend = plume::RenderBlend::INV_DEST_COLOR;
    blend.dstBlend = plume::RenderBlend::ONE;
    blend.srcBlendAlpha = plume::RenderBlend::INV_DEST_ALPHA;
    blend.dstBlendAlpha = plume::RenderBlend::ONE;
  }
  desc.depthTargetFormat = plume::RenderFormat::UNKNOWN;
  // Under multiview every host pass draws both layers in one go; the pixel
  // shaders pick the source layer by SV_ViewID.
  desc.viewMask = layered ? 0x3u : 0u;
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
                    plume::RenderFormat format, u32 role, u32 layers) {
  for (auto &sc : c.scratch)
    if (sc->width == width && sc->height == height && sc->format == format &&
        sc->role == role && sc->layers == layers)
      return sc.get();
  auto sc = std::make_unique<Scratch>();
  sc->width = width;
  sc->height = height;
  sc->format = format;
  sc->role = role;
  sc->layers = layers;
  plume::RenderTextureDesc desc =
      plume::RenderTextureDesc::ColorTarget(width, height, format);
  desc.arraySize = layers; // plume's default view is a 2D array past 1 layer
  sc->texture = CreateHostTexture(s.device.get(), desc, "host-post-scratch");
  if (!sc->texture) {
    BD_ERROR("[post] scratch {}x{} failed; chain disabled", width, height);
    c.failed = true;
    return nullptr;
  }
  const plume::RenderTexture *attachments[1] = {sc->texture.get()};
  plume::RenderFramebufferDesc fb_desc(attachments, 1);
  fb_desc.viewMask = layers > 1 ? 0x3u : 0u;
  sc->framebuffer = s.device->createFramebuffer(fb_desc);
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
// keep_bound: leave fb bound afterwards. The composite's target is the tile
// the guest's 2D passes alias next, and the same framebuffer bound again
// continues plume's render pass; unbinding ended it and the 2D pass began a
// new one over the same image (a full-res store and load, 2026-09-03).
void PassAt(VideoState &s, plume::RenderPipeline *pipeline,
            plume::RenderFramebuffer *fb, u32 x, u32 y, u32 width, u32 height,
            const PostPush &push, bool keep_bound) {
  auto *cmd = s.command_list;
  cmd->setFramebuffer(fb);
  cmd->setPipeline(pipeline);
  cmd->setViewports(plume::RenderViewport(float(x), float(y), float(width), float(height)));
  cmd->setScissors(plume::RenderRect(i32(x), i32(y), i32(x + width), i32(y + height)));
  cmd->setGraphicsPushConstants(kCopyPushConstantRangeIndex, &push,
                                kCopyPushConstantByteOffset, sizeof(push));
  cmd->drawInstanced(3, 1, 0, 0);
  if (!keep_bound)
    cmd->setFramebuffer(nullptr);
}

void Pass(VideoState &s, plume::RenderPipeline *pipeline,
          plume::RenderFramebuffer *fb, u32 width, u32 height,
          const PostPush &push, bool keep_bound = false) {
  auto *cmd = s.command_list;
  cmd->setFramebuffer(fb);
  cmd->setPipeline(pipeline);
  cmd->setViewports(plume::RenderViewport(0.0f, 0.0f, float(width), float(height)));
  cmd->setScissors(plume::RenderRect(0, 0, i32(width), i32(height)));
  cmd->setGraphicsPushConstants(kCopyPushConstantRangeIndex, &push,
                                kCopyPushConstantByteOffset, sizeof(push));
  cmd->drawInstanced(3, 1, 0, 0);
  if (!keep_bound)
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
         t->layers <= 2;
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
  if (!dst || !dst->texture || dst->layers > 2)
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
bool BuildDofPyramid(VideoState &s, Chain &c, GuestTexture *scene_texture,
                     GuestTexture *depth_texture, const DofParameters &parameters) {
  c.dof = DofInputs{};
  GuestTexture *scene = Source(s, scene_texture);
  if (!scene)
    return false;
  const bool layered = scene->layers > 1;
  if (REXCVAR_GET(bd_host_post_atlas)) {
    // One pass: the five levels side by side in an atlas half the scene's
    // height (W/2 + W/4 + W/8 + W/16 + W/16 = W), each filtered from the
    // scene by post_pyramid_ps. Five render passes into the guest's level
    // textures before (2026-09-04).
    auto *pyr = Pipeline(s, c, Shader::Pyramid, scene->format, layered);
    Scratch *atlas = GetScratch(s, c, scene->width, std::max(1u, scene->height / 2),
                                scene->format, 2, scene->layers);
    if (!pyr || !atlas)
      return false;
    const float level_scale = SourceScale(scene_texture);
    c.dof.scene_scale = level_scale;
    c.dof.scene_src = scene;
    c.dof.scene_tex = scene_texture;
    Transition(s, atlas->texture.get(), atlas->layout, plume::RenderTextureLayout::COLOR_WRITE);
    u32 x = 0;
    for (u32 level = 0; level < 5; ++level) {
      const u32 shift = std::min(level + 1, 4u);
      const u32 w = std::max(1u, scene->width >> shift);
      const u32 h = std::max(1u, scene->height >> shift);
      PassAt(s, pyr, atlas->framebuffer.get(), x, 0, w, h,
             PostPush{scene->descriptorIndex, level,
                      float(REXCVAR_GET(bd_host_post_blur)), level_scale},
             /*keep_bound=*/level + 1 < 5);
      c.dof.rects[level][0] = float(x) / float(atlas->width);
      c.dof.rects[level][1] = 0.0f;
      c.dof.rects[level][2] = float(w) / float(atlas->width);
      c.dof.rects[level][3] = float(h) / float(atlas->height);
      x += w;
    }
    Transition(s, atlas->texture.get(), atlas->layout, plume::RenderTextureLayout::SHADER_READ);
    c.dof.atlas = atlas;
    for (u32 i = 0; i < 5; ++i)
      c.dof.levels[i] = nullptr;
    c.dof.depth = depth_texture;
    c.dof.parameters = parameters;
    c.dof.valid = Readable(Content(c.dof.depth));
    if (c.dof_frames++ < 3)
      BD_INFO("[post] dof atlas {}x{} from the {}x{} scene in one pass, depth {}",
              atlas->width, atlas->height, scene->width, scene->height,
              c.dof.valid ? "yes" : "no");
    return true;
  }
  auto *dual = Pipeline(s, c, Shader::DualDown, scene->format, layered);
  if (!dual)
    return false;
  u32 prev_slot = scene->descriptorIndex;
  // Param1 of the first level: the scene's resolve scale when the scene is an
  // alias of the unscaled surface. 0 means 1 to the shader.
  float level_scale = SourceScale(scene_texture);
  c.dof.scene_scale = level_scale;
  c.dof.scene_src = scene;
  c.dof.scene_tex = scene_texture;
  u32 filled = 0;
  for (u32 slot = 2; slot <= 6; ++slot) {
    GuestTexture *dst = s.textures[slot];
    if (!dst || !dst->texture || dst->layers != scene->layers ||
        dst->width == 0)
      break;
    plume::RenderFramebuffer *fb = BeginGuestTarget(s, dst);
    if (!fb)
      break;
    auto *pipe = dst->format == scene->format
                     ? dual
                     : Pipeline(s, c, Shader::DualDown, dst->format, layered);
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
  c.dof.depth = depth_texture;
  c.dof.parameters = parameters;
  c.dof.valid = Readable(Content(c.dof.depth));
  if (c.dof_frames++ < 3)
    BD_INFO("[post] dof pyramid: {} levels from the {}x{} scene, depth {}, "
            "params ({:.3g}, {:.3g}, {:.3g}, {:.3g})",
            filled, scene->width, scene->height, c.dof.valid ? "yes" : "no",
            parameters.aperture, parameters.blur_scale, parameters.authored_range,
            parameters.focus_depth);
  return true;
}

// The bloom mask into the slot-1 texture the guest's ms_tex samples (and the
// host composite reads): bright pass at the mask's size, two blur passes.
GuestTexture *BuildBloomMask(VideoState &s, Chain &c, GuestTexture *scene,
                             u32 device_guest) {
  GuestTexture *dst = s.textures[1];
  if (!scene || !dst || !dst->texture || dst->layers != scene->layers ||
      dst->width == 0 || dst->width > scene->width)
    return nullptr;
  const bool layered = scene->layers > 1;
  const float threshold = GuestPixelConstant(device_guest, 27, 0);
  const float intensity = GuestPixelConstant(device_guest, 27, 1);
  const u32 w = dst->width;
  const u32 h = dst->height;
  const plume::RenderFormat fmt = dst->format;
  auto *bright = Pipeline(s, c, Shader::Bright, fmt, layered);
  auto *blur = Pipeline(s, c, Shader::Blur, fmt, layered);
  Scratch *a = GetScratch(s, c, w, h, fmt, 0, scene->layers);
  Scratch *b = GetScratch(s, c, w, h, fmt, 1, scene->layers);
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

// One pass for both composites into an explicit output attachment, using only
// native values. The compatibility intercept imports its registers separately.
// Draw producers consume only this explicit native destination, whether the
// final attachment came through a boundary adapter or is private post scratch.
struct PostAttachment {
  plume::RenderTexture *texture;
  plume::RenderFramebuffer *framebuffer;
  u32 width, height, layers;
  plume::RenderFormat format;
};
PostAttachment Attachment(VideoState &s, GuestTexture *target) {
  if (!target) return {};
  return {target->texture, GetFramebuffer(s, target, nullptr),
          target->width, target->height, target->layers, target->format};
}
PostAttachment Attachment(Scratch *target) {
  return {target->texture.get(), target->framebuffer.get(),
          target->width, target->height, target->layers, target->format};
}
bool HostComposite(VideoState &s, Chain &c, GuestTexture *scene,
                   GuestTexture *bloom, const PostAttachment &rt,
                   const BloomParameters &parameters,
                   const HeatShimmerParameters &heat = {}, GuestTexture *heat_image = nullptr,
                   u32 heat_sampler = 0) {
#if defined(REBLUE_D3D12)
  (void)s; (void)c; (void)scene; (void)bloom; (void)rt; (void)parameters;
  return false; // the parameter block rides the Vulkan dynamic UBO binding
#else
  const bool fold = REXCVAR_GET(bd_host_post_bloom_fold);
  // Every early exit named with its frame: a frame the host composite skips
  // shows the guest's own composite over levels the host never wrote - a
  // whole frame of sky (2026-09-03).
  auto bail = [&](const char *why) {
    static u32 told = 0;
    if (told++ < 40)
      BD_WARN("[post] frame {}: composite skipped: {} (dof valid {}, scene {}, "
              "bloom {}, rt {})",
              FrameStatFrameCount(), why, c.dof.valid ? 1 : 0,
              scene ? 1 : 0, bloom ? 1 : 0,
              rt.texture ? 1 : 0);
    return false;
  };
  if (!REXCVAR_GET(bd_host_post_composite) || !c.dof.valid || !scene ||
      (!bloom && !fold))
    return bail("inputs");
  if (!rt.texture || !rt.framebuffer || rt.layers > 2)
    return bail("target");
  GuestTexture *depth = Source(s, c.dof.depth);
  if (!depth)
    return bail("depth not readable");
  CompositeConstants k{};
  if (heat.enabled) {
    const auto *noise = Source(s, heat_image);
    if (!noise || !heat_sampler) return bail("heat noise image/sampler");
    k.heat[0] = heat.amplitude_x;
    k.heat[1] = heat.amplitude_y;
    k.heat[2] = heat.noise_scale;
    k.heat[3] = heat.depth_power;
    k.heat_animation[0] = heat.phase;
    k.heat_image[0] = 1;
    k.heat_image[1] = noise->descriptorIndex;
    k.heat_image[2] = heat_sampler;
  }
  k.dof[0] = c.dof.parameters.aperture;
  k.dof[1] = c.dof.parameters.blur_scale;
  k.dof[2] = c.dof.parameters.focus_depth;
  k.dof[3] = c.dof.scene_scale; // the scene tap's factor, 0 = 1
  for (u32 i = 0; i < 4; ++i) {
    k.w0[i] = parameters.scene_weight[i];
    k.w1[i] = parameters.bloom_weight[i];
  }
  k.indices0[0] = depth->descriptorIndex;
  k.bloom[0] = parameters.threshold;
  k.bloom[1] = parameters.intensity;
  k.bloom[2] = fold ? 1.0f : 0.0f;
  k.bloom[3] = c.dof.atlas ? 1.0f : 0.0f;
  if (c.dof.atlas) {
    k.indices1[2] = c.dof.atlas->slot;
    std::memcpy(k.rects, c.dof.rects, sizeof(k.rects));
  } else {
    for (u32 i = 0; i < 5; ++i) {
      GuestTexture *level = Content(c.dof.levels[i]);
      if (!Readable(level))
        return bail("a dof level not readable");
      Transition(s, level->texture, level->layout, plume::RenderTextureLayout::SHADER_READ);
      const u32 idx = level->descriptorIndex;
      if (i < 3)
        k.indices0[1 + i] = idx;
      else
        k.indices1[i - 3] = idx;
    }
  }
  auto alloc = UploadHostConstants(&k, sizeof(k));
  if (!alloc.memory)
    return bail("constants");
  const bool layered = rt.layers > 1;
  auto *pipe = Pipeline(s, c, Shader::Composite, rt.format, layered);
  if (!pipe)
    return bail("pipeline");
  if (bloom)
    Transition(s, bloom->texture, bloom->layout, plume::RenderTextureLayout::SHADER_READ);
  plume::RenderFramebuffer *fb = rt.framebuffer;
  if (!fb)
    return false;
  const u32 offsets[3] = {s.constant_dyn_offsets[0], alloc.dynamicOffset,
                          s.constant_dyn_offsets[2]};
  s.command_list->setGraphicsDescriptorSetDynamic(
      s.constant_descriptor_set.get(), kConstantDescriptorSetIndex, offsets, 3);
  Pass(s, pipe, fb, rt.width, rt.height,
       PostPush{scene->descriptorIndex,
                bloom ? bloom->descriptorIndex : 0u,
                float(REXCVAR_GET(bd_host_post_debug_depth)
                          ? 1
                          : REXCVAR_GET(bd_host_post_debug)),
                0.0f},
       /*keep_bound=*/true);
  if (c.composite_frames++ < 3)
    BD_INFO("[post] composite into {}x{}: dof ({:.3g}, {:.3g}, focus {:.3g}) "
            "w0 {:.3g} w1 {:.3g}",
            rt.width, rt.height, k.dof[0], k.dof[1], k.dof[2], k.w0[0],
            k.w1[0]);
  return true;
#endif
}

bool RenderLensFlare(VideoState &s, Chain &c, const PostAttachment &output,
                     const LensFlareParameters &parameters,
                     const std::array<GuestTexture *, 4> &images) {
  if (!parameters.count)
    return true;
  auto sprites = parameters.sprites;
  static bool reported_images = false;
  if (!reported_images) {
    for (u32 i = 0; i < images.size(); ++i) {
      const auto *asset = Content(images[i]);
      BD_INFO("[native-post] optical image {} {}x{} descriptor {} native {:016X} source alias {}",
          i, asset->width, asset->height, asset->descriptorIndex,
          asset->nativeGpu ? asset->nativeGpu->asset->id : 0,
          asset != images[i]);
    }
    reported_images = true;
  }
  for (auto *image : images)
    if (!Source(s, image)) return false;
  for (u32 i = 0; i < parameters.count; ++i)
    sprites[i].texture = Content(images[sprites[i].texture])->descriptorIndex;
  const auto allocation = UploadHostConstants(sprites.data(), sizeof(sprites));
  auto *pipeline = Pipeline(s, c, Shader::LensFlare, output.format, output.layers > 1);
  auto *framebuffer = output.framebuffer;
  if (!allocation.memory || !pipeline || !framebuffer)
    return false;
  const u32 offsets[3] = {allocation.dynamicOffset, s.constant_dyn_offsets[1],
                          s.constant_dyn_offsets[2]};
  auto *cmd = s.command_list;
  cmd->setGraphicsDescriptorSetDynamic(s.constant_descriptor_set.get(),
      kConstantDescriptorSetIndex, offsets, 3);
  cmd->setFramebuffer(framebuffer);
  cmd->setPipeline(pipeline);
  cmd->setViewports(plume::RenderViewport(0, 0, float(output.width), float(output.height)));
  cmd->setScissors(plume::RenderRect(0, 0, i32(output.width), i32(output.height)));
  cmd->drawInstanced(6, parameters.count, 0, 0);
  return true;
}
} // namespace

bool HostPostRender(GuestTexture *scene, GuestTexture *depth, GuestTexture *output,
                    const DofParameters &dof, const BloomParameters &bloom,
                    const LensFlareParameters &flare,
                    const std::array<GuestTexture *, 4> &flare_images,
                    const PostAdjustments &adjustments, const ScanlineParameters &scanline,
                    const GradeParameters &grade, GuestTexture *grain_image,
                    const HeatShimmerParameters &heat, GuestTexture *heat_image) {
#if defined(REBLUE_D3D12)
  return false;
#else
  if (!REXCVAR_GET(bd_host_post) || !REXCVAR_GET(bd_host_post_composite) ||
      !REXCVAR_GET(bd_host_post_atlas) || !REXCVAR_GET(bd_host_post_bloom_fold))
    return false;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  auto &c = chain();
  auto *source = Content(scene);
  auto *z = Content(depth);
  if (!s.ready || c.failed || !Readable(source) || !Readable(z) ||
      !output || !output->texture || source == output || z == output ||
      source->layers != z->layers || source->layers != output->layers)
    return false;
  if (flare.count > flare.sprites.size()) return false;
  if (flare.count) {
    for (auto *image : flare_images) {
      if (auto *asset = Content(image); asset && asset->texture)
        BindTextureSRVLocked(s, asset);
      if (!Readable(Content(image)) || Content(image) == output ||
          Content(image)->layers != 1) return false;
    }
    for (u32 i = 0; i < flare.count; ++i)
      if (flare.sprites[i].texture >= flare_images.size()) return false;
  }
  u32 grain_sampler = 0;
  u32 heat_sampler = 0;
  const auto prepare_noise = [&](GuestTexture *image, u32 &sampler) {
    auto *asset = Content(image);
    if (asset && asset->texture) BindTextureSRVLocked(s, asset);
    if (!Readable(asset) || asset == output || asset->layers != 1) return false;
    plume::RenderSamplerDesc recipe;
    recipe.minFilter = recipe.magFilter = plume::RenderFilter::LINEAR;
    recipe.mipmapMode = plume::RenderMipmapMode::LINEAR;
    recipe.addressU = recipe.addressV = plume::RenderTextureAddressMode::WRAP;
    recipe.addressW = plume::RenderTextureAddressMode::CLAMP;
    sampler = ResolveSlotLocked(recipe);
    return sampler != 0; // no silent clamp/linear substitution
  };
  if (grade.grain && !prepare_noise(grain_image, grain_sampler)) return false;
  if (heat.enabled && !prepare_noise(heat_image, heat_sampler)) return false;
  // Ordered native passes ping-pong between at most two private images. The
  // composite writes the first input directly; no full-image seed copies.
  const auto plan = MakePostPasses(adjustments.Active(), scanline.enabled, grade.Active());
  std::array<Scratch *, 2> scratch{};
  for (u32 i = 0; i < plan.scratch_count; ++i) {
    scratch[i] = GetScratch(s, c, output->width, output->height, output->format, 3 + i, output->layers);
    if (!scratch[i]) return false;
  }
  const auto destination = Attachment(s, output);
  const auto attachment = [&](u32 index) {
    return index == PostPasses::kOutput ? destination : Attachment(scratch[index]);
  };
  const auto composed = attachment(plan.composite_output);
  if (!composed.framebuffer || !destination.framebuffer) return false;
  Video::OpenCommandListLocked();
  if (!s.command_list_open || !s.command_list)
    return false;
  if (s.plume_framebuffer_bound)
    DrawQueueFlush(s.command_list);
  HostTargetDropLinks(s, output);
  if (!BuildDofPyramid(s, c, scene, depth, dof) || !c.dof.valid)
    throw std::runtime_error("Native post atlas production failed");
  const auto write_attachment = [&](u32 index) {
    if (index == PostPasses::kOutput)
      Transition(s, output->texture, output->layout, plume::RenderTextureLayout::COLOR_WRITE);
    else
      Transition(s, scratch[index]->texture.get(), scratch[index]->layout,
                 plume::RenderTextureLayout::COLOR_WRITE);
  };
  write_attachment(plan.composite_output);
  if (!HostComposite(s, c, source, nullptr, composed, bloom, heat, heat_image, heat_sampler))
    throw std::runtime_error("Native post composite failed");
  if (!RenderLensFlare(s, c, composed, flare, flare_images))
    throw std::runtime_error("Native lens-flare submission failed");
  for (u32 i = 0; i < plan.count; ++i) {
    const auto step = plan.steps[i];
    auto *input = scratch[step.input];
    const auto target = attachment(step.output);
    s.command_list->setFramebuffer(nullptr);
    Transition(s, input->texture.get(), input->layout,
               plume::RenderTextureLayout::SHADER_READ);
    write_attachment(step.output);
    Shader shader = Shader::Adjust;
    PostPush push{input->slot, 0, 0, 0};
    if (step.effect == PostEffect::Adjust) {
      push.src2 = std::bit_cast<u32>(adjustments.reverse_pivot);
      push.param0 = adjustments.fisheye_enabled ? adjustments.fisheye : 0.0f;
      push.param1 = adjustments.reverse_enabled ? adjustments.reverse_strength : 0.0f;
    } else if (step.effect == PostEffect::Scanline) {
      shader = Shader::Scanline;
      push.param0 = scanline.strength;
      push.param1 = scanline.phase;
    } else {
      shader = Shader::Grade;
      if (grade.grain) {
        auto *noise = Source(s, grain_image);
        if (!noise) throw std::runtime_error("Native grading grain image failed");
        push.src2 = noise->descriptorIndex;
      }
      struct GradeConstants {
        float gain_gamma[4], bias_saturation[4], target_blend[4], strength_phase[4];
        u32 enabled_sampler[4];
      };
      static_assert(sizeof(GradeConstants) == 80);
      const GradeConstants constants{
          {grade.gain.r, grade.gain.g, grade.gain.b, grade.gamma},
          {grade.bias.r, grade.bias.g, grade.bias.b, grade.saturation},
          {grade.target.r, grade.target.g, grade.target.b, grade.blend},
          {grade.discolor_strength, grade.grain_strength, grade.phase_x, grade.phase_y},
          {u32(grade.discolor), u32(grade.grain), u32(grade.correction), grain_sampler}};
      const auto allocation = UploadHostConstants(&constants, sizeof(constants));
      if (!allocation.memory) throw std::runtime_error("Native grading constants failed");
      const u32 offsets[3] = {s.constant_dyn_offsets[0], allocation.dynamicOffset,
                              s.constant_dyn_offsets[2]};
      s.command_list->setGraphicsDescriptorSetDynamic(s.constant_descriptor_set.get(),
          kConstantDescriptorSetIndex, offsets, 3);
    }
    auto *pipeline = Pipeline(s, c, shader, target.format, target.layers > 1);
    if (!pipeline) throw std::runtime_error("Native post effect pipeline failed");
    Pass(s, pipeline, target.framebuffer, target.width, target.height, push, true);
  }
  output->surfaceDrawn = true;
  // The complete source is consumed before publishing the new output alias.
  DetachSourceSurfaceLocked(s, scene);
  c.dof.valid = false;
  s.command_list->setFramebuffer(nullptr);
  Transition(s, output->texture, output->layout, plume::RenderTextureLayout::SHADER_READ);
  s.plume_framebuffer_bound = false;
  s.draw_framebuffer_bound = false;
  s.bound_fb_rt = nullptr;
  s.bound_fb_ds = nullptr;
  s.dirtyStates.viewport = true;
  s.dirtyStates.scissorRect = true;
  s.dirtyStates.pipelineState = true;
  return true;
#endif
}

bool HostPostPrepareDof(GuestTexture *scene, GuestTexture *depth,
                        const DofParameters &parameters) {
#if defined(REBLUE_D3D12)
  return false;
#else
  if (!REXCVAR_GET(bd_host_post) || !REXCVAR_GET(bd_host_post_composite) ||
      !REXCVAR_GET(bd_host_post_atlas))
    return false;
  auto &s = state();
  std::unique_lock<std::mutex> lock(s.mutex);
  auto &c = chain();
  if (c.failed || !s.ready || !Readable(Content(scene)) || !Readable(Content(depth)) ||
      Content(scene)->layers != Content(depth)->layers)
    return false;
  Video::OpenCommandListLocked();
  if (!s.command_list_open || !s.command_list)
    return false;
  if (s.plume_framebuffer_bound)
    DrawQueueFlush(s.command_list);
  const bool built = BuildDofPyramid(s, c, scene, depth, parameters);
  // No DoF tile target, quad, seed or resolve is needed: the later combined
  // composite consumes this atlas and the explicit scene source directly.
  s.command_list->setFramebuffer(nullptr);
  s.plume_framebuffer_bound = false;
  s.draw_framebuffer_bound = false;
  s.bound_fb_rt = nullptr;
  s.bound_fb_ds = nullptr;
  s.dirtyStates.viewport = true;
  s.dirtyStates.scissorRect = true;
  s.dirtyStates.pipelineState = true;
  return built && c.dof.valid;
#endif
}

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

bool HostPostWillIntercept(u64 ps_hash) {
  if (!REXCVAR_GET(bd_host_post))
    return false;
  Chain &c = chain();
  return !c.failed && (ps_hash == kDof || ps_hash == kMsTex);
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
  // The composite writes its whole target; the dof draw's target is never
  // read at all under the host chain (the composite reads the scene the dof
  // draw saw), so neither needs seeding from its predecessor.
  return !c.failed && (ps_hash == kMsTex || ps_hash == kDof);
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
    // Explicit compatibility importer; the native producer never reaches it.
    const DofParameters parameters{GuestPixelConstant(device_guest, 27, 0),
        GuestPixelConstant(device_guest, 27, 1), GuestPixelConstant(device_guest, 27, 2),
        GuestPixelConstant(device_guest, 27, 3)};
    const bool built = BuildDofPyramid(s, c, s.textures[1], s.textures[0], parameters);
    {
      static u32 told = 0;
      if ((!built || !c.dof.valid) && told++ < 40)
        BD_WARN("[post] frame {}: dof pyramid {} valid {} (depth {}, slot1 {})",
                FrameStatFrameCount(), built ? "built" : "NOT built",
                c.dof.valid ? 1 : 0, c.dof.depth ? 1 : 0,
                s.textures[1] ? 1 : 0);
    }
    RestoreGuestDraw(s);
    // With the host composite the guest's dof draw is not needed; without
    // it (no depth, D3D12) the guest composites over the host levels.
    return built && REXCVAR_GET(bd_host_post_composite) && c.dof.valid;
  }
  case kMsTex: {
    if (s.plume_framebuffer_bound)
      DrawQueueFlush(s.command_list);
    // The scene the dof draw saw (see DofInputs); the ms_tex slot 0 is the
    // guest's re-resolve of its dropped dof output.
    GuestTexture *scene = c.dof.valid && Readable(c.dof.scene_src)
                              ? c.dof.scene_src
                              : Source(s, s.textures[0]);
    // (scene is null under multiview, where the chain does not run on the
    // two-layer targets yet; the null must not be compared into a deref.)
    if (scene && scene == c.dof.scene_src)
      Transition(s, scene->texture, scene->layout,
                 plume::RenderTextureLayout::SHADER_READ);
    // The bloom mask reads the first dof level rather than the scene: half
    // the texels, and already scaled when the scene is a scaled alias.
    GuestTexture *bloom_src =
        c.dof.valid && c.dof.levels[0] ? Content(c.dof.levels[0]) : nullptr;
    if (!Readable(bloom_src) || c.dof.scene_scale == 1.0f)
      bloom_src = scene;
    // Folded: the composite takes the bright pass of dof level 2 (240x135,
    // twice dual-downsampled, a spread comparable to the guest's two 9-tap
    // blurs at 480x270) instead of three passes into a mask texture
    // (2026-09-03; approximate visuals are the owner's call).
    GuestTexture *bloom = REXCVAR_GET(bd_host_post_bloom_fold)
                              ? nullptr
                              : BuildBloomMask(s, c, bloom_src, device_guest);
    c.bloom_mask = bloom;
    BloomParameters parameters;
    parameters.threshold = GuestPixelConstant(device_guest, 27, 0);
    parameters.intensity = GuestPixelConstant(device_guest, 27, 1);
    for (u32 lane = 0; lane < 4; ++lane) {
      parameters.scene_weight[lane] = GuestPixelConstant(device_guest, 13, lane);
      parameters.bloom_weight[lane] = GuestPixelConstant(device_guest, 14, lane);
    }
    VerifyNativePostParameters(parameters);
    const bool composed = HostComposite(s, c, scene, bloom, Attachment(s, s.render_target), parameters);
    // Nothing reads the scene texture after the composite (a field frame's
    // only materialisation was the surface's reuse next frame), so its
    // resolve links are dropped here: with them, the scaled copy would still
    // be made when the surface is rebound - the copy the alias exists to
    // remove.
    if (composed) {
      DetachSourceSurfaceLocked(s, c.dof.scene_tex);
      DetachSourceSurfaceLocked(s, s.textures[0]);
    }
    c.dof.valid = false;
    RestoreGuestDraw(s);
    return composed;
  }
  default:
    return false;
  }
}

} // namespace bd::gpu
