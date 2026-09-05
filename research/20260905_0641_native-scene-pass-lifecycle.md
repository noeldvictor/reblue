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
