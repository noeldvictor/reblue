/**
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause License
 */
#include "gpu/host_upload.h"
#include "core/logging.h"
#include "gpu/device.h"
#include "gpu/upload_page_arena.h"
#include "gpu/vertex_pull.h"
#include <cstring>

namespace bd::gpu {
namespace {
struct UploadPage {
  std::unique_ptr<plume::RenderBuffer> buffer;
  uint8_t *mapped = nullptr;
  ~UploadPage() {
    if (mapped)
      buffer->unmap();
  }
};
using Arena = UploadPageArena<UploadPage, kNumFrames>;
Arena &Uploads() {
  static Arena arena(4u << 20, 64u << 20, 256ull << 20);
  return arena;
}
} // namespace

HostUploadAllocation AllocateHostUpload(uint32_t size, uint32_t alignment) {
  auto &s = state();
  if (!s.ready || !s.device || !s.command_list_open)
    return {};
  auto &arena = Uploads();
  const auto before = arena.Stats();
  auto allocation =
      arena.Allocate(Video::CurrentFrameSlot(), size, alignment,
                     [&](uint32_t capacity) -> std::unique_ptr<UploadPage> {
                       auto page = std::make_unique<UploadPage>();
                       page->buffer = CreateHostBuffer(
                           s.device.get(),
                           plume::RenderBufferDesc::UploadBuffer(
                               capacity, plume::RenderBufferFlag::VERTEX |
                                             plume::RenderBufferFlag::INDEX |
                                             plume::RenderBufferFlag::STORAGE),
                           "host-upload-page");
                       if (!page->buffer)
                         return {};
                       page->mapped =
                           static_cast<uint8_t *>(page->buffer->map());
                       if (!page->mapped)
                         return {};
                       return page;
                     });
  if (!allocation.resource) {
    const auto failures = arena.Stats().failures;
    if (failures <= 8 || failures % 256 == 0)
      BD_ERROR("[host-upload] refused {} bytes alignment {}: {} reserved, "
               "{} failures; no in-flight data overwritten",
               size, alignment, arena.Stats().reserved, failures);
    return {};
  }
  const auto &stats = arena.Stats();
  if (stats.peak_reserved != before.peak_reserved)
    BD_INFO("[host-upload] {} reserved bytes, peak slot {} bytes, "
            "{} pages created / {} retired; {} failures",
            stats.reserved, stats.peak_frame_bytes, stats.created,
            stats.retired, stats.failures);
  return {allocation.resource->mapped + allocation.offset,
          plume::RenderBufferReference(allocation.resource->buffer.get(),
                                       allocation.offset),
          allocation.size};
}

HostUploadAllocation UploadHostData(const void *data, uint32_t size,
                                    uint32_t alignment) {
  if (!data)
    return {};
  auto allocation = AllocateHostUpload(size, alignment);
  if (allocation.memory)
    std::memcpy(allocation.memory, data, size);
  return allocation;
}

void ResetHostUploadsAfterFence(uint32_t slot) {
  auto &arena = Uploads();
  arena.ResetAfterFence(slot, [](UploadPage &page, bool retiring) {
    // Raw IA bindings are no longer valid even when the page is reused.
    Video::ScrubBufferBindingsLocked(page.buffer.get());
    if (retiring)
      VertexPullForgetBuffer(page.buffer.get());
  });
  static uint64_t resets = 0;
  if (++resets % 600 == 0) {
    const auto &stats = arena.Stats();
    BD_INFO(
        "[host-upload] post-fence {} reserved bytes, peak {} / slot {} bytes, "
        "{} pages created / {} retired; {} failures",
        stats.reserved, stats.peak_reserved, stats.peak_frame_bytes,
        stats.created, stats.retired, stats.failures);
  }
}

bool IsHostUploadBuffer(const plume::RenderBuffer *buffer) {
  return buffer && Uploads().Contains([&](const UploadPage &p) {
    return p.buffer.get() == buffer;
  });
}
} // namespace bd::gpu
