/**
 * @file    native_sun_camera_bridge.cpp
 * @brief   Native sun camera producer and temporary engine matrix/getter outputs.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/scene/native_sun_camera_bridge.h"
#include "gpu/scene/native_transform_bridge.h"
#include "gpu/scene/native_view.h"
#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/frame_stats.h"
#include <mutex>
#include <numbers>
#include <rex/cvar.h>

REXCVAR_DECLARE(bool, bd_native_sun_camera);
REXCVAR_DECLARE(bool, bd_native_views);
REXCVAR_DECLARE(bool, bd_host_frustum);
REXCVAR_DECLARE(bool, bd_shadow_fit_diag);
REXCVAR_DECLARE(double, bd_shadow_fit_distance);
namespace bd::gpu::scene {
namespace {
constexpr uint32_t kPrimary = (uint32_t(-32035) << 16) + 24832;
constexpr uint32_t kSnapshot = (uint32_t(-32034) << 16) - 22052;
std::mutex camera_mutex;
std::optional<NativeSunCamera> current;
uint32_t current_frame = 0;
struct Stats {
  uint64_t produced = 0, inactive = 0, refused = 0, snapshots = 0;
  uint32_t frame = 0;
};
thread_local Stats stats;
bool Words(uint64_t address, uint64_t bytes) {
  if (!address || (address & 3) || address + bytes - 1 > UINT32_MAX ||
      !bd::mem::try_at<uint8_t>(uint32_t(address))) return false;
  for (auto page = (address & ~uint64_t(4095)) + 4096; page < address + bytes; page += 4096)
    if (!bd::mem::try_at<uint8_t>(uint32_t(page))) return false;
  return true;
}
void Report() {
  const auto frame = FrameStatFrameCount();
  if (frame - stats.frame < 300) return;
  BD_INFO("[native-sun-camera] fitted {} inactive {} refused {} native snapshots {}; "
          "current-view orthographic fit; authored light inputs and engine getter adapters remain",
          stats.produced, stats.inactive, stats.refused, stats.snapshots);
  stats.frame = frame;
}
void Snapshot(const RenderMatrix &view) {
  const auto inverse = InverseRenderMatrix(view);
  const auto direction = NormalizeViewDirection({-inverse[8], -inverse[9], -inverse[10]});
  const auto wrap = [](float angle) {
    constexpr float two_pi = 2*std::numbers::pi_v<float>;
    angle = std::fmod(angle, two_pi);
    return angle < 0 ? angle + two_pi : angle;
  };
  const std::array<float, 3> angles{
      wrap(-std::atan2(direction[1], std::hypot(direction[0], direction[2]))),
      wrap(std::atan2(direction[0], direction[2])), 0};
  for (uint32_t i = 0; i < 3; ++i) {
    bd::mem::store<float>(kSnapshot + i*4, inverse[12+i]);
    bd::mem::store<float>(kSnapshot + 12 + i*4, angles[i]);
  }
  ++stats.snapshots;
}
} // namespace
void InvalidateNativeSunCamera() {
  std::lock_guard lock(camera_mutex);
  current.reset();
}
std::optional<NativeSunCamera> GetNativeSunCamera() {
  std::lock_guard lock(camera_mutex);
  return current_frame == FrameStatFrameCount() ? current : std::nullopt;
}
bool ProduceNativeSunCamera(uint32_t source, uint32_t target, uint32_t dimension) {
  InvalidateNativeSunCamera();
  if (!REXCVAR_GET(bd_native_sun_camera) || !REXCVAR_GET(bd_native_views) ||
      !REXCVAR_GET(bd_host_frustum)) return false;
  const auto *transforms = GetNativeRenderTransforms();
  if (source != kPrimary || !transforms || !Words(source, 352) ||
      !Words(target, 12) || !Words(kSnapshot, 24)) {
    ++stats.refused;
    Report();
    return false;
  }
  if (!bd::mem::load<uint8_t>(source + 8)) {
    Snapshot(transforms->inputs.view);
    ++stats.inactive;
    Report();
    return true; // authored inactive shadow: no light matrix update
  }
  const double reach = REXCVAR_GET(bd_shadow_fit_distance);
  auto fit = BuildNativeSunCamera(transforms->inputs.view, transforms->inputs.projection,
      bd::mem::load<float>(source + 20), bd::mem::load<float>(source + 24), dimension,
      reach, reach);
  if (!fit) {
    ++stats.refused;
    Report();
    return false; // never publish a partial fit or an invented identity camera
  }
  if (REXCVAR_GET(bd_shadow_fit_diag) && FrameStatFrameCount() > 1800 &&
      FrameStatFrameCount() - stats.frame >= 300) {
    SunVector point;
    for (uint32_t i = 0; i < 3; ++i) point[i] = bd::mem::load<float>(target + i*4);
    const auto clip = TransformSunPoint(point, 1, fit->view_projection);
    BD_INFO("[native-sun-camera] pitch/yaw ({:.5f} {:.5f}) extent {:.3f} texel {:.5f} "
            "depth {:.3f}; target ({:.3f} {:.3f} {:.3f}) clip ({:.5f} {:.5f} {:.5f}); "
            "source eye ({:.3f} {:.3f} {:.3f})",
            bd::mem::load<float>(source + 20), bd::mem::load<float>(source + 24),
            fit->half_extent, fit->world_texel, fit->depth_range, point[0], point[1], point[2],
            clip[0], clip[1], clip[2], bd::mem::load<float>(source + 28),
            bd::mem::load<float>(source + 32), bd::mem::load<float>(source + 36));
  }
  // No guest projection builder, trigonometric helper, perspective warp or
  // object-ray query executes. The current native view owns all fit geometry.
  Snapshot(transforms->inputs.view);
  // The automatic engine fitter leaves the authored light-position getter
  // untouched. Remaining caster consumers still use that ABI; the actual
  // orthographic eye belongs only to the native camera and its view matrix.
  for (uint32_t i = 0; i < 3; ++i) {
    bd::mem::store<float>(source + 40 + i*4, float(fit->focus[i]));
  }
  for (uint32_t i = 0; i < 16; ++i) {
    bd::mem::store<float>(source + 68 + i*4, fit->view[i]);
    bd::mem::store<float>(source + 132 + i*4, fit->projection[i]);
  }
  SunVector point;
  for (uint32_t i = 0; i < 3; ++i) point[i] = bd::mem::load<float>(target + i*4);
  const auto projected = TransformSunPoint(point, 1, fit->view_projection);
  for (uint32_t i = 0; i < 4; ++i)
    bd::mem::store<float>(source + 336 + i*4, float(projected[i]));
  {
    std::lock_guard lock(camera_mutex);
    current = std::move(fit);
    current_frame = FrameStatFrameCount();
  }
  ++stats.produced;
  Report();
  return true;
}
} // namespace bd::gpu::scene
