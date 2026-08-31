/**
 * @file    gpu/multiview_resolve.cpp
 * @brief   Flattens a two-layer multiview target into one side-by-side image.
 *
 * Multiview renders the scene once and rasterises it into two array layers, one
 * per eye - which is the whole point, and where the saving is. What it cannot
 * do on its own is survive the post chain: every read of a surface goes through
 * a single bindless SRV declared `Texture2D`, so a reader sees one layer and
 * the stereo pair collapses on the first pass that samples it.
 *
 * The alternative is a second bindless heap declared `Texture2DArray` sampled
 * by `ViewIndex`, which reaches into XenosRecomp's texture declarations, the
 * descriptor set layout and every `tfetch`, and then has to fit inside Adreno's
 * `maxBoundDescriptorSets = 4` - which is already full.
 *
 * So instead the two layers are resolved into one image the moment the guest
 * stops drawing into them: layer 0 into the left half, layer 1 into the right.
 * The post chain then runs mono over a side-by-side frame, exactly as it
 * already does for `bd_stereo`, and `xr_session` already knows how to split
 * that into per-eye `imageRect`s. One pass, two triangles, no descriptor rework.
 *
 * @copyright Copyright (c) 2026 re:Blue contributors
 * @license   BSD 3-Clause - see LICENSE
 */
#include "gpu/frame.h"

#include "core/logging.h"
#include "gpu/backend.h"
#include "gpu/device.h"
#include <memory>
#include <unordered_map>

#include "gpu/resources.h"

REXCVAR_DECLARE(bool, bd_mv_resolve);
REXCVAR_DECLARE(bool, bd_mv_debug_clear);
REXCVAR_DECLARE(bool, bd_mv_debug_known_srv);

namespace bd::gpu {

namespace {

// Matches the present blit's push constant block: the copy shaders take a
// descriptor index first, and ignore the rest.
struct ResolvePushConstants {
  u32 descriptor_index;
  u32 descriptor_index_2;
  float unused0;
  float unused1;
};

// A resolve pipeline per render-target format.
//
// s.copy_color_pipeline cannot be reused: it hardcodes B8G8R8A8_UNORM, and a
// Vulkan pipeline is only compatible with a render pass of the same attachment
// format. The guest's scene surfaces are not all that format, so binding it
// against their companion is a render-pass incompatibility - which is undefined
// rather than an error, and showed up as an entirely black frame while the
// draw count stayed normal.
std::unordered_map<u32, std::unique_ptr<plume::RenderPipeline>> g_resolve_pipes;

plume::RenderPipeline *ResolvePipelineFor(VideoState &s,
                                          plume::RenderFormat format) {
  const u32 key = static_cast<u32>(format);
  auto it = g_resolve_pipes.find(key);
  if (it != g_resolve_pipes.end())
    return it->second.get();
  if (!s.copy_vs || !s.copy_color_ps || !s.pipeline_layout)
    return nullptr;

  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = s.pipeline_layout.get();
  desc.vertexShader = s.copy_vs.get();
  desc.pixelShader = s.copy_color_ps.get();
  desc.depthFunction = plume::RenderComparisonFunction::ALWAYS;
  desc.depthEnabled = false;
  desc.depthWriteEnabled = false;
  desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  desc.cullMode = plume::RenderCullMode::NONE;
  desc.renderTargetCount = 1;
  desc.renderTargetFormat[0] = format;
  desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
  desc.depthTargetFormat = plume::RenderFormat::UNKNOWN;
  auto pipe =
      CreateHostGraphicsPipeline(s.device.get(), desc, "multiview-resolve");
  if (!pipe) {
    BD_ERROR("[mv] resolve pipeline for format {} failed", key);
    return nullptr;
  }
  plume::RenderPipeline *raw = pipe.get();
  g_resolve_pipes.emplace(key, std::move(pipe));
  BD_INFO("[mv] built a resolve pipeline for format {}", key);
  return raw;
}

} // namespace

void ResolveMultiviewSurfaceLocked(VideoState &s, GuestTexture *tex) {
  // Unconditional entry census, by size. Everything so far has inferred what
  // reaches this function from the fact that the frame is black; this says.
  {
    static std::mutex m;
    static std::map<u64, u32> seen;
    static u32 told = 0;
    std::lock_guard<std::mutex> lock(m);
    const u64 key = tex ? (u64(tex->width) << 32 | tex->height) : 0;
    if (++seen[key] == 1 && told < 12) {
      ++told;
      BD_INFO("[mv] resolve ENTERED for {}x{} layers={} companion={} dirty={}",
              tex ? tex->width : 0, tex ? tex->height : 0,
              tex ? tex->layers : 0, tex && tex->resolvedTexture,
              tex && tex->multiviewDirty);
    }
  }
  if (!REXCVAR_GET(bd_mv_resolve))
    return;
  if (!tex || tex->layers < 2 || !tex->resolvedTexture ||
      !tex->resolvedFramebuffer || !s.command_list)
    return;
  plume::RenderPipeline *pipeline = ResolvePipelineFor(s, tex->format);
  if (!pipeline)
    return;
  // Register the per-eye views here rather than at surface creation.
  //
  // surface_pool is a *pool*: a GuestTexture can be handed a different physical
  // texture later, which leaves a view registered at creation pointing at an
  // image the surface no longer owns - and a stale view samples black, which is
  // exactly the symptom. Binding them against the texture that is live right
  // now removes that whole class of bug.
  for (u32 layer = 0; layer < 2; ++layer) {
    if (tex->layerView[layer] && tex->layerViewOf == tex->texture)
      continue;
    plume::RenderTextureViewDesc eye;
    eye.format = tex->format;
    eye.dimension = plume::RenderTextureViewDimension::TEXTURE_2D;
    eye.mipLevels = 1;
    eye.arraySize = 1;
    eye.arrayIndex = layer;
    tex->layerView[layer] = tex->texture->createTextureView(eye);
    if (!tex->layerView[layer])
      return;
    if (tex->layerDescriptorIndex[layer] == kInvalidDescriptorIndex)
      tex->layerDescriptorIndex[layer] = Video::AllocateBindlessTextureSlot();
    if (tex->layerDescriptorIndex[layer] == kInvalidDescriptorIndex)
      return;
    SetBindlessTextureLocked(s, tex->layerDescriptorIndex[layer],
                                    tex->texture, tex->layerView[layer].get());
  }
  tex->layerViewOf = tex->texture;

  // The array image has been rendered into and is about to be sampled; the
  // companion is about to be drawn into. Both have to move before either is
  // touched, or the read races the write on a tiler.
  const plume::RenderTextureBarrier to_resolve[] = {
      plume::RenderTextureBarrier(tex->texture,
                                  plume::RenderTextureLayout::SHADER_READ),
      plume::RenderTextureBarrier(tex->resolvedTexture,
                                  plume::RenderTextureLayout::COLOR_WRITE),
  };
  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, nullptr, 0,
                           to_resolve, 2);

  s.command_list->setFramebuffer(tex->resolvedFramebuffer.get());

  // Diagnostic, on bd_mv_debug_clear: paint the companion instead of resolving
  // into it. If the capture comes back this colour the render pass is running
  // and the copy draw is at fault; if it stays black the pass itself never
  // executes. Separating those two is otherwise guesswork.
  if (REXCVAR_GET(bd_mv_debug_clear))
    s.command_list->clearColor(0, plume::RenderColor(1.0f, 0.0f, 1.0f, 1.0f));

  s.command_list->setPipeline(pipeline);

  const float w = static_cast<float>(tex->width);
  const float h = static_cast<float>(tex->height);
  const float half = w * 0.5f;

  for (u32 eye = 0; eye < 2; ++eye) {
    // Each eye is squeezed into its half. The copy shader draws a full-screen
    // triangle in clip space, so restricting the viewport is the whole of the
    // squeeze - no UV maths, and no dependence on the source size.
    const float x = eye ? half : 0.0f;
    s.command_list->setViewports(plume::RenderViewport(x, 0.0f, half, h));
    s.command_list->setScissors(
        plume::RenderRect(static_cast<i32>(x), 0,
                          static_cast<i32>(x + half), static_cast<i32>(h)));
    // bd_mv_debug_known_srv samples the surface's own descriptor - the one the
    // rest of the renderer uses and which is known to work - instead of the
    // per-eye views this file registers. If the output becomes the scene twice,
    // the pass, the draw and the sampling are all fine and only the per-eye
    // slot registration is wrong; if it stays black the fault is upstream.
    const u32 src_desc = REXCVAR_GET(bd_mv_debug_known_srv)
                             ? tex->descriptorIndex
                             : tex->layerDescriptorIndex[eye];
    ResolvePushConstants pc{src_desc, 0, 0.0f, 0.0f};
    s.command_list->setGraphicsPushConstants(kCopyPushConstantRangeIndex, &pc,
                                             kCopyPushConstantByteOffset,
                                             sizeof(pc));
    s.command_list->drawInstanced(3, 1, 0, 0);
  }

  s.command_list->setFramebuffer(nullptr);

  // The companion is what everything samples from here, and the array image
  // goes back to being a render target so the next frame can draw into it.
  const plume::RenderTextureBarrier done[] = {
      plume::RenderTextureBarrier(tex->resolvedTexture,
                                  plume::RenderTextureLayout::SHADER_READ),
      plume::RenderTextureBarrier(tex->texture,
                                  plume::RenderTextureLayout::COLOR_WRITE),
  };
  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, nullptr, 0,
                           done, 2);

  // Publish the companion under a slot the resolve owns. Built here rather than
  // at surface creation for the same reason the per-eye views are: the pool can
  // hand this GuestTexture a different physical texture at any time, and a view
  // registered against the old one samples black.
  if (tex->resolvedViewOf != tex->resolvedTexture) {
    plume::RenderTextureViewDesc rv;
    rv.format = tex->format;
    rv.dimension = plume::RenderTextureViewDimension::TEXTURE_2D;
    rv.mipLevels = 1;
    tex->resolvedView = tex->resolvedTexture->createTextureView(rv);
    if (tex->resolvedView) {
      if (tex->resolvedDescriptorIndex == kInvalidDescriptorIndex)
        tex->resolvedDescriptorIndex = Video::AllocateBindlessTextureSlot();
      if (tex->resolvedDescriptorIndex != kInvalidDescriptorIndex) {
        SetBindlessTextureLocked(s, tex->resolvedDescriptorIndex,
                                 tex->resolvedTexture,
                                 tex->resolvedView.get());
        tex->resolvedViewOf = tex->resolvedTexture;
      }
    }
  }

  tex->multiviewDirty = false;

  static std::atomic<u32> n{0};
  const u32 seen = n.fetch_add(1, std::memory_order_relaxed);
  if (seen == 0 || seen == 500)
    BD_INFO("[mv] resolved {}x{} to side-by-side ({} times)", tex->width,
            tex->height, seen + 1);

  // The framebuffer and pipeline the guest had bound are gone, and its viewport
  // with them. Everything the next draw needs has to be re-flushed.
  s.draw_framebuffer_bound = false;
  s.dirtyStates.pipelineState = true;
  s.dirtyStates.viewport = true;
  s.dirtyStates.scissorRect = true;
}

} // namespace bd::gpu
