/**
 * @file    mesh_lod.cpp
 * @brief   Vertex-clustered index lists for the shadow and reflection views.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license   MIT
 */
#include "gpu/scene/mesh_lod.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>
#include <rex/graphics/xenos.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/device.h"
#include "gpu/format.h"
#include "gpu/frame_stats.h"

namespace bd::gpu::scene {

namespace {

struct Entry {
  std::unique_ptr<plume::RenderBuffer> buffer;
  u32 count = 0;
  plume::RenderFormat format = plume::RenderFormat::R16_UINT;
  bool usable = false;
};

struct State {
  std::mutex mutex;
  std::unordered_map<u64, Entry> entries;
  u32 built = 0, hits = 0, refused = 0;
  u64 tris_in = 0, tris_out = 0;
  u32 last_log_frame = 0;
};

State &state_() {
  static State st;
  return st;
}

u64 Mix(u64 h, u64 v) {
  h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  return h;
}

u64 KeyOf(const MeshLodRequest &r) {
  u64 h = 0x1234567ull;
  h = Mix(h, reinterpret_cast<u64>(r.index_buffer));
  h = Mix(h, reinterpret_cast<u64>(r.vertex_buffer));
  h = Mix(h, r.index_mirror_va);
  h = Mix(h, r.vertex_mirror_va);
  h = Mix(h, (u64(r.start_index) << 32) | r.count);
  h = Mix(h, (u64(u32(r.base_vertex)) << 32) | r.stream_offset);
  h = Mix(h, (u64(r.stride) << 32) | r.position_offset);
  h = Mix(h, (u64(r.position_type) << 32) | r.grid);
  h = Mix(h, u64(r.primitive_type) | (u64(r.index_format) << 8));
  return h;
}

inline float HalfToFloat(u16 h) {
  const u32 sign = (h >> 15) & 1u;
  const u32 exp = (h >> 10) & 0x1Fu;
  const u32 man = h & 0x3FFu;
  u32 bits;
  if (exp == 0) {
    if (man == 0) {
      bits = sign << 31;
    } else {
      // Subnormal: normalise.
      u32 e = 127 - 15 + 1;
      u32 m = man;
      while (!(m & 0x400u)) {
        m <<= 1;
        --e;
      }
      m &= 0x3FFu;
      bits = (sign << 31) | (e << 23) | (m << 13);
    }
  } else if (exp == 31) {
    bits = (sign << 31) | 0x7F800000u | (man << 13);
  } else {
    bits = (sign << 31) | ((exp - 15 + 127) << 23) | (man << 13);
  }
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

// Reads one vertex's position from guest memory. Component order does not
// matter to the clustering (a fixed permutation of axes clusters identically),
// so the 16-bit pair order the engine's dword swap leaves is not undone.
bool ReadPosition(const u8 *v, u32 type, float out[3]) {
  switch (static_cast<D3DDeclType>(type)) {
  case D3DDeclType::kFloat3:
  case D3DDeclType::kFloat4:
    for (u32 i = 0; i < 3; ++i) {
      u32 w;
      std::memcpy(&w, v + i * 4, 4);
      w = __builtin_bswap32(w);
      std::memcpy(&out[i], &w, 4);
    }
    return true;
  case D3DDeclType::kShort4:
  case D3DDeclType::kShort4N:
  case D3DDeclType::kUShort4N:
  case D3DDeclType::kShort2:
  case D3DDeclType::kShort2N:
  case D3DDeclType::kUShort2N:
    for (u32 i = 0; i < 3; ++i) {
      u16 w;
      std::memcpy(&w, v + i * 2, 2);
      w = __builtin_bswap16(w);
      out[i] = float(static_cast<i16>(w));
    }
    return true;
  case D3DDeclType::kFloat16_4:
  case D3DDeclType::kFloat16_2:
    for (u32 i = 0; i < 3; ++i) {
      u16 w;
      std::memcpy(&w, v + i * 2, 2);
      out[i] = HalfToFloat(__builtin_bswap16(w));
    }
    return true;
  default:
    return false;
  }
}

// Vertex clustering onto original vertices at one grid: cells across the
// mesh's longest axis; each cell keeps the vertex nearest its centroid;
// triangles remap, degenerate and duplicate ones drop.
std::vector<u32> Cluster(const std::vector<u32> &tris, const std::vector<float> &pos,
                         const std::vector<u8> &seen, u32 max_index,
                         const float mn[3], float ext, u32 grid) {
  const float cell = ext / float(grid);
  const u32 g1 = grid + 1;
  const u32 ntri = u32(tris.size() / 3);
  struct Cell {
    float sum[3] = {0, 0, 0};
    u32 n = 0;
    u32 rep = ~0u;
    float best = 1e30f;
  };
  std::unordered_map<u32, Cell> cells;
  cells.reserve(ntri);
  auto cell_of = [&](const float *p) {
    u32 c[3];
    for (u32 i = 0; i < 3; ++i)
      c[i] = std::min(u32((p[i] - mn[i]) / cell), grid);
    return c[0] + c[1] * g1 + c[2] * g1 * g1;
  };
  for (u32 v = 0; v <= max_index; ++v) {
    if (!(seen[v >> 3] & (1u << (v & 7))))
      continue;
    const float *p = &pos[size_t(v) * 3];
    Cell &c = cells[cell_of(p)];
    for (u32 i = 0; i < 3; ++i)
      c.sum[i] += p[i];
    ++c.n;
  }
  for (u32 v = 0; v <= max_index; ++v) {
    if (!(seen[v >> 3] & (1u << (v & 7))))
      continue;
    const float *p = &pos[size_t(v) * 3];
    Cell &c = cells[cell_of(p)];
    float d = 0.0f;
    for (u32 i = 0; i < 3; ++i) {
      const float dd = p[i] - c.sum[i] / float(c.n);
      d += dd * dd;
    }
    if (d < c.best) {
      c.best = d;
      c.rep = v;
    }
  }
  std::vector<u32> out;
  out.reserve(tris.size());
  std::unordered_set<u64> dedupe;
  dedupe.reserve(ntri);
  for (u32 t = 0; t < ntri; ++t) {
    const u32 a = cells[cell_of(&pos[size_t(tris[t * 3]) * 3])].rep;
    const u32 b = cells[cell_of(&pos[size_t(tris[t * 3 + 1]) * 3])].rep;
    const u32 c = cells[cell_of(&pos[size_t(tris[t * 3 + 2]) * 3])].rep;
    if (a == b || b == c || a == c)
      continue;
    // Duplicate triangles (same three vertices, same winding) collapse; a
    // flipped duplicate is kept, both sides of a caster matter to a
    // one-sided shadow pass.
    u32 s0 = a, s1 = b, s2 = c;
    if (s1 < s0)
      std::swap(s0, s1);
    if (s2 < s1)
      std::swap(s1, s2);
    if (s1 < s0)
      std::swap(s0, s1);
    const bool same_winding = (a == s0 && b == s1) || (b == s0 && c == s1) ||
                              (c == s0 && a == s1);
    const u64 key = (u64(s0) << 42) ^ (u64(s1) << 21) ^ u64(s2) ^
                    (same_winding ? 0 : (1ull << 63));
    if (!dedupe.insert(key).second)
      continue;
    out.push_back(a);
    out.push_back(b);
    out.push_back(c);
  }
  return out;
}

u32 g_reason[40];
#define REFUSE(n) do { ++g_reason[n]; return false; } while (0)
bool Build(const MeshLodRequest &r, Entry &e, u64 &tris_in, u64 &tris_out) {
  if (!r.device || !r.count || !r.stride || !r.grid || r.grid > 256)
    REFUSE(1);
  const bool idx32 = r.index_format == plume::RenderFormat::R32_UINT;
  const u32 isz = idx32 ? 4 : 2;
  if (u64(r.start_index + r.count) * isz > r.index_mirror_size)
    REFUSE(2);
  const u8 *ib = bd::mem::try_at<u8>(r.index_mirror_va + r.start_index * isz);
  if (!ib)
    REFUSE(3);
  const u8 *vb_base = bd::mem::try_at<u8>(r.vertex_mirror_va);
  if (!vb_base)
    REFUSE(4);
  const u64 vb_size = r.vertex_mirror_size;

  // The source indices, and the triangle list they describe.
  std::vector<u32> idx(r.count);
  u32 max_index = 0;
  // The guest's strips restart at the all-ones index (0xFFFF in a 16-bit
  // buffer): a run ends there and the next begins with fresh parity.
  const u32 restart = idx32 ? 0xFFFFFFFFu : 0xFFFFu;
  for (u32 i = 0; i < r.count; ++i) {
    u32 v;
    if (idx32) {
      u32 w;
      std::memcpy(&w, ib + i * 4, 4);
      v = __builtin_bswap32(w);
    } else {
      u16 w;
      std::memcpy(&w, ib + i * 2, 2);
      v = __builtin_bswap16(w);
    }
    idx[i] = v;
    if (v != restart)
      max_index = std::max(max_index, v);
  }
  std::vector<u32> tris; // triples
  tris.reserve(r.count * 3);
  const auto prim = static_cast<rex::graphics::xenos::PrimitiveType>(r.primitive_type);
  if (prim == rex::graphics::xenos::PrimitiveType::kTriangleStrip) {
    u32 run = 0;
    for (u32 t = 0; t < r.count; ++t) {
      if (idx[t] == restart) {
        run = 0;
        continue;
      }
      if (++run < 3)
        continue;
      u32 a = idx[t - 2], b = idx[t - 1], c = idx[t];
      if (a == b || b == c || a == c)
        continue;
      if ((run - 3) & 1)
        std::swap(b, c);
      tris.push_back(a);
      tris.push_back(b);
      tris.push_back(c);
    }
  } else if (prim == rex::graphics::xenos::PrimitiveType::kTriangleList) {
    for (u32 t = 0; t + 2 < r.count; t += 3) {
      const u32 a = idx[t], b = idx[t + 1], c = idx[t + 2];
      if (a == b || b == c || a == c || a == restart || b == restart ||
          c == restart)
        continue;
      tris.push_back(a);
      tris.push_back(b);
      tris.push_back(c);
    }
  } else {
    REFUSE(5);
  }
  const u32 ntri = u32(tris.size() / 3);
  if (ntri < 8)
    REFUSE(6);
  tris_in = ntri;

  // The referenced vertices' positions.
  const u64 vfirst = u64(i64(r.base_vertex));
  std::vector<u8> seen((max_index + 8) / 8, 0);
  std::vector<float> pos(size_t(max_index + 1) * 3, 0.0f);
  float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
  for (u32 v : tris) {
    if (seen[v >> 3] & (1u << (v & 7)))
      continue;
    seen[v >> 3] |= u8(1u << (v & 7));
    const u64 at = u64(r.stream_offset) + (vfirst + v) * r.stride + r.position_offset;
    if (at + 12 > vb_size) {
      REFUSE(7);
    }
    float *p = &pos[size_t(v) * 3];
    if (!ReadPosition(vb_base + at, r.position_type, p))
      REFUSE(8);
    for (u32 i = 0; i < 3; ++i) {
      if (!std::isfinite(p[i]))
        REFUSE(9);
      mn[i] = std::min(mn[i], p[i]);
      mx[i] = std::max(mx[i], p[i]);
    }
  }
  const float ext = std::max({mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]});
  if (!(ext > 0.0f))
    REFUSE(10);
  auto area_of = [&](const std::vector<u32> &list) {
    double area = 0.0;
    for (size_t t = 0; t + 2 < list.size(); t += 3) {
      const float *a = &pos[size_t(list[t]) * 3];
      const float *b = &pos[size_t(list[t + 1]) * 3];
      const float *c = &pos[size_t(list[t + 2]) * 3];
      const float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
      const float v[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
      const float x = u[1] * v[2] - u[2] * v[1];
      const float y = u[2] * v[0] - u[0] * v[2];
      const float z = u[0] * v[1] - u[1] * v[0];
      area += 0.5 * std::sqrt(double(x) * x + double(y) * y + double(z) * z);
    }
    return area;
  };
  const double area_in = area_of(tris);
  std::vector<u32> out;
  // A thin caster narrower than a cell flattens to a line and its shadow
  // goes with it (a fence's shadow became a sliver at grid 24, 2026-09-04):
  // the list must keep most of the mesh's surface, and a grid that does
  // not is refined until one does or the saving is gone.
  for (u32 grid = r.grid; grid <= 256; grid *= 2) {
    out = Cluster(tris, pos, seen, max_index, mn, ext, grid);
    const u32 out_tris = u32(out.size() / 3);
    if (out_tris == 0 || out_tris * 5 > ntri * 4) {
      out.clear();
      break; // saves too little already; finer saves less
    }
    if (area_in <= 0.0 || area_of(out) >= 0.8 * area_in)
      break;
    out.clear();
  }
  const u32 out_tris = u32(out.size() / 3);
  tris_out = out_tris;
  if (out_tris == 0)
    REFUSE(11);

  const u32 bytes = u32(out.size()) * isz;
  auto buffer = r.device->createBuffer(
      plume::RenderBufferDesc::UploadBuffer(bytes, plume::RenderBufferFlag::INDEX));
  if (!buffer)
    REFUSE(12);
  void *m = buffer->map();
  if (!m)
    REFUSE(13);
  if (idx32) {
    std::memcpy(m, out.data(), bytes);
  } else {
    u16 *d = static_cast<u16 *>(m);
    for (size_t i = 0; i < out.size(); ++i)
      d[i] = u16(out[i]);
  }
  buffer->unmap();
  e.buffer = std::move(buffer);
  e.count = u32(out.size());
  e.format = r.index_format;
  e.usable = true;
  return true;
}

} // namespace

bool MeshLodFor(const MeshLodRequest &req, MeshLodResult &out) {
  auto &st = state_();
  const u64 key = KeyOf(req);
  std::lock_guard lock(st.mutex);
  auto it = st.entries.find(key);
  if (it == st.entries.end()) {
    if (st.entries.size() >= 8192) {
      // The cache is bounded by parking every buffer for the fence and
      // starting over; the next frame rebuilds what it still draws.
      for (auto &kv : st.entries)
        if (kv.second.buffer)
          Video::ParkBufferUntilFence(std::move(kv.second.buffer));
      st.entries.clear();
    }
    Entry e;
    u64 tin = 0, tout = 0;
    const bool ok = Build(req, e, tin, tout);
    if (ok) {
      ++st.built;
      st.tris_in += tin;
      st.tris_out += tout;
    } else {
      ++st.refused;
    }
    it = st.entries.emplace(key, std::move(e)).first;
  } else if (it->second.usable) {
    ++st.hits;
  }
  const Entry &e = it->second;
  if (!e.usable)
    return false;
  out.view = plume::RenderIndexBufferView(e.buffer->at(0), e.count * (e.format == plume::RenderFormat::R32_UINT ? 4 : 2), e.format);
  out.count = e.count;
  return true;
}

void MeshLodLogMaybe() {
  auto &st = state_();
  const u32 frame = FrameStatFrameCount();
  std::lock_guard lock(st.mutex);
  if (frame / 300 == st.last_log_frame / 300)
    return;
  st.last_log_frame = frame;
  std::string reasons;
  for (u32 i = 0; i < 40; ++i)
    if (g_reason[i])
      reasons += fmt::format(" r{}={}", i, g_reason[i]);
  BD_INFO("[lod] lists built {} (refused {}{}), hits {}, triangles {} -> {} in "
          "the built lists ({:.0f}%)",
          st.built, st.refused, reasons, st.hits, st.tris_in, st.tris_out,
          st.tris_in ? 100.0 * double(st.tris_out) / double(st.tris_in) : 0.0);
}

} // namespace bd::gpu::scene
