/**
 * @file    gpu/settings.cpp
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/settings.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <numbers>
#include <string>
#include <string_view>

#include <rex/cvar.h>

#include "core/logging.h"
#include "core/settings.h" // kCvarGroup

REXCVAR_DECLARE(bool, bd_pso_precache);
REXCVAR_DECLARE(bool, bd_geometry_gpu_upload);
REXCVAR_DECLARE(bool, bd_dred);
REXCVAR_DECLARE(i32, bd_anisotropy);
REXCVAR_DECLARE(i32, bd_supersampling);
REXCVAR_DECLARE(i32, bd_msaa);
REXCVAR_DECLARE(bool, bd_ntsc_filter);
REXCVAR_DECLARE(double, bd_dof_strength);
REXCVAR_DECLARE(double, bd_reflection_upscale);
REXCVAR_DECLARE(i32, bd_shadow_dimension);
REXCVAR_DECLARE(double, bd_shadow_distance);
REXCVAR_DECLARE(i32, bd_aspect_ratio);
REXCVAR_DECLARE(i32, bd_fov_offset);
REXCVAR_DECLARE(bool, bd_vsync);
REXCVAR_DECLARE(i32, bd_diag_verbosity);
REXCVAR_DECLARE(i32, bd_surface_pool_budget_pct);

REXCVAR_DEFINE_BOOL(bd_pso_precache, true, kCvarGroup,
                    "Precompile pipelines during loads instead of at first "
                    "draw.");

REXCVAR_DEFINE_BOOL(bd_geometry_gpu_upload, true, kCvarGroup,
                    "Place static geometry in the GPU_UPLOAD heap when the "
                    "device supports it. Off uses UPLOAD instead, costing the "
                    "write-combine win on AMD. Requires restart.");

REXCVAR_DEFINE_BOOL(bd_dred, true, kCvarGroup,
                    "Record D3D12 auto-breadcrumbs and page-fault allocations "
                    "so a lost device names the op and resource it died on. "
                    "Costs a little GPU time per op. Requires restart.");

REXCVAR_DEFINE_INT32(bd_anisotropy, 16, kCvarGroup,
                     "Anisotropic texture filtering level.")
    .range(0, 16);

REXCVAR_DEFINE_INT32(bd_supersampling, 1, kCvarGroup,
                     "Scene supersampling (SSAA) factor. Only 1/2/4. Above 1 "
                     "this takes the AA path and bd_msaa is ignored. Requires "
                     "restart.")
    .range(1, 4)
    .validator([](std::string_view v) {
      int n = 0;
      auto r = std::from_chars(v.data(), v.data() + v.size(), n);
      return r.ec == std::errc() && (n == 1 || n == 2 || n == 4);
    })
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// Measured on a Quest 2 (Adreno 650), title screen, everything else stock:
//
//   3664x1920  6.9 fps, 119ms on the GPU fence
//   1280x720  26.2 fps,   1.5ms
//
// The headset panel is 3664x1920 across both eyes, and the renderer sizes the
// scene to it, so a Quest was drawing a 720p game at seven megapixels and
// spending 119ms a frame doing it. Blue Dragon is natively 1280x720/30fps, so
// capping at 720 is the game's own resolution rather than a compromise - and
// the image is resampled onto a quad the compositor draws at arm's length
// anyway.
//
// 0 disables the cap. Desktops keep it off: there the whole point is running
// the game at a resolution it never saw.
REXCVAR_DEFINE_INT32(bd_max_render_height,
#if defined(__ANDROID__)
                     720,
#else
                     0,
#endif
                     kCvarGroup,
                     "Cap the scene render height in pixels, preserving aspect. "
                     "0 disables. Defaults to 720 on Android, where drawing at "
                     "the full headset panel resolution costs about 20x what "
                     "the game needs.")
    .range(0, 16384)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// Diagnostic. A field scene submits ~2925 draws and spends ~110ms on the GPU
// fence, and that cost is unchanged by halving the render resolution - so it is
// not fill-bound. The suspicion is the tiler's binning pass, which scales with
// draw calls and vertex count rather than pixels.
//
// Capping the draws answers it directly: if the fence falls in proportion the
// frame is draw-bound and culling is the lever. The frame renders incorrectly
// while this is set; it is a measurement, not a quality setting.
// Translated shaders read every guest constant register with a raw load from a
// device address, so a skinned vertex shader does 20-40 loads out of the
// constant buffer per vertex. An UPLOAD heap is host-visible write-combine and
// the GPU reads it uncached; GPU_UPLOAD is DEVICE_LOCAL | HOST_VISIBLE, still
// mappable but cached for the GPU. Same physical memory on a UMA part.
// Measured on a Quest 2 and it makes no difference: 2834 draws at 205.5ms with
// it off, 2851 draws at 208.2ms with it on. Kept, off, because the reasoning is
// sound on paper and may hold on another Adreno - but it is not the fix.
REXCVAR_DEFINE_BOOL(bd_constants_gpu_upload, false, kCvarGroup,
                    "Place shader constants in the GPU_UPLOAD heap when the "
                    "device has one. Measured as no change on a Quest 2. "
                    "Requires restart.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(bd_debug_max_pso, 0, kCvarGroup,
                     "Stop switching pipelines after N per frame; later draws "
                     "reuse the last one. 0 disables. Diagnostic only - the "
                     "scene renders with wrong materials.")
    .range(0, 100000);

REXCVAR_DEFINE_INT32(bd_debug_max_draws, 0, kCvarGroup,
                     "Stop submitting after N draws per frame. 0 disables. "
                     "Diagnostic only - the frame renders wrong.")
    .range(0, 100000);

REXCVAR_DEFINE_INT32(bd_debug_fill_scale, 100, kCvarGroup,
                     "Shrink the scissor to N percent of the viewport in each "
                     "axis, clipping fragments while leaving geometry, draw "
                     "count and every pipeline state identical. Diagnostic "
                     "only - the frame renders into a corner. Isolates "
                     "fragment cost from everything else.")
    .range(10, 100);

// Applied where bd_supersampling already scales the scene surfaces, so the
// guest asks for the smaller size itself and its viewports, resolve rects and
// post-process chain all follow. A Quest 2 field frame is fill-bound: clipping
// the scissor to 25% takes the GPU fence from 141ms to 0.1ms while the draw
// count rises, so fragments are the whole GPU cost.
REXCVAR_DEFINE_INT32(bd_render_scale, 100, kCvarGroup,
                     "Render the 3D scene at N percent of the design canvas in "
                     "each axis, 100 = native 1280x720. 50 quarters the "
                     "fragment cost. Distinct from bd_max_render_height, which "
                     "sizes the output fit and leaves the scene alone. "
                     "Requires restart.")
    .range(25, 100)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// True off switches, as opposed to the quality knobs beside them. Both work by
// forcing the pass to its smallest legal surface rather than by suppressing the
// draws: the frame is fill-bound, so the fragments are the cost and the draws
// are free. Keeping the pass alive keeps every guest-side invariant that hangs
// off its texture intact.
// Groundwork for stereo, not stereo. Renders the guest's scene a second time
// from the same camera, which is visually wrong on purpose: it answers whether a
// second full scene render per frame is possible at all, and what it costs,
// before any per-eye matrices or targets are introduced.
// EFFECTS, not scene geometry - every caller of the guest function this scales
// is inside bdEffectUpdate. Named bdVisualObjectGetMaxDrawDistance, which reads
// like general object culling and is not.
//
// Kept because particles are alpha-blended overdraw and the frame is fill-bound,
// so an effect-heavy scene is exactly where this should bite. UNVERIFIED: a
// desktop field scene at 1.0/0.5/0.25 gave 847/836/843 draws, i.e. nothing,
// because that scene has almost no effects. Needs a battle to test.
REXCVAR_DEFINE_DOUBLE(bd_effect_distance, 1.0, kCvarGroup,
                      "Effect/particle draw-distance multiplier, 1.0 = stock. "
                      "Below 1.0 culls distant effects earlier. Untested - it "
                      "moved nothing in a field scene; try it in a battle.")
    .range(0.1, 2.0);

// Renderer-side stereo: every recorded draw submitted once per eye, into a
// half-width viewport each. One guest frame, one render list, two views - the
// opposite of bd_stereo_test, which tried to make the guest produce the second
// view and could not.
// Fork goal 3. A present-time post-process rather than a material change: Blue
// Dragon already ships toon shaders, so the characters are lit with a ramp
// already, and what the art wants on top is ink lines and flatter colour. Doing
// it at present covers every material without touching XenosRecomp, without a
// shader cache rebuild, and without having to know which draws are characters.
// Scales the bounding radius the guest tests against its own frustum in
// bdSceneNodeCullTraverse. Below 1.0 marginal nodes fail the test and the guest
// skips their draw itself - no control flow redirected, no return address
// needed. The CPU floor is real computation (43ms of GPU freed on a Quest moved
// `elsewhere` by 1.2ms), and the census puts node submission at the top of it.
// View-space distance beyond which a scene node is not drawn. The centre the
// guest tests is in view space with the camera at the origin, so this is a plain
// length; measured on device, mid-distance props sit around 700-820 and their
// own radii run 4-11. An object is kept if its radius reaches inside the limit,
// so a distant cliff does not vanish while the pebble beside it stays.
//
// 0 disables. This is the lever for the CPU floor: ~43ms of a Quest frame is
// guest code and bdSceneNodeDrawSingle is 23x everything else in it, walked
// once per node for about a thousand individually placed objects.
REXCVAR_DEFINE_DOUBLE(bd_cull_distance, 0.0, kCvarGroup,
                      "View-space distance beyond which scene nodes are not "
                      "drawn. 0 disables. Cuts CPU, and things pop in.")
    .range(0.0, 100000.0);

REXCVAR_DEFINE_DOUBLE(bd_cull_bias, 1.0, kCvarGroup,
                      "Scales the bounding radius used by the scene-graph cull. "
                      "Below 1.0 culls more aggressively, cutting draws and the "
                      "CPU cost of submitting them; things pop in at the edges.")
    .range(0.2, 1.0);

// One-shot frame capture. Writes the finished composited frame - the same
// image the headset is handed - to logs/capture/ as raw RGBA plus a one-line
// header, and clears itself.
//
// This exists because "verify the pixels, not a proxy" is a rule here and
// there was no way to keep it in VR: the Quest system screenshot intents do
// not fire on this Horizon build, adb screencap does not see compositor
// layers, and every other check available was a log line. A VR claim that
// rests on "swapchain format 37" has been wrong before.
//
// Raw rather than PNG because the tree vendors stb_image but not
// stb_image_write, and a capture is pulled to a host that can convert it in
// three lines. Not worth a new dependency.
// Seconds rather than a bool, because args.txt is read once at launch and a
// bool would therefore only ever capture the title screen. Autoplay reaches a
// field scene at a known time, so "capture at t=200s" is the whole interface.
REXCVAR_DEFINE_DOUBLE(bd_capture_after_s, 0.0, kCvarGroup,
                      "Write the composited frame to logs/capture/ once, this "
                      "many seconds after start. 0 disables. Stalls one "
                      "frame.")
    .range(0.0, 100000.0);

// Mirror the VR image into the desktop window.
//
// Off on Android and it must stay that way: presenting the Android surface in
// VR costs 124ms a frame, which is the single largest win this port ever made
// (see EnsureOffscreen). Nobody looks at that surface on a headset anyway.
//
// On the desktop it is the difference between a simulator run you can watch and
// a black window - the game renders to an offscreen target in VR and never
// presents, so without this the window shows nothing and looks broken.
#if defined(__ANDROID__)
REXCVAR_DEFINE_BOOL(bd_xr_mirror, false, kCvarGroup,
#else
REXCVAR_DEFINE_BOOL(bd_xr_mirror, true, kCvarGroup,
#endif
                    "Mirror the VR image into the flat window. Costs a full "
                    "present per frame, so it is off on Android where nothing "
                    "displays that surface.");

// Splits the multiview resolve in two so a broken frame can be attributed:
// whether the two layers are flattened at all, and whether readers are pointed
// at the flattened image. Both default on; turning either off in isolation is
// how the black-frame regressions get localised.
REXCVAR_DEFINE_BOOL(bd_mv_debug_known_srv, false, kCvarGroup,
                    "Multiview diagnostic: resolve from the surface's own "
                    "descriptor rather than the per-eye array views, to tell a "
                    "bad slot registration from a bad pass.");

REXCVAR_DEFINE_BOOL(bd_mv_debug_clear, false, kCvarGroup,
                    "Multiview diagnostic: fill the resolve target with magenta "
                    "instead of resolving, to tell a dead render pass from a "
                    "dead draw.");

REXCVAR_DEFINE_BOOL(bd_mv_resolve, true, kCvarGroup,
                    "Run the multiview resolve pass that flattens the two "
                    "layers into one side-by-side image.");

// Capture the layered array itself, both slices, rather than the resolved
// companion. Diagnostic: it answers whether the array has content at all.
// Seeding a freshly acquired colour target from its predecessor reproduces the
// persistence of an EDRAM tile that does not exist. 14 full-surface copies a
// frame, and the bulk of the resolve category's 19% of GPU time. Off renders
// incorrectly wherever a pass relied on inherited content - it is a
// measurement handle, meant to be paired with bd_ab_flag.
REXCVAR_DEFINE_BOOL(bd_seed_targets, true, kCvarGroup,
                    "Seed freshly acquired colour targets from their "
                    "predecessor (EDRAM persistence emulation).");
// Clears the layered scene target magenta inside its own render pass. With
// bd_mv_capture_array it answers whether anything at all reaches a viewMask=3
// attachment. Destroys the scene; a diagnostic only.
REXCVAR_DEFINE_BOOL(bd_mv_test_clear, false, kCvarGroup,
                    "Magenta-clear the layered scene target (diagnostic).");
// Capture the scene's resolved companion - what the resolve writes and the
// post chain reads - rather than whatever present would have chosen.
REXCVAR_DEFINE_BOOL(bd_mv_capture_resolved, false, kCvarGroup,
                    "Capture the scene surface's resolved companion.");
REXCVAR_DEFINE_BOOL(bd_mv_capture_array, false, kCvarGroup,
                    "Capture the multiview array instead of its companion.");
REXCVAR_DEFINE_BOOL(bd_mv_redirect_srv, true, kCvarGroup,
                    "Point a multiview surface's sampled view at the resolved "
                    "companion rather than at array layer 0.");

REXCVAR_DEFINE_BOOL(bd_cel_shading, false, kCvarGroup,
                    "Cel shading: posterised colour and ink outlines, applied "
                    "over the finished frame. Costs one full-screen pass.");

// Multiview stereo: one draw renders both eyes into a two-layer target, with
// each recompiled vertex shader reading SV_ViewID to pick its eye. The point is
// the CPU - bd_stereo submits every scene draw twice and costs ~8ms a frame of
// doubled recording on a Quest, and the frame is entirely CPU-bound.
//
// Requires a restart: the scene surfaces have to be created with two layers.
// Which layer of a multiview scene target the flat present samples. The whole
// point is to be able to look at layer 1: multiview rendering "works" is
// otherwise only ever "it did not crash and the draw count did not double",
// and this shows the second eye actually received a different view.
REXCVAR_DEFINE_INT32(bd_stereo_debug_layer, 0, kCvarGroup,
                     "Which eye layer the flat present samples, 0 or 1. "
                     "Diagnostic for multiview. Requires restart.")
    .range(0, 1)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(bd_stereo_multiview, false, kCvarGroup,
                    "Stereo by Vulkan multiview - one draw, two layers, "
                    "SV_ViewID per eye. Cheaper than bd_stereo, which submits "
                    "every draw twice. Requires restart.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(bd_stereo, false, kCvarGroup,
                    "Submit every draw twice, into left and right half-width "
                    "viewports. Step one of stereo: no per-eye matrices yet, "
                    "so both halves show the same view.");

// Parallax, not a shift. The per-eye upload adds separation * clip.z to
// clip.x, so a vertex moves horizontally in proportion to its depth - which is
// the depth cue. Adding separation * clip.w instead would slide the whole image
// sideways and look like nothing at all.
REXCVAR_DEFINE_DOUBLE(bd_stereo_separation, 0.03, kCvarGroup,
                      "Eye separation for bd_stereo, as a clip-space skew. 0 "
                      "gives two identical views; larger diverges them and "
                      "deepens the effect. Comfort varies, so this is a knob.")
    .range(0.0, 0.2);

// The other half of an off-axis frustum. Separation alone puts the whole world
// behind the screen, which fuses badly; this moves each eye's projection centre
// so parallax is zero at a chosen distance and nearer things come forward of it.
REXCVAR_DEFINE_DOUBLE(bd_stereo_convergence, 0.0, kCvarGroup,
                      "Convergence for bd_stereo: shifts each eye's projection "
                      "centre, setting the distance at which parallax is zero. "
                      "0 puts the convergence plane at infinity.")
    .range(-0.2, 0.2);

REXCVAR_DEFINE_BOOL(bd_stereo_test, false, kCvarGroup,
                    "Render the 3D scene twice per frame from the same camera. "
                    "Diagnostic only - the image is unchanged and the cost "
                    "doubles. Measures whether stereo is reachable.");

REXCVAR_DEFINE_BOOL(bd_shadows, true, kCvarGroup,
                    "Sun shadows. Off renders the shadow map at 64x64, which "
                    "costs nothing. Requires restart.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(bd_reflections, true, kCvarGroup,
                    "Planar water reflections. The reflection re-renders the "
                    "scene, so off is a large saving; it pins the reflection "
                    "to its 128-wide floor.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(bd_msaa, 4, kCvarGroup,
                     "MSAA sample count for the 3D scene: 0 = off, 2, 4, 8. "
                     "Clamped to device support, ignored while "
                     "bd_supersampling > 1. Requires restart.")
    .range(0, 8)
    .validator([](std::string_view v) {
      int n = 0;
      auto r = std::from_chars(v.data(), v.data() + v.size(), n);
      return r.ec == std::errc() && (n == 0 || n == 2 || n == 4 || n == 8);
    })
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(bd_ntsc_filter, false, kCvarGroup,
                    "Restore BD's analog-TV scanline filter. Every shipped "
                    "db_posteffect record disables it, so it only shows up in "
                    "the Battle Viewer, where it strobes the whole screen.");

// Not a preset member: how much depth-of-field a player wants is taste, and it
// costs the same at every setting.
REXCVAR_DEFINE_DOUBLE(bd_dof_strength, 1.0, kCvarGroup,
                      "Depth-of-field intensity, 1.0 = the game's own blur, "
                      "0 = off.")
    .range(0.0, 1.0)
    .validator([](std::string_view v) {
      f64 d = 0;
      return rex::cvar::ParseDouble(v, d) && std::isfinite(d);
    });

// A ceiling rather than a factor, so the size the game asks for still carries.
// Water sits in a fraction of the frame and its reflection re-renders the
// scene, so the fill this buys back is worth more than the sharpness it costs.
REXCVAR_DEFINE_DOUBLE(bd_reflection_upscale, 2.0, kCvarGroup,
                      "Ceiling on how far the planar water reflection is "
                      "scaled above BD's own 320-wide base. 1.0 = the size "
                      "the game asks for, higher trades fill rate for a "
                      "sharper reflection.")
    .range(1.0, 8.0)
    .validator([](std::string_view v) {
      f64 d = 0;
      return rex::cvar::ParseDouble(v, d) && std::isfinite(d);
    });

// 4096 costs roughly 5fps of the 31 a Quest 2 has to give, for a shadow map
// resampled onto a quad. 1024 is the better trade there; desktops keep 4096.
REXCVAR_DEFINE_INT32(bd_shadow_dimension,
#if defined(__ANDROID__)
                     1024,
#else
                     4096,
#endif
                     kCvarGroup,
                     "Sun shadow-map resolution in pixels. Only "
                     "512/1024/2048/4096/8192, requires restart.")
    .range(512, 8192)
    .validator([](std::string_view v) {
      int n = 0;
      auto r = std::from_chars(v.data(), v.data() + v.size(), n);
      return r.ec == std::errc() &&
             (n == 512 || n == 1024 || n == 2048 || n == 4096 || n == 8192);
    })
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// A range alone does not reject NaN: neither NaN < min nor NaN > max is ever
// true, so it passes validation and reaches shadowPcfScale, where clamp and
// max propagate it into the uploaded constant.
REXCVAR_DEFINE_DOUBLE(bd_shadow_distance, 2.0, kCvarGroup,
                      "Sun shadow draw-distance multiplier (1.0 = X360 "
                      "native).")
    .range(1.0, 4.0)
    .validator([](std::string_view v) {
      f64 d = 0;
      return rex::cvar::ParseDouble(v, d) && std::isfinite(d);
    });

REXCVAR_DEFINE_INT32(bd_aspect_ratio,
                     static_cast<i32>(bd::gpu::AspectMode::Auto), kCvarGroup,
                     "Output aspect ratio: 0 = 16:9, 1 = 4:3, 2 = 16:10, "
                     "3 = 21:9, 4 = 32:9, 5 = match the display, 6 = fill the "
                     "display and stretch.")
    .range(0, static_cast<i32>(bd::gpu::AspectMode::Stretch));

REXCVAR_DEFINE_INT32(bd_fov_offset, 0, kCvarGroup,
                     "Horizontal degrees added to the game's own field of view "
                     "at 16:9, which the menu counts off 45. 0 keeps how the "
                     "game frames itself. Battle and event scenes hold the "
                     "game's own value, since their effects are drawn to span "
                     "it.")
    .range(0, 75);

REXCVAR_DEFINE_BOOL(bd_vsync, true, kCvarGroup, "Vertical sync.");
// The runtime pins our render thread to the big cluster and leaves the guest
// on all eight cores, so guest workers crowd out the renderer while the prime
// core idles. Off restores whatever the scheduler chooses.
REXCVAR_DEFINE_BOOL(bd_thread_policy, true, kCvarGroup,
                    "Place guest threads across the big.LITTLE clusters.");
// Horizon OS refuses shell perf on a Quest 2, so simpleperf cannot attach and
// tools/profile_quest.py has never produced a profile. This samples ourselves.
// Restores the old precision-spin tail on guest Sleep. Measured at 15.9% of
// all CPU samples, which is why it is off.
// Four clock reads and three atomics per draw. Measured at 3.4% of all CPU
// samples at ~1200 draws a frame, which is why it is off by default; the
// mutex/bindFB/flushState split in the [perf] line needs it.
// Culled nodes jump straight to the guest's own "not visible" continuation,
// skipping sub_82287788 - the per-node visibility test, 7.1% of all samples and
// the hottest function in the process. The distance cull rejects ~95% of nodes,
// so nearly all of that work was computed and discarded. Off reverts to
// applying the same decision after the test instead of instead of it.
// Checks a host maths replacement against the recompiled original while both
// exist. A diagnostic - it runs both - but it is what makes a host takeover a
// fact rather than a reading of the disassembly.
// bdSinCos is 2.4% of samples and is pure: an angle in, a sine through r3 and
// a cosine through r4. The mapping was established by running the host version
// against the recompiled one, not by reading the polynomial, and a captured
// frame confirms the world is oriented correctly with it on.
//
// Correct, and NOT proven faster. Two back-to-back pairs said it was worth a
// third to a half of the main thread; a third pair, run OFF/ON/OFF minutes
// later, measured 5.12 / 5.18 / 8.62ms. The two OFF runs differ by 68% from
// each other with no configuration change at all, and the ON run sits between
// them - so the earlier result was drift, not effect.
//
// Do not re-enable this on the strength of another pair of runs. The desktop
// workload drifts far more than back-to-back pairing controls for, and the only
// method that can settle a change this size is alternating the two paths
// *within a single run*. See research/20260830_0100_the-first-profile.md.
REXCVAR_DEFINE_BOOL(bd_host_sincos, false, kCvarGroup,
                    "Compute bdSinCos on the host instead of the guest.");
REXCVAR_DEFINE_BOOL(bd_verify_guest_math, false, kCvarGroup,
                    "Compare host maths replacements against the guest.");
REXCVAR_DEFINE_BOOL(bd_cull_early, true, kCvarGroup,
                    "Skip the visibility test for distance-culled nodes.");
REXCVAR_DEFINE_BOOL(bd_draw_phase_timing, false, kCvarGroup,
                    "Per-draw phase timing for the [perf] breakdown.");
REXCVAR_DEFINE_BOOL(bd_sleep_spin, false, kCvarGroup,
                    "Busy-wait the tail of a guest Sleep for precision.");
REXCVAR_DEFINE_BOOL(bd_sample_profiler, false, kCvarGroup,
                    "Sample guest thread PCs into logs/guest_profile.txt.");
REXCVAR_DEFINE_INT32(bd_sample_hz, 1000, kCvarGroup,
                     "Sampling profiler rate in Hz.")
    .range(50, 4000);

REXCVAR_DEFINE_INT32(bd_diag_verbosity, 0, kCvarGroup,
                     "Render diagnostic log verbosity: 0 silent, 1 fallback "
                     "diagnostics, 2 per-frame telemetry.")
    .range(0, 2);

REXCVAR_DEFINE_INT32(bd_surface_pool_budget_pct, 0, kCvarGroup,
                     "Percent of adapter VRAM the render-target pool may hold "
                     "parked. 0 = auto (three eighths of it). Lower trades "
                     "allocation hitches for VRAM headroom.")
    .range(0, bd::gpu::kSurfacePoolBudgetCapPercent);

namespace bd::gpu {
namespace {

std::string FormatCvar(f64 v) {
  char buf[32];
  auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), v);
  return ec == std::errc() ? std::string(buf, end) : std::string("0");
}

std::string FormatCvar(i32 v) { return std::to_string(v); }
std::string FormatCvar(bool v) { return v ? "true" : "false"; }

constexpr i32 kAALevelOff = 1;
constexpr i32 kSuperSampleMaxLevel = 4;
constexpr i32 kMultiSampleMaxLevel = 8;
constexpr i32 kAAPowerOfTwoStep = 2;
constexpr f64 kShadowDistanceEpsilon = 0.01;

// bd_supersampling's validator accepts only 1/2/4 and bd_msaa's only
// 0/2/4/8, so a level between two legal values must round down to one
// instead of picking a number the validator rejects outright, which
// would leave the write a silent no-op and the pair disagreeing. Every
// legal non-off value on both cvars is a power of two, so the nearest one
// at or below the level (and at or below the path's cap) is always legal.
// Callers always pass level > kAALevelOff.
constexpr i32 SnapAALevel(i32 level, i32 cap) {
  i32 snapped = kAALevelOff;
  for (i32 v = kAAPowerOfTwoStep; v <= cap; v *= kAAPowerOfTwoStep) {
    if (level >= v)
      snapped = v;
  }
  return snapped;
}

struct PresetBundle {
  i32 superSampling;
  i32 msaa;
  i32 anisotropy;
  f64 shadowDistance;
  i32 shadowDimension;
};

// Cost-ranked: the AA path is the expensive setting, so Low and Medium stay on
// multisampling and High and Ultra step onto supersampling. The 8192 shadow
// map costs real VRAM and fill, so it is Ultra only. Anisotropic filtering is
// near-free on modern GPUs and the menus offer it as a plain on/off, so every
// preset takes the full level.
constexpr PresetBundle kPresets[kQualityPresetCount] = {
    /* Low    */ {1, 0, 16, 1.0, 2048},
    /* Medium */ {1, 4, 16, 2.0, 4096},
    /* High   */ {2, 0, 16, 2.0, 4096},
    /* Ultra  */ {4, 0, 16, 4.0, 8192},
};

} // namespace

Settings &Settings::Get() {
  static Settings s;
  return s;
}

void Settings::AdoptAnisotropy() { anisotropy_ = REXCVAR_GET(bd_anisotropy); }
void Settings::AdoptNTSCFilter() { ntscFilter_ = REXCVAR_GET(bd_ntsc_filter); }
void Settings::AdoptDOFStrength() {
  dofStrength_ = REXCVAR_GET(bd_dof_strength);
}
void Settings::AdoptShadowDistance() {
  shadowDistance_ = REXCVAR_GET(bd_shadow_distance);
}
void Settings::AdoptReflectionUpscale() {
  reflectionUpscale_ = REXCVAR_GET(bd_reflection_upscale);
}
void Settings::AdoptVsync() { vsync_ = REXCVAR_GET(bd_vsync); }
void Settings::AdoptDiagVerbosity() {
  diagVerbosity_ = REXCVAR_GET(bd_diag_verbosity);
}
void Settings::AdoptAspectRatio() {
  aspectRatio_ = REXCVAR_GET(bd_aspect_ratio);
}
void Settings::AdoptFOVOffset() { fovOffset_ = REXCVAR_GET(bd_fov_offset); }
void Settings::AdoptShadowDimension() {
  shadowDimension_ = REXCVAR_GET(bd_shadow_dimension);
}
void Settings::AdoptPSOPrecache() {
  psoPrecache_ = REXCVAR_GET(bd_pso_precache);
}
void Settings::AdoptGeometryGPUUpload() {
  geometryGPUUpload_ = REXCVAR_GET(bd_geometry_gpu_upload);
}
void Settings::AdoptDRED() { dred_ = REXCVAR_GET(bd_dred); }
void Settings::AdoptSurfacePoolBudgetPercent() {
  surfacePoolBudgetPercent_ = REXCVAR_GET(bd_surface_pool_budget_pct);
}
void Settings::AdoptSuperSampling() {
  superSampling_ = REXCVAR_GET(bd_supersampling);
}
void Settings::AdoptMSAA() { msaa_ = REXCVAR_GET(bd_msaa); }

bool Settings::SetAnisotropy(i32 v) {
  return rex::cvar::SetFlagByName("bd_anisotropy", FormatCvar(v));
}

bool Settings::SetNTSCFilter(bool v) {
  return rex::cvar::SetFlagByName("bd_ntsc_filter", FormatCvar(v));
}

bool Settings::SetDOFStrength(f64 v) {
  return rex::cvar::SetFlagByName("bd_dof_strength", FormatCvar(v));
}

bool Settings::SetShadowDistance(f64 v) {
  return rex::cvar::SetFlagByName("bd_shadow_distance", FormatCvar(v));
}

// Both writes attempted, for the same reason SetAAPair attempts both.
bool Settings::SetShadowQuality(f64 distance, i32 dimension) {
  const bool dist = SetShadowDistance(distance);
  const bool dim =
      rex::cvar::SetFlagByName("bd_shadow_dimension", FormatCvar(dimension));
  return dist && dim;
}

bool Settings::SetSurfacePoolBudgetPercent(i32 v) {
  return rex::cvar::SetFlagByName("bd_surface_pool_budget_pct", FormatCvar(v));
}

bool Settings::SetVsync(bool v) {
  return rex::cvar::SetFlagByName("bd_vsync", FormatCvar(v));
}

bool Settings::SetDiagVerbosity(i32 v) {
  return rex::cvar::SetFlagByName("bd_diag_verbosity", FormatCvar(v));
}

bool Settings::SetAspectRatio(i32 v) {
  return rex::cvar::SetFlagByName("bd_aspect_ratio", FormatCvar(v));
}

bool Settings::SetFOVOffset(i32 v) {
  return rex::cvar::SetFlagByName("bd_fov_offset", FormatCvar(v));
}

// Both angles halved, so the ratio of their tangents is what scales a camera's
// own half-angle. The authored value short-circuits rather than dividing a
// tangent by itself.
f64 Settings::FOVTanScale() const {
  if (fovOffset_ == 0)
    return 1.0;
  constexpr f64 kHalfDegreesToRadians = std::numbers::pi / 360.0;
  return std::tan((kAuthoredFOVDegrees + fovOffset_) * kHalfDegreesToRadians) /
         std::tan(kAuthoredFOVDegrees * kHalfDegreesToRadians);
}

gpu::AAMode Settings::AAMode() const {
  return superSampling_ > kAALevelOff ? gpu::AAMode::SuperSample
                                      : gpu::AAMode::MultiSample;
}

i32 Settings::AALevel() const {
  if (AAMode() == gpu::AAMode::SuperSample)
    return superSampling_;
  return msaa_ > kAALevelOff ? msaa_ : kAALevelOff;
}

// Both writes are attempted even if the first is rejected, so a partial
// failure shows up in the return instead of being hidden by a short-circuit
// that would leave the path and its multiplier disagreeing.
bool Settings::SetAAPair(i32 superSampling, i32 msaa) {
  const bool ss =
      rex::cvar::SetFlagByName("bd_supersampling", FormatCvar(superSampling));
  const bool ms = rex::cvar::SetFlagByName("bd_msaa", FormatCvar(msaa));
  BD_DEBUG("[config] AA: supersampling={} msaa={}", superSampling, msaa);
  return ss && ms;
}

// Switching path carries the current multiplier across. Enabling supersampling
// bumps an Off level to 2x, since a no-op enable reads as broken, and caps at
// the supersampling ceiling.
bool Settings::SetAAMode(gpu::AAMode mode) {
  i32 level = AALevel();
  if (mode == gpu::AAMode::SuperSample) {
    level = std::clamp(level, 2, MaxAALevel(gpu::AAMode::SuperSample));
    return SetAAPair(level, 0);
  }
  return SetAAPair(kAALevelOff, level > kAALevelOff ? level : 0);
}

i32 Settings::MaxAALevel(gpu::AAMode mode) {
  return mode == gpu::AAMode::SuperSample ? kSuperSampleMaxLevel
                                          : kMultiSampleMaxLevel;
}

bool Settings::SetAALevel(i32 level) {
  if (level <= kAALevelOff)
    return SetAAPair(kAALevelOff, 0);
  const i32 snapped = SnapAALevel(level, MaxAALevel(AAMode()));
  if (AAMode() == gpu::AAMode::SuperSample)
    return SetAAPair(snapped, 0);
  return SetAAPair(kAALevelOff, snapped);
}

gpu::QualityPreset Settings::QualityPreset() const {
  for (u32 i = 0; i < kQualityPresetCount; ++i) {
    const PresetBundle &p = kPresets[i];
    const i32 ss = superSampling_ > kAALevelOff ? superSampling_ : kAALevelOff;
    // SetAAPair always writes msaa 0 alongside superSampling > 1, so both
    // settings exact-match rather than msaa being ignored on the supersampling
    // bundles.
    if (ss == p.superSampling && msaa_ == p.msaa &&
        anisotropy_ == p.anisotropy &&
        std::abs(shadowDistance_ - p.shadowDistance) < kShadowDistanceEpsilon &&
        shadowDimension_ == p.shadowDimension) {
      return static_cast<gpu::QualityPreset>(i);
    }
  }
  return gpu::QualityPreset::Custom;
}

bool Settings::SetQualityPreset(gpu::QualityPreset preset) {
  const u32 i = static_cast<u32>(preset);
  if (i >= kQualityPresetCount)
    return false; // Custom is a state, not a target
  const PresetBundle &p = kPresets[i];
  bool ok = SetAAPair(p.superSampling, p.msaa);
  ok = SetAnisotropy(p.anisotropy) && ok;
  ok = SetShadowDistance(p.shadowDistance) && ok;
  ok = rex::cvar::SetFlagByName("bd_shadow_dimension",
                                FormatCvar(p.shadowDimension)) &&
       ok;
  BD_DEBUG("[config] quality preset = {}", ToString(preset));
  return ok;
}

void Settings::AdoptCvars() {
  AdoptAnisotropy();
  AdoptNTSCFilter();
  AdoptDOFStrength();
  AdoptShadowDistance();
  AdoptReflectionUpscale();
  AdoptVsync();
  AdoptDiagVerbosity();
  AdoptAspectRatio();
  AdoptFOVOffset();
  AdoptShadowDimension();
  AdoptPSOPrecache();
  AdoptGeometryGPUUpload();
  AdoptDRED();
  AdoptSurfacePoolBudgetPercent();
  AdoptSuperSampling();
  AdoptMSAA();
}

void Settings::Init() {
  AdoptCvars();

  auto reg = [](const char *name, void (Settings::*adopt)()) {
    rex::cvar::RegisterChangeCallback(
        name, [adopt](std::string_view, std::string_view) {
          (Settings::Get().*adopt)();
        });
  };
  reg("bd_anisotropy", &Settings::AdoptAnisotropy);
  reg("bd_ntsc_filter", &Settings::AdoptNTSCFilter);
  reg("bd_dof_strength", &Settings::AdoptDOFStrength);
  reg("bd_shadow_distance", &Settings::AdoptShadowDistance);
  reg("bd_reflection_upscale", &Settings::AdoptReflectionUpscale);
  reg("bd_vsync", &Settings::AdoptVsync);
  reg("bd_diag_verbosity", &Settings::AdoptDiagVerbosity);
  reg("bd_aspect_ratio", &Settings::AdoptAspectRatio);
  reg("bd_fov_offset", &Settings::AdoptFOVOffset);
  reg("bd_shadow_dimension", &Settings::AdoptShadowDimension);
  reg("bd_pso_precache", &Settings::AdoptPSOPrecache);
  reg("bd_geometry_gpu_upload", &Settings::AdoptGeometryGPUUpload);
  reg("bd_dred", &Settings::AdoptDRED);
  reg("bd_surface_pool_budget_pct", &Settings::AdoptSurfacePoolBudgetPercent);
  reg("bd_supersampling", &Settings::AdoptSuperSampling);
  reg("bd_msaa", &Settings::AdoptMSAA);
}

} // namespace bd::gpu
