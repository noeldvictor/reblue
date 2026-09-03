/**
 * @file    gpu/shadow_fit.cpp
 * @brief   The sun shadow frustum fitted to the view on the host.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/shadow_fit.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <rex/cvar.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/device.h"
#include "gpu/frame_stats.h"
#include "gpu/resources.h"
#include "gpu/scene/guest_scene.h"
#include "gpu/settings.h"

REXCVAR_DECLARE(bool, bd_shadow_fit);
REXCVAR_DECLARE(f64, bd_shadow_fit_distance);
REXCVAR_DECLARE(bool, bd_shadow_fit_diag);

namespace bd::gpu {
namespace {

// A 4x4 in the guest's register layout, row r at register base + r, applied
// as clip = M * v (column convention): the guest camera's c32-35 carries the
// unit view direction and the eye distance in its fourth row, which is the
// w row of M * v. The light matrix has the same layout.
struct Mat4 {
  float m[4][4];
};

Mat4 Mul(const Mat4 &a, const Mat4 &b) {
  Mat4 r{};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      float s = 0.0f;
      for (int k = 0; k < 4; ++k)
        s += a.m[i][k] * b.m[k][j];
      r.m[i][j] = s;
    }
  return r;
}

void Transform(const float v[4], const Mat4 &m, float out[4]) {
  for (int i = 0; i < 4; ++i)
    out[i] = m.m[i][0] * v[0] + m.m[i][1] * v[1] + m.m[i][2] * v[2] +
             m.m[i][3] * v[3];
}

// Gauss-Jordan; false when singular.
bool Invert(const Mat4 &in, Mat4 &out) {
  double a[4][8];
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j)
      a[i][j] = in.m[i][j];
    for (int j = 0; j < 4; ++j)
      a[i][4 + j] = (i == j) ? 1.0 : 0.0;
  }
  for (int c = 0; c < 4; ++c) {
    int p = c;
    for (int r = c + 1; r < 4; ++r)
      if (std::fabs(a[r][c]) > std::fabs(a[p][c]))
        p = r;
    if (std::fabs(a[p][c]) < 1e-12)
      return false;
    if (p != c)
      for (int j = 0; j < 8; ++j)
        std::swap(a[p][j], a[c][j]);
    const double d = a[c][c];
    for (int j = 0; j < 8; ++j)
      a[c][j] /= d;
    for (int r = 0; r < 4; ++r) {
      if (r == c)
        continue;
      const double f = a[r][c];
      if (f == 0.0)
        continue;
      for (int j = 0; j < 8; ++j)
        a[r][j] -= f * a[c][j];
    }
  }
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      out.m[i][j] = static_cast<float>(a[i][4 + j]);
  return true;
}

Mat4 ReadRegs(const float *regs, u32 reg) {
  Mat4 m;
  std::memcpy(m.m, regs + reg * 4, sizeof(m.m));
  return m;
}

void WriteRegs(float *regs, u32 reg, const Mat4 &m) {
  std::memcpy(regs + reg * 4, m.m, sizeof(m.m));
}

struct State {
  Mat4 camera_vp{};
  bool camera_valid = false;
  u32 camera_frame = 0;
  // The fit computed at the shadow pass, applied to the scene pass after.
  Mat4 clip_fix{};
  bool fix_valid = false;
  bool fix_tried = false;
  u32 fix_frame = 0;
  f64 zoom = 1.0;
  u32 diag_told = 0;
  u32 diag_frame = 0;
};

State &state_() {
  static State s;
  return s;
}

// The camera frustum's eight corners, near to the fit distance along the
// frustum edges, in world space.
bool FrustumCorners(const Mat4 &vp, float distance, float out[8][4]) {
  Mat4 inv;
  if (!Invert(vp, inv))
    return false;
  float near_pt[4][3], far_pt[4][3];
  int n = 0;
  for (int sy = -1; sy <= 1; sy += 2)
    for (int sx = -1; sx <= 1; sx += 2) {
      const float cn[4] = {float(sx), float(sy), 0.0f, 1.0f};
      const float cf[4] = {float(sx), float(sy), 1.0f, 1.0f};
      float wn[4], wf[4];
      Transform(cn, inv, wn);
      Transform(cf, inv, wf);
      if (std::fabs(wn[3]) < 1e-9f || std::fabs(wf[3]) < 1e-9f)
        return false;
      for (int k = 0; k < 3; ++k) {
        near_pt[n][k] = wn[k] / wn[3];
        far_pt[n][k] = wf[k] / wf[3];
      }
      ++n;
    }
  for (int i = 0; i < 4; ++i) {
    float dir[3];
    float len = 0.0f;
    for (int k = 0; k < 3; ++k) {
      dir[k] = far_pt[i][k] - near_pt[i][k];
      len += dir[k] * dir[k];
    }
    len = std::sqrt(len);
    const float t = len > 0.0f ? std::min(1.0f, distance / len) : 0.0f;
    for (int k = 0; k < 3; ++k) {
      out[i][k] = near_pt[i][k];
      out[4 + i][k] = near_pt[i][k] + dir[k] * t;
    }
    out[i][3] = out[4 + i][3] = 1.0f;
  }
  return true;
}

// The pass, from the bound targets: the shadow map is the depth-only target
// at the shadow dimension; the scene is a colour target with depth, at
// least 512 wide (the reflection is 240 wide, the stubs smaller).
enum class Pass { Other, Shadow, Scene };

Pass ClassifyPass(const VideoState &s) {
  const GuestTexture *rt = s.render_target;
  const GuestTexture *ds = s.depth_stencil;
  if (!rt && ds) {
    const u32 dim = static_cast<u32>(std::max(512, Settings::Get().ShadowDimension()));
    if (ds->width == dim && ds->height == dim)
      return Pass::Shadow;
  }
  if (rt && ds && rt->width >= 512)
    return Pass::Scene;
  return Pass::Other;
}

} // namespace

void ShadowFitOnVertexBlock(float *regs, const VideoState &s) {
  auto &st = state_();
  const Pass pass = ClassifyPass(s);
  if (pass == Pass::Other)
    return;
  const u32 frame = FrameStatFrameCount();
  const bool diag = REXCVAR_GET(bd_shadow_fit_diag) && st.diag_told < 40 &&
                    frame > 1800 && frame != st.diag_frame;

  if (pass == Pass::Scene) {
    // The camera: the guest's own camera view (render view 1) only. The 2D
    // and post passes draw into the same target under an orthographic
    // matrix, and taking theirs projected the occlusion proxies off-screen
    // and culled the scene (2026-09-03).
    const u32 view_now = bd::mem::try_load<u32>(scene::kRenderViewIdVa);
    {
      static u32 told = 0;
      if (told < 12 && frame > 800 && s.render_target->width < 1900) {
        ++told;
        BD_INFO("[shadow-fit] scene block: view {} frame {} rt {}x{} row3 "
                "({:.3f} {:.3f} {:.3f} {:.3f})",
                view_now, frame, s.render_target ? s.render_target->width : 0u,
                s.render_target ? s.render_target->height : 0u, regs[32 * 4 + 12],
                regs[32 * 4 + 13], regs[32 * 4 + 14], regs[32 * 4 + 15]);
      }
    }
    // The camera is the perspective block: the 2D and post passes draw into
    // the same target under an orthographic matrix (row 3 = 0 0 0 1).
    const bool perspective = regs[32 * 4 + 12] != 0.0f ||
                             regs[32 * 4 + 13] != 0.0f ||
                             regs[32 * 4 + 14] != 0.0f;
    (void)view_now;
    if (perspective) {
      const Mat4 vp = ReadRegs(regs, 32);
      st.camera_vp = vp;
      st.camera_valid = true;
      st.camera_frame = frame;
    }
    if (st.fix_valid && st.fix_frame == frame && REXCVAR_GET(bd_shadow_fit))
      WriteRegs(regs, 36, Mul(st.clip_fix, ReadRegs(regs, 36)));
    return;
  }

  // The shadow pass. The fit is computed once a frame, from the previous
  // frame's camera, and applied to every draw's light matrix.
  const Mat4 light = ReadRegs(regs, 32);
  if (st.fix_tried && st.fix_frame == frame) {
    if (st.fix_valid && REXCVAR_GET(bd_shadow_fit))
      WriteRegs(regs, 32, Mul(st.clip_fix, light));
    return;
  }
  st.fix_tried = true;
  st.fix_frame = frame;
  st.fix_valid = false;
  st.zoom = 1.0;
  if (!st.camera_valid)
    return;
  float corners[8][4];
  const float distance = float(REXCVAR_GET(bd_shadow_fit_distance));
  if (!FrustumCorners(st.camera_vp, distance, corners))
    return;
  float minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f;
  for (int i = 0; i < 8; ++i) {
    float c[4];
    Transform(corners[i], light, c);
    const float w = std::fabs(c[3]) > 1e-9f ? c[3] : 1.0f;
    const float x = c[0] / w, y = c[1] / w;
    minx = std::min(minx, x);
    maxx = std::max(maxx, x);
    miny = std::min(miny, y);
    maxy = std::max(maxy, y);
  }
  if (diag) {
    ++st.diag_told;
    st.diag_frame = frame;
    BD_INFO("[shadow-fit] frame {}: shadow {}x{}, camera from frame {}; "
            "frustum in light clip x [{:.3f} {:.3f}] y [{:.3f} {:.3f}]; light "
            "row0 ({:.3f} {:.3f} {:.3f} {:.3f}) row3 ({:.3f} {:.3f} {:.3f} "
            "{:.3f}); camera row3 ({:.3f} {:.3f} {:.3f} {:.3f})",
            frame, s.depth_stencil ? s.depth_stencil->width : 0u,
            s.depth_stencil ? s.depth_stencil->height : 0u, st.camera_frame,
            minx, maxx, miny, maxy, light.m[0][0], light.m[0][1],
            light.m[0][2], light.m[0][3], light.m[3][0], light.m[3][1],
            light.m[3][2], light.m[3][3], st.camera_vp.m[3][0],
            st.camera_vp.m[3][1], st.camera_vp.m[3][2], st.camera_vp.m[3][3]);
  }
  // Inside the guest's own box only: a box that does not contain the
  // frustum says the convention or the camera is wrong; no fit that frame.
  const bool sane = minx > -1.5f && maxx < 1.5f && miny > -1.5f &&
                    maxy < 1.5f && maxx > minx && maxy > miny;
  if (!sane || !REXCVAR_GET(bd_shadow_fit))
    return;
  const i32 dim = std::max(512, Settings::Get().ShadowDimension());
  const float texel = 2.0f / float(dim);
  // A square window around the frustum's box with a two-texel margin, never
  // wider than the guest's own box, its centre snapped to the map's texel
  // grid so a moving camera does not shimmer.
  float cx = 0.5f * (minx + maxx), cy = 0.5f * (miny + maxy);
  float half = 0.5f * std::max(maxx - minx, maxy - miny) + 2.0f * texel;
  half = std::min(half, 1.0f);
  const float step = texel * half;
  cx = std::round(cx / step) * step;
  cy = std::round(cy / step) * step;
  // clip' = C * clip: x' = (x - cx * w) / half, projective so the divide by
  // w still lands on the fitted window.
  Mat4 fix{};
  fix.m[0][0] = 1.0f / half;
  fix.m[1][1] = 1.0f / half;
  fix.m[2][2] = 1.0f;
  fix.m[3][3] = 1.0f;
  fix.m[0][3] = -cx / half;
  fix.m[1][3] = -cy / half;
  st.clip_fix = fix;
  st.fix_valid = true;
  st.zoom = half;
  WriteRegs(regs, 32, Mul(fix, light));
}

f64 ShadowFitZoom() { return state_().zoom; }

bool ShadowFitCamera(float out[16], u32 &frame) {
  auto &st = state_();
  if (!st.camera_valid)
    return false;
  std::memcpy(out, st.camera_vp.m, sizeof(st.camera_vp.m));
  frame = st.camera_frame;
  return true;
}

} // namespace bd::gpu
