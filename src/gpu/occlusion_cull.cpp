/**
 * @file    gpu/occlusion_cull.cpp
 * @brief   Host occlusion culling on the scene walk.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/occlusion_cull.h"

#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>

#include "core/logging.h"
#include "gpu/backend.h"
#include "gpu/constant_buffers.h"
#include "gpu/device.h"
#include "gpu/frame_stats.h"
#include "gpu/resources.h"
#include "gpu/shadow_fit.h"

#if defined(REBLUE_D3D12)
#include "src/gpu/shaders/hlsl/occ_proxy_vs.hlsl.dxil.h"
#include "src/gpu/shaders/hlsl/occ_proxy_ps.hlsl.dxil.h"
#else
#include "src/gpu/shaders/hlsl/occ_proxy_vs.hlsl.spirv.h"
#include "src/gpu/shaders/hlsl/occ_proxy_ps.hlsl.spirv.h"
#endif

REXCVAR_DECLARE(bool, bd_occlusion_cull);
REXCVAR_DECLARE(bool, bd_occlusion_diag);

namespace bd::gpu {
namespace {

constexpr u32 kQueriesPerSlot = 2048;
// Spheres per uploaded block: 256 registers less the four projection rows.
constexpr u32 kSpheresPerBlock = 252;

struct Proxy {
  u64 key;
  float centre[3];
  float radius;
};

struct Slot {
  std::unique_ptr<plume::RenderQueryPool> pool;
  std::vector<u64> keys; // per query index
  u32 used = 0;
  bool pending = false;
};

struct NodeState {
  u32 zero_frames = 0;   // consecutive results with no sample
  u32 last_result = 0;   // frame of the last result
};

struct State {
  Slot slots[kNumFrames];
  u32 active = ~0u;
  bool unsupported = false;
  std::vector<Proxy> proxies; // this frame's, camera view
  std::unordered_map<u64, NodeState> nodes;
  std::unique_ptr<plume::RenderShader> vs;
  std::unique_ptr<plume::RenderShader> ps;
  bool shaders_tried = false;
  std::unordered_map<u64, std::unique_ptr<plume::RenderPipeline>> pipelines;
  // Counters, per 300 frames.
  u32 n_noted = 0, n_skipped = 0, n_queried = 0, n_occluded_results = 0;
  u32 diag_frame = 0;
};

State &st() {
  static State s;
  return s;
}

plume::RenderPipeline *PipelineFor(VideoState &s, const GuestTexture *rt,
                                   const GuestTexture *ds) {
  auto &o = st();
  if (!o.shaders_tried) {
    o.shaders_tried = true;
    o.vs = s.device->createShader(REBLUE_BLOB_SYMBOL(occ_proxy_vs),
                                  sizeof(REBLUE_BLOB_SYMBOL(occ_proxy_vs)),
                                  "main", kHostShaderFormat);
    o.ps = s.device->createShader(REBLUE_BLOB_SYMBOL(occ_proxy_ps),
                                  sizeof(REBLUE_BLOB_SYMBOL(occ_proxy_ps)),
                                  "main", kHostShaderFormat);
    if (!o.vs || !o.ps)
      BD_ERROR("[occ] proxy shaders failed; occlusion culling is off");
  }
  if (!o.vs || !o.ps || !s.pipeline_layout)
    return nullptr;
  const u64 key = (u64(rt->format) << 32) ^ (u64(ds->format) << 8) ^
                  u64(rt->sampleCount) ^ (u64(rt->layers > 1) << 63);
  auto it = o.pipelines.find(key);
  if (it != o.pipelines.end())
    return it->second.get();
  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = s.pipeline_layout.get();
  desc.vertexShader = o.vs.get();
  desc.pixelShader = o.ps.get();
  desc.depthFunction = plume::RenderComparisonFunction::LESS_EQUAL;
  desc.depthEnabled = true;
  desc.depthWriteEnabled = false;
  desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  desc.cullMode = plume::RenderCullMode::NONE;
  desc.renderTargetCount = 1;
  desc.renderTargetFormat[0] = rt->format;
  desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
  desc.renderTargetBlend[0].renderTargetWriteMask = 0;
  desc.depthTargetFormat = ds->format;
  desc.multisampling.sampleCount = rt->sampleCount;
  desc.viewMask = rt->layers > 1 ? 0x3u : 0u;
  auto pipe = CreateHostGraphicsPipeline(s.device.get(), desc, "occ-proxy");
  plume::RenderPipeline *raw = pipe.get();
  o.pipelines.emplace(key, std::move(pipe));
  if (!raw)
    BD_ERROR("[occ] proxy pipeline for formats {}/{} failed", u32(rt->format),
             u32(ds->format));
  return raw;
}

} // namespace

void OcclusionCullFrameBegin(plume::RenderDevice *device,
                             plume::RenderCommandList *cmd, u32 slot) {
  auto &o = st();
  o.active = ~0u;
  o.proxies.clear();
  if (!REXCVAR_GET(bd_occlusion_cull) || o.unsupported || !device || !cmd ||
      slot >= kNumFrames)
    return;
  Slot &sl = o.slots[slot];
  if (!sl.pool) {
    sl.pool = device->createOcclusionQueryPool(kQueriesPerSlot);
    if (!sl.pool) {
      o.unsupported = true;
      BD_INFO("[occ] occlusion queries are not available; culling is off");
      return;
    }
    sl.keys.resize(kQueriesPerSlot, 0ull);
    BD_INFO("[occ] query pool for slot {} created", slot);
  }
  sl.used = 0;
  sl.pending = false;
  cmd->resetQueryPool(sl.pool.get(), 0, kQueriesPerSlot);
  o.active = slot;
}

void OcclusionCullCollect(u32 slot) {
  auto &o = st();
  if (slot >= kNumFrames)
    return;
  Slot &sl = o.slots[slot];
  if (!sl.pending || !sl.pool || sl.used == 0)
    return;
  sl.pending = false;
  sl.pool->queryResults(sl.used);
  const u64 *results = sl.pool->getResults();
  const u32 frame = FrameStatFrameCount();
  for (u32 i = 0; i < sl.used; ++i) {
    NodeState &n = o.nodes[sl.keys[i]];
    if (results[i] == 0) {
      ++n.zero_frames;
      ++o.n_occluded_results;
    } else {
      n.zero_frames = 0;
    }
    n.last_result = frame;
  }
  if (frame - o.diag_frame >= 300) {
    if (o.diag_frame && REXCVAR_GET(bd_occlusion_diag))
      BD_INFO("[occ] per frame: {:.1f} nodes noted, {:.1f} queried, {:.1f} "
              "results occluded, {:.1f} draws skipped; {} nodes tracked",
              o.n_noted / 300.0, o.n_queried / 300.0,
              o.n_occluded_results / 300.0, o.n_skipped / 300.0,
              o.nodes.size());
    o.n_noted = o.n_queried = o.n_occluded_results = o.n_skipped = 0;
    o.diag_frame = frame;
    // Nodes not seen for a while drop out of the table.
    for (auto it = o.nodes.begin(); it != o.nodes.end();) {
      if (frame - it->second.last_result > 600)
        it = o.nodes.erase(it);
      else
        ++it;
    }
  }
}

bool OcclusionCullOccluded(u64 key) {
  auto &o = st();
  if (o.active >= kNumFrames)
    return false;
  auto it = o.nodes.find(key);
  if (it == o.nodes.end())
    return false;
  const u32 frame = FrameStatFrameCount();
  // Two zero results running, the latest from the previous frame or the one
  // before (the slot's fence can lag a frame).
  const bool occluded = it->second.zero_frames >= 2 &&
                        frame - it->second.last_result <= 2;
  if (occluded)
    ++o.n_skipped;
  return occluded;
}

void OcclusionCullNote(u64 key, const float centre[3], float radius) {
  auto &o = st();
  if (o.active >= kNumFrames || o.proxies.size() >= kQueriesPerSlot)
    return;
  Proxy p;
  p.key = key;
  std::memcpy(p.centre, centre, sizeof(p.centre));
  p.radius = radius;
  o.proxies.push_back(p);
  ++o.n_noted;
}

void OcclusionCullEmit(VideoState &s) {
  auto &o = st();
  static u32 why_told = 0;
  auto why = [&](const char *reason) {
    if (why_told++ < 6)
      BD_INFO("[occ] emit skipped: {} (active {}, proxies {})", reason,
              o.active, o.proxies.size());
  };
  if (o.active >= kNumFrames || o.proxies.empty() || !s.command_list) {
    if (o.active < kNumFrames && o.proxies.empty())
      why("no proxies noted this frame");
    return;
  }
  const GuestTexture *rt = s.bound_fb_rt;
  const GuestTexture *ds = s.bound_fb_ds;
  if (!rt || !ds) {
    why("no bound targets");
    return;
  }
  float proj[16];
  u32 proj_frame = 0;
  if (!ShadowFitCamera(proj, proj_frame) ||
      proj_frame != FrameStatFrameCount()) {
    if (why_told < 6)
      BD_INFO("[occ] camera frame {} vs frame {}", proj_frame, FrameStatFrameCount());
    why("no camera this frame");
    o.proxies.clear();
    return;
  }
  auto *pipe = PipelineFor(s, rt, ds);
  auto *set = Video::ConstantDescriptorSet();
  if (!pipe || !set) {
    o.proxies.clear();
    return;
  }
  Slot &sl = o.slots[o.active];
  auto *cmd = s.command_list;
  // The queue's flush leaves whatever framebuffer its last draw carried;
  // the proxies go into the scene's, over its whole extent.
  if (!s.pending_framebuffer) {
    o.proxies.clear();
    return;
  }
  cmd->setFramebuffer(s.pending_framebuffer);
  cmd->setViewports(plume::RenderViewport(0.0f, 0.0f, float(rt->width), float(rt->height)));
  cmd->setScissors(plume::RenderRect(0, 0, i32(rt->width), i32(rt->height)));
  cmd->setPipeline(pipe);
  // One block per kSpheresPerBlock proxies: the projection rows, then the
  // spheres, bound through the guest vertex constant window.
  std::vector<float> block(256 * 4);
  size_t i = 0;
  while (i < o.proxies.size() && sl.used < kQueriesPerSlot) {
    const size_t n = std::min<size_t>(kSpheresPerBlock, o.proxies.size() - i);
    std::memset(block.data(), 0, block.size() * sizeof(float));
    std::memcpy(block.data(), proj, sizeof(proj));
    for (size_t k = 0; k < n; ++k) {
      const Proxy &p = o.proxies[i + k];
      float *dst = block.data() + (4 + k) * 4;
      dst[0] = p.centre[0];
      dst[1] = p.centre[1];
      dst[2] = p.centre[2];
      dst[3] = p.radius;
    }
    auto alloc = UploadHostConstants(block.data(),
                                     static_cast<u32>(block.size() * sizeof(float)));
    if (!alloc.size)
      break;
    u32 offsets[3] = {alloc.dynamicOffset, s.constant_dyn_offsets[1],
                      s.constant_dyn_offsets[2]};
    cmd->setGraphicsDescriptorSetDynamic(set, kConstantDescriptorSetIndex,
                                         offsets, 3);
    for (size_t k = 0; k < n && sl.used < kQueriesPerSlot; ++k) {
      const u32 q = sl.used++;
      sl.keys[q] = o.proxies[i + k].key;
      cmd->beginQuery(sl.pool.get(), q);
      cmd->drawInstanced(36, 1, 0, static_cast<u32>(k));
      cmd->endQuery(sl.pool.get(), q);
      ++o.n_queried;
    }
    i += n;
  }
  sl.pending = sl.used > 0;
  o.proxies.clear();
  // The next guest draw rebinds its own pipeline and constants: the draw
  // path re-emits both on the next flush.
  s.dirtyStates.pipelineState = true;
  InvalidateSharedBinding();
}

} // namespace bd::gpu
