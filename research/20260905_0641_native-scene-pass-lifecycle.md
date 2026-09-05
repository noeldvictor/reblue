# Native scene pass lifecycle

2026-09-05, Windows Vulkan desktop, EDT. This is scene attachment/lifecycle
ownership, not a completed host frame or removal of all engine producers.

## Source and implementation

The guest-source guide directed reading `render_tweaks.toml`, `render_list.toml`
and the exact translated bodies:

- `sub_82186BA0`, generated file 53: scene begin, surface creation, pass entry,
  clears, sampler/state/view setup, camera-frustum cache and parameter producers.
- `sub_82187010`, file 9: depth then colour output, pass restoration, two handle
  releases. The colour output uses exponent bias -2 when the HDR byte is set.
- `hcgD3DCreateSurface` / `hcgD3DCreateSurfaceEx`, files 52 / 97: 16-slot engine
  allocation list and count/pixel accounting; the latter fixes the depth format.
- `bdRenderTargetRelease`, file 47: unregister/account when the handle is listed,
  but always release even an unlisted handle.
- `bdResolveToTexture`, file 85: unwrap container +4, null no-op, otherwise call
  the resolve adapter with mip/face zero. `sub_8246D1C8`, file 65, tails the
  already-native texture description getter, whose dimensions are host fields.

Both complete scene entry-point bodies now have host replacements. Scene begin
allocates explicit `SceneColor` / `SceneDepth` roles from the persistent host
target owner, with the configured native MSAA count and validated output-sized
extents. It never calls the engine surface constructors, enters their 16-slot
allocation table or classifies a role from square dimensions/format/MSAA flags.
Square scenes and non-MSAA scenes keep scene identities. Engine format words
remain only as existing resource-header/allocation adapters.

The SDK-independent policy tests full-size desktop/headset/square dimensions,
supersampling and percentage scaling, zero/overflow refusal, primary-view alpha
and colour-write policy, and the authored quarter-scale HDR exposure contract.
It does not reproduce the old EDRAM half-width/tiling branch.

Typed native pass entry/exit share the existing host attachment stack. The
temporary adapter still publishes engine target/extent getters and preserves
the engine's seven-level entry limit; the native stack itself is dynamic.
Scene ownership stores explicit native attachment references and checks source,
scope depth, getter handles and live bindings at exit. It restores the prior
pass before releasing the two adapter references. The persistent GPU images
remain owned by the host target cache. Resource lookups/releases occur outside
the video lock.

Native scene end publishes depth then colour from those saved images.
`PublishSceneOutput` accepts an explicit source, destination and float exposure;
it does not inspect EDRAM flags, last-drawn sources or the square/undrawn shadow
heuristic. An empty pass consumes its pending attachment clears before output
publication instead of reading a persistent image's old contents. Unsupported
begin inputs take a counted original scope before scene publications; original
and native scopes unwind according to their recorded ownership even if the
correctness setting changes. Unexpected mutations of a native scope fail
explicitly, rather than running a second original teardown.

## Remaining dependencies

This removes the two original lifecycle bodies and their surface-allocation,
description, tiling and resolve-wrapper calls on the supported path. It is not
zero guest rendering execution:

- `sub_82186840` still executes the engine camera/projection/frustum cache, once
  per native scene. Its six-plane builder is already native.
- Engine render-state 308 remains a counted setter adapter (two calls per
  scene). Its meaning/producer conversion is not inferred from its values.
- Sampler defaults, supported render states, view transform and parameter
  builders call the already-native producers through their existing engine ABI.
  Engine input blocks, parameter descriptors and getter publications remain.
- Destination textures still belong to the engine scene table. Native output
  publication shares the current image copy/scale/MSAA shader and lazy-link
  implementation. It still publishes the compatibility post/UI chain head.
  This is not removal of downstream tile matching/seeding or replacement with
  ordinary attachment MSAA resolves throughout the frame.
- Engine traversal/frame scheduling, other pass producers, native animation,
  effects/UI, persistent scene associations and full-game qualification remain.

`bd_native_scene_passes` defaults on. `bd_host_targets=false` or an unsupported
pass adapter also takes compatibility at entry; those are correctness controls,
not a plan to retain the old renderer. Getter checks are not an independent
original-producer comparison. Neither lifecycle is executed twice to compare
GPU resource effects.

## Build and initial verification

The devloop guide was used with the current AGENTS desktop-only target rules.
All 20 texture/state/scene CTests, the separate material CTest, three existing
reflection source guards and two new scene-boundary guards pass. Source guards
check the absence of engine allocation/resolve calls inside the native pair,
restore-before-release order and explicit-source publication without a registry
lookup under the video lock. They are not a runtime ABI/concurrency proof.

The host-only `reblue` Vulkan build succeeded; codegen reported its module up
to date and no guest translation unit rebuilt. No dependency changed. Final
initial executable: 47,315,968 bytes, 06:41:20 EDT, source `797af0fa6` dirty.
Process 24296 started at 06:41:21, log `reblue_725.log`, with the original five
profile settings: autoplay/perf true, capture delay 60, minimum 600 draws,
120 frames. All five audited; 1673 archives / 119346 record names mounted.

At 06:42:06 the run had 1801 native begins / 1800 ends, 3600 explicit output
publications, 869 empty-pass clears, 13470 ownership checks and no compatibility,
refusal or wrong-ownership entries. Remaining camera-cache calls: 1801;
state-308 adapters: 3602; parameter adapters: 19811. Periodic counters are sampled
mid-scope, so the one-entry difference is not a balanced-shutdown/leak audit.
Capture and final-eye qualification are recorded below when completed. Known
late-scene popping/text and VR letterboxing/blur/depth limitations remain open.

## Normal desktop sequence

Process 24296 was stopped at 06:44:14 after preserving all 120 current-run
captures in `out/verification/native_scene_flat`. They are 1920x1080, from
`frame_1788604944_0.raw` to `frame_1788604947_119.raw` (06:42:24-06:42:27).
All 119 pairs stay below the 6% jump threshold. No frame exceeds 0.30% cyan,
with zero 2-60% patches, median 0.011% and maximum 0.02%. Actual first/last
previews show Shu and the village with solid scenery and animated shadows;
no broad banding or cyan patch. The preview tool's `--mono` flag only selects
flat image analysis; this was normal native execution, not a legacy control.

Last reported totals: 9301 begins / 9300 ends, 18600 explicit outputs, zero
null outputs, 869 empty-pass clears and 65970 matching ownership checks.
There were zero compatibility begin/end calls, refusals or wrong-ownership
entries. The remaining camera-cache calls were 9301, state-308 adapters 18602
and parameter adapters 102311. No error/critical/VK_ERROR or upload-exhaustion
entries were found. All analysis commands completed successfully. The original
five-setting profile was not modified. This short sequence does not qualify
later scenery/text, additional game modes, deeper GPU nesting or final eyes.

## VR guard failure and layer-allocation correction

The first implementation/desktop checkpoint was committed and pushed as
`98a1800`. Its first final-eye run (process 14140, started 06:46:48, log 726)
exited at 06:46:51 with `Native scene depth publication failed`, before any
capture. This run is a failure, not VR qualification. The 15-setting profile
audited, and the simulator successfully created a 1440x1584-per-eye session.

The strict publication layer check exposed the resource creation boundary:
`D3DDevice_CreateTexture_hook` explicitly excluded depth formats from layered
allocation, while `SurfacePool` created a two-layer scene depth attachment.
The previous output-copy path permitted writing that into a one-layer depth
texture, so subsequent depth consumers could not access an independent right
eye. The new entry/exit path correctly refused the mismatch.

The correction allocates both colour-capable and depth-capable 2D output
textures with two layers when multiview and layered outputs are enabled.
It acts at creation, without re-backing live images, rewriting in-flight
descriptors or dropping the strict native output check. Cubes, volumes and
non-attachment sampled formats keep their existing layout. The shared,
SDK-independent layer policy has all 64 Boolean combinations tested, including
depth-only stereo and depth cubes. All 21 CTests and five source guards pass
again. The host-only rebuild completed at 06:49:37, 47,315,456 bytes, source
`98a18002e` dirty; no guest translation unit or dependency rebuilt.

The corrected run (process 24916, started 06:50:20, log 727) uses that new
binary and the same temporary profile. Its normal native path passes the
depth publication check; final-eye pixel results follow below.

## Corrected final-eye sequence

Process 24916 was stopped at 06:52:20 after 120 final captures, preserved in
`out/verification/native_scene_layered_vr`: `frame_1788605483_0.raw` through
`frame_1788605490_119.raw`, 06:51:23-06:51:30. These are stacked 1440x3168
final eyes, not the intermediate scene array. All 119 pairs stay below 6%;
all 120 frames have zero measured cyan and zero patches. Actual first/last
previews show both eyes with stable scenery, but substantial letterboxing and
blur. Stereo analysis returns INCONCLUSIVE for both: usable bands 44/52%,
disparities -1/-2 pixels, spread 1. This does not establish stereo depth,
comfort, full-height scene content or headset performance.

Last scene totals: 16201 begins / 16200 ends, 32400 explicit outputs, 10746
empty-pass clears, 124147 ownership checks, and zero compatibility, refusal,
null-output or wrong-ownership entries. Camera-cache calls: 16201; state-308
adapters: 32402; parameter adapters: 178211. No error/critical/VK_ERROR or
upload-exhaustion entries. The runtime and final eyes are 1440x1584 per eye,
but the scene attachment remains 1440x808. This fixes depth-output layer
allocation, not that separate sizing/framing limitation.

All 15 profile settings audited: original autoplay/perf/delay/count, minimum
450 draws, native scene passes on, VR on, replay stereo off, multiview and
layered textures on, scene-array capture and mirror off, camera mode 2,
diorama height 0, XR render scale 1.0. Process-only environment: absolute
`out/xrsim-build/reblue_xrsim.json`, absolute 31232-byte runtime DLL,
`XRSIM_WIDTH=1440`, `XRSIM_HEIGHT_PX=1584`, `XRSIM_HEIGHT=0`. The vrsim guide
was used; no Quest or Thor was run. Analysis finished before the next renderer
launched. The original five-setting profile was restored exactly; final flat
recheck process 25288 started at 06:53:10 with the same corrected binary.

## Final corrected-build flat recheck

The layer fix and VR evidence were committed/pushed as `23c52e0`. Process
25288 (log 728) ran from 06:53:10 to 06:55:56 with the 06:49:37 binary and
the exact original five settings; all five audited and the full archive set
mounted. Its 120 1920x1080 captures are in
`out/verification/native_scene_layered_flat`, from `frame_1788605653_0.raw`
to `frame_1788605658_119.raw`, frames 2846-2965, 06:54:13.260-06:54:18.736.
There are 0/119 large jumps and no frames over 0.30% cyan: zero patches,
median 0.012%, maximum 0.02%. Actual first/last images again show Shu and
the village with solid scenery, animated shadows and no broad banding/cyan.

Last counters: 7501 native begins / 7500 ends, 15000 explicit output
publications, 874 empty-pass clears and 53375 matching ownership checks.
Compatibility begin/end, refusals, null outputs and wrong ownership are all
zero. Remaining engine camera-cache calls: 7501; state-308 adapters: 15002;
parameter adapters: 82511. No error/critical/VK_ERROR or upload-exhaustion
entries. All analysis finished; no renderer remains running and the original
profile is restored. Flat and final-eye runs used the same corrected binary,
with no overlapping renderer/analysis workload. These are correctness checks,
not an FPS improvement claim. The known late-scene scenery/text failure was
not requalified, and the full desktop completion gate remains open.
