/**
 * @file    gpu/sampler_cache.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/sampler_cache.h"
#include "gpu/sampler_key.h"

#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <rex/graphics/xenos.h>

#include "core/logging.h"
#include "gpu/bindless_allocator.h"
#include "gpu/device.h"
#include "gpu/settings.h"

REXCVAR_DECLARE(f64, bd_debug_mip_bias);

namespace bd::gpu {

namespace {

namespace xe = rex::graphics::xenos;

struct CachedSampler {
  std::unique_ptr<plume::RenderSampler> sampler;
  u32 slot = 0;
};

struct Cache {
  std::unordered_map<SamplerKey, CachedSampler, SamplerKeyHash> map;
  std::mutex mutex;
};

Cache &cache() {
  static Cache c;
  return c;
}

// plume has no half-way clamp, so those take the plain clamp of the same
// mirroring.
plume::RenderTextureAddressMode ConvertClamp(xe::ClampMode mode) {
  using A = plume::RenderTextureAddressMode;
  switch (mode) {
  case xe::ClampMode::kRepeat:
    return A::WRAP;
  case xe::ClampMode::kMirroredRepeat:
    return A::MIRROR;
  case xe::ClampMode::kClampToEdge:
  case xe::ClampMode::kClampToHalfway:
    return A::CLAMP;
  case xe::ClampMode::kMirrorClampToEdge:
  case xe::ClampMode::kMirrorClampToHalfway:
    return A::MIRROR_ONCE;
  case xe::ClampMode::kClampToBorder:
    return A::BORDER;
  case xe::ClampMode::kMirrorClampToBorder:
    return A::MIRROR_ONCE;
  }
  return A::CLAMP;
}

// Only kPoint is nearest: kLinear, kBaseMap and kUseFetchConst all sample
// linearly.
bool IsNearest(xe::TextureFilter filter) {
  return filter == xe::TextureFilter::kPoint;
}

} // namespace

plume::RenderSamplerDesc DecodeSamplerRecipe(const u32 fc[6]) {
  xe::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, fc, sizeof(fetch));

  plume::RenderSamplerDesc d;
  d.addressU = ConvertClamp(fetch.clamp_x);
  d.addressV = ConvertClamp(fetch.clamp_y);
  d.addressW = ConvertClamp(fetch.clamp_z);

  d.magFilter = IsNearest(fetch.mag_filter) ? plume::RenderFilter::NEAREST
                                            : plume::RenderFilter::LINEAR;
  d.minFilter = IsNearest(fetch.min_filter) ? plume::RenderFilter::NEAREST
                                            : plume::RenderFilter::LINEAR;
  d.mipmapMode = IsNearest(fetch.mip_filter) ? plume::RenderMipmapMode::NEAREST
                                             : plume::RenderMipmapMode::LINEAR;

  d.borderColor = fetch.border_color == xe::BorderColor::k_ABGR_White
                      ? plume::RenderBorderColor::OPAQUE_WHITE
                      : plume::RenderBorderColor::TRANSPARENT_BLACK;
  return d;
}

plume::RenderSamplerDesc ApplySamplerPolicy(plume::RenderSamplerDesc d,
                                          i32 aniso, float mip_bias,
                                          bool clamp_volume) {

  // Anisotropy is off by default, as BD's fetch constant sets aniso_filter to
  // 0. bd_anisotropy opts in, and only when every filter is already LINEAR:
  // D3D12_FILTER_ANISOTROPIC hardcodes LINEAR min/mag, so enabling aniso on a
  // POINT sampler silently upgrades it to bilinear on D3D12 but not on Vulkan.
  const bool aniso_on = aniso > 0 &&
                        d.mipmapMode == plume::RenderMipmapMode::LINEAR &&
                        d.minFilter == plume::RenderFilter::LINEAR &&
                        d.magFilter == plume::RenderFilter::LINEAR;
  d.anisotropyEnabled = aniso_on;
  d.maxAnisotropy = aniso_on ? static_cast<u32>(aniso) : 1u;

  // A probe, not a setting: the Quest's counters read 0.9% of texture
  // fetches from a non-base level after the host mip chains landed
  // (2026-09-03), the same as before them. A large positive bias makes every
  // reachable chain visible as blur in a capture; no blur means the chain is
  // not what the sampler sees.
  d.mipLODBias = mip_bias;
  // Shell fur W is shell depth, not a repeating coordinate.
  if (clamp_volume)
    d.addressW = plume::RenderTextureAddressMode::CLAMP;
  return d;
}

plume::RenderSamplerDesc DecodeFromFetch(const u32 fc[6]) {
  return ApplySamplerPolicy(DecodeSamplerRecipe(fc), Settings::Get().Anisotropy(),
                            float(REXCVAR_GET(bd_debug_mip_bias)), false);
}

u32 ResolveSlotLocked(const plume::RenderSamplerDesc &desc) {
  auto &s = state();
  if (!s.ready || !s.device || !s.sampler_descriptor_set)
    return 0;

  const SamplerKey key(desc);

  auto &c = cache();
  {
    std::lock_guard lock(c.mutex);
    auto it = c.map.find(key);
    if (it != c.map.end())
      return it->second.slot;
  }

  // Heap slot alloc relies on the state().mutex the caller already holds.
  const u32 slot = BindlessAllocateSlot(s.sampler_descriptor_used, 1, 0);
  if (slot == 0) {
    static std::atomic<u32> warned{0};
    if (warned.fetch_add(1, std::memory_order_relaxed) < 4) {
      BD_WARN(
          "[sampler-cache] bindless sampler heap full, falling back to slot 0");
    }
    return 0;
  }

  auto sampler = s.device->createSampler(desc);
  if (!sampler) {
    s.sampler_descriptor_used[slot] = false;
    BD_WARN("[sampler-cache] native sampler creation failed");
    return 0;
  }
  s.sampler_descriptor_set->setSampler(SamplerDescriptor(slot), sampler.get());

  std::lock_guard lock(c.mutex);
  // On a race, return the winner's slot. Ours leaks (no reclaim on drop here).
  auto it = c.map.find(key);
  if (it != c.map.end())
    return it->second.slot;
  c.map.emplace(key, CachedSampler{std::move(sampler), slot});
  return slot;
}

} // namespace bd::gpu
