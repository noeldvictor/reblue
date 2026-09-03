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
| `bd_stereo_multiview` (one submission, two-layer targets, array bindless heap) | **works, correct crossed stereo on the desktop (2026-09-02)**; on the Quest **23.5 ms GPU, 24.3 ms CPU, 379 draws (2026-09-02 22:40, resolve chain on, after the per-draw cut)** against 59 ms in the morning; the host post chain does not run on two-layer targets yet (`BeginGuestTarget` refuses layers != 1), so the guest's fifteen post quads and the five resolve passes are still in that frame, and the capture came back black (the resolved-companion capture site; unverified on the panel). **Fixed on the desktop (23:50-00:10)**: the host post chain runs on two-layer targets (exposure, dof, bloom right), the shadow pass gets multiview pipelines (shadow in both eyes). Quest (00:15-00:30): **23.0 ms GPU with the resolve chain on and 23.0 with it off** (`bd_mv_resolve=false` no longer costs 277 ms - the per-bind descriptor copy was that), stacked capture non-black on both layers, disparity 2 px in a blurred close-up (stereo not confirmed on device). Each layer is a full 1376x720, twice the pixels per eye of side-by-side, which is where the 9 ms over side-by-side sit. **`bd_mv_half_width` (each layer half the guest width, the same pixels per eye as side-by-side) renders a correct pair on the desktop with the layered post chain (03:00, `out/shot_mv_half.png`) and is the Android default now; the old "present chain does not follow" is gone.** **Quest, 2026-09-03: half width read 39.8 ms** because only the scene surface was halved and the guest resolved each layer back up to a 1376-wide chain (six eager resolves); **halving `Output::LatchedFit` instead puts the whole guest chain at 680x720 a layer: 24.6 ms GPU p50, resolves back to two, stacked capture a correct crossed pair on device** (run 7, `research/20260903_0950_...md`). Side-by-side in the same kind of scene: 20.8. The gap is the scene pass: 14.6 ms of render for the two 688x720 layers against 12-13 for side-by-side's 1376x720 |
| Field-scene frame rate | **60 fps, vsync-locked** (98% of field frames in one 60 Hz slot, `gpu_total_ms` 15.8, `other_ms` 16.3, 2026-09-02 21:05) on side-by-side with shadows and reflections off, in a lighter scene than the 2026-09-01 one (20 fps then); target 72 fps, see "The direction" |
| Character-anchored camera modes, diorama in battle | composed and unit-tested; tuning against a capture still wanted |
| Tourist mode | HP/MP top-up works (desktop); encounter suppression never fires (`bdPlayerField*` family is dead, see closed doors) |
| Post chain (bloom, depth of field) | **host-owned since 2026-09-02** (`gpu/post_chain.cpp`, `bd_host_post`): the guest's 15 tile-and-resolve quads a frame are replaced by host passes into the guest's own textures; image verified on desktop and Quest captures. The composites moved to the host next (one full-res pass); measure that once. |
| Cel shading | **on the characters (2026-09-03, 00:50)**: `bd_cel_characters` sets a spec constant (`SPEC_CONSTANT_CEL`, XenosRecomp) on every skinned draw, and the recompiled pixel shader bands its lit colour before export; the world is untouched (desktop screenshot `out/shot_cel.png`). Four bands, no outline yet. **In the options menu (01:55)**: a "Cel Shading (Characters)" on/off row on the Graphics page (`settings_rows.cpp`, label in `res/embed/localization.toml`) bound to the cvar; compiled and booted, the row itself not yet looked at (autoplay never opens the menu) |
| Fixed foveated rendering | fragment density map on the app's own pass measured expensive and ineffective; `XR_FB_foveation` needs the scene in the XR swapchain - not started |
| Occlusion culling | distance cull only (`bd_cull_distance`); the walk itself is host code since 2026-09-02 (`gpu/scene/host_walk.cpp`, `bd_host_walk`), so a host cull attaches there |
| Instancing / indirect draws | **instancing on the deferred queue (2026-09-02)**: every guest vertex shader has an instanced twin reading its whole constant block from an `InstanceRecord`, the queue merges consecutive equal draws (`bd_draw_instancing`, with `bd_draw_instancing_reorder`); **Quest: -8 ms GPU for the pair**, singles stay on the plain pipeline. Indirect draws not started |
| Host-issued node draws (`bd_host_draw`) | **village: 580 of 659 node runs a frame are the host's, 43 templates volatile (2026-09-03, 02:40)** - the 36 texture-volatile ones are real: a slot the interpreter sets in one run and not in another holds whatever the previous node bound, so merging slot sets or reading the texture from the visual painted the rock with the reflection map (reverted, `6911572` undone): draws replayed from templates, skinned ones included, and **the render-list entries built by the host** (`bd_host_list_build`: 306 entries in 217 runs a frame from recorded entry images, the guest's own allocator, a fresh matrix and palette; the matrix source verified on 52,455 runs). Every draw of the scene pass carries a node tag (`config/hooks/render_list.toml`). Side-by-side runs on the Quest; the list build is desktop-verified only (headset offline): a desktop within-run A/B (`bd_ab_flag="bd_host_list_build"`, 03:30) reads **-8.8% CPU per draw** with it on |
| Sun occlusion descriptor set on Adreno | **back (2026-09-02)**: the layout is four real sets now, see the note in `bindless_allocator.h` |
| AYN Thor (Adreno 740) | renders a field scene since the constant rewrite; **not a test target** |

## What is true now, measured. Quest 2, 2026-08-31 to 2026-09-03.

**THE SCENE PASS IS BOUND BY FRAGMENTS x TEXTURE FETCHES (2026-09-03, 09:50).** Nine device
runs, `research/20260903_0950_device-qualification-...md`. The head build qualified:
host-built render list live (63 entries in 39 runs a frame, matrix check 5,397 ok / 0 off),
stereo crossed and correct in every capture, multiview half width a correct pair on device.
Autoplay lands somewhere different each run - a rock close-up read 20.8 ms GPU on
side-by-side where last night's village read 13.2 - so only within-run A/Bs and same-build
traces are compared. What those say: the two-layer 688x720 scene pass renders in **14.6 ms**
(plus 4.5 of compositor preemption) against 12-13 for side-by-side's 1376x720, the same pixels
per eye and twice the draws, so multiview's one-submission saving is invisible; the counters
read 99% fragments, **texture pipes 72% busy, ALUs 21% used, 2.27 fetches per fragment,
1.26 G fragments a second**; the depth prepass A/B costs **+22.6% GPU** (every draw twice,
nothing rejected worth having), so the fragments are not hidden ones; and `bd_debug_mip_bias=6`
blurs the Quest exactly like the desktop **without moving the frame (20.2 vs 20.8)**, so the
mip chains are sampled on the device and cache misses are not the cost. The "0.9% non-base
fetches" counter, read as "no mips" on 2026-09-02, does not mean that. The cost is the fetch
count, at the texture units' throughput. Levers: the number of fragments the visible layers
produce (the render list's sorted and translucent materials, foliage cards, fog and glow quads
- `bd_debug_skip_list_draws` / `bd_debug_skip_blended` A/Bs measure their share), fetches per
fragment, and foveation (stage 7). 1440x1584 a layer is 4.7x today's pixels at this cost.

**THE SCENE PASS IS DRAW-BOUND (2026-09-02, 18:00).** Mono render-stage traces: 535 draws
render in 19.2-19.7 ms; 332 draws (`bd_cull_distance=20`) in 12.3-14.4 ms - ~36 us of GPU
per draw, and the count is what moves it. Nothing that reduces fragment work moved it: depth
ALWAYS, the depth prepass, the density map, position-only vertex shaders, opaque cutouts
(`bd_cutout_opaque`), host mip chains, the host post chain. The realtime counters' "99%
shading fragments, 6.6 per pixel" described where the GPU sat, not what it waited on. The cost
is the per-draw translation of the Xbox 360 draw ABI; see "The direction" below. **Shipping
side-by-side stereo: 37.5 ms GPU** (39.2 on 2026-09-01), 20 fps tier.

**THE QUEST NUMBERS FOR THE DAY'S WORK (2026-09-02 night, verify defaults, side-by-side).**
Normal build (instancing and its reorder on, host walk, host draw): **45-47 ms GPU, 18-22 ms
CPU**, 20 fps tier; yesterday's build read 37.5 / 13 in the same configuration. Within-run A/Bs
settled what each switch is worth: **instancing plus its reorder is -8 ms** (52.9 -> 44.9;
either alone is nothing, because without the reorder no repeated draw is consecutive);
the host-issued draw is flat on the GPU (52.7 vs 53.0) and the host walk is flat (46.6 vs
47.2); a probe build with no instancing machinery in the shaders reads 52.3, the same as
"reorder off", so that machinery is free in the plain variant. Every scene draw through the
instanced variant (storage-buffer constant reads) put the scene pass at 28.0 ms - the
record path is for real groups only now. The CPU went 27 -> 21 ms once the instanced twin
stopped compiling on the render thread (`FindPipeline`, never `GetOrCreate` there). **The
remaining 8-10 ms against yesterday is in none of the runtime switches; the untested change
is the descriptor layout (step A), which needs a build probe.** Cross-run drift stayed at
5-8 ms; only the A/B arms and same-build traces above count. `research/20260902_1630_...md`.

**THE PER-DRAW ABI IS CUT (2026-09-02, 16:30).** Three steps on the existing draw queue, each
verified on a village capture, `research/20260902_1630_the-per-draw-abi-cut-...md`:

- **Four real descriptor sets** (`bindless_allocator.h` note): the three texture heaps as three
  bindings of set 0, samplers set 1, the three dynamic constant ranges alone in set 2 (the
  per-draw bind copies three descriptors, not a heap), the occlusion counter set 3 and back on
  Android. VUID 03001 is gone by construction.
- **Every constant upload is keyed by content** within the frame slot, and the vertex compare
  covers only the registers the shader declares (`ShaderCacheEntry::constantRegisterMask`).
  `[perf] constants per frame` reads it: VS uploads 640 -> 3, PS 520 -> ~40 a frame.
- **Instancing on the queue.** The instance record is the **whole** vertex block, because the
  guest writes per-node registers beyond the world matrix and palette (c57, a foliage
  collision vector) and any of them would keep two nodes apart. Blended depth-writers are
  reorderable (`bd_draw_instancing_reorder_blended`), or nothing in a field scene may move.
  `[draw-queue] instancing: N draws in -> M issued` is the number; the village issues 239
  for 262 (few repeated meshes), a transition flush has 316 keys for 666 draws.

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

**Most node draws never came from `bdSceneNodeDrawSingle` directly (2026-09-02, 20:40).**
415 of the village's 599 node keys draw nothing in the interpreter: a sorted or translucent
material becomes an entry of a global render list that `sub_8227F360` sorts by depth and draws
later - 319 draws a frame, the majority of the scene pass, untagged until now. A midasm hook at
that loop's head (`0x8227F524`, jump to the loop tail on true) tags each entry, replays it from a
template and skips the guest's iteration. Bytes 291 and 294 of the entry move every frame and
the loop never reads them; keep them out of any identity. **Skinned nodes replay too**: the
bone upload is a gather of palette slots (`ctx.palette + slot * 64`, table on the interpreter's
stack for a direct node, at entry+800 for a list draw), so the template stores the slot list and
the replay gathers the matrices live. `research/20260902_2040_the-render-list-and-the-bones-...md`.
The Quest run of that build sat at 60 fps (`gpu_total_ms` 18.5 at 481 draws) **in a lighter
scene than yesterday's** (261 node draws against 535); it is not a comparison.

**THE SCENE PASS IS 3.7 MS; THE FRAME IS NINETEEN PASSES (2026-09-02, 21:00).** The render-stage
trace of the bones build: scene pass 3.67 ms for ~470 draws (under 8 us a draw, against 36 us
yesterday - the per-draw ABI cut landed), and a 16.3 ms GPU frame of which 3.0 ms is
compositor preemption at pass boundaries: shadow stub 64x64 (1.2), reflection stub 128x72
(1.3), scene copy (0.6), dof chain (1.4), bright mask and blurs (2.5), four full-res passes
after the composite (4.0), present (0.5). **Fewer passes is the GPU lever now**: scene into a
texture the composite reads, one composite into the eye-resolution swapchain image with gamma
folded in, the shadow and reflection stubs skipped when off.
`research/20260902_2100_the-scene-pass-is-3-7-ms-...md`.

**The CPU frame is the guest's "Draw Thread"; "SDLThread" is SDL's pump (2026-09-02, 21:00).**
The first profile since the capture hang: the Draw Thread carries the walk, the node draws,
the uploads and Present, and a quarter of its samples were the SDK heap's recursive mutex under
`BaseHeap::QueryProtect`, reached from every `bd::mem::try_load` the host scene code makes
(now behind a per-thread page cache, `memory_helpers.cpp`). SDLThread was SDL3's Android
`SDL_WaitEvent` spinning (no blocking wait on Android) - 100% of a big core, pinned there as
"guest main" by `threading.cpp` for a week; the SDK fork's loop polls and sleeps now, and the
policy gives the big cluster to the Draw Thread. Before those fixes: `other_ms` 18.2,
`gpu_total_ms` 16.9 at 474 draws, 90% of field frames in one 60 Hz slot. **After (21:05):
`other_ms` 16.3, `gpu_total_ms` 15.8 at 477 draws, 98% of field frames in one slot - a
vsync-locked 60 fps in that scene**, side-by-side, shadows and reflections off. With the
reflection stub pass skipped by the walk (21:30): draws 378, `gpu_total_ms` 14.8 (p10 13.8),
99% of field frames in one slot. **Direct present** (the gamma pass renders into the
runtime's swapchain image, `bd_xr_direct_present`, 21:50) and **eleven of the fourteen EDRAM
seed copies gone** (the guest's dropped post draws no longer bind their targets; the composite's
target is discarded, not seeded; 22:10) left `gpu_total_ms` at 14.6-14.9: seeds 13 -> 3,
framebuffer binds 21 -> 10, barrier calls 102 -> 51 a frame, and the GPU number did not move,
so those copies were cheap on the GPU. The render-stage trace's ~3 ms of "Preempt" is the
compositor's own GPU work between our passes, not ours to remove. What is left of the app's
GPU: the scene (3.7), the guest's scaled scene resolves (0.25-scale full copy plus two
half-res copies the host chain overwrites, ~1.5 ms), the shadow stub (1.2, becomes real
shadows in the target configuration), the post and 2D passes. **The guest's scene resolves
then went too (22:50-23:30)**: the two half-res scaled copies had no reader under the host chain,
and the x0.25 full-res copy is an alias now, the scale applied by the host passes (the composite
reads the scene as the dof draw saw it; the first attempt read the guest's re-resolved dof
target, a seeded unscaled copy, and the frame came out four times too bright). Quest:
`gpu_total_ms` p50 **13.2** (p10 12.1), `rs_eager` 1, seeds 2, 99% of field frames in one
60 Hz slot - the median is under the 72 Hz boundary of 13.9 ms for the first time; the p90 is
not.

**What is left on the CPU, per thread** (`out/device/profile_setmove.txt`, 2026-09-01; the
SDLThread paragraph below is superseded by the profile above):

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
- **A capture used to hang the render thread** (2026-09-02, fixed in `2d3f8af`): the log
  stopped at `[capture] wrote`, the profile never dumped, and everything measured after
  `bd_capture_after_s` was a frozen frame. If a run goes quiet after the capture line again,
  `cdb -pv -p <pid> -c "~*k"` on the desktop process names the wait in seconds.
- **`tools/stereo_check.py` has been confidently wrong four times**, each time on an image that
  was not a stereo pair (a composited panel, a misdecoded array, a mono frame twice). Look at the
  capture before believing the verdict. Only a `--stacked` grab from `bd_mv_capture_array` gives a
  stereo verdict on device.
- **Draw counts are not costs.** "715 PSO switches" counted guest state changes, not pipeline
  binds (opaque draws take 14). "`bdSceneNodeDrawSingle` is 23x the next consumer" was a call
  count; it profiles at ~5%. "Most draws are blended full-screen passes" reasoned from counts
  while the measured time was 81% in the scene pass. This mistake has been made four times.

## Owner decision, 2026-09-03 10:00: desktop only until every Xbox 360 paradigm is gone

**No Quest runs until the host owns the frame.** The morning's nine device runs (the note
above) qualified the head build and then measured the 360's frame model itself: a
fragment-bound scene pass under a blend-heavy material layering, seventeen to twenty-six
passes a frame with EDRAM seeds, resolves and a preemption slot at every boundary, Xenos
shaders run one to one. Measuring that again is worthless; replacing it is the work. The
owner's words: "until all xbox360 paradigms are removed it seems worthless to test on quest
2", "we must remove edram emulation", "i'm okay with updating engine code".

What "removed" means, each verified on a desktop capture, in this order:

1. **The host owns the targets (stage 4).** Scene colour and depth are host textures the
   host creates (two layers, per eye, the size it chooses); the guest's set-target, clear
   and resolve calls for render views 0, 1 and 3 map onto host passes; a resolve destination
   *is* the host texture (no copy); the seed copies, tile aliasing, held clears and the
   surface pool's EDRAM matching go. The front buffer is the host composite's 8-bit output.
2. **The host owns the passes.** Scene, dof at half res, bloom at quarter res, one composite
   with gamma folded in straight into the runtime's swapchain (multiview, two layers), the
   guest's 2D and effects as one overlay pass. Five or six passes, not twenty.
3. **The host owns the materials.** The dominant material families get host shaders (fewer
   fetches, fog per vertex, a lighting-model slot: guest look or cel); the render list's
   "sorted" materials that are opaque cutouts draw opaque; the rest of the Xenos shaders
   stay only for what is not yet converted. The two `bd_debug_skip_*` A/Bs (last device
   runs) say which class carries the fragments.
4. **Assets at the asset level (stage 3).** Meshes as triangle lists in host buffers, merged
   statics, LODs and impostors; textures with chains in the device's native compression;
   materials as host pipelines. Cooked once on the desktop from the recompiled loaders.
5. **Shadows, animation, culling, indirect draws on the host** (stages 5, 6, 8), then
   **foveation** on the host frame (stage 7).

The PC shows the same structure: with `bd_gpu_timing_segments` on the desktop (10:00,
multiview half width, 960x1080 a layer, 813 draws) the GPU frame is **5.5 ms, of which the
scene pass is 1.6**; the other 4 ms is passes, copies and resolves that a host frame would
not have. The desktop vsync hides it; it is the same waste the Quest pays at its rates.

Engine code is in scope: the recompiled render path (`bdCameraRender`, the render views,
the posteff chain, `bdSceneNodeDrawSingle`, the render list loop) can be replaced by host
code outright, not only hooked. The desktop loop, `tools/rdc_outline.py` pass lists and
`bd_capture_after_s` captures are the verification. The Quest comes back for one run when
item 2 ships and the frame is the host's.

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

0. **Cut the per-draw ABI on the queue** - shipped 2026-09-02 (four sets, content-keyed
   constants, instancing on the queue; "What is true now"). The recorder below tags those
   queued draws with identity; it is not a second draw path.
   **Stage 1 shipped 2026-09-02 too** (`gpu/scene/`): the DrawSingle hook tags every node
   draw, `bd_scene_record_after_s` writes an N-frame `.bdsw` walk, `tools/scene_walk_dump.py`
   reads it (village: 464 tagged node draws a frame, 26 repeats). **Stage 2a shipped**:
   `bd_host_walk` (default on) replaces `bdSceneNodeCullTraverse` with a host walk over the
   guest's draw nodes - same cull hooks, the guest's own visibility test, identical draw count
   and frame. Culling, LOD and the host-issued draw (2b) attach there now.
   **Stage 2b shipped 2026-09-02 evening**: `bd_host_draw` (default on) issues a node's draws
   from a host template instead of running the 1,935-instruction interpreter, for direct nodes and (since 20:30)
   for the guest's deferred render list, skinned nodes included (bone slots stored, matrices
   gathered live); foliage (c57) waits for the host's vector to be trusted. The
   template holds each sub-draw's host state and the registers the interpreter SETS (the
   setter hooks, not a value diff - a same-value write is still a write); a register that
   moves between frames is taken from the latest interpreted node of the same visual in the
   same frame, and one node per visual per frame is interpreted to keep those fresh. The
   world rows c20-c23 are rebuilt from the palette slot (transposed, translation in .w,
   verified over 3728 draws). Village: 350 of 659 node draws a frame host-issued (2026-09-02, 20:30; 111 of 420
   before the render list and the bones), 39 volatile templates. `[node] host-issued N of M` is the number. Replayed draws stay off the
   instance-record path (`bd_host_draw_records`, default off): on it, the village's big rock
   was hidden in some frames while its own node drew every frame, so a replayed draw covers it
   - the mechanism is still unnamed (`bd_node_diag_mesh` logs a node's draws from both
   paths). It measured flat on the Quest's GPU. The rest is the foliage and skinned
   nodes (stage 6, animation on the host) and the per-visual lighting and camera registers
   (VS c0-c4, PS c0-c13, copied from the deferred-state shadow at 0x82DD80D8 - which holds
   the last interpreted node's pass, so it cannot be read live; tried and reverted).
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

## The next steps, desktop only (written 2026-09-03 10:05)

No device runs: see the owner decision above. The last device measurement, the render-list
A/B (`bd_debug_skip_list_draws`, run 11): **-1.2% GPU** with the guest's sorted and
translucent draws dropped, so the fragment cost is in the tree-walk materials - terrain,
rock, buildings - not in the translucent layering. The blended-class A/B was not run.

**Done 10:05-10:45 (desktop, `PLUME_FB_TRACE`):** the scene pass was eight render passes
and is one: plume no longer ends a pass for a rebind of the open framebuffer (fork
`0bf3d63`/`6a6f679`), the sun-occlusion counter's copies moved out of the pass to the
list's begin and submit, and the resolve-source barrier goes ahead of the queued draws
instead of flushing them first. 26 passes a frame -> 19, image unchanged. **Then the chain seeds became tile
aliases** (`bd_chain_alias`, 11:07): a fresh full-screen surface bound after the chain
head is the head's texture, the way both were one EDRAM tile; seeds 2 -> 0 a frame,
barrier calls 44 -> 35, image identical. Left of the EDRAM residue in the tail: the
16-to-8-bit front conversion. The trace is the
instrument for the rest: run the desktop with `PLUME_FB_TRACE=<path>` set, read one frame
between two `1920x1080` present passes; `pass ended by barriers` lines name the texture,
`host surface|texture guest .. plume ..` lines name the guest object behind it.

1. **Stage 4, the host owns the targets.** Start in `gpu/draw_framebuffer.cpp` and
   `gpu/resolve.cpp`: a host scene target per render view (colour+depth, two layers under
   multiview) bound when the guest binds the scene surface; the guest's resolve of the scene
   becomes "the destination is this texture" (alias always, no size or format gate, the
   post chain already applies the resolve scale); `SeedFreshColorTarget`, the held clears,
   the surface pool's EDRAM matching and `MaterializeInboundLocked` retire for the scene
   chain. Verify: `[seed]` and `[resolve] eager` at zero for the scene chain,
   `tools/rdc_outline.py` pass list, capture unchanged.
2. **The passes.** Composite with gamma folded in, into an 8-bit host target that the
   guest's 2D draws overlay, presented without the front copy; under XR straight into the
   layered swapchain (`XR_FB_foveation` attaches there later).
3. **The materials.** From a desktop RenderDoc capture, the five most common pixel shaders
   of the tree-walk draws by fragment count (`tools/rdc_outline.py` per draw); host
   replacements by shader hash (`BloomMaskClampBlob` in `guest_shaders.cpp` is the
   substitution mechanism), fewer fetches, a lighting-model slot.
4. **Assets** (stage 3), then shadows, animation, culling, foveation.

Each step: build, `bd_xr_autoplay` desktop run, capture, look.

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

**The descriptor layout's spec violation is fixed (2026-09-02)**: the layer used to report
`VUID-VkDescriptorSetLayoutCreateInfo-descriptorType-03001` (a dynamic uniform buffer in a set
with an update-after-bind binding). The three HLSL texture spaces are three bindings of one
set now, the constants have a set of their own, and the sun-occlusion set is back on Android.
Confirm on the next `validate_quest.sh` run; the desktop has no validation layer installed.

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
- **`[perf] constants per frame`** counts vertex and pixel block uploads against skips, and
  **`[draw-queue] instancing: N draws in -> M issued per flush`** is the instancing factor;
  the one-shot `[draw-queue] instancing diag` lines (three per run, scene-sized flushes)
  count distinct key components and name the registers that keep same-mesh draws apart.
  Read those before theorising about why draws do not merge.
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
