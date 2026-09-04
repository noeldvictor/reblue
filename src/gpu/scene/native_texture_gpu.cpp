/**
 * @file    native_texture_gpu.cpp
 * @brief   Content-keyed immutable GPU textures and fence-gated residency.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license   BSD 3-Clause License
 */
#include "gpu/scene/native_texture_gpu.h"
#include "core/logging.h"
#include "gpu/bindless_allocator.h"
#include "gpu/constant_buffers.h"
#include "gpu/device.h"
#include "gpu/frame.h"
#include "gpu/frame_stats.h"
#include "gpu/gpu_timing.h"
#include "gpu/scene/fenced_asset_cache.h"
#include <algorithm>
#include <cstring>

namespace bd::gpu::scene {
// Accounts canonical image payloads, not driver allocation granularity. This
// native image share is separate from the CPU asset and geometry budgets.
struct NativeTextureGpuStore {
  FencedAssetCache<NativeTextureGpu> images{256ull << 20};
  uint64_t requests = 0;
};
namespace {
plume::RenderFormat Format(NativeTextureFormat format) {
  switch (format) {
  case NativeTextureFormat::BC1:
    return plume::RenderFormat::BC1_UNORM;
  case NativeTextureFormat::BC2:
    return plume::RenderFormat::BC2_UNORM;
  case NativeTextureFormat::BC3:
    return plume::RenderFormat::BC3_UNORM;
  case NativeTextureFormat::RGBA8:
    return plume::RenderFormat::R8G8B8A8_UNORM;
  default:
    return plume::RenderFormat::UNKNOWN;
  }
}
uint8_t Alpha(const NativeTextureData &data) {
  const auto &bytes = data.images[0];
  switch (data.format) {
  case NativeTextureFormat::BC1:
    for (size_t off = 0; off < bytes.size(); off += 8) {
      const uint16_t c0 =
          uint16_t(bytes[off]) | (uint16_t(bytes[off + 1]) << 8);
      const uint16_t c1 =
          uint16_t(bytes[off + 2]) | (uint16_t(bytes[off + 3]) << 8);
      if (c0 > c1)
        continue;
      for (size_t b = off + 4; b < off + 8; ++b)
        for (unsigned shift = 0; shift < 8; shift += 2)
          if (((bytes[b] >> shift) & 3) == 3)
            return 2;
    }
    return 1;
  case NativeTextureFormat::BC2:
    for (size_t off = 0; off < bytes.size(); off += 16)
      for (size_t b = off; b < off + 8; ++b)
        if (bytes[b] != 255)
          return 2;
    return 1;
  case NativeTextureFormat::BC3:
    for (size_t off = 0; off < bytes.size(); off += 16)
      if (bytes[off] != 255 || bytes[off + 1] != 255)
        return 2;
    return 1;
  default:
    return 0;
  }
}
NativeTextureGpuHandle Upload(VideoState &s, const NativeTextureHandle &asset) {
  const auto &data = asset->data;
  auto result = std::make_shared<NativeTextureGpu>();
  result->asset = asset;
  result->format = Format(data.format);
  const bool volume = data.dimension == NativeTextureDimension::Volume;
  const bool cube = data.dimension == NativeTextureDimension::Cube;
  result->dimension =
      volume ? plume::RenderTextureViewDimension::TEXTURE_3D
             : (cube ? plume::RenderTextureViewDimension::TEXTURE_CUBE
                     : plume::RenderTextureViewDimension::TEXTURE_2D_ARRAY);
  result->alpha_opaque = Alpha(data);
  plume::RenderTextureDesc desc;
  desc.dimension = volume ? plume::RenderTextureDimension::TEXTURE_3D
                          : plume::RenderTextureDimension::TEXTURE_2D;
  desc.width = data.width;
  desc.height = data.height;
  desc.depth = data.depth;
  desc.mipLevels = data.mip_levels;
  desc.arraySize = NativeTextureLayers(data);
  desc.format = result->format;
  desc.flags =
      cube ? plume::RenderTextureFlag::CUBE : plume::RenderTextureFlag::NONE;
  desc.multisampling.sampleCount = plume::RenderSampleCount::COUNT_1;
  result->image = CreateHostTexture(s.device.get(), desc, "native-texture");
  if (!result->image)
    return {};
  plume::RenderTextureViewDesc view;
  view.format = result->format;
  view.dimension = result->dimension;
  view.mipLevels = data.mip_levels;
  view.arraySize = NativeTextureLayers(data);
  view.arrayIndex = 0;
  result->view = result->image->createTextureView(view);
  if (!result->view)
    return {};

  struct UploadImage {
    ConstantAllocation allocation;
    uint32_t width, height, depth, row_width;
  };
  std::vector<UploadImage> uploads(data.images.size());
  const auto edge = NativeTextureBlockEdge(data.format);
  const auto block = NativeTextureBlockBytes(data.format);
  // Retain only one CPU staging image at a time. All failures precede the
  // descriptor allocation and command recording: no GPU object leaks a slot.
  for (uint32_t i = 0; i < data.images.size(); ++i) {
    const uint32_t mip = i % data.mip_levels;
    auto &u = uploads[i];
    u.width = std::max(data.width >> mip, 1u);
    u.height = std::max(data.height >> mip, 1u);
    u.depth = std::max(data.depth >> mip, 1u);
    const uint32_t rows = (u.height + edge - 1) / edge;
    const uint32_t tight = ((u.width + edge - 1) / edge) * block;
    const uint32_t pitch = (tight + 255u) & ~255u;
    u.row_width = (pitch / block) * edge;
    const uint64_t size = uint64_t(pitch) * rows * u.depth;
    if (size > kNativeTextureMaxBytes)
      return {};
    std::vector<uint8_t> staging(size);
    for (uint64_t row = 0; row < uint64_t(rows) * u.depth; ++row)
      std::memcpy(staging.data() + row * pitch,
                  data.images[i].data() + row * tight, tight);
    u.allocation = UploadHostBytes(staging.data(), uint32_t(size), 0x200);
    if (!u.allocation.memory)
      return {};
  }
  result->descriptor = AllocateSlot(s);
  if (result->descriptor == kInvalidDescriptorIndex)
    return {};
  WriteTextureDescriptor(s, result->descriptor, result->image.get(),
                         result->view.get());
  plume::RenderTextureBarrier pre(result->image.get(),
                                  plume::RenderTextureLayout::COPY_DEST);
  s.command_list->barriers(plume::RenderBarrierStage::COPY, &pre, 1);
  NoteBarrierCall(1, BarrierSite::TexUpload);
  MarkInter(s.command_list);
  for (uint32_t i = 0; i < uploads.size(); ++i) {
    const auto &u = uploads[i];
    s.command_list->copyTextureRegion(
        plume::RenderTextureCopyLocation::Subresource(
            result->image.get(), i % data.mip_levels, i / data.mip_levels),
        plume::RenderTextureCopyLocation::PlacedFootprint(
            u.allocation.ref.ref, result->format, u.width, u.height, u.depth,
            u.row_width, u.allocation.ref.offset));
  }
  plume::RenderTextureBarrier post(result->image.get(),
                                   plume::RenderTextureLayout::SHADER_READ);
  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, &post, 1);
  NoteBarrierCall(1, BarrierSite::TexUpload);
  MarkInter(s.command_list);
  return result;
}
} // namespace

NativeTextureGpuHandle
AcquireNativeTextureGpu(const NativeTextureHandle &asset) {
  if (!asset || !ValidateNativeTexture(asset->data))
    return {};
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (!s.ready || s.shutting_down.load())
    return {};
  BeginCommandList(s);
  if (!s.command_list_open)
    return {};
  if (!s.native_texture_gpu)
    s.native_texture_gpu = std::make_shared<NativeTextureGpuStore>();
  auto &store = *s.native_texture_gpu;
  uint64_t bytes = 0;
  for (const auto &image : asset->data.images)
    bytes += image.size();
  auto result =
      store.images.Acquire(asset->id, bytes, [&] { return Upload(s, asset); });
  if (result && result->asset->data != asset->data) {
    BD_ERROR("[native-texture-gpu] conflicting content ID {:016x}", asset->id);
    return {};
  }
  if (!result || ++store.requests == 1 || store.requests % 64 == 0) {
    const auto a = store.images.Stats();
    BD_INFO("[native-texture-gpu] {} uploaded, {} reused, {} retired, {} "
            "resident, {} payload bytes; {} refused, {} failed",
            a.created, a.reused, a.retired, a.resident, a.bytes, a.refused,
            a.failed);
  }
  return result;
}
void DrainNativeTextureGpuLocked(VideoState &s, uint32_t slot) {
  if (!s.native_texture_gpu)
    return;
  s.native_texture_gpu->images.AfterFence(
      slot, [&](const NativeTextureGpu &gpu) {
        const auto null_index =
            gpu.dimension == plume::RenderTextureViewDimension::TEXTURE_3D
                ? kNullTexture3DDescriptorIndex
                : (gpu.dimension ==
                           plume::RenderTextureViewDimension::TEXTURE_CUBE
                       ? kNullTextureCubeDescriptorIndex
                       : kNullTexture2DDescriptorIndex);
        WriteTextureDescriptor(s, gpu.descriptor,
                               s.null_textures[null_index].get(),
                               s.null_texture_views[null_index].get());
        BindlessFreeSlot(s.descriptor_slot_used, gpu.descriptor,
                         kNullTextureDescriptorCount);
      });
}
void MarkUnusedNativeTextureGpuLocked(VideoState &s, uint32_t recording_slot) {
  if (s.native_texture_gpu)
    s.native_texture_gpu->images.MarkUnused(recording_slot);
}
} // namespace bd::gpu::scene
