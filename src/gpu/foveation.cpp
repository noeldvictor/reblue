/**
 * @file    gpu/foveation.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/foveation.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>

#include "core/logging.h"
#include "gpu/device.h"

REXCVAR_DECLARE(bool, bd_foveation);
REXCVAR_DECLARE(double, bd_foveation_strength);

namespace bd::gpu {

namespace {

// The device reports 32x32 on a Quest 2. Asking for a finer map than the
// hardware's tile size buys nothing - the driver rounds up - and a coarser one
// throws away control for no gain.
constexpr u32 kTile = 32;

struct Map {
  std::unique_ptr<plume::RenderTexture> texture;
  u32 width = 0;
  u32 height = 0;
  std::vector<u8> texels; // pending upload; empty once uploaded
  bool ready = false;     // safe to attach to a render pass
};

std::unordered_map<u64, Map> g_maps;

u64 Key(u32 w, u32 h) { return (u64(w) << 32) | h; }

// Defined below, after the public lookup.
void CreateQueued(u32 width, u32 height);

// Full rate in the middle, falling off toward the edges.
//
// r is the distance from the centre as a fraction of the half-diagonal, so it
// reaches 1 at the corners. Density is 1 in the inner region and eases down to
// `floor` at the corners, which is where a lens has the least acuity and where
// the eye is least likely to be looking in a seated RPG.
u8 DensityAt(float nx, float ny, float floor_density) {
  const float r = std::min(1.0f, std::sqrt(nx * nx + ny * ny));
  constexpr float kFullTo = 0.35f; // full rate out to here
  if (r <= kFullTo)
    return 255;
  const float t = (r - kFullTo) / (1.0f - kFullTo);
  const float d = 1.0f - (1.0f - floor_density) * t * t;
  return static_cast<u8>(std::clamp(d, 0.05f, 1.0f) * 255.0f + 0.5f);
}

// The fragments a pass over this target shades, against the smallest pass
// worth a density attachment (the old 1024x512 threshold, as an area).
bool FoveationBigEnough(u32 width, u32 height, u32 layers) {
  const u64 fragments =
      u64(width) * height * (layers > 1 ? u64(layers) : u64(1));
  return fragments >= 1024ull * 512ull;
}

} // namespace

bool FoveationWanted(u32 width, u32 height, u32 layers) {
  if (!REXCVAR_GET(bd_foveation))
    return false;
  auto *device = Video::HostDevice();
  if (!device || !device->getCapabilities().fragmentDensityMap)
    return false;
  // Only the pass worth foveating. Small post targets are already cheap - the
  // whole post chain is under 8ms of a 56ms frame - and foveating them would
  // add a density attachment, and so a distinct pipeline variant, for nothing.
  //
  // Counted in fragments, not in width. The old test wanted 1024 wide, which
  // the multiview scene layer never is - 960 on the desktop and 688 on a Quest
  // under bd_mv_half_width - so foveation could not fire on the path this port
  // actually ships, whatever the device supported (2026-09-04). A two-layer
  // target shades every fragment twice, which is the whole reason it is the
  // pass worth foveating.
  if (!FoveationBigEnough(width, height, layers))
    return false;

  // AND the map must actually be live. The pipeline and the framebuffer both
  // ask this question, and if one said yes while the map was still pending the
  // framebuffer would get a null density attachment while the pipeline's render
  // pass declared one - incompatible, which Vulkan leaves undefined.
  //
  // Readiness only changes at a frame boundary (FoveationBeginFrame), never
  // mid-frame, so both sides see the same answer for a whole frame.
  const auto it = g_maps.find(Key(width, height));
  return it != g_maps.end() && it->second.ready;
}

void FoveationEnsure(u32 width, u32 height, u32 layers) {
  if (!REXCVAR_GET(bd_foveation) || !FoveationBigEnough(width, height, layers))
    return;
  if (g_maps.find(Key(width, height)) != g_maps.end())
    return;
  auto *device = Video::HostDevice();
  if (!device || !device->getCapabilities().fragmentDensityMap)
    return;
  CreateQueued(width, height);
}

plume::RenderTexture *FoveationMapFor(u32 width, u32 height) {
  const auto it = g_maps.find(Key(width, height));
  return (it != g_maps.end() && it->second.ready) ? it->second.texture.get()
                                                 : nullptr;
}

namespace {

// Build the map and queue its upload for the next frame start.
void CreateQueued(u32 width, u32 height) {
  auto *device = Video::HostDevice();
  if (!device)
    return;

  const u32 mw = (width + kTile - 1) / kTile;
  const u32 mh = (height + kTile - 1) / kTile;

  Map map;
  map.width = mw;
  map.height = mh;
  auto desc = plume::RenderTextureDesc::Texture2D(
      mw, mh, 1, plume::RenderFormat::R8G8_UNORM,
      plume::RenderTextureFlag::FRAGMENT_DENSITY_MAP);
  map.texture = device->createTexture(desc);
  if (!map.texture) {
    BD_ERROR("[foveation] could not create a {}x{} density map", mw, mh);
    return;
  }

  // R is horizontal density, G is vertical. Both fall off together here - an
  // anisotropic map would matter for a wide FOV panel, which this is not.
  const float floor_density =
      float(std::clamp(REXCVAR_GET(bd_foveation_strength), 0.1, 1.0));
  std::vector<u8> texels(size_t(mw) * mh * 2);
  for (u32 y = 0; y < mh; ++y) {
    const float ny = (float(y) + 0.5f) / float(mh) * 2.0f - 1.0f;
    for (u32 x = 0; x < mw; ++x) {
      const float nx = (float(x) + 0.5f) / float(mw) * 2.0f - 1.0f;
      const u8 d = DensityAt(nx, ny, floor_density);
      texels[(size_t(y) * mw + x) * 2 + 0] = d;
      texels[(size_t(y) * mw + x) * 2 + 1] = d;
    }
  }

  // Queued, not uploaded. The upload happens at the next frame start; until
  // then the map is not ready and nothing foveates, so the pipeline and the
  // framebuffer keep agreeing.
  map.texels = std::move(texels);
  BD_INFO("[foveation] {}x{} density map queued for a {}x{} target, floor {:.2f}",
          mw, mh, width, height, floor_density);
  g_maps[Key(width, height)] = std::move(map);
}

} // namespace

void FoveationBeginFrame(plume::RenderCommandList *cmd) {
  if (!cmd)
    return;
  for (auto &[key, map] : g_maps) {
    if (map.ready || map.texels.empty())
      continue;
    if (Video::UploadDensityMap(map.texture.get(), map.texels.data(), map.width,
                                map.height)) {
      map.ready = true;
      map.texels.clear();
      map.texels.shrink_to_fit();
      BD_INFO("[foveation] {}x{} density map uploaded and live", map.width,
              map.height);
    }
  }
}

void FoveationShutdown() { g_maps.clear(); }

} // namespace bd::gpu
