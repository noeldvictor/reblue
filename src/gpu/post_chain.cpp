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
 * The two composites stay the guest's draws. The thirteen producers are
 * replaced: at the dof draw the host downsamples and blurs the scene into
 * the very texture objects slots 2-6 name, and at the ms_tex draw it builds
 * the mask into the slot-1 texture, reading threshold and intensity from the
 * guest's c27. Nothing goes through the tile, nothing is resolved, and the
 * guest samples what it expects.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/post_chain.h"

#include <memory>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/backend.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/frame.h"
#include "gpu/frame_stats.h"
#include "gpu/resources.h"

#if defined(REBLUE_D3D12)
#include "src/gpu/shaders/hlsl/post_blur_ps.hlsl.dxil.h"
#include "src/gpu/shaders/hlsl/post_bright_ps.hlsl.dxil.h"
#include "src/gpu/shaders/hlsl/post_down_ps.hlsl.dxil.h"
#else
#include "src/gpu/shaders/hlsl/post_blur_ps.hlsl.spirv.h"
#include "src/gpu/shaders/hlsl/post_bright_ps.hlsl.spirv.h"
#include "src/gpu/shaders/hlsl/post_down_ps.hlsl.spirv.h"
#endif

REXCVAR_DECLARE(bool, bd_host_post);

namespace bd::gpu {
namespace {

// The guest's post pixel shaders, by XenosRecomp cache hash
// (tools/shader_cache/shader_hashes.csv).
constexpr u64 kQuoter = 0x77344D98A7F5B956ull;
constexpr u64 kMsWeight = 0x1E2676BD7DBBE4F7ull;
constexpr u64 kBrightPass = 0xFFDBD782126EB6E8ull;
constexpr u64 kDof = 0xF6FF1BED057E0FC4ull;
constexpr u64 kMsTex = 0x620B403BCBBF1B98ull;

enum class Shader : u32 { Down = 0, Blur = 1, Bright = 2 };

// Same layout as the copy passes' push block (copy_common.hlsli).
struct PostPush {
  u32 src;
  u32 src2;
  float param0;
  float param1;
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

struct Chain {
  std::unique_ptr<plume::RenderShader> shaders[3];
  std::unordered_map<u64, std::unique_ptr<plume::RenderPipeline>> pipelines;
  std::vector<std::unique_ptr<Scratch>> scratch;
  bool failed = false;
  u32 dof_frames = 0;
  u32 bloom_frames = 0;
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
  c.shaders[0] = s.device->createShader(REBLUE_SHADER_BLOB(post_down_ps),
                                        "main", kHostShaderFormat);
  c.shaders[1] = s.device->createShader(REBLUE_SHADER_BLOB(post_blur_ps),
                                        "main", kHostShaderFormat);
  c.shaders[2] = s.device->createShader(REBLUE_SHADER_BLOB(post_bright_ps),
                                        "main", kHostShaderFormat);
  if (!c.shaders[0] || !c.shaders[1] || !c.shaders[2]) {
    BD_ERROR("[post] host post shaders failed to create; chain disabled");
    c.failed = true;
    return false;
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

// blur(src) -> dst through the two scratch textures of dst's size: the box
// downsample first when ratio > 1, then the two gaussian directions.
bool DownAndBlur(VideoState &s, Chain &c, u32 src_slot, u32 ratio,
                 GuestTexture *dst, u32 *unblurred_slot) {
  const u32 w = dst->width;
  const u32 h = dst->height;
  const plume::RenderFormat fmt = dst->format;
  auto *down = Pipeline(s, c, Shader::Down, fmt);
  auto *blur = Pipeline(s, c, Shader::Blur, fmt);
  Scratch *a = GetScratch(s, c, w, h, fmt, 0);
  Scratch *b = GetScratch(s, c, w, h, fmt, 1);
  if (!down || !blur || !a || !b)
    return false;
  // a = downsample(src)
  Transition(s, a->texture.get(), a->layout, plume::RenderTextureLayout::COLOR_WRITE);
  Pass(s, down, a->framebuffer.get(), w, h, PostPush{src_slot, 0, float(ratio), 0.0f});
  Transition(s, a->texture.get(), a->layout, plume::RenderTextureLayout::SHADER_READ);
  if (unblurred_slot)
    *unblurred_slot = a->slot;
  // b = blur_x(a)
  Transition(s, b->texture.get(), b->layout, plume::RenderTextureLayout::COLOR_WRITE);
  Pass(s, blur, b->framebuffer.get(), w, h, PostPush{a->slot, 0, 1.0f, 0.0f});
  Transition(s, b->texture.get(), b->layout, plume::RenderTextureLayout::SHADER_READ);
  // dst = blur_y(b)
  plume::RenderFramebuffer *fb = BeginGuestTarget(s, dst);
  if (!fb)
    return false;
  Pass(s, blur, fb, w, h, PostPush{b->slot, 0, 0.0f, 1.0f});
  EndGuestTarget(s, dst);
  return true;
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

// The dof composite samples depth in slot 0, the scene in slot 1 and five
// blurred levels in slots 2-6 (1/2, 1/4, 1/8, 1/16, 1/16). Fill 2-6.
void BuildDofPyramid(VideoState &s, Chain &c) {
  GuestTexture *scene = Content(s.textures[1]);
  if (!Readable(scene))
    return;
  Transition(s, scene->texture, scene->layout, plume::RenderTextureLayout::SHADER_READ);
  u32 prev_slot = scene->descriptorIndex;
  u32 prev_w = scene->width;
  bool any = false;
  for (u32 slot = 2; slot <= 6; ++slot) {
    GuestTexture *dst = s.textures[slot];
    if (!dst || !dst->texture || dst->layers != 1 || dst->width == 0)
      continue;
    // Downsample from the previous *unblurred* level, as the guest's quoter
    // chain did; the last level repeats its predecessor's size.
    const u32 ratio = prev_w >= dst->width * 2 ? prev_w / dst->width : 1;
    u32 unblurred = prev_slot;
    if (!DownAndBlur(s, c, prev_slot, ratio, dst, &unblurred))
      break;
    prev_slot = unblurred;
    prev_w = dst->width;
    any = true;
  }
  if (any && c.dof_frames++ < 3)
    BD_INFO("[post] dof pyramid from {}x{} scene: slots 2-6 filled by the host",
            scene->width, scene->height);
}

// The bloom composite samples the scene in slot 0 and the mask in slot 1.
void BuildBloomMask(VideoState &s, Chain &c, u32 device_guest) {
  GuestTexture *scene = Content(s.textures[0]);
  GuestTexture *dst = s.textures[1];
  if (!Readable(scene) || !dst || !dst->texture || dst->layers != 1 ||
      dst->width == 0 || dst->width > scene->width)
    return;
  float threshold = 0.25f;
  float intensity = 1.0f;
  if (const auto *device = bd::mem::at<const D3DDevice>(device_guest)) {
    threshold = float(device->psFloatConstants[27][0]);
    intensity = float(device->psFloatConstants[27][1]);
  }
  const u32 w = dst->width;
  const u32 h = dst->height;
  const plume::RenderFormat fmt = dst->format;
  auto *bright = Pipeline(s, c, Shader::Bright, fmt);
  auto *blur = Pipeline(s, c, Shader::Blur, fmt);
  Scratch *a = GetScratch(s, c, w, h, fmt, 0);
  Scratch *b = GetScratch(s, c, w, h, fmt, 1);
  if (!bright || !blur || !a || !b)
    return;
  Transition(s, scene->texture, scene->layout, plume::RenderTextureLayout::SHADER_READ);
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
    return;
  Pass(s, blur, fb, w, h, PostPush{b->slot, 0, 0.0f, 1.0f});
  EndGuestTarget(s, dst);
  if (c.bloom_frames++ < 3)
    BD_INFO("[post] bloom mask {}x{} from {}x{} scene, threshold {:.3g} "
            "intensity {:.3g}",
            w, h, scene->width, scene->height, threshold, intensity);
}

} // namespace

bool HostPostIntercept(VideoState &s, u64 ps_hash, u32 device_guest) {
  if (!REXCVAR_GET(bd_host_post) || !s.command_list || !s.render_target)
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
  case kDof:
    if (s.plume_framebuffer_bound)
      DrawQueueFlush(s.command_list);
    BuildDofPyramid(s, c);
    RestoreGuestDraw(s);
    return false;
  case kMsTex:
    if (s.plume_framebuffer_bound)
      DrawQueueFlush(s.command_list);
    BuildBloomMask(s, c, device_guest);
    RestoreGuestDraw(s);
    return false;
  default:
    return false;
  }
}

} // namespace bd::gpu
