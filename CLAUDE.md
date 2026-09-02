# CLAUDE.md

Guidance for Claude Code working in this repository. Rewritten 2026-09-01 from a 168 KB file
that had accreted eight contradictory "the bottleneck is X" sections. The trail those sections
recorded lives in `research/`; this file keeps only what is currently true, what is closed, and
how to measure. When a fact here is superseded, replace it and say when.

## THE RULE. Read this before anything else, and never break it.

**The most critical priority in this repository is rebuilding the foundation of Blue Dragon's
rendering - in the recompiled guest code as well as the host renderer - to use MODERN GPU and VR
techniques.** Multiview, fixed foveated rendering, occlusion culling, draw batching, instancing,
indirect draws, and the Vulkan features that carry them. The target is a better frame rate at
similar visual quality on a Quest 2.

This is a standing directive from the owner. It outranks every other piece of work in this file.

**Never do any of these:**

- Say a mandated technique is "not worth it", too expensive, or too risky to build.
- Benchmark one of them against the path it replaces, or present such a comparison as a reason.
- Quietly keep the old path because it measured better.
- Ask whether to proceed with one of them. Build it.

The techniques are settled industry practice. Whether they are a good idea is **not a question
this project asks**. The only questions are *where in the recompilation and the renderer does the
code have to change*, and *is the implementation correct*. Point measurement at those two.

This rule was broken once, on 2026-08-29: multiview was timed against side-by-side and reported
as slower. The comparison was forbidden, and it was invalid anyway (both paths were on at once,
so every triangle rasterised four times). Do not repeat it.

**Do not tune settings to buy frame rate at the cost of the image.** `bd_render_scale=25` hit
30 fps and was worn: "blurry gibberish". Readability first, then earn the frame rate back.

## What this repo is

A personal fork of [zolaware/reblue](https://github.com/zolaware/reblue), a static recompilation
of the Xbox 360 game *Blue Dragon*. The PowerPC XEX is translated ahead of time into C++ by the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) and compiled natively; there is no
interpreter and no JIT. Everything in `src/` is host code that implements the console's services
or reaches into the recompiled guest through hooks.

Fork goals, in the owner's priority order:

1. **VR, with 6DOF head tracking.** Third-person, first-person and diorama camera modes, plus a
   world-scale control. Movement stays on the controller; the head drives the camera.
2. **ARM64 Android** - Quest 2 natively. (An AYN Thor exists but is not a test target; see the
   Thor note below.)
3. **Cel shading on characters**, optional, from the in-game options menu.
4. **Tourist mode** - infinite HP, 999 stats, encounter suppression.

Owner decisions of 2026-09-02, binding until changed: **72 fps native on the Quest 2 at
1440x1584 per eye is the target**; the frame becomes host-owned in stages (see "The
direction"); multiview only; assets cooked once on the desktop and shipped, generated LODs,
impostors and merges allowed, 1.5 GB budget; materials carry a lighting-model slot so cel
shading is a switch; animation moves to the host after the walk; shadows on in the target
configuration, not measured apart; approximate visuals are fine; desktop first, one Quest run
per finished stage.

The fork is low-stakes and AI-driven. Prefer working, understandable changes over polish. No
support infrastructure (issue templates, changelogs) unless asked.

`docs/VR_PORT_PLAN.md` is the longer plan. Its header dates from 2026-08-30 and is behind this
file wherever they disagree.

### Layout

```
CMakeLists.txt          hand-edited; "rexglue migrate" will not touch it
cmake/                  shader compilation, codegen wiring, embedding, build info
config/                 the manifest's function map and per-feature hook tables (TOML)
generated/              the recompiled guest, a build product - never edit, never commit
docs/                   VR_PORT_PLAN.md
reblue_manifest.toml    entrypoint, hook includes, recompiler flags
research/               dated research notes - see the last section
src/                    all host code
thirdparty/             plume (render backend), XenosRecomp, implot, stb, zstd, miniz, ...
tools/                  the instruments; every one is named below
```

`src/` subsystems, each its own `bd::` namespace: `core/` (settings, logging, threading),
`gpu/` (the renderer), `engine/` (game logic reached through hooks), `platform/`, `vfs/`, `ui/`,
`audio/`, `installer/`, and `xr/` (VR: pose to per-eye matrices, camera modes, culling, session).

`src/xr/` splits deliberately: `xr_math.h`, `xr_camera.*`, `xr_cull.*`, `xr_settings.*` reach no
OpenXR header and take their settings pushed in, which is the only reason `tools/xr_math_test`
can exercise them. Keep it that way. Handedness is converted in exactly one place,
`FromOpenXRPose` in `xr_math.h`; OpenXR is right-handed -Z forward, the game is D3D9-era
left-handed +Z forward, and they differ by a mirror on Z.

## State of the port

Everything in this table was seen on a Quest 2 (Adreno 650) unless marked desktop.

| piece | state |
| --- | --- |
| SDK cross-built for android-arm64, APK, VFS over full game data, Vulkan, OpenXR session, Touch controllers as a pad, head pose driving the view | works |
| `bd_stereo` (side-by-side, submits every draw twice) | **works, correct crossed depth**, the shipping stereo route |
| `bd_stereo_multiview` (one submission, two-layer targets, array bindless heap) | **works, correct crossed stereo on the desktop (2026-09-02)**; on the Quest 59 ms GPU with the obsolete resolve chain on, 277 ms off - the tiling question |
| Field-scene frame rate | **20 fps** (3 slots at 60 Hz), 37.5 ms GPU, on side-by-side (2026-09-02); target 72 fps, see "The direction" |
| Character-anchored camera modes, diorama in battle | composed and unit-tested; tuning against a capture still wanted |
| Tourist mode | HP/MP top-up works (desktop); encounter suppression never fires (`bdPlayerField*` family is dead, see closed doors) |
| Post chain (bloom, depth of field) | **host-owned since 2026-09-02** (`gpu/post_chain.cpp`, `bd_host_post`): the guest's 15 tile-and-resolve quads a frame are replaced by host passes into the guest's own textures; image verified on desktop and Quest captures. The composites moved to the host next (one full-res pass); measure that once. |
| Cel shading | not started |
| Fixed foveated rendering | fragment density map on the app's own pass measured expensive and ineffective; `XR_FB_foveation` needs the scene in the XR swapchain - not started |
| Occlusion culling | distance cull only (`bd_cull_distance`), hooked in `bdSceneNodeCullTraverse` |
| Instancing / indirect draws | seams exist (`bd_draw_defer` queue, host `bdSceneNodeDrawSingle` in `gpu/hooks/scene_node.cpp`); stage 1 of "The direction" (the scene-walk recorder and host submission) is being built on them |
| Sun occlusion descriptor set on Adreno | dropped, not fixed (Adreno has 4 sets, the renderer wants 5) |
| AYN Thor (Adreno 740) | renders a field scene since the constant rewrite; **not a test target** |

## What is true now, measured. Quest 2, 2026-08-31 to 2026-09-02.

**THE SCENE PASS IS DRAW-BOUND (2026-09-02, 18:00).** Mono render-stage traces: 535 draws
render in 19.2-19.7 ms; 332 draws (`bd_cull_distance=20`) in 12.3-14.4 ms - ~36 us of GPU
per draw, and the count is what moves it. Nothing that reduces fragment work moved it: depth
ALWAYS, the depth prepass, the density map, position-only vertex shaders, opaque cutouts
(`bd_cutout_opaque`), host mip chains, the host post chain. The realtime counters' "99%
shading fragments, 6.6 per pixel" described where the GPU sat, not what it waited on. The cost
is the per-draw translation of the Xbox 360 draw ABI; see "The direction" below. **Shipping
side-by-side stereo: 37.5 ms GPU** (39.2 on 2026-09-01), 20 fps tier.

**`bd_stereo` is GPU-bound at ~39 ms with ~13 ms of CPU** (2026-09-01 evening; **34.4 ms
GPU after the 2026-09-02 work below, mono trace configuration**,
`verify_quest.sh` defaults: reflections and shadows off, cull 350, MSAA off, 60 Hz). The
field-scene mean is 50.0 ms (3 slots, 20 fps); stretches of the run sit at **33.3 ms, 30 fps**
whenever the GPU dips under the boundary. Read the `[xr]` log line, not `other_ms` (see the
instrument note below):

| | `bd_stereo`, morning | `bd_stereo`, evening | `bd_stereo_multiview` (2026-08-31, resolve chain on) |
| --- | --- | --- | --- |
| `dt_ms` | 50.3 | **50.0** (33.3 in lighter stretches) | 67.2 |
| `gpu_total_ms` | 38.2 | 39.2 | 60.8 |
| `[xr] xrWait` | 5-7 ms | 19-26 ms | 17-41 ms |
| `[xr] elsewhere` (CPU) | 43-45 ms | **12-14 ms** | 27-39 ms |

What moved the CPU from 44 to 13 ms in one day, both found with the per-thread profile:

- **The Adreno driver copies a descriptor set's contents on every bind with dynamic offsets.**
  The three per-draw guest constant ranges sat in the same set as the 4096-entry bindless
  texture array, so every draw copied the array: 56% of the render thread's samples in one
  driver `memcpy`. Shrinking the array to 1024 as a probe took that to 6%; **moving the
  constant ranges into the 256-entry sampler set** (bindings 0-2 of set 3, `SamplerDescriptor()`
  applies the shift, `shader_common.h` and every host shader moved with it) took it to 1.3%.
- The morning's 66.9 -> 50.3 ms came from reflections and shadows being *actually off* for
  the first time on a headset (the `args.txt` trap below). Those two passes are **CPU** levers on
  the side-by-side path, because every draw of theirs is submitted twice.

**`bd_stereo_multiview` on the Quest: 59 ms GPU, 13.5 ms CPU, 15 fps** with the resolve pass
on (its default again), and **277 ms** without it - see "Start here" item 1. Its CPU is now the
same as side-by-side's, so once its GPU comes down it is the faster path by construction.

**THE GPU IS NOT TILING (2026-09-02).** `tools/gpu_drawtrace_quest.sh` runs the headset's own
render-stage trace: every surface of a field frame executes in **`Mode: 0 (Direct)` with one bin
the size of the surface**, the scene pass at **24.5 ms** rendering straight to system memory.
The realtime metrics (`tools/gpu_metrics_quest.sh`) agree: 99% of GPU time shading fragments,
~6.6 fragments per scene pixel, ALUs 22% used, texture pipes 66% busy with 25% L1 misses,
shaders stalled 21%. That is why, measured within one run each, **depth ALWAYS, the depth
prepass and the fragment density map all changed nothing**: none of them touches a direct-mode
pass's cost. In-pass timestamps were one trigger (`bd_gpu_timing_segments`, now off on Android;
a second full-size pass bins with them off) and the scene pass is still direct, so the remaining
trigger is a property of that pass - under side-by-side it carries every draw twice. **Getting
the scene pass to bin is the GPU work, ahead of everything else.**

**The scene pass now begins with CLEAR on colour and depth (2026-09-02, 11:00).** A desktop
capture read with `tools/rdc_outline.py` (it prints each pass's load/store ops now) showed the
guest's clear landing as a zero-draw pass on a colour-only framebuffer and the scene pass
LOADing the target it had just cleared: the guest clears the scene colour, renders the shadow
and reflection passes, then binds colour+depth. Plume now holds a deferred clear per texture
across framebuffer switches (`HeldClear` in `plume_vulkan.cpp`) and flushes it only when a
barrier or copy touches the texture; the host keeps that target out of the speculative
SHADER_READ flip (`held_clear_rt`), flushes the draw queue before that flip, and materialises
resolves out of a depth surface before its clear. Quest: surfaces per frame 34 -> 30,
`gpu_total_ms` 38.5 -> 37.5, image verified on the desktop. `PLUME_FB_TRACE=<path>` writes
plume's framebuffer/pass/clear sequence with the host's barrier sites interleaved; it is how the
order was found, and the way to find the next one.

**The headset frame is composed at 1466x768, not the panel's 3664x1920 (2026-09-02, 12:50):
`gpu_total_ms` 37.5 -> 34.4, 1.1 ms above the 30 fps boundary.** The XR swapchain and the
offscreen present target took the window's size, the whole panel, for a 1376x720 game frame:
a 2.7 ms gamma upsample and a 0.6-0.8 ms copy every frame, resampled again by the compositor.
`bd_xr_present_scale` (0.4 of the window, aspect preserved) puts that pass at 2.0 ms including
preemption. The trap that hid it: the XR quad path creates the swapchain on the frame the
session turns Running, before any offscreen target exists, and the size is locked from then
on; both paths now size it the same way. Verified on a pulled 1466x768 capture (13:15): the
whole frame, correctly framed. Two bugs found on the way, both by looking at pixels: the
composite pass took its viewport from the window (the first capture was the top-left 40% of the
game, magnified), and the guest-surface capture site consumed the one-shot latch in mono so the
composite site never recorded.

**Two thirds of the texture data had no mip chain (2026-09-02, 16:20).** The mirror histogram
with texel totals: 316 textures ship a chain and hold 21.0 M texels; 196 ship none and hold
43.1 M, and they are the 1024x1024 and 2048x1024 world textures in DXT1/3/5. Every fragment of
those sampled the base level, which is what the texture pipes at 66% with 25% L1 misses were.
`gpu/host_mips.cpp` (`bd_host_mips`) builds the chain at upload: decode, box-filter, re-encode
with stb_dxt, punch-through and DXT3 alpha by hand. Image verified on the desktop and the
Quest; **the Quest frame did not move: 37.6 ms.** The chains are used (the sampler's LOD range
is unbounded), so the texture pipes' load is fragment volume, not cache misses from
minification. Keep the chains for the image; do not expect frame time from them.

**The post chain is host-owned (2026-09-02, 15:45)** and buys no Quest frame time: 37.6 ms
against 37.5 before. Its GPU share was small; the rewrite removed the tile-and-resolve
structure and fifteen guest draws. The frame is the scene pass.

**What is left on the CPU, per thread** (`out/device/profile_setmove.txt`):

- **Five threads at 55-75% of a core each are our own PSO precache workers** running the
  Adreno shader compiler (75% of their samples in `libllvm-qgl.so`): `pso_predictor` expands
  every technique into cores x cull x spec x skinning, the desktop compiles the 5,000+ in
  twenty seconds and Adreno takes minutes. `bd_pso_precache=false` measured an identical frame
  (50.1 ms, GPU 39.1, CPU 16.5) with 41 lazy compiles, so it is not a lever either way.
  `bd_thread_policy` counts these as guest workers and pins them to the little cores.
- **The thread at 100% of a big core (`SDLThread`) is SDL's event pump spinning** -
  `SDL_PumpEvents`, joystick detection, `clock_gettime`, mutex churn - not guest code. The SDK's
  `RunMainMessageLoop` calls `SDL_WaitEvent`, which on Android should block; something keeps it
  awake and it has not been identified.
- The audio worker sits at **75-78%** of a core on its own.

**The frame rate is quantised.** The compositor paces to whole refresh intervals: at 60 Hz a
frame costs `ceil(work / 16.67) * 16.67`. 66.7 ms is 15 fps, 50 ms is 20, 33.3 ms is 30. A
saving that does not cross a boundary is worth zero fps and is still real. Quote the milliseconds
and the distance to the next boundary, never fps.

### Instrument semantics that have produced wrong conclusions

- **`other_ms` in the perf CSV includes `xrWaitFrame`.** It is `dt - (fence + submit + drain +
  pace)`, and the compositor wait is not subtracted. A GPU-bound frame therefore reads as 66 ms of
  "CPU" too, because the GPU cost is absorbed by `xrWaitFrame` and the fence reads zero. This is
  how "the frame is CPU-bound on both paths" got written into this file on 2026-08-31. **The
  `[xr] ... frame X = xrWait A + ... + elsewhere B` line in the log separates them**; `elsewhere`
  is the CPU.
- **A near-zero `fence_ms` does not mean the GPU is idle.** It means the GPU was not the last
  thing waited on.
- **`args.txt` booleans were inverted until 2026-09-01.** The cvar parser is CLI11 and registers
  every boolean as a *flag*: `--bd_reflections` on one line means true, and the `false` on the
  next line was an ignored positional. So **every `args.txt`-driven run that set a boolean to
  false ran with it true** - `bd_reflections=false`, `bd_shadows=false`,
  `bd_stereo_multiview=false`, all of them, for as long as `tools/bench_quest.py`,
  `tools/verify_quest.sh` and `tools/stereo_check.py` have existed. `bd::AuditProfileConfig`
  caught it: `'bd_stereo_multiview' did not take effect: file says 'false', live value is
  'true'`. The activity now joins `--name` and its value line into `--name=value`, which CLI11
  parses correctly. Any older device number whose configuration turned a boolean off through
  `args.txt` is suspect; the ones that used the profile TOML are fine.
- **A value option given twice in `args.txt` drops the whole file** (CLI11's multi-option
  policy throws, and the activity's "args.txt added N" line never prints). A run passing
  `bd_cull_distance=20` over `verify_quest.sh`'s default 350 started with every setting at its
  default; the `[config]` audit named all 11 at once, which is the tell. The script dedupes
  keys now (caller wins, 2026-09-02); anything else that writes `args.txt` must too.
- **The sampling profiler's ring is the last 16 seconds** (65536 samples, four threads, 1 kHz),
  and it used to dump every 600 frames, so the profile on disk described whatever the run was
  doing then - on 2026-09-01 a transition: 78% of samples in libc `syscall`/`nanosleep`, 18% in
  the Adreno shader compiler, 0.4% in `libreblue.so`. It now dumps once at
  `bd_capture_after_s` and stops, so the profile and the capture describe the same moment.
- **`tools/stereo_check.py` has been confidently wrong four times**, each time on an image that
  was not a stereo pair (a composited panel, a misdecoded array, a mono frame twice). Look at the
  capture before believing the verdict. Only a `--stacked` grab from `bd_mv_capture_array` gives a
  stereo verdict on device.
- **Draw counts are not costs.** "715 PSO switches" counted guest state changes, not pipeline
  binds (opaque draws take 14). "`bdSceneNodeDrawSingle` is 23x the next consumer" was a call
  count; it profiles at ~5%. "Most draws are blended full-screen passes" reasoned from counts
  while the measured time was 81% in the scene pass. This mistake has been made four times.

## The direction (owner, 2026-09-02 evening): a host-owned frame

The last trace of the day settled where the frame goes: **the scene pass is draw-bound, not
fragment-bound.** 535 draws render in 19.5 ms and 332 draws in 12.3-14.4 ms - about 36 us of
GPU per draw - and every fragment-side change (depth ALWAYS, the prepass, the density map,
position-only vertex shaders, opaque cutouts, host mip chains, the host post chain) moved
nothing. The cost is the per-draw translation of the Xbox 360 draw ABI: a descriptor set with
three dynamic offsets, up to sixteen vertex streams, viewport, scissor and pipeline for each
node's draw. The owner's answer is to stop translating and own the frame:

> "once everything is moved to host, we can much easier see how to boost super performance
> ... alter rendering pipelines and assets to do clever things like billboarding etc."

**Target: 72 fps native on the Quest 2 at the runtime's 1440x1584 per eye**, multiview only.
Approximate visuals are fine. Desktop first; one Quest run with a capture per finished stage.
Stages, each shipping working:

1. **Record the guest's scene walk.** A recorder on the `bdSceneNodeDrawSingle` seam writes
   what each node draw is (mesh, material pipeline, textures, transform, bone palette) and
   which guest structures it came from. This is how the host learns the scene tree; the host
   then submits the recorded list itself - sorted by pipeline, instanced where mesh and
   material repeat, streams bound once per mesh, descriptor sets once per frame, a push
   constant carrying each draw's constant base. Removes the 36 us per draw.
2. **The host walks the tree.** The host reads the guest's scene tree data directly and
   decides what to draw: its own culling, LOD, instancing. The guest's walk is switched off.
3. **Assets cooked once.** A desktop playthrough with the recorder fills a host cache (meshes
   in host vertex/index buffers, textures with chains, materials as pipelines with a
   lighting-model slot: the guest look, and cel), shipped with the game data; unvisited areas
   convert live and add themselves. Generated assets are allowed: decimated LODs, impostor
   billboards, merged static geometry, atlases. Budget 1.5 GB on the headset.
4. **The host owns the targets.** Scene colour and depth, shadow map, post textures, present;
   the EDRAM resolve emulation (tile aliasing, seed copies, deferred resolves) goes.
5. **Shadows on the host** from the host scene list into a small map fitted to the view; the
   guest's 4096x4096 second geometry pass and stencil pass are skipped. Shadows are on in the
   target configuration and are not measured as a separate cost.
6. **Animation on the host** (bone palettes evaluated by the host, skinning on the GPU).
7. **Multiview and foveation on the host frame**, rendered at per-eye resolution straight
   into the runtime's swapchain (`XR_FB_foveation`).
8. **Occlusion culling and indirect draws** on the host list; billboarding at distance.

Effects and particles stay on the old per-draw path until the host frame is stable.

Parked, for the record: the Adreno render-mode hunt (eighteen probes, nothing the app
controls moved the scene pass off `Direct`; foveation on the app's own pass depended on it -
stage 7 renders into the runtime's swapchain instead), multiview's 277 ms without the resolve
chain (not shipped), and `bd_mv_half_width` (62.7 -> 41.7 ms, present chain does not follow;
superseded by stage 7).

## Multiview: where it is and what is left

The array bindless heap landed 2026-08-31 (`Texture2DArray`, every 2D read carries
`SV_ViewID` as the layer, pixel shaders at `ps_6_1`). Multiview present went from black to
95.7% non-black, and the five full-resolution resolve passes that flattened the pair - 79.5 MB
of tile traffic a frame - are no longer needed for correctness. They are still **on by default**
(`bd_mv_resolve`), because on the Quest their absence makes every pass ~4.5x slower for a reason
not yet named (item 1 above); `bd_mv_redirect_srv` is off, so nothing samples the companion.

**The remaining bug: present shows two identical halves while the scene array holds correct
stereo** (`far -4, near -26`). Everything upstream is eliminated and written down in
`research/20260831_1700_the-array-heap-fixed-multiview.md`: the guest's EDRAM resolve is per-eye
(colour and depth), every host pipeline carries a view mask, plume's copy and view paths are
right, the present sample site is verified against the live image. RenderDoc then counted **24
single-layer views over two-layer full-resolution images**, and the reading that matched them:

`D3DDevice_CreateTexture_hook` set `texture->layers` *after* `BindTextureSRV`, and
`BindTextureSRV` discards a view whose `textureViewOf` is unset and rebuilds it with
`arraySize = layers`, which read 0 - so every two-layer resolve destination got a one-layer
sampling view. That was a real bug and is fixed (2026-09-01, `gpu/hooks/resource.cpp`), **and it
was not the cause**: the desktop still presents two byte-identical halves with it in, with the
obsolete slice views gone, and with `bd_mv_layered_textures=true`.

**FIXED on 2026-09-02, verified in a capture.** The MSAA resolve now reads the multisampled
scene through one single-slice view per eye and picks the slice by `SV_ViewID`
(`resolve.cpp`, `resolve_msaa_color/depth.hlsli` at `ps_6_1`); the readback shows the pair
surviving that draw (72.7% of bytes differ) and reaching the present-side targets (61%). The
day of "black" results before that came from `bd_mv_resolve` defaulting on again for the Quest's
277 ms; it is Android-only now. The paragraphs below are how it was found.

**Found, by reading pixels.** `tools/rdc_layer_diff.py` (run as
`RDC_CAPTURE=<abs path> qrenderdoc.exe --python tools/rdc_layer_diff.py`; report in
`~/rdc_layer_diff.log`) reads both array layers of every two-layer target back after every pass.
The scene passes hold a pair; the very next draw - the host's EDRAM resolve into a two-layer
guest texture - outputs the scene's **layer 0 into both layers** (96.7% match to layer 0, 27% to
layer 1), under a multiview pass, through a two-slice view. On the desktop the scene is MSAA 4x
by default, so that draw is `resolve_msaa_color`, whose heap is `Texture2DMS`: **a type with no
layer axis**, so both views load layer 0. `copy_color_ps` (the non-MSAA path the Quest takes at
`bd_msaa=0`) was already per-eye, which is why three sessions of fixing views and descriptors
changed nothing on the desktop.

**The obvious fix does not work yet.** `Texture2DMSArray` + `SV_ViewID` compiles and creates its
pipeline, and the frame goes 0% non-black - a probe returning `SV_ViewID` as colour is black too,
so that draw writes nothing under that shader. Reverted. Next: capture the black draw, or give
the MSAA resolve per-layer slice views of the multisampled target (the `layerView` mechanism in
`multiview_resolve.cpp`) and keep `Texture2DMS`. See the 2026-09-01 note.

Two settings are still needed together for a multiview run, and the log says whether they took:

```
[config] all N settings in reblue.toml took effect
[mv] present rt=... layers=2
2-layer targets: 39
```

`layers=1` or `0` two-layer targets means the run is not testing multiview.

**One known spec violation in the descriptor layout**, reported by the validation layer as
`VUID-VkDescriptorSetLayoutCreateInfo-descriptorType-03001`: a set with an update-after-bind
binding may not also hold dynamic uniform buffers, and the sampler set does (the texture set did
before, identically). Adreno's four-set limit forces the sharing; the driver accepts it. The
honest fix is collapsing HLSL spaces 0/1/2 into one physical set with three bindings, which
frees a slot for a constants-only set and for the dropped sun-occlusion set at once.

`bd_mv_layered_textures` (two-layer guest render-target textures, so the EDRAM resolve has a
layered destination) is architecturally required and defaults off only until the fix above is
verified on the desktop; then it becomes the default.

## Closed doors. Do not retry these.

Each was measured or proven; the note that closed it is in `research/`.

- **`REBLUE_RELAXED_GUEST_MEMORY`** hangs the game on ARM64 and measured 0% on x86.
- **`non_argument_as_local`** codegen flag miscompiles the guest's IO path (fatal file load 0.2 s
  after VFS mount). The 36% reduction in `ctx.` accesses was disconnected from correctness.
- **Host code replacing `bdSetSamplerState`** measured +1.4% and +3.1% slower in within-run A/Bs.
  The guest already early-outs on 100% of calls; a recompiled prologue costs about what a cvar
  read and a bounds-checked load cost.
- **Deduplicating render or sampler state**: the guest already does it.
- **Detail culling** (`bd_cull_min_pixels`): 0 nodes; the 350-unit distance cull already rejects
  95% and the two overlap completely.
- **Draw sorting** (`bd_draw_sort`): fires, correct, buys nothing while LRZ is invalidated by
  blended depth-writing draws. Pipeline-major order is actively wrong on a tiler.
- **`bd_blend_no_depth_write`**: replaces cliffs with flat grey; DoF and fog sample depth.
- **The alpha-test `OpKill` theory for LRZ**: `bd_debug_no_alpha_test` moved 56.18 -> 56.12 ms.
- **Fragment density map on the app's own render pass**: +17 ms and barely reduced shading.
- **`SSCALED` vertex formats**: Adreno does not expose them. Bind `SNORM` and multiply by 32767.
- **Hoisting HLSL constant `#define`s to entry statics**: DXC already CSEs them; +0.2% SPIR-V.
- **`bd_host_sincos`**: two-run pairs said -33% to -45%; the within-run A/B said +2.9%.
- **`bd_pso_precache` on the Quest**: off measured an identical frame. The five background
  compiler threads it runs for minutes are not what the frame waits on.
- **`bd_shadows`, `bd_reflections` as fill levers**: measured "not a lever" - but through
  `args.txt` booleans, so the measurement is void (see the parser trap above). Unmeasured.
- **The `bdPlayerField*` family** (`bdPlayerFieldMovementUpdate`, `bdPlayerFieldCheckEncounter`)
  never runs; an unconditional entry counter read zero over a 200 s walk. Hook something else.
- **SteamVR null driver, the Oculus runtime without a headset, Meta XR Simulator**: none can run
  on this machine. `tools/xrsim/` replaced them.
- **`adb screencap`, Quest screenshot intents**: cannot see compositor layers. Use
  `bd_capture_after_s`.
- **`simpleperf`**: Horizon OS refuses it regardless of `profileable`. Use `bd_sample_profiler`.
- **`gpu_busy_percentage`** reads 99% with the app force-stopped.
- **Mesa Turnip on the headset path** (`bd_vulkan_icd=turnip`): the Meta runtime dispatches and
  validates every handle-only OpenXR call through Android's platform loader, so an ICD we load
  ourselves reaches the VkInstance and no further (`XR_ERROR_HANDLE_INVALID` from
  `xrCreateVulkanDeviceKHR`, 2026-09-02). Only a forwarding layer under the platform loader could
  carry it, and the flat path cannot host it either.
- **The AYN Thor** as a test device. It is a different GPU with a stricter driver; a Thor result
  says nothing about the Quest, and the owner has said Quest only.

Two things that look expensive and are not: FPSCR flush-mode switching (cached compare) and
indirect dispatch (range check plus table lookup).

## The instruments, and the trap each one carries

**One command for the device: `bash tools/verify_quest.sh "k=v,k=v"`.** Installs the staged
APK, writes the settings to *both* `args.txt` and the profile TOML, runs autoplay 170 s, and
pulls the frame breakdown, the per-frame CSV, the thread split at 150 s, the profile pinned to
the capture, and the capture itself into `out/device/`. It prints the `[config]` audit lines:
read them first in any run that surprises you. It force-stops, clears artefacts before the run,
and shouts on a crash - because a run that died 33 s in once handed back the previous day's
files as that day's result. **Never run two device measurements at once.**

- **`python tools/perf_summary.py out/device/perf.csv`** selects field frames by draw count
  (>= 300; a menu is ~20) instead of averaging a file whose tail is a menu. It reports CPU and
  GPU per arm of a within-run A/B.
- **Within-run A/B, never two runs.** Across restarts the same configuration drifts 30-70%.
  `bd_ab_flag = "bd_some_bool"` (quoted) plus `bd_ab_period` flips the cvar during one run and
  labels each frame; unquoted, the value is dropped and every frame reads `ab_arm=255`.
- **`bd_capture_after_s`** writes the composited frame as raw RGBA with a one-line header to
  `logs/capture/`. `bd_capture_min_draws` holds it until a frame has that many draws, which is
  the only way to catch a field scene rather than a menu: autoplay lands somewhere different
  every run. `bd_mv_capture_array` photographs the largest colour+depth target's two layers
  stacked; `tools/stereo_check.py --stacked` reads that, `--raw` reads a side-by-side capture.
- **`bd_sample_profiler`** samples the guest main, worker and render threads at 1 kHz with
  `SIGPROF` (or `SuspendThread` on Windows), dumps at the capture moment, and
  `tools/symbolize_profile.py` names the `libreblue.so` PCs from the unstripped build.
- **`bd_renderdoc` + `bd_renderdoc_after_s`** capture headlessly from inside the app;
  `renderdoccmd convert` then `python tools/rdc_outline.py` prints one line per render pass with
  its view mask. Read the view mask, not the layer count. It named the last two multiview bugs
  after inference had failed.
- **`python tools/spv_caps.py <hlsl_dump> --require-absent Int64`** decodes every
  `OpCapability` in the compiled shaders. Name the preset when you quote it; the win-amd64 dump
  is the DXIL variant and always reads clean.
- **`python tools/callgraph.py callers|tree|subtree|hot <fn>`** over all 18,777 recompiled
  functions, seven seconds, cached.
- **`bd_guest_census`** counts calls into named guest functions per frame; **`[perf]`** prints
  the per-target census with measured GPU ms per render target.
- **`bash tools/validate_quest.sh`** runs the Khronos validation layer on device and turns it
  off again. Khronos ships no Windows binaries.
- **`tools/xr_math_test/`** compiles `xr_math.h` standalone. It has caught two sign bugs.
- **`tools/shot_window.ps1`** photographs the reblue window by process. If reblue is not
  running it photographs something else and returns a near-white image; check the dimensions.

**Verify the pixels, not a proxy.** A log line is not a feature. A capture that has been looked
at, or a number read off a run, is.

## The desktop loop is the first stop, and the VR path runs on it

A desktop run is ~90 s, needs no headset, reaches a real field scene under `bd_xr_autoplay`,
prints the same census, and captures at full resolution. Setup, once:

- The registry record must name **the directory holding the exe**: `HKCU\Software\Zolaware\reblue\Install`,
  `InstallRoot` and `SchemaVersion = 3`. Anywhere else raises a modal with no log line, which
  looks exactly like a hang. Game data at `<InstallRoot>/game` (a junction is fine).
- Cvars go in `<InstallRoot>/profiles/default/reblue.toml`, flat TOML. Command-line flags do not
  work. **One syntax error discards the whole file**; the `[config]` audit says so.
- **The window must be foregrounded** or autoplay never leaves the title screen.
- Read `other_ms` from the CSV, not `dt_ms`: the desktop is vsync-locked at 16.67.

**`tools/xrsim/` is our own headless OpenXR runtime** - two views, real swapchain images, a pose
that is a function of frame index. It is the only way to exercise the camera modes and the
character anchor without a headset. Invoke the `vrsim` skill. Three traps: do not minimise the
window, the manifest's `library_path` must be absolute, and a missing entry point crashes at
PC 0. The check: `[xr] cam:` must show `eye` differing from `game`.

## Build and dev loop

**Invoke the `devloop` skill** before building, running or deploying. The essentials:

```sh
export PATH="/c/Program Files/LLVM/bin:$PATH"; export VCPKG_ROOT="C:/vcpkg"
cmake --build --preset win-amd64-release --target reblue      # desktop; output is reblue_vk.exe
cmake --build --preset android-arm64-release --target reblue && bash tools/build_apk.sh
```

- **Never rebuild the guest to test host code.** `reblue_recomp` and `reblue_generated` are
  separate object libraries; if they rebuild, a codegen input changed. Never wipe the build
  directory. Build one target.
- Loop lengths: a cvar experiment is ~30 s and builds nothing; a `src/` change is ~10 s compile,
  ~15 s package, ~5 s install, ~30 s to the title; a codegen flag change rebuilds 54 TUs.
- **A XenosRecomp change needs two manual steps** or it never reaches the device: rebuild
  `out/host-xenosrecomp`, delete `generated/shader_cache.cpp` in the build tree, rebuild. Verify
  in the emitted HLSL (`--target reblue_shader_hlsl_dump`), never in the source.
- **Neither an SDK header nor an SDK codegen change reaches the guest on its own**: delete the
  `reblue_recomp` objects by hand, and run the prebuilt `rexglue codegen` manually. Verify in
  `generated/`.
- Regenerating `reblue_pch.h` does not invalidate the PCH; delete `cmake_pch.hxx.pch`.
- `rexglue_DIR` caches to whichever slice was configured last; an Android slice has no Windows
  import libraries and the symptom is `IMPORTED_IMPLIB not set`.
- **Git Bash rewrites Unix-looking arguments into Windows paths.** `MSYS_NO_PATHCONV=1` on every
  adb call, Windows-style paths for anything local, and print every push result. Three silent
  failures have each cost hours.
- **Never `adb uninstall`** (deletes the game data). **`kRequiresRestart` cvars need a
  `force-stop`.** An APK missing `libopenxr_loader.so` installs, dies in `dlopen` before
  `main()`, writes no log, and the newest log on device is the previous run's.
- The proximity sensor suspends an unworn headset; `verify_quest.sh` broadcasts
  `com.oculus.vrpowermanager.prox_close` throughout.
- Game data: `tools/extract_game_data.py --all --skip-media`, straight off an adb-connected
  device. A full install mounts **1673 archives / 119346 record names**; the manifest-only path
  mounts 1274 / 70008 and dies at "new game". Push directories, never files in a loop.

## Reading and patching the guest

**Do not install a decompiler.** `generated/` holds the whole XEX as C++ with the PowerPC
interleaved as comments and every `config/functions.toml` name applied. `ctx.r3` is the first
argument. **Invoke the `guest-source` skill** before looking for anything in the binary.

Two patch mechanisms, and it matters which one a change belongs in:

1. **Function replacement.** `config/functions.toml` names guest addresses; a host
   `REX_HOOK(name, fn)` / `REX_HOOK_RAW` is a strong definition that overrides the weak alias the
   recompiler emits. `D3DDevice_SetTexture` and `bdSceneNodeDrawSingle` are replaced this way and
   verified live. The `duplicate symbol` failure on `Visual__DrawVerticesUP` is a property of that
   symbol, not the mechanism.
2. **Midasm hooks.** `config/hooks/*.toml` places a callback at an instruction address with
   named registers, optionally redirecting control flow. Read the TOML before the hook body; its
   address comments are the only map of the binary. `REX_HOOK*` symbols live in `OBJECT`
   libraries, never `STATIC` archives.

**Rewriting the recompilation is in scope and is the point.** The guest's rendering ABI is a
Xenon's - big-endian structs, fetch constants, per-node draw submission through
`bdSceneNodeDrawSingle` (1,935 guest instructions, ~370 guest memory ops per node, 2084 calls a
frame). Replacing whole rendering paths with host code that thinks in modern terms is permitted.
The seam is already host code; build instancing and indirect draws on it, and check a capture
after each step rather than attempting the whole function in one leap.

The Xenos shaders are translated by `XenosRecomp` to HLSL, then DXC to SPIR-V with real spec
constants; Vulkan needs no runtime shader compiler. Guest constants are three dynamic uniform
buffers in one 64 MiB buffer bound once (`CONSTANT_BUFFER_DYNAMIC` in plume), re-based per draw.
That change alone took the Quest from 6.3 to 15 fps and removed `OpCapability Int64`.

## Forked dependencies

Work on `main` in every fork; push changes there, do not add patch files.

| submodule | fork | branch | carries |
| --- | --- | --- | --- |
| `thirdparty/plume` | `noeldvictor/plume` | `main` | `VulkanInterfaceOptions` for OpenXR, multiview view masks, dynamic UBOs, `TEXTURE_2D_ARRAY` views, `discardTexture`, layer-correct `copyTexture` |
| `thirdparty/XenosRecomp` | `noeldvictor/XenosRecomp` | **`reblue`** | `SV_ViewID` on vertex *and* pixel shaders, the array bindless heap, the per-eye skew, UBO constants, `vs_6_1`/`ps_6_1` |
| ReXGlue SDK (cloned to `out/rexglue-src`, not a submodule) | `noeldvictor/rexglue-sdk` | `android-arm64` | the Android port: fibers via libucontext, `ASharedMemory_create`, activity harness, logcat |

XenosRecomp's `main` is the upstream history and `reblue` is unrelated to it; moving the work
onto `main` means a force-push and has not been done. **Never commit `thirdparty/libmspack`**
(symlinks check out as text on Windows). XenosRecomp is the *shader* recompiler; XenonRecomp is
a different project and nothing here uses it.

## Conventions

No `.clang-format`; match the surrounding file. Headers open with a Doxygen block (`@file`,
`@brief`, `@copyright`, `@license`); keep existing copyright lines. `PascalCase` functions and
types, `snake_case` locals, trailing underscore for private members, `k`-prefixed constants.
Namespaces close with a trailing comment. C++23, no extensions; integer aliases from
`rex/types.h`. Guest memory is big-endian: every read of a guest structure swaps, with
`__builtin_bswap*`.

**Comments explain why, not what**, and in this repo the "why" is usually a measurement or a
guest-side reason a workaround exists. Record the number and the date.

Never report a build success that did not happen, and never leave a "this does not work" claim
in place without a date and a reason.

## Research notes

Findings go in `research/` as `YYYYMMDD_HHMM_<slug>.md`, dated at the time of writing, sources
linked at the end. They are a log, not a wiki: write a new file rather than editing an old one, so
what was believed when a decision was made stays legible. When research changes the plan, say so
in the note and update this file's "What is true now" and "Start here" sections. Superseded
sections do not stay in this file; they are already in the notes.
