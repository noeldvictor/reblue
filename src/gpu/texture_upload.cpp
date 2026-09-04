/**
 * @file    gpu/texture_upload.cpp
 * @brief   BC and RGBA host mirror texture builders, and mapped-memory
 *          texture upload.
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/bindless_allocator.h"
#include "gpu/frame.h"

#include <memory>
#include <mutex>
#include <vector>
#include <cstring>
#include <atomic>
#include <rex/cvar.h>
#include <rex/runtime.h>

#include <plume_render_interface.h>

#include "core/profiling.h"

#include "core/logging.h"
#include "gpu/constant_buffers.h"
#include "gpu/format.h"
#include "gpu/frame_stats.h"
#include "gpu/gpu_timing.h"
#include "gpu/texture_upload.h"

REXCVAR_DECLARE(bool, bd_native_textures);

namespace bd::gpu {

namespace {

scene::NativeTextureLibrary &TextureAssets() {
  static scene::NativeTextureLibrary library([] {
    std::filesystem::path root;
    if (auto *runtime = rex::Runtime::instance()) root = runtime->cache_root();
    if (root.empty()) root = std::filesystem::current_path();
    return root / "native_textures" / "v1";
  }());
  return library;
}
void NoteNativeTexture() {
  static std::atomic<uint32_t> requests{0};
  const auto n = ++requests;
  if (n != 1 && n % 64) return;
  const auto a = TextureAssets().Stats();
  BD_INFO("[native-texture] {} cooked, {} loaded, {} resident, {} bytes, {} memory hits; "
          "mips {} generated / {} cached; {} invalid, {} write failures, {} budget refusals",
          a.cooked, a.loaded, a.resident, a.bytes, a.memory_hits, a.generated_mips, a.cached_mips,
          a.invalid, a.write_failures, a.budget_refusals);
}
scene::NativeTextureFormat NativeFormat(plume::RenderFormat format) {
  switch (format) {
  case plume::RenderFormat::BC1_UNORM: return scene::NativeTextureFormat::BC1;
  case plume::RenderFormat::BC2_UNORM: return scene::NativeTextureFormat::BC2;
  case plume::RenderFormat::BC3_UNORM: return scene::NativeTextureFormat::BC3;
  case plume::RenderFormat::R8G8B8A8_UNORM: return scene::NativeTextureFormat::RGBA8;
  default: return scene::NativeTextureFormat(0);
  }
}
plume::RenderFormat HostFormat(scene::NativeTextureFormat format) {
  switch (format) {
  case scene::NativeTextureFormat::BC1: return plume::RenderFormat::BC1_UNORM;
  case scene::NativeTextureFormat::BC2: return plume::RenderFormat::BC2_UNORM;
  case scene::NativeTextureFormat::BC3: return plume::RenderFormat::BC3_UNORM;
  case scene::NativeTextureFormat::RGBA8: return plume::RenderFormat::R8G8B8A8_UNORM;
  default: return plume::RenderFormat::UNKNOWN;
  }
}

// Dims + per-subresource upload list describing one host BC/RGBA mirror. The
// four BuildBCMirror* wrappers fill this and delegate to BuildBCMirrorCore.
struct BCMirrorDesc {
  ResourceType resource_type;
  plume::RenderTextureDimension tex_dim;
  plume::RenderTextureViewDimension view_dim;
  plume::RenderFormat format;
  u32 width;
  u32 height;
  u32 depth; // 0 for 2D/cube (GuestTexture::depth default), N volume
  u32 mip_levels;
  u32 array_size; // 1 for 2D/volume, 6 for cube
  plume::RenderTextureFlags flags;
  const char *caller_name;
};

struct BCSubresourceUpload {
  const void *data;
  size_t size;
  u32 width;            // footprint texel width
  u32 height;           // footprint texel height
  u32 fp_depth;         // footprint depth (1 for 2D/mip, N for volume)
  u32 row_width_texels; // PlacedFootprint rowWidth
  u32 subresource;      // mip index
  u32 array_index;      // array/face index (0 for non-cube)
};

// No committed=true: every subresource is copyTextureRegion-uploaded before
// first sample. Per-texture committed heaps for thousands of BC textures
// triggered TDR during load. Uploads precede AllocateSlot so a failure path
// deletes the texture without leaking a slot (GuestTexture has no destructor-
// side slot release).
// 1 when every texel of a BC1/2/3 level has alpha one, 2 when any has not,
// 0 for other formats. Reads the block headers only: BC1's transparent mode
// is colour0 <= colour1 with an index of 3; BC2 carries explicit nibbles;
// BC3's alpha block is opaque only when both endpoints are 255 (an eight-value
// block could still be all-255 through its indices; treated as partial, which
// only keeps a draw on the conservative side).
u8 ScanBCAlpha(plume::RenderFormat format, const void *data, size_t size) {
  const u8 *p = static_cast<const u8 *>(data);
  switch (format) {
  case plume::RenderFormat::BC1_UNORM:
  case plume::RenderFormat::BC1_UNORM_SRGB:
    for (size_t off = 0; off + 8 <= size; off += 8) {
      const u16 c0 = u16(p[off]) | (u16(p[off + 1]) << 8);
      const u16 c1 = u16(p[off + 2]) | (u16(p[off + 3]) << 8);
      if (c0 > c1)
        continue;
      for (u32 b = 4; b < 8; ++b) {
        const u8 idx = p[off + b];
        if ((idx & 3) == 3 || ((idx >> 2) & 3) == 3 || ((idx >> 4) & 3) == 3 ||
            ((idx >> 6) & 3) == 3)
          return 2;
      }
    }
    return 1;
  case plume::RenderFormat::BC2_UNORM:
  case plume::RenderFormat::BC2_UNORM_SRGB:
    for (size_t off = 0; off + 16 <= size; off += 16)
      for (u32 b = 0; b < 8; ++b)
        if (p[off + b] != 0xFF)
          return 2;
    return 1;
  case plume::RenderFormat::BC3_UNORM:
  case plume::RenderFormat::BC3_UNORM_SRGB:
    for (size_t off = 0; off + 16 <= size; off += 16)
      if (p[off] != 0xFF || p[off + 1] != 0xFF)
        return 2;
    return 1;
  default:
    return 0;
  }
}

GuestTexture *UploadTextureBridge(const BCMirrorDesc &d,
                                  const BCSubresourceUpload *uploads,
                                  u32 upload_count,
                                  scene::NativeTextureHandle asset = {}) {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (!s.ready)
    return nullptr;
  BeginCommandList(s);
  if (!s.command_list_open)
    return nullptr;

  auto *t = new GuestTexture(d.resource_type);
  t->nativeAsset = std::move(asset);
  if (t->nativeAsset) t->contentHash = t->nativeAsset->id;
  t->width = d.width;
  t->height = d.height;
  t->depth = d.depth;
  t->mipLevels = d.mip_levels;
  t->format = d.format;
  t->viewDimension = d.view_dim;

  plume::RenderTextureDesc desc;
  desc.dimension = d.tex_dim;
  desc.width = d.width;
  desc.height = d.height;
  desc.depth = (d.depth > 0) ? d.depth : 1;
  desc.mipLevels = d.mip_levels;
  desc.arraySize = d.array_size;
  desc.format = d.format;
  desc.flags = d.flags;
  desc.multisampling.sampleCount = plume::RenderSampleCount::COUNT_1;
  t->textureHolder = CreateHostTexture(s.device.get(), desc, "bc-mirror");
  t->texture = t->textureHolder.get();
  if (!t->texture) {
    delete t;
    return nullptr;
  }

  plume::RenderTextureViewDesc view_desc;
  view_desc.format = d.format;
  view_desc.dimension = d.view_dim;
  view_desc.mipLevels = d.mip_levels;
  t->textureView = t->texture->createTextureView(view_desc);

  std::vector<ConstantAllocation> allocs(upload_count);
  for (u32 i = 0; i < upload_count; ++i) {
    if (uploads[i].subresource == 0 && uploads[i].array_index == 0)
      t->alphaOpaque = ScanBCAlpha(d.format, uploads[i].data, uploads[i].size);
    allocs[i] = UploadHostBytes(uploads[i].data,
                                static_cast<u32>(uploads[i].size), 0x200);
    if (!allocs[i].memory) {
      BD_ERROR("{}: upload of subresource {} ({} bytes) failed", d.caller_name,
               i, uploads[i].size);
      delete t;
      return nullptr;
    }
  }

  const u32 slot = AllocateSlot(s);
  if (slot == kInvalidDescriptorIndex) {
    BD_ERROR("{}: bindless heap full", d.caller_name);
    delete t;
    return nullptr;
  }
  WriteTextureDescriptor(s, slot, t->texture, t->textureView.get());
  t->descriptorIndex = slot;

  plume::RenderTextureBarrier pre(t->texture,
                                  plume::RenderTextureLayout::COPY_DEST);
  s.command_list->barriers(plume::RenderBarrierStage::COPY, &pre, 1);
  NoteBarrierCall(1, BarrierSite::TexUpload);
  MarkInter(s.command_list);
  t->layout = plume::RenderTextureLayout::COPY_DEST;

  for (u32 i = 0; i < upload_count; ++i) {
    s.command_list->copyTextureRegion(
        plume::RenderTextureCopyLocation::Subresource(
            t->texture, uploads[i].subresource, uploads[i].array_index),
        plume::RenderTextureCopyLocation::PlacedFootprint(
            allocs[i].ref.ref, d.format, uploads[i].width, uploads[i].height,
            uploads[i].fp_depth, uploads[i].row_width_texels,
            allocs[i].ref.offset));
  }

  plume::RenderTextureBarrier post(t->texture,
                                   plume::RenderTextureLayout::SHADER_READ);
  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, &post, 1);
  NoteBarrierCall(1, BarrierSite::TexUpload);
  MarkInter(s.command_list);
  t->layout = plume::RenderTextureLayout::SHADER_READ;
  return t;
}

GuestTexture *BuildBCMirrorCore(const BCMirrorDesc &d,
                               const BCSubresourceUpload *uploads, u32 upload_count) {
  if (REXCVAR_GET(bd_native_textures) && uploads && upload_count == d.mip_levels * d.array_size) {
    scene::NativeTextureData data;
    data.format = NativeFormat(d.format);
    data.dimension = d.depth ? scene::NativeTextureDimension::Volume :
        (d.array_size == 6 ? scene::NativeTextureDimension::Cube : scene::NativeTextureDimension::Image2D);
    data.width = d.width; data.height = d.height; data.depth = std::max(d.depth, 1u);
    data.mip_levels = d.mip_levels;
    data.images.resize(upload_count);
    const auto edge = scene::NativeTextureBlockEdge(data.format);
    bool valid = scene::NativeTextureBlockBytes(data.format) != 0;
    for (u32 i = 0; valid && i < upload_count; ++i) {
      const auto &u = uploads[i];
      const auto index = u.array_index * d.mip_levels + u.subresource;
      if (!u.data || u.subresource >= d.mip_levels || u.array_index >= d.array_size ||
          u.width != std::max(d.width >> u.subresource, 1u) ||
          u.height != std::max(d.height >> u.subresource, 1u) ||
          u.fp_depth != std::max(data.depth >> u.subresource, 1u) || !data.images[index].empty()) {
        valid = false; break;
      }
      const uint64_t pitch = uint64_t(u.row_width_texels / edge) * scene::NativeTextureBlockBytes(data.format);
      valid = scene::ImportNativeTextureImage(data.format, u.width, u.height, u.fp_depth,
          {static_cast<const uint8_t *>(u.data), u.size}, pitch,
          pitch * ((u.height + edge - 1) / edge), data.images[index]);
    }
    if (valid) {
      auto asset = TextureAssets().Resolve(std::move(data));
      NoteNativeTexture();
      if (asset)
        if (auto *texture = BuildNativeTexture(asset)) return texture;
    }
  }
  return UploadTextureBridge(d, uploads, upload_count);
}

} // namespace

GuestTexture *BuildNativeTexture(const scene::NativeTextureHandle &asset) {
  if (!asset || !scene::ValidateNativeTexture(asset->data)) return nullptr;
  const auto &data = asset->data;
  BCMirrorDesc d{};
  const bool volume = data.dimension == scene::NativeTextureDimension::Volume;
  const bool cube = data.dimension == scene::NativeTextureDimension::Cube;
  d.resource_type = volume ? ResourceType::VolumeTexture : ResourceType::Texture;
  d.tex_dim = volume ? plume::RenderTextureDimension::TEXTURE_3D : plume::RenderTextureDimension::TEXTURE_2D;
  d.view_dim = volume ? plume::RenderTextureViewDimension::TEXTURE_3D :
      (cube ? plume::RenderTextureViewDimension::TEXTURE_CUBE : plume::RenderTextureViewDimension::TEXTURE_2D);
  d.format = HostFormat(data.format);
  d.width = data.width; d.height = data.height; d.depth = volume ? data.depth : 0;
  d.mip_levels = data.mip_levels; d.array_size = scene::NativeTextureLayers(data);
  d.flags = cube ? plume::RenderTextureFlag::CUBE : plume::RenderTextureFlag::NONE;
  d.caller_name = "BuildNativeTexture";
  std::vector<std::vector<uint8_t>> staging(data.images.size());
  std::vector<BCSubresourceUpload> uploads(data.images.size());
  const auto edge = scene::NativeTextureBlockEdge(data.format);
  const auto block = scene::NativeTextureBlockBytes(data.format);
  for (u32 i = 0; i < data.images.size(); ++i) {
    const u32 mip = i % data.mip_levels;
    const u32 w = std::max(data.width >> mip, 1u), h = std::max(data.height >> mip, 1u);
    const u32 depth = std::max(data.depth >> mip, 1u), rows = (h + edge - 1) / edge;
    const u32 tight = ((w + edge - 1) / edge) * block, pitch = (tight + 255u) & ~255u;
    auto &bytes = staging[i];
    bytes.resize(uint64_t(pitch) * rows * depth);
    for (uint64_t row = 0; row < uint64_t(rows) * depth; ++row)
      std::memcpy(bytes.data() + row * pitch, data.images[i].data() + row * tight, tight);
    uploads[i] = {bytes.data(), bytes.size(), w, h, depth, (pitch / block) * edge, mip, i / data.mip_levels};
  }
  return UploadTextureBridge(d, uploads.data(), u32(uploads.size()), asset);
}

GuestTexture *BuildNativeMipTexture(u32 width, u32 height, u32 guest_format, const BCMipLevel &base) {
  plume::RenderFormat host_format;
  if (!BCFormatFromGuestByte(guest_format, host_format) || !base.data) return nullptr;
  scene::NativeTextureData data;
  data.format = NativeFormat(host_format); data.width = width; data.height = height;
  data.images.resize(1);
  const auto edge = scene::NativeTextureBlockEdge(data.format);
  const uint64_t pitch = uint64_t(base.row_width_texels / edge) * scene::NativeTextureBlockBytes(data.format);
  if (!scene::ImportNativeTextureImage(data.format, width, height, 1,
      {static_cast<const uint8_t *>(base.data), base.size}, pitch,
      pitch * ((height + edge - 1) / edge), data.images[0])) return nullptr;
  auto asset = TextureAssets().Resolve(std::move(data), true);
  NoteNativeTexture();
  return asset ? BuildNativeTexture(asset) : nullptr;
}

GuestTexture *BuildBCMirrorTexture(u32 width, u32 height, u32 format,
                                          const void *block_data,
                                          size_t block_data_size,
                                          u32 staging_row_bytes,
                                          u32 row_width_texels) {
  plume::RenderFormat fmt;
  if (!BCFormatFromGuestByte(format, fmt)) {
    BD_WARN("BuildBCMirrorTexture: unsupported BC format 0x{:X}", format);
    return nullptr;
  }
  BCMirrorDesc d{};
  d.resource_type = ResourceType::Texture;
  d.tex_dim = plume::RenderTextureDimension::TEXTURE_2D;
  d.view_dim = plume::RenderTextureViewDimension::TEXTURE_2D;
  d.format = fmt;
  d.width = width;
  d.height = height;
  d.depth = 0;
  d.mip_levels = 1;
  d.array_size = 1;
  d.flags = plume::RenderTextureFlag::NONE;
  d.caller_name = "BuildBCMirrorTexture";
  BCSubresourceUpload up{};
  up.data = block_data;
  up.size = block_data_size;
  up.width = width;
  up.height = height;
  up.fp_depth = 1;
  up.row_width_texels = row_width_texels;
  up.subresource = 0;
  up.array_index = 0;
  (void)staging_row_bytes;
  return BuildBCMirrorCore(d, &up, 1);
}

GuestTexture *BuildBCMirrorTexture2DMips(u32 width, u32 height,
                                                u32 format,
                                                const BCMipLevel *levels,
                                                u32 level_count) {
  if (!levels || level_count == 0)
    return nullptr;
  plume::RenderFormat fmt;
  if (!BCFormatFromGuestByte(format, fmt)) {
    BD_WARN("BuildBCMirrorTexture2DMips: unsupported format 0x{:X}", format);
    return nullptr;
  }
  BCMirrorDesc d{};
  d.resource_type = ResourceType::Texture;
  d.tex_dim = plume::RenderTextureDimension::TEXTURE_2D;
  d.view_dim = plume::RenderTextureViewDimension::TEXTURE_2D;
  d.format = fmt;
  d.width = width;
  d.height = height;
  d.depth = 0;
  d.mip_levels = level_count;
  d.array_size = 1;
  d.flags = plume::RenderTextureFlag::NONE;
  d.caller_name = "BuildBCMirrorTexture2DMips";
  std::vector<BCSubresourceUpload> ups(level_count);
  for (u32 i = 0; i < level_count; ++i) {
    ups[i].data = levels[i].data;
    ups[i].size = levels[i].size;
    ups[i].width = levels[i].width;
    ups[i].height = levels[i].height;
    ups[i].fp_depth = 1;
    ups[i].row_width_texels = levels[i].row_width_texels;
    ups[i].subresource = i;
    ups[i].array_index = 0;
  }
  return BuildBCMirrorCore(d, ups.data(), level_count);
}

GuestTexture *BuildBCMirrorTextureCube(u32 width, u32 height, u32 format,
                                              const u8 *faces[6],
                                              size_t face_byte_size,
                                              u32 staging_row_bytes,
                                              u32 row_width_texels) {
  plume::RenderFormat fmt;
  if (!BCFormatFromGuestByte(format, fmt)) {
    BD_WARN("BuildBCMirrorTextureCube: unsupported BC format 0x{:X}", format);
    return nullptr;
  }
  BCMirrorDesc d{};
  d.resource_type = ResourceType::Texture;
  d.tex_dim = plume::RenderTextureDimension::TEXTURE_2D;
  d.view_dim = plume::RenderTextureViewDimension::TEXTURE_CUBE;
  d.format = fmt;
  d.width = width;
  d.height = height;
  d.depth = 0;
  d.mip_levels = 1;
  d.array_size = 6;
  d.flags = plume::RenderTextureFlag::CUBE;
  d.caller_name = "BuildBCMirrorTextureCube";
  BCSubresourceUpload ups[6];
  for (int face = 0; face < 6; ++face) {
    ups[face].data = faces[face];
    ups[face].size = face_byte_size;
    ups[face].width = width;
    ups[face].height = height;
    ups[face].fp_depth = 1;
    ups[face].row_width_texels = row_width_texels;
    ups[face].subresource = 0;
    ups[face].array_index = static_cast<u32>(face);
  }
  (void)staging_row_bytes;
  return BuildBCMirrorCore(d, ups, 6);
}

GuestTexture *BuildBCMirrorTextureVolume(
    u32 width, u32 height, u32 depth, u32 format, const void *slice_data,
    size_t slice_data_size, u32 staging_row_bytes, u32 row_width_texels) {
  plume::RenderFormat fmt;
  if (!BCFormatFromGuestByte(format, fmt)) {
    BD_WARN("BuildBCMirrorTextureVolume: unsupported BC format 0x{:X}", format);
    return nullptr;
  }
  BCMirrorDesc d{};
  d.resource_type = ResourceType::VolumeTexture;
  d.tex_dim = plume::RenderTextureDimension::TEXTURE_3D;
  d.view_dim = plume::RenderTextureViewDimension::TEXTURE_3D;
  d.format = fmt;
  d.width = width;
  d.height = height;
  d.depth = depth;
  d.mip_levels = 1;
  d.array_size = 1;
  d.flags = plume::RenderTextureFlag::NONE;
  d.caller_name = "BuildBCMirrorTextureVolume";
  BCSubresourceUpload up{};
  up.data = slice_data;
  up.size = slice_data_size;
  up.width = width;
  up.height = height;
  up.fp_depth = depth;
  up.row_width_texels = row_width_texels;
  up.subresource = 0;
  up.array_index = 0;
  (void)staging_row_bytes;
  return BuildBCMirrorCore(d, &up, 1);
}

void UploadTextureFromMapped(GuestTexture *tex) {
  BD_CPU_ZONE("UploadTextureFromMapped");
  if (!tex || !tex->texture || !tex->mappedMemory)
    return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (!s.ready)
    return;
  BeginCommandList(s);
  if (!s.command_list_open)
    return;

  // Full-subresource CPU upload replaces any deferred resolve into this
  // texture, so drop the link and a later materialization cannot clobber it.
  DetachSourceSurfaceLocked(s, tex);

  const u32 pitch = ComputeTexturePitch(tex);
  const u32 texel = plume::RenderFormatSize(tex->format);
  if (!pitch || !texel)
    return;
  const u32 slice_pitch = pitch * tex->height;

  // No byte swap: X360 texture texels are not dword-swapped, so this is a
  // plain copy. Per-format channel order is not yet handled.
  ConstantAllocation up =
      UploadGuestBytes(tex->mappedMemory, slice_pitch, 0x200);
  if (!up.memory)
    return;

  plume::RenderTextureBarrier pre(tex->texture,
                                  plume::RenderTextureLayout::COPY_DEST);
  s.command_list->barriers(plume::RenderBarrierStage::COPY, &pre, 1);
  NoteBarrierCall(1, BarrierSite::TexUpload);
  MarkInter(s.command_list);
  tex->layout = plume::RenderTextureLayout::COPY_DEST;

  // rowWidth is in texels: pitch (bytes) / per-texel size.
  s.command_list->copyTextureRegion(
      plume::RenderTextureCopyLocation::Subresource(tex->texture, 0),
      plume::RenderTextureCopyLocation::PlacedFootprint(
          up.ref.ref, tex->format, tex->width, tex->height, 1, pitch / texel,
          up.ref.offset));

  plume::RenderTextureBarrier post(tex->texture,
                                   plume::RenderTextureLayout::SHADER_READ);
  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, &post, 1);
  NoteBarrierCall(1, BarrierSite::TexUpload);
  MarkInter(s.command_list);
  tex->layout = plume::RenderTextureLayout::SHADER_READ;
}

} // namespace bd::gpu
