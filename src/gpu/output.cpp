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

#include <algorithm>
#include <cmath>

#include <rex/cvar.h>
#include <rex/ui/flags.h>

#include "gpu/device.h"
#include "gpu/settings.h"

REXCVAR_DECLARE(i32, bd_max_render_height);

namespace bd::gpu {

bool Output::LatchedFit(u32 &w, u32 &h) {
  static u32 latched_w = 0;
  static u32 latched_h = 0;
  if (latched_w == 0) {
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
    latched_w = fit_w;
    latched_h = fit_h;
  }
  w = latched_w;
  h = latched_h;
  return true;
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
    return static_cast<double>(w) / static_cast<double>(h);
  return 16.0 / 9.0;
}

double Output::ProjectionAspect() {
  return StretchToFill() ? kDesignCanvasAspect : RenderAspect();
}

bool Output::DesignFitActive() {
  return std::fabs(ProjectionAspect() - kDesignCanvasAspect) >
         kDesignCanvasAspectEpsilon;
}

float Output::DesignScaleX() {
  const double ar = ProjectionAspect();
  if (!DesignFitActive() || ar <= kDesignCanvasAspect)
    return 1.0f;
  return static_cast<float>(kDesignCanvasAspect / ar);
}

float Output::DesignScaleY() {
  const double ar = ProjectionAspect();
  if (!DesignFitActive() || ar >= kDesignCanvasAspect)
    return 1.0f;
  return static_cast<float>(ar / kDesignCanvasAspect);
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
