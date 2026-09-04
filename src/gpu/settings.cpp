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

// Drive RenderDoc's in-application API.
//
// A frame capture is the only instrument that answers "which draw wrote
// nothing, and into what" without inference, and this port has now lost that
// argument repeatedly - the multiview resolve had ten causes eliminated by
// measurement and none of them right. Khronos ships no Windows validation
// binaries, so on the desktop RenderDoc *is* the instrument.
//
// Two cvars rather than one because the module has to be loaded before the
// VkInstance exists (RenderDoc hooks at load time) while the trigger has to
// wait for autoplay to reach a field scene.
REXCVAR_DEFINE_BOOL(bd_renderdoc, false, kCvarGroup,
                    "Load RenderDoc at startup so a capture can be triggered. "
                    "Set RENDERDOC_DLL to override the module path.");

// Seconds, for the same reason bd_capture_after_s is: args.txt is read once at
// launch, so a bool would only ever capture the title screen.
REXCVAR_DEFINE_DOUBLE(bd_renderdoc_after_s, 0.0, kCvarGroup,
                      "Trigger a RenderDoc capture this many seconds after "
                      "start, into logs/renderdoc/. 0 disables. Needs "
                      "bd_renderdoc.")
    .range(0.0, 100000.0);

REXCVAR_DEFINE_INT32(bd_renderdoc_frames, 1, kCvarGroup,
                     "Consecutive frames the RenderDoc trigger captures.")
    .range(1, 16);

// Mirror the VR image into the desktop window.
//
// Off on Android and it must stay that way: presenting the Android surface in
// VR costs 124ms a frame, which is the single largest win this port ever made
// (see EnsureOffscreen). Nobody looks at that surface on a headset anyway.
//
// On the desktop it is the difference between a simulator run you can watch and
// a black window - the game renders to an offscreen target in VR and never
// presents, so without this the window shows nothing and looks broken.
// The size the headset frame is composed at, as a fraction of the window,
// which on a Quest 2 is the whole panel (3664x1920). Composing a ~1376x720
// game frame at the window's size was a 2.7 ms gamma pass and a 0.8 ms copy
// every frame, upsampling what the compositor resamples again anyway
// (render-stage trace, 2026-09-02).
// 0.4 of a 3664x1920 window is 1466x768, just above the game's frame.
REXCVAR_DEFINE_DOUBLE(bd_xr_present_scale, 0.4, kCvarGroup,
                      "Headset frame size as a fraction of the window, "
                      "aspect preserved. 1 = the window's size. Requires "
                      "restart.");
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

// Give two layers only to surfaces at or above half the design canvas.
//
// The frame is fill-bound and multiview rasterises every target twice, post
// chain included - but the post chain reads the resolved side-by-side companion
// and does not need two eyes. Off by default: the same narrowing, applied too
// widely, once collapsed the stereo pair and cost a session and a half to find.
// Flip render targets out of write layout in one batch when the framebuffer
// changes, instead of one at a time on the draw that first samples them.
//
// The reasoning: every barrier ends the active render pass (plume's barriers()
// calls endActiveRenderPass unconditionally), which on a tiler is a tile store
// and reload of the bound target. At a framebuffer change the pass ends anyway;
// mid-pass it does not.
//
// MEASURED NULL, 2026-08-30, within one run on a Quest 2. The mechanism fires -
// bar_drawfb 48 -> 54, barriers 104 -> 107 - because a surface flipped to
// SHADER_READ has to flip back, so it adds transitions rather than moving them.
// gpu_draw 51.90 -> 51.00ms, -1.7%, inside noise.
//
// The useful part is the elimination: barrier count went UP 3% while GPU time
// went slightly DOWN, so the ~104 barriers a frame are not what the GPU is
// spending its time on. Do not re-run this experiment.
// Hold bd_capture_after_s until a frame has at least this many draws.
//
// Autoplay does not land in the same place twice, so a time-gated capture
// photographs a menu or a loading screen as often as the scene, and a black
// grab is indistinguishable from a rendering bug. A desktop field scene is
// ~2000 draws and a Quest one ~550; a menu is 20-800.
// Collect a render pass's draws and emit them at the end of the pass instead of
// one at a time as the guest submits them.
//
// On its own this changes nothing about the image - the draws come out in the
// same order - and that is the point: it is the step that has to be proven
// pixel-identical before bd_draw_sort is allowed to reorder anything.
// Clear the back buffer to magenta before the gamma blit.
//
// The flat present path renders black on both the AYN Thor and the desktop
// while the Quest's XR path is correct, and reading the code has not settled
// whether the frame never reaches the display or the blit draws nothing.
REXCVAR_DEFINE_BOOL(bd_debug_present_clear, false, kCvarGroup,
                    "Clear the back buffer to magenta before the present blit.");

// ON. Verified on a Quest 2: pixel-correct stereo, crossed disparity, and a
// frame identical to immediate submission (66.86ms against 66.82, gpu_total
// 56.24 against 56.40). It is neutral on its own and it is the seam every later
// technique attaches to - instancing, indirect draws, GPU culling - so it ships
// rather than rotting behind a flag.
//
// Side-by-side stereo and user-pointer draws are excluded inside DispatchDraw
// and submit immediately; a draw that cannot be deferred flushes the queue
// first, so guest submission order is exact either way.
// Dump one field frame's render passes in order, with the draws each took. The
// per-target census aggregates and cannot show sequence, so it cannot say
// whether a pass runs twice - and the scene pass is 45ms of a 56ms frame.
// Fixed foveated rendering on the scene pass, via a fragment density map.
//
// The scene pass is ~45ms of a 56ms frame on a Quest 2 - measured per render
// target - and it is two-layer, so every fragment it saves is saved twice.
// Unlike XR_FB_foveation this needs no present rewrite: a density map decorates
// an ordinary render pass.
REXCVAR_DEFINE_BOOL(bd_foveation, false, kCvarGroup,
                    "Fixed foveated rendering on the scene pass.");

// Shading rate at the corners, as a fraction of full. 1.0 is no foveation, 0.25
// is a quarter rate in the periphery. The centre 35% of the radius always stays
// at full rate - this trades peripheral detail, never the middle of the image,
// because the one headset session that failed here failed on readability.
REXCVAR_DEFINE_DOUBLE(bd_foveation_strength, 0.5, kCvarGroup,
                      "Peripheral shading rate as a fraction of full.")
    .range(0.1, 1.0);

REXCVAR_DEFINE_BOOL(bd_dump_passes, false, kCvarGroup,
                    "Log one field frame's render passes in order.");

REXCVAR_DEFINE_BOOL(bd_draw_defer, true, kCvarGroup,
                    "Defer draw submission to the end of the render pass.");

// Sort the deferred draws: opaque grouped by pipeline and near-to-far, blended
// left in submission order behind them.
//
// Grouping collapses pipeline switches (~114 against ~553 draws). Near-first is
// what lets Adreno's low-resolution Z reject a hidden fragment before shading
// it, which is why this is a GPU win and not only a CPU one. Requires
// bd_draw_defer.
// Flush the deferred queue after every draw. Functionally the same as immediate
// submission, but through the record-and-replay path - which separates "the
// state capture is wrong" from "the batching is wrong". Requires bd_draw_defer.
REXCVAR_DEFINE_BOOL(bd_draw_defer_each, false, kCvarGroup,
                    "Flush the draw queue after every draw, to isolate capture "
                    "correctness from batching.");

// OFF, and measured rather than assumed. It is correct - stereo stays crossed -
// and it buys nothing here: the guest already submits pipeline-coherently, 166
// opaque draws taking 14 pipeline binds, and only 166 of 562 draws are opaque at
// all. Reordering for no gain is risk without return, so it stays available and
// off until something makes it pay.
REXCVAR_DEFINE_BOOL(bd_draw_sort, false, kCvarGroup,
                    "Sort deferred draws by pipeline and depth.");

// Instancing on the deferred draw queue. A scene node draw whose vertex
// shader carries the record redirect stages its per-node constants (world,
// previous world, bone palettes) into an InstanceRecord; the queue merges
// consecutive draws that share the pipeline, the mesh (vertex and index
// views), the material (pixel and shared constant offsets) and the rest of
// the vertex block into one drawIndexedInstanced. Every merged draw is a
// whole per-draw cost saved - ~36 us of GPU on a Quest 2 - and even a group
// of one stops the vertex constant window from moving per node.
REXCVAR_DEFINE_BOOL(bd_draw_instancing, true, kCvarGroup,
                    "Merge scene draws that share a mesh and material into "
                    "instanced draws (deferred queue only).");
// Bring equal draws together inside each run of order-independent draws
// (opaque, depth-tested, no stencil) so more of them are consecutive. Never
// moves a draw across one that has to keep its place.
// A draw alone in its instancing group takes the plain pipeline, its record
// uploaded as an ordinary vertex window. Measured on the Quest 2 (2026-09-02):
// every scene draw through the record path cost 45 ms of GPU against 37.5,
// the storage-buffer constant reads outweighing the window re-base.
REXCVAR_DEFINE_BOOL(bd_draw_instancing_singles_plain, false, kCvarGroup,
                    "Emit single-draw instancing groups through the plain "
                    "pipeline.");
// ON, and worth 8 ms of GPU: a within-run A/B on the Quest 2 (2026-09-02,
// verify defaults) put the GPU frame at 52.9 ms with it off and 44.9 ms on -
// fewer pipeline switches and the instances it brings together outweigh any
// change in overdraw on this direct-mode pass. (Read backwards for an hour
// the first time: the summary's arm 0 is the flag off.)
REXCVAR_DEFINE_BOOL(bd_draw_instancing_reorder, true, kCvarGroup,
                    "Reorder order-independent scene draws so instances "
                    "become consecutive (measured -8 ms GPU on).");
// Blended draws that write depth count as order-independent for the reorder
// above. The guest leaves blending on for most opaque and cut-out materials
// (64% of scene draws blend and write depth), so without this almost nothing
// in a field scene may move. A real transparency does not write depth and
// keeps its place either way.
// The scene recorder (gpu/scene/scene_recorder.cpp): after this many seconds,
// record bd_scene_record_frames frames of node draws - mesh, material,
// textures, transform, pass - and write <cache>/scene_walk/walk_<stamp>.bdsw.
// Off at 0. tools/scene_walk_dump.py reads the file.
REXCVAR_DEFINE_DOUBLE(bd_scene_record_after_s, 0.0, kCvarGroup,
                      "Seconds after launch to start recording the scene "
                      "walk; 0 = off.");
REXCVAR_DEFINE_INT32(bd_scene_record_frames, 8, kCvarGroup,
                     "Frames of scene walk to record once armed.");

// The scene tree walk on the host (gpu/scene/host_walk.cpp): the traversal,
// the cull and the draw hand-off are host code; the per-node interpreter is
// still the guest's. Bit-identical to the guest walk by construction.
// One-shot diagnostic: which constant registers bdSceneNodeDrawSingle writes
// per node draw, printed once after 4000 draws. The host-issued node draw
// (stage 2b) has to reproduce exactly these.
REXCVAR_DEFINE_BOOL(bd_node_write_diag, false, kCvarGroup,
                    "Print which constant registers the per-node interpreter "
                    "writes, once.");

// Host-issued node draws (gpu/scene/host_draw.cpp): a node whose draw the
// interpreter has produced once is drawn by the host from that template for
// the next bd_host_draw_refresh frames, world rebuilt from the palette slot;
// then the interpreter runs once and refreshes it. Skinned nodes, foliage
// (c57) and animated materials keep the interpreter.
// ON, with replayed draws kept off the instance-record path
// (bd_host_draw_records): with them on it, the village's big rock was hidden
// in some frames (desktop, 2026-09-02 night); three captures with them off
// showed it every time. The mechanism is not yet named - the rock's own node
// is interpreted and complete in every frame, so a replayed draw is what
// covers it.
REXCVAR_DEFINE_BOOL(bd_host_draw, true, kCvarGroup,
                    "Issue scene node draws from host templates, skipping the "
                    "guest's per-node interpreter.");
// Diagnostic: the mesh (guest VA) whose queued draws are logged in full, both
// when the interpreter issues them and when the host replays them, so the
// two can be diffed.
REXCVAR_DEFINE_INT32(bd_node_diag_mesh, 0, kCvarGroup,
                     "Log every queued draw of this mesh VA (0 = off).");
// Whether a host-issued node draw may stage an instance record (and so join
// an instancing group). Off: the two configurations in which the village's
// rock vanished both had replayed draws on the record path.
// Diagnostic: every node the replay would issue is composed by the replay
// and then interpreted, and the interpreter's draws are diffed against the
// replay's composition ([verify] lines). The frame stays correct.
REXCVAR_DEFINE_INT32(bd_host_draw_verify_every, 0, kCvarGroup,
                     "Verify every Nth replay candidate in a normal run "
                     "(0 = off).")
    .range(0, 100000);
REXCVAR_DEFINE_BOOL(bd_host_draw_verify, false, kCvarGroup,
                    "Diff the host replay against the interpreter per draw "
                    "(the interpreter draws).");
// Diagnostic: 1 = the record mask as designed, 2 = the group's window is
// rebound but every mask is all ones, 3 = the masks are computed but the
// window is not rebound (2026-09-03, the cyan skirt).
// Off: registers c64 and up always come from the record (see
// CommitInstanceRecords). On reinstates the mask there - wrong under
// drawIndexedIndirect on the desktop as of 2026-09-03.
REXCVAR_DEFINE_BOOL(bd_record_mask_high, false, kCvarGroup,
                    "Mask registers c64 and up as well (diagnostic).");
REXCVAR_DEFINE_INT32(bd_record_mask_mode, 1, kCvarGroup,
                     "Record mask diagnostic mode (1 normal, 2 rebind only, "
                     "3 masks only).")
    .range(1, 10);
REXCVAR_DEFINE_BOOL(bd_host_draw_records, true, kCvarGroup,
                    "Let host-issued node draws use instance records.");
REXCVAR_DEFINE_INT32(bd_host_draw_refresh, 16, kCvarGroup,
                     "Frames a host node draw template is used before the "
                     "interpreter refreshes it.");

REXCVAR_DEFINE_BOOL(bd_host_list_build, true, kCvarGroup,
                    "Emit the guest's deferred render-list entries from a "
                    "host template instead of running the per-node "
                    "interpreter to build them.");
// The draw bisector (draw_queue.cpp BisectDrops): N windows over the first
// `span` queued draws of a frame, one window dropped at a time, advancing
// every `frames` frames. 0 windows = off.
REXCVAR_DEFINE_INT32(bd_debug_bisect_windows, 0, kCvarGroup,
                     "Draw bisector: windows over a frame's draws (0 = off).")
    .range(0, 4096);
REXCVAR_DEFINE_INT32(bd_debug_bisect_frames, 15, kCvarGroup,
                     "Draw bisector: frames per window.")
    .range(1, 10000);
REXCVAR_DEFINE_INT32(bd_debug_bisect_span, 1024, kCvarGroup,
                     "Draw bisector: draws covered by the windows.")
    .range(1, 100000);
REXCVAR_DEFINE_BOOL(bd_debug_skip_list_draws, false, kCvarGroup,
                    "Probe: drop the scene-pass draws that come from the "
                    "guest's deferred render list (sorted and translucent "
                    "materials). For a within-run A/B of their GPU share.");
REXCVAR_DEFINE_BOOL(bd_debug_skip_blended, false, kCvarGroup,
                    "Probe: drop the scene-pass draws that blend. For a "
                    "within-run A/B of their GPU share.");
REXCVAR_DEFINE_DOUBLE(bd_debug_mip_bias, 0.0, kCvarGroup,
                      "Probe: mip LOD bias applied to every guest sampler. "
                      "A large positive value blurs whatever samples a "
                      "reachable mip chain.");
REXCVAR_DEFINE_BOOL(bd_cel_characters, false, kCvarGroup,
                    "Cel-shade the skinned draws (the characters): the "
                    "recompiled pixel shaders band their lit colour under a "
                    "spec constant the host sets for those draws.");
REXCVAR_DEFINE_BOOL(bd_xr_direct_present, true, kCvarGroup,
                    "Render the present pass straight into the runtime's "
                    "swapchain image instead of an offscreen frame copied "
                    "into it (one full-frame copy and a preemption slot "
                    "fewer per frame).");
REXCVAR_DEFINE_BOOL(bd_walk_skip_stubs, true, kCvarGroup,
                    "With reflections off, the host walk skips the 128x72 "
                    "reflection stub pass instead of rendering it.");
REXCVAR_DEFINE_BOOL(bd_host_walk, true, kCvarGroup,
                    "Walk the scene tree on the host instead of in the guest.");

// Off since 2026-09-04: the village rock's flat patch of clear colour was
// this reorder. The ground pieces are blended depth-writers, a transparent
// skirt piece moved ahead of its ground piece leaves the ground failing the
// depth test where the two overlap, and which frames it happens in follows
// the group keys, which follow which node the host interpreted that frame.
// A within-run A/B read 40 patch frames of 120 with it on against 4 (the arm
// boundaries) with it off. Blended depth-writers still instance when they
// are consecutive; the Quest cost of losing their reorder is unmeasured.
REXCVAR_DEFINE_BOOL(bd_draw_instancing_reorder_blended, false, kCvarGroup,
                    "Let blended depth-writing draws be reordered for "
                    "instancing (wrong where they overlap).");

// Side-by-side stereo: emit every left-eye draw, then every right-eye draw,
// instead of alternating the viewport on every draw. Image-identical; removes
// ~1000 viewport/scissor changes from the scene pass, which is one candidate
// for what keeps that pass in direct (non-tiled) rendering on Adreno.
REXCVAR_DEFINE_BOOL(bd_draw_eye_major, true, kCvarGroup,
                    "Emit deferred side-by-side draws eye-major rather than "
                    "alternating viewports per draw.");

// Probe: end and reopen the deferred pass's render pass every N draws (0 =
// never). Adreno runs the ~500-draw scene pass in direct mode while a small
// pass on the same surface type bins; if the trigger is the size of the pass,
// the chunks bin. Each split costs a tile load and store.
REXCVAR_DEFINE_INT32(bd_pass_split_draws, 0, kCvarGroup,
                     "Split a deferred render pass every N draws (probe).")
    .range(0, 100000);

// Per-segment and per-target GPU timestamps, written INSIDE render passes.
// Off on Android: a timestamp inside a pass takes Adreno out of tiled
// rendering for that pass, and the on-device render-stage trace showed every
// surface of a field frame in direct mode with these on. gpu_total_ms keeps
// its frame begin/end pair either way; the per-target census and the
// gpu_draw/resolve/inter split need this on, and cost the tiler to read.
#if defined(__ANDROID__)
REXCVAR_DEFINE_BOOL(bd_gpu_timing_segments, false, kCvarGroup,
                    "Write per-segment GPU timestamps inside render passes "
                    "(forces Adreno out of tiled rendering).");
#else
REXCVAR_DEFINE_BOOL(bd_gpu_timing_segments, true, kCvarGroup,
                    "Write per-segment GPU timestamps inside render passes.");
#endif

// Depth prepass on the deferred queue: every depth-writing draw of a pass is
// emitted first with colour writes off, then the pass is emitted again with
// depth writes off and a LEQUAL test, so only the nearest fragment at each
// pixel is shaded. The scene pass costs ~7 ms per eye at 1376x720 on a Quest 2
// with ~2x overdraw shaded in full, because 64% of its draws blend and write
// depth and that switches the tiler's low-resolution Z off for the rest of the
// pass. This makes rejection independent of LRZ. Needs bd_draw_defer.
REXCVAR_DEFINE_BOOL(bd_depth_prepass, false, kCvarGroup,
                    "Emit a colour-off depth prepass before each deferred "
                    "pass, then shade with depth writes off (needs "
                    "bd_draw_defer).");

// The early-out half of bdSetSamplerState, done host-side. ~23,000 guest calls
// a frame, the vast majority of which exist only to discover they have nothing
// to do - and each pays a recompiled prologue to find out. The slow path still
// runs the guest body, so this cannot change behaviour.
// Default OFF: measured SLOWER than the guest it replaces. See the note in
// gpu/hooks/scene_node.cpp - two within-run A/Bs put it at +1.4% and +3.1% on
// us/draw. Kept because it is correct and because the finding matters more than
// the code: the recompiled early-out is already cheap enough that a host
// wrapper cannot beat it.
REXCVAR_DEFINE_BOOL(bd_host_sampler_state, false, kCvarGroup,
                    "Take bdSetSamplerState's unchanged-value early-out in "
                    "host code instead of in recompiled guest code.");

// 337 of 530 field-scene draws blend AND write depth - 64% of the frame,
// counted on the desktop, not inferred. On a Xenon that was free: EDRAM had no
// low-resolution Z to lose. On a tiler it is the documented way to invalidate
// LRZ, and it invalidates it for the REST OF THE PASS, so those 337 draws cost
// every later draw its early rejection.
//
// That matches what is measured: the scene carries ~2x overdraw (forcing depth
// ALWAYS doubles desktop GPU time) and front-to-back sorting buys exactly
// nothing, which is the signature of a tiler that never rejects.
//
// Transparent geometry conventionally tests depth and does not write it - a
// blended surface does not occlude what comes after it. This restores that.
REXCVAR_DEFINE_BOOL(bd_blend_no_depth_write, false, kCvarGroup,
                    "Blended draws test depth but do not write it, which is "
                    "what lets a tiler keep low-resolution Z.");

// The other way round, and the more promising one. All 337 of those draws use
// SRC_ALPHA/INV_SRC_ALPHA - not one is additive - and 64% of a frame is far too
// much to be real transparency. That is the shape of an X360 habit: leaving
// blending enabled on geometry whose alpha is always 1.0, where the blend is a
// no-op the hardware still has to honour.
//
// If they are no-op blends, clearing alphaBlendEnable restores the tiler's
// early rejection AND leaves the image untouched - unlike suppressing the depth
// write, which breaks the depth-of-field and fog passes that sample depth.
REXCVAR_DEFINE_BOOL(bd_blend_off_when_opaque, false, kCvarGroup,
                    "Clear alphaBlendEnable on blended draws that write depth, "
                    "to test whether those blends do anything at all.");

// Consecutive frames to capture once the gates admit one: a few-frame
// artefact is a jump between neighbours (tools/capture_seq.py).
REXCVAR_DEFINE_INT32(bd_capture_frames, 1, kCvarGroup,
                     "Consecutive frames to capture from the first admitted.")
    .range(1, 600);
REXCVAR_DEFINE_INT32(bd_capture_min_draws, 0, kCvarGroup,
                     "Delay bd_capture_after_s until a frame has this many "
                     "draws, so the capture lands on a scene not a menu.")
    .range(0, 100000);

REXCVAR_DEFINE_BOOL(bd_barrier_hoist, false, kCvarGroup,
                    "Batch render-target layout transitions at framebuffer "
                    "changes rather than mid-pass.");

// Measurement only: force every multiview target to a single layer. The image
// is wrong by construction - one eye's worth of post chain - and the number it
// produces is the ceiling on what making the post chain mono could ever save.
// Measurement only: never set the alpha-test spec constant, so no pixel shader
// takes its clip() path. Renders wrongly - cutouts become opaque quads - and
// answers whether the discard is what stops Adreno's low-resolution Z from
// rejecting on a frame that is provably fragment-bound.
// Measurement only: force every pipeline's depth compare to ALWAYS, so no
// fragment is ever rejected by depth. Renders wrongly. If depth rejection is
// doing any work this must be SLOWER; if the frame does not move, nothing was
// being rejected and the full overdraw is already being shaded.
// A Vulkan ICD to load directly, bypassing the platform loader. "turnip"
// looks for libvulkan_freedreno.so beside libreblue.so (packaged with
// EXTRA_LIBS in tools/build_apk.sh); anything else is a path. Mesa's Turnip
// logs why a render pass fell back to system-memory rendering, which the
// Adreno blob will not say and which is the open question on the Quest.
REXCVAR_DEFINE_STRING(bd_vulkan_icd, "", kCvarGroup,
                      "Vulkan ICD to load directly: \"turnip\" or a path. "
                      "Empty uses the platform loader.");
// Probe: bindless sets without update-after-bind. Formally invalid (the
// renderer writes descriptors while command buffers using them are pending),
// so a render-stage trace only - it asks whether update-after-bind is what
// keeps Adreno from binning the guest draws.
// Probes for the driver's global render-mode choice (2026-09-02: no pass of
// ours bins on the Quest while the compositor's do). Robustness is a real
// lever on Adreno regardless: robustBufferAccess bounds-checks every load.
REXCVAR_DEFINE_BOOL(bd_vulkan_no_robust, false, kCvarGroup,
                    "Leave robustBufferAccess and robustness2 access checks "
                    "off at device creation. Requires restart.");
REXCVAR_DEFINE_STRING(bd_vulkan_no_ext, "", kCvarGroup,
                      "Comma-separated optional device extensions to treat "
                      "as unsupported (probe). Requires restart.");
REXCVAR_DEFINE_BOOL(bd_debug_no_uab, false, kCvarGroup,
                    "Probe: create the bindless sets without "
                    "update-after-bind (invalid on purpose).");
// Probe: no depth writes in any pipeline. Destroys the image (everything
// overdraws); exists to ask the render-stage trace whether depth-writing
// draws are what keeps the scene pass direct - the one full-size pass that
// binned (2026-09-02) was the effects instance, which does not write depth.
// The host-owned post chain (gpu/post_chain.cpp): the depth-of-field pyramid
// and the bloom mask are produced by host passes into the guest's own
// textures, and the thirteen guest draws that made them through the EDRAM
// tile are dropped. The guest's two composites run unchanged.
// Host-generated mip chains for DXT1/3/5 textures the guest ships without
// one - two thirds of its texture data, the world textures (2026-09-02).
// Built once at upload on the CPU; costs about a third more texture memory.
REXCVAR_DEFINE_BOOL(bd_host_mips, true, kCvarGroup,
                    "Generate mip chains on the host for compressed textures "
                    "that ship without one.");
REXCVAR_DEFINE_BOOL(bd_host_post, true, kCvarGroup,
                    "Host-owned post chain: the bloom and depth-of-field "
                    "pyramids are built by host passes; the guest's producer "
                    "draws are skipped.");
// The composite half of the host chain: one full-resolution pass replaces the
// guest's depth-of-field composite, the resolve after it and the bloom
// composite. Off keeps the guest's two composites over the host's pyramid.
REXCVAR_DEFINE_BOOL(bd_host_post_composite, true, kCvarGroup,
                    "Host post chain: fold the depth-of-field and bloom "
                    "composites into one host pass (Vulkan only).");
REXCVAR_DEFINE_DOUBLE(bd_host_post_blur, 2.0, kCvarGroup,
                      "Host post chain: width of the per-level downsampling "
                      "blur, in target texels. 1 = the nominal 13-tap kernel.");
REXCVAR_DEFINE_INT32(bd_host_post_debug, 0, kCvarGroup,
                     "Host composite debug view: 1 depth, 2 blur level, 3 "
                     "scene only. 0 = the composite.");
REXCVAR_DEFINE_INT32(bd_dump_post_draws, 0, kCvarGroup,
                     "Log every post-effect draw (by pixel shader) of this "
                     "many field-scene frames: target, sampled textures, "
                     "parameter registers. 0 = off.");
// Alpha-tested geometry the guest draws blended over inverse source alpha
// AND depth-writing - foliage, fences, cutouts - becomes opaque: the alpha
// test decides the pixel, blending is dropped, depth stays written, so a
// tiler can reject what lies behind it. ~300 of a field frame's 470 scene
// draws are of this kind (2026-09-02). Edges go from soft to hard.
REXCVAR_DEFINE_BOOL(bd_cutout_opaque, false, kCvarGroup,
                    "Draw alpha-tested, depth-writing, alpha-blended "
                    "geometry as opaque cutouts.");
// Probe: every source-over, depth-writing blended draw becomes opaque, so the
// queue's front-to-back sort and early depth rejection apply to the whole
// scene. Breaks the alpha skirts; the fragment census under it is the lower
// bound the interior/skirt split can reach (2026-09-04).
REXCVAR_DEFINE_BOOL(bd_debug_blend_off, false, kCvarGroup,
                    "Probe: blended depth-writing draws drawn opaque.");
REXCVAR_DEFINE_BOOL(bd_debug_no_depth_write, false, kCvarGroup,
                    "Probe: disable depth writes in every pipeline.");
REXCVAR_DEFINE_BOOL(bd_debug_no_stencil_bias, false, kCvarGroup,
                    "Probe: strip stencil and depth bias from every pipeline "
                    "(renders wrongly on purpose).");
REXCVAR_DEFINE_BOOL(bd_debug_depth_always, false, kCvarGroup,
                    "Measurement: depth compare ALWAYS. Renders wrongly.");

REXCVAR_DEFINE_BOOL(bd_debug_no_alpha_test, false, kCvarGroup,
                    "Measurement: never enable alpha test. Renders wrongly.");

REXCVAR_DEFINE_BOOL(bd_mv_force_mono_targets, false, kCvarGroup,
                    "Measurement: force every multiview target to one layer. "
                    "Renders incorrectly on purpose.");

// Multiview at side-by-side's pixel count. Side-by-side gives each eye half
// the scene width (688x720 of 1376x720); multiview rendered two full-width
// layers, twice the fragments, and its 62.7 ms on the Quest (2026-09-02) was
// that and nothing else. The guest's scene resolution is halved in width
// (bdSceneResolutionScaleHook), so the guest sizes its own post chain, and
// the full-width projection squeezes each layer exactly as side-by-side does.
// Off until the present-side chain follows: measured 2026-09-02 14:20, the
// scene target became 688x720x2 and the frame 62.7 -> 41.7 ms, but the front
// buffer the guest resolves into is still sized from the output fit (1376
// wide) and the presented frame was black.
#if defined(__ANDROID__)
// On the headset each layer is one eye: half the guest's width per layer
// gives the same pixels per eye as side-by-side (verified as a correct pair
// on the desktop with the layered host post chain, 2026-09-03). The per-eye
// size then follows bd_render_scale like the flat frame does.
REXCVAR_DEFINE_BOOL(bd_mv_half_width, true, kCvarGroup,
#else
REXCVAR_DEFINE_BOOL(bd_mv_half_width, false, kCvarGroup,
#endif
                    "Multiview: render each layer at half the scene width, "
                    "matching side-by-side's per-eye pixels. Requires restart.");
REXCVAR_DEFINE_BOOL(bd_mv_small_targets_mono, false, kCvarGroup,
                    "Under multiview, give two layers only to surfaces at or "
                    "above half the design canvas.");

// ON, and only because of a Quest measurement nobody can yet explain. The
// bindless heap is Texture2DArray now, so every reader samples the layer it
// wants and present flattens the pair itself; this chain - five full-resolution
// passes a frame - exists only because a Texture2D heap could not read an
// array. But on a Quest 2 (2026-09-01) multiview reads 59 ms of GPU with it on
// and **277 ms** with it off, every pass ~4.5x slower, and a run with the chain
// on but its companion never sampled (bd_mv_redirect_srv=false) still reads 59.
// So it is not the array sampling and not the companion; it is something the
// resolve pass does to the frame - its barriers, or its breaking of the render
// pass - that Adreno needs. Until that is named, the chain stays on for the
// headset. The desktop does not care either way.
//
// Android only. On the desktop the chain is what turns the multiview frame
// black - with the array heap, present samples the array, and the chain's
// barriers and companion redirect fight it - and it hid the working per-eye
// MSAA resolve behind a black screen for a day (2026-09-02).
#if defined(__ANDROID__)
REXCVAR_DEFINE_BOOL(bd_mv_resolve, true, kCvarGroup,
                    "Run the multiview resolve pass that flattens the two "
                    "layers into one side-by-side image (off is 4.5x slower "
                    "on Adreno, unexplained).");
#else
REXCVAR_DEFINE_BOOL(bd_mv_resolve, false, kCvarGroup,
                    "Run the obsolete multiview resolve pass (blacks the "
                    "frame with the array heap).");
#endif

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
// The console's own model instead of the copy: a fresh full-screen surface
// bound after the chain head shares the head's texture, the way both were one
// EDRAM tile. Removes the two chain seed copies of the frame's tail; the
// image the next pass samples is materialised from the head first, which is
// the resolve the guest asked for (2026-09-03).
// Stage 4 (2026-09-03): the shadow map and the scene colour and depth are
// persistent host surfaces (gpu/host_targets.h) rather than pooled scratch
// the guest recreates every frame; their clears are the host's and their
// resolves never copy.
REXCVAR_DEFINE_BOOL(bd_host_targets, true, kCvarGroup,
                    "The shadow map and the scene pair are host-owned "
                    "persistent targets (stage 4).");
// The tail's 2D passes begin with a full-frame bd_simple2d quad copying the
// front texture into the new tile, which under the chain alias is the same
// image: the quad is skipped and the front texture's link is left in place
// instead of copied, so the two 2D passes continue the composite's render
// pass. Two full-res blits and two full-screen draws a frame (2026-09-03).
// The bloom mask's three passes (bright, blur, blur at 480x270) folded into
// the composite as the bright pass of dof level 2 (2026-09-03).
// A bool twin of bd_host_post_debug = 1 (the scene depth as the frame), so
// bd_ab_flag can alternate colour and depth frames in one capture sequence.
REXCVAR_DEFINE_BOOL(bd_host_post_debug_depth, false, kCvarGroup,
                    "Composite the scene depth instead of the image.");
// The five dof levels as one pass into a level atlas (post_pyramid_ps), each
// filtered from the scene directly (2026-09-04); off = the five-pass chain.
REXCVAR_DEFINE_BOOL(bd_host_post_atlas, true, kCvarGroup,
                    "Build the dof pyramid as one pass into a level atlas.");
REXCVAR_DEFINE_BOOL(bd_host_post_bloom_fold, true, kCvarGroup,
                    "Compute bloom in the composite from a dof level instead "
                    "of three mask passes.");
REXCVAR_DEFINE_BOOL(bd_tail_identity_skip, true, kCvarGroup,
                    "Skip the tail's identity copy quads and the blits they "
                    "needed.");
REXCVAR_DEFINE_BOOL(bd_chain_alias, true, kCvarGroup,
                    "Alias a fresh full-screen surface to the chain head's "
                    "texture instead of seeding it with a copy.");
// Desktop instrument for the materials stage: a pipeline-statistics query
// around every queued draw, folded per pixel shader at readback and printed
// every 300 frames as "[frag] ... the top ten". Which shaders produce the
// fragments the Quest's counters say the scene pass is bound by
// (2026-09-03).
// The host material shaders replace the guest's scene pixel shaders by hash
// at link time (guest_shaders.cpp): bd_normal_ps first, the family after.
// Read once per shader link, so a change needs a restart.
// The host walk's visibility test on the host: the guest's own six planes
// (its per-pass global table) against each node's sphere, instead of a call
// into the recompiled test per node (3% of the Draw Thread's samples on the
// desktop, 2026-09-03). The other view paths (reflection, shadow, the point
// test) still call the guest.
// Vertex pulling (gpu/vertex_pull.h): instanced draws whose pull info
// staged go through the pulled pipeline twin, binding no vertex streams.
// The step before indirect draws; verified on the desktop by capture.
// Indirect draws over the pulled pipeline: consecutive pulled draws that
// share pipeline, material, index buffer and pass geometry become one
// drawIndexedIndirect with a command per instancing group.
// Instance records carry a per-register mask of what differs from the
// group's uniform block, and the shader reads only those from the record
// (constant_buffers.h). Off writes all-ones masks: the old whole-record read.
// The cyan skirt of 2026-09-03 was blamed on the mask for an hour (a
// mask-off run read clean); it was the replay's pass camera and stale
// templates (gpu/scene/host_draw.cpp, PassRegs and why_drift), and the mask
// reads clean in the same scene with those in.
REXCVAR_DEFINE_BOOL(bd_record_mask, true, kCvarGroup,
                    "Instance records mask the registers that differ from "
                    "the group's uniform block.");
REXCVAR_DEFINE_BOOL(bd_draw_indirect, true, kCvarGroup,
                    "Batch pulled draws into drawIndexedIndirect calls "
                    "(needs bd_draw_pull).");
REXCVAR_DEFINE_BOOL(bd_draw_pull, true, kCvarGroup,
                    "Draw instanced groups through the vertex-pulling "
                    "pipeline twin (no per-mesh stream binds).");
REXCVAR_DEFINE_BOOL(bd_host_cull_diag, false, kCvarGroup,
                    "Run the guest's visibility test beside the host's and "
                    "log disagreements.");
// The shadow and reflection walks culled by distance from the scene camera
// (gpu/scene/host_walk.cpp): the shadow's default is the fitted map's reach.
REXCVAR_DEFINE_DOUBLE(bd_shadow_cull_distance, 500.0, kCvarGroup,
                      "Shadow casters beyond this distance from the camera "
                      "are not drawn (0 = off).")
    .range(0.0, 100000.0);
REXCVAR_DEFINE_DOUBLE(bd_reflection_cull_distance, 350.0, kCvarGroup,
                      "Reflection-view nodes beyond this distance from the "
                      "camera are not drawn (0 = off).")
    .range(0.0, 100000.0);
// Coarse index lists for the shadow and reflection views
// (gpu/scene/mesh_lod.cpp): cells across a mesh's longest axis, 0 = the
// original mesh. The views are a 1024 map and a 128x72 reflection on the
// Quest; the census of 2026-09-04 read 81k and 49k triangles a frame for them.
// The Draw Thread's per-node work, cut after the 2026-09-04 profile: the
// replay's 8 KB guest block copy only when the guest wrote a constant since
// the last one, the streams resolved once per template until a physical
// buffer moves, and instance records carrying only the registers the vertex
// shader declares.
REXCVAR_DEFINE_BOOL(bd_host_draw_fast, true, kCvarGroup,
                    "Replay caches: skip the guest block copy and the stream "
                    "resolution when nothing changed.");
REXCVAR_DEFINE_BOOL(bd_record_declared, true, kCvarGroup,
                    "Instance records carry only the registers the vertex "
                    "shader declares.");
// The host lit material's distance tier: the normal map fetch is skipped
// where the map is already minified past two texels a pixel (its detail is
// averaged away there) and the four-gather shadow kernel becomes one gather
// where the shadow map is minified (the penumbra is under a pixel). Fetches,
// which the Quest's scene pass is bound by, not visible detail (2026-09-04).
// The XR swapchain as a two-layer array, one layer an eye, submitted as two
// projection views by array index instead of a side-by-side panel the
// compositor resamples. Multiview only, and the shape stage 7's foveation
// attaches to (2026-09-04).
REXCVAR_DEFINE_BOOL(bd_xr_layered_swapchain, true, kCvarGroup,
                    "Two-layer XR swapchain, one array layer an eye "
                    "(multiview only).");
REXCVAR_DEFINE_BOOL(bd_material_tier, true, kCvarGroup,
                    "Host lit material drops fetches where the footprint says "
                    "they are invisible (bd_material_tier_bits).");
REXCVAR_DEFINE_INT32(bd_material_tier_bits, 2, kCvarGroup,
                     "1 = normal map skipped past eight texels a pixel, 2 = one "
                     "shadow gather where the map is minified.")
    .range(0, 3);
// A blended draw may move back to join its instancing group when every draw
// it crosses has a bounding sphere that does not overlap its own in the view -
// two draws that never write the same pixel cannot affect each other whatever
// the order. This is bd_draw_instancing_reorder_blended's replacement: that
// one moved blended draws freely and put the clear colour through the ground
// at the village rock (2026-09-03).
REXCVAR_DEFINE_BOOL(bd_pool_census, false, kCvarGroup,
                    "Log the surfaces the guest still acquires from the pool, "
                    "by shape, every 300 frames.");
REXCVAR_DEFINE_BOOL(bd_draw_gather_blended, true, kCvarGroup,
                    "Blended draws may join an instancing group across draws "
                    "they provably do not overlap.");
REXCVAR_DEFINE_INT32(bd_draw_gather_window, 64, kCvarGroup,
                     "How far back the blended gather looks for its group.")
    .range(0, 1024);
REXCVAR_DEFINE_BOOL(bd_lod, true, kCvarGroup,
                    "Coarse index lists for the shadow and reflection views "
                    "(the grids below); off draws every view at full detail.");
REXCVAR_DEFINE_INT32(bd_lod_shadow_grid, 24, kCvarGroup,
                   "Shadow casters draw a vertex-clustered list at this grid "
                   "(0 = full detail).")
    .range(0, 256);
// The scene view's own distance LOD: a node farther than the distance draws
// the grid list, twice as far the grid halved. The census of 2026-09-04 put
// 63% of the scene's triangles beyond 350 units.
REXCVAR_DEFINE_DOUBLE(bd_lod_scene_distance, 300.0, kCvarGroup,
                      "Scene nodes beyond this distance draw a clustered list "
                      "(0 = off).")
    .range(0.0, 100000.0);
// The cell as a fraction of the node's distance: 0.004 is about four pixels
// at 1080p with the game's vertical field of view; the grid follows from the
// mesh's extent, so a small far hut and a large near rock get cells of the
// same size on screen. Distances are quantised to half-octaves so a mesh
// holds a handful of lists.
REXCVAR_DEFINE_DOUBLE(bd_lod_scene_cell, 0.004, kCvarGroup,
                      "Scene LOD cell size as a fraction of the node's view "
                      "distance (pixels on screen, roughly x1000 / 1080p).")
    .range(0.0001, 1.0);
REXCVAR_DEFINE_INT32(bd_lod_reflection_grid, 16, kCvarGroup,
                   "Reflection-view meshes draw a vertex-clustered list at "
                   "this grid (0 = full detail).")
    .range(0, 256);
REXCVAR_DEFINE_BOOL(bd_host_cull, true, kCvarGroup,
                    "Host frustum test for the scene walk's default view "
                    "path, from the guest's own plane table.");
REXCVAR_DEFINE_BOOL(bd_host_materials, true, kCvarGroup,
                    "Substitute host material shaders for the guest's scene "
                    "pixel shaders (bd_normal_ps family).");
REXCVAR_DEFINE_BOOL(bd_frag_census, false, kCvarGroup,
                    "Count fragment shader invocations per guest pixel shader "
                    "(Vulkan pipeline statistics, desktop).");
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
REXCVAR_DEFINE_BOOL(bd_mv_redirect_srv, false, kCvarGroup,
                    "Point a multiview surface's sampled view at the resolved "
                    "companion rather than at the array (obsolete, needs "
                    "bd_mv_resolve).");

// Guest render-target textures get two layers under multiview, so the guest's
// EDRAM resolve has a layered destination instead of collapsing the stereo pair
// into a mono texture.
//
// Architecturally right and OFF by default, because with it on the right eye
// comes out black - present emits a side-by-side pair whose left half renders
// and whose right half is empty, which is worse than the mono-but-complete
// frame without it. The flatten and the resolve are correct; something writing
// these textures is not producing layer 1. Off until that is found, rather than
// reverted, because the destination genuinely has to be layered.
// On since 2026-09-02: with the per-eye MSAA resolve the pair survives into
// these textures and out to present - crossed stereo, verified on screen.
REXCVAR_DEFINE_BOOL(bd_mv_layered_textures, true, kCvarGroup,
                    "Give guest render-target textures two layers under "
                    "multiview so the EDRAM resolve keeps both eyes.");

// Diagnostic: present outputs |layer1 - layer0| amplified instead of the frame.
// Black means the surface present samples has identical layers, so the stereo
// pair was already flattened upstream and the present-side flatten is innocent.
REXCVAR_DEFINE_BOOL(bd_mv_debug_layer_diff, false, kCvarGroup,
                    "Present shows the difference between the two array layers "
                    "instead of the frame.");

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
                      "deepens the effect. Comfort varies, so this is a knob. "
                      "The multiview path needs far more than the side-by-side "
                      "one: 0.03 measures sub-pixel there, 0.2 gives crossed "
                      "far -2 / near -8.")
    // Was 0.2, which happened to be both the largest usable value and the only
    // one that works on the multiview path - so there was no headroom to tune
    // in, and the default looked mono. Why multiview wants an order of
    // magnitude more than side-by-side for the same clip-space constant is not
    // yet explained; see research/20260830_0700_multiview-has-depth.md.
    .range(0.0, 1.0);

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

// The sun shadow frustum fitted to the view on the host (gpu/shadow_fit.h):
// the guest's light box recentred and zoomed onto the camera frustum's near
// part, in the light's own clip space, so the map's texels land where the
// camera looks. Stage 5's first piece (2026-09-03).
// Host occlusion culling (gpu/occlusion_cull.h): a view-space cube proxy per
// node under an occlusion query at the end of the scene pass; a node whose
// proxy passed no sample two frames running is not drawn. Stage 8's second
// half, desktop first (2026-09-03).
// One line per queued scene draw into logs/draw_ledger.txt, to diff a frame
// with a hole against its neighbour (tools/capture_seq.py names the frame).
REXCVAR_DEFINE_BOOL(bd_draw_ledger, false, kCvarGroup,
                    "Write every queued scene draw to logs/draw_ledger.txt.");
// On since 2026-09-04. The walk notes each camera-view node's sphere, the
// scene pass draws the proxies under occlusion queries at its end, and a node
// whose proxy drew nothing last frame has its draws dropped by the queue. A
// village frame notes 84 nodes, reads 33 occluded and skips 30 draws; 120
// consecutive captures show no popping (0 neighbour jumps over 6%).
REXCVAR_DEFINE_BOOL(bd_occlusion_cull, true, kCvarGroup,
                    "Occlusion-cull scene nodes by last frame's proxy queries.");
REXCVAR_DEFINE_BOOL(bd_occlusion_diag, true, kCvarGroup,
                    "Log the occlusion culling counters every 300 frames.");
REXCVAR_DEFINE_BOOL(bd_shadow_fit, true, kCvarGroup,
                    "Fit the sun shadow frustum to the camera's view.");
REXCVAR_DEFINE_DOUBLE(bd_shadow_fit_distance, 500.0, kCvarGroup,
                      "How far along the view the fitted shadow frustum "
                      "reaches, in world units.")
    .range(10.0, 5000.0);
REXCVAR_DEFINE_BOOL(bd_shadow_fit_diag, false, kCvarGroup,
                    "Log the camera frustum's place in the light box.");
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
