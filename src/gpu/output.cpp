/**
 * @file    gpu/output.cpp
 * @brief   Output geometry: the latched render size, the aspect the frame is
 *          built for, and the fit that centers one inside the other.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/output.h"
#include "gpu/native_output_geometry.h"

#include <algorithm>
#include <cmath>

#include <rex/cvar.h>
#include <rex/ui/flags.h>

#include "gpu/device.h"
#include "xr/xr_session.h"
#include "xr/xr_settings.h"
#include "gpu/settings.h"

REXCVAR_DECLARE(i32, bd_max_render_height);
REXCVAR_DECLARE(bool, bd_stereo_multiview);
REXCVAR_DECLARE(bool, bd_mv_half_width);
REXCVAR_DECLARE(bool, bd_mv_no_squeeze);
REXCVAR_DECLARE(bool, bd_xr_eye_sized);
REXCVAR_DECLARE(double, bd_xr_render_scale);

namespace bd::gpu {

namespace {
// The latched rect at the composed aspect, before the multiview halving
// below: the aspect the frame is projected and fitted for.
u32 g_latched_full_w = 0;
bool g_eye_latched = false;
} // namespace

bool Output::LatchedFit(u32 &w, u32 &h) {
  static u32 latched_w = 0;
  static u32 latched_h = 0;
  // The headset decides the frame's shape, not the desktop window it happens
  // to have been launched beside. Until 2026-09-04 this fitted the window and
  // then halved the width for multiview, which is how a 960x1080 target came
  // to hold 1.778-shaped content and lose half of itself at present.
  //
  // Re-latched once, when the runtime first reports its per-eye rect: the
  // window path has to serve until then, because the device, the window and
  // the guest's first surfaces all exist before the XR session does. Refusing
  // to answer until the session appeared left the app with no render target at
  // all and it died before writing a log.
  // Multiview only. Under bd_stereo the two eyes pack into one panel and the
  // half-width squeeze is what makes that work, so the eye-sized frame is not
  // meaningful there and that path is deliberately left as it was - it has not
  // been verified against this and does not need it.
  if (REXCVAR_GET(bd_xr_eye_sized) && REXCVAR_GET(bd_stereo_multiview) &&
      !g_eye_latched) {
    auto &session = bd::xr::Session::Get();
    const u32 eye_w = session.RecommendedWidth();
    const u32 eye_h = session.RecommendedHeight();
    if (eye_w && eye_h) {
      const double scale = REXCVAR_GET(bd_xr_render_scale);
      // The 3D frame fills the native eye. Fitting the entire scene to the
      // authored HUD canvas letterboxed the runtime frustum. Only
      // 2D vertices use DesignScaleX/Y; their canvas does not size the scene.
      if (const auto extent = ScaleEyeExtent({eye_w, eye_h}, scale)) {
        latched_w = extent->width;
        latched_h = extent->height;
        g_latched_full_w = latched_w;
        g_eye_latched = true;
        BD_INFO("[output] eye-sized frame: runtime {}x{} an eye x{:.2f} -> "
                "native scene {}x{} at aspect {:.3f}; separate 2D canvas",
                eye_w, eye_h, scale, latched_w, latched_h,
                double(latched_w) / double(latched_h));
      }
    }
  }
  if (latched_w == 0) {
    // The headset decides the frame's shape, not the desktop window it happens
    // to have been launched beside. Until 2026-09-04 this fitted the window and
    // then halved the width for multiview, which is how a 960x1080 target came
    // to hold 1.778-shaped content and lose half of itself at present.
    //
    // Latched like everything else here, so it waits for a session that has
    // reported its per-eye size - the same trap the XR swapchain hit, where the
    // first present happens before the runtime has said anything and the answer
    // is fixed from then on.
    // (the eye-sized latch happens above, before this window fit)
    // The requested render size in every display mode, not just windowed.
    // 0, or a partial pair, means follow the swapchain.
    const i32 cfg_w = REXCVAR_GET(window_width);
    const i32 cfg_h = REXCVAR_GET(window_height);
    u32 sw = 0;
    u32 sh = 0;
    if (cfg_w > 0 && cfg_h > 0) {
      sw = std::clamp<u32>(static_cast<u32>(cfg_w), 320u, 16384u);
      sh = std::clamp<u32>(static_cast<u32>(cfg_h), 240u, 16384u);
    } else {
      sw = Video::OutputWidth();
      sh = Video::OutputHeight();
    }
    if (!sw || !sh)
      return false;
    i32 off_x = 0, off_y = 0;
    u32 fit_w = 0, fit_h = 0;
    ComputeFit(sw, sh, ConfiguredAspect(), fit_w, fit_h, off_x, off_y);
    if (!fit_w || !fit_h)
      return false;
    // Applied after the aspect fit so the cap is on what is actually drawn,
    // and by height so an ultrawide window narrows rather than growing.
    const i32 cap = REXCVAR_GET(bd_max_render_height);
    if (cap > 0 && fit_h > static_cast<u32>(cap)) {
      const double scale = static_cast<double>(cap) / fit_h;
      // Round the width rather than truncating: a 1279-wide target for a 16:9
      // cap changes the aspect by enough to shift the letterbox a pixel.
      fit_w = std::max<u32>(1u, static_cast<u32>(std::lround(fit_w * scale)));
      fit_h = static_cast<u32>(cap);
    }
    g_latched_full_w = fit_w;
    // Multiview at side-by-side's pixels per eye: every guest texture - the
    // back buffer, the scene, the resolves and the post chain - is half the
    // composed width, and each array layer holds one eye's half. Halving only
    // the scene surface (2026-09-02) left the guest resolving each 688-wide
    // layer up into 1376-wide layered targets and running its whole chain at
    // twice side-by-side's pixels: 39.8 ms of Quest GPU against 20.8, with
    // six eager resolves a frame against two (2026-09-03, 09:26). The frame
    // is still projected and fitted at the full aspect (RenderAspect), the
    // same anamorphic squeeze side-by-side's half-width viewports carry.
    if (REXCVAR_GET(bd_stereo_multiview) && REXCVAR_GET(bd_mv_half_width)) {
      fit_w = std::max<u32>(8u, (fit_w / 2u) & ~7u);
      // The squeeze above is side-by-side's: two half-width eyes pack into one
      // full-width panel and the compositor un-squeezes each half onto a whole
      // eye. On the layered path each array layer *is* an eye, so nothing
      // un-squeezes it but the present's own aspect fit - which does it by
      // throwing away the vertical factor of two, measured at 2.01 source
      // pixels a destination pixel (2026-09-04). Halving the height too keeps
      // the target's aspect equal to RenderAspect, so the present maps 1:1 and
      // the discarded half is never shaded.
      if (REXCVAR_GET(bd_mv_no_squeeze)) {
        fit_h = std::max<u32>(8u, (fit_h / 2u) & ~7u);
        g_latched_full_w = fit_w; // no squeeze: RenderAspect is the target's
      }
    }
    latched_w = fit_w;
    latched_h = fit_h;
  }
  w = latched_w;
  h = latched_h;
  return true;
}

bool Output::EyeSized() {
  u32 w = 0, h = 0;
  return LatchedFit(w, h) && g_eye_latched;
}

bool Output::ProjectionEye() {
  return EyeSized() && bd::xr::Settings::Get().Mode() != bd::xr::CameraMode::Cinema;
}

u32 Output::LatchedFullWidth() {
  u32 w = 0, h = 0;
  if (!LatchedFit(w, h))
    return 0;
  return g_latched_full_w ? g_latched_full_w : w;
}

double Output::ConfiguredAspect() {
  switch (static_cast<AspectMode>(Settings::Get().AspectRatio())) {
  case AspectMode::Standard:
    return 4.0 / 3.0;
  case AspectMode::Wide:
    return 16.0 / 10.0;
  case AspectMode::Ultrawide:
    return 64.0 / 27.0;
  case AspectMode::SuperUltrawide:
    return 32.0 / 9.0;
  case AspectMode::Auto:
  case AspectMode::Stretch:
    return 0.0;
  case AspectMode::Original:
    break;
  }
  return 16.0 / 9.0;
}

bool Output::StretchToFill() {
  return static_cast<AspectMode>(Settings::Get().AspectRatio()) ==
         AspectMode::Stretch;
}

double Output::RenderAspect() {
  u32 w = 0, h = 0;
  if (LatchedFit(w, h) && h)
    return static_cast<double>(LatchedFullWidth()) / static_cast<double>(h);
  return 16.0 / 9.0;
}

double Output::ProjectionAspect() {
  // Cinema still composes a flat authored picture, fitted as one image at
  // present. Projection-mode VR ignores desktop stretch/aspect preferences.
  if (EyeSized())
    return ProjectionEye() ? RenderAspect() : kDesignCanvasAspect;
  return StretchToFill() ? kDesignCanvasAspect : RenderAspect();
}

bool Output::DesignFitActive() {
  return std::fabs(ProjectionAspect() - kDesignCanvasAspect) >
         kDesignCanvasAspectEpsilon;
}

float Output::DesignScaleX() {
  return DesignCanvasScale(ProjectionAspect(), kDesignCanvasAspect,
                           kDesignCanvasAspectEpsilon)[0];
}

float Output::DesignScaleY() {
  return DesignCanvasScale(ProjectionAspect(), kDesignCanvasAspect,
                           kDesignCanvasAspectEpsilon)[1];
}

float Output::DesignOverscanX() {
  const double ar = ProjectionAspect();
  if (!DesignFitActive() || ar <= kDesignCanvasAspect)
    return 0.0f;
  return static_cast<float>(kDesignCanvasWidth * 0.5 *
                            (ar / kDesignCanvasAspect - 1.0));
}

float Output::DesignOverscanY() {
  const double ar = ProjectionAspect();
  if (!DesignFitActive() || ar >= kDesignCanvasAspect)
    return 0.0f;
  return static_cast<float>(kDesignCanvasHeight * 0.5 *
                            (kDesignCanvasAspect / ar - 1.0));
}

void Output::ComputeFit(u32 swapW, u32 swapH, double aspect, u32 &fitW,
                        u32 &fitH, i32 &offX, i32 &offY) {
  u32 w = swapW;
  u32 h = swapH;
  if (swapW && swapH && aspect > 0.0) {
    const double ar = aspect;
    if (static_cast<double>(swapW) >= static_cast<double>(swapH) * ar) {
      // Wider than the target ratio, so limit by height and pillarbox.
      h = swapH;
      w = static_cast<u32>(static_cast<double>(swapH) * ar + 0.5);
    } else {
      // Narrower, so limit by width and letterbox.
      w = swapW;
      h = static_cast<u32>(static_cast<double>(swapW) / ar + 0.5);
    }
  }
  w = (w & ~7u);
  h = (h & ~7u);
  if (w > swapW)
    w = swapW & ~7u;
  if (h > swapH)
    h = swapH & ~7u;
  fitW = w;
  fitH = h;
  offX = static_cast<i32>((swapW - w) / 2);
  offY = static_cast<i32>((swapH - h) / 2);
}

} // namespace bd::gpu
