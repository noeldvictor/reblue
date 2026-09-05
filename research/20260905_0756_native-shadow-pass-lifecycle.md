# Native sun-shadow attachment lifecycle

2026-09-05, Windows Vulkan desktop, EDT. This replaces sun-shadow begin/end
execution and attachment ownership, not the complete shadow system or frame.

## Source and implementation

The guest-source skill directed inspection of `config/hooks/render_tweaks.toml`
and the exact translated `sub_82187168` / `sub_82187330` bodies in generated
files 47 / 58. These are the sun-shadow begin/end pair. The adjacent secondary
shadow pair (`sub_821873E0` / `sub_82187588`) is separate and remains unconverted.
Generated code, hook configuration and dependencies were not edited.

The native begin acquires the explicit persistent `HostTargetClass::Shadow`
depth attachment, enters a typed depth-only pass and holds an explicit output
texture reference. It uses the same shadow-size setting as the sampleable
texture creation hook, including the existing 64-pixel shadows-off adapter.
It does not invoke engine surface creation, enter the 16-slot allocation list,
classify a role from shape, or call the engine pass wrapper. A nonzero old
descriptor handle, unsupported mapped input or mismatched layer policy takes
a counted compatibility scope before native pass publications.

The complete native end checks scope/source/nesting, attachment getters and
live binding, and the output association saved at entry. An empty caster pass
consumes the owned attachment's pending far-depth clear before output; it
does not use the square-and-undrawn heuristic or an inferred last-drawn source.
Output uses the shared explicit-source publication operation with post/UI
tile-chain publication disabled. The prior pass is restored before the depth
adapter is released; the retained output reference is then released too. The
persistent host image remains owned. A native scope always unwinds natively
even if its correctness setting changes. Unexpected ownership changes throw,
not execute a second original lifecycle after GPU effects.

`bd_native_shadow_passes` defaults on. Supported raster/blend/alpha settings,
sampler defaults, both view transforms and final frustum/cache production use
already-native producers through their current ABI. The engine pass-mode byte
and camera-position getter stores preserve their authored values. These are
temporary getter publications, not native scene identities.

## Remaining dependencies

- `sub_82283068`: original scene-camera snapshot/derived orientation producer,
  called once and counted for each native sun-shadow begin.
- `sub_821752E8`: original light-camera fitting/object-query producer, likewise
  called once and counted. Its coverage hook remains active. This checkpoint
  does not claim to replace its fitting or geometry queries.
- Engine camera/light inputs, output texture creation/association, caster
  scheduling and other shadow modes; the secondary shadow pair is unchanged.
- Shared texture/resource headers, lazy image links and copy implementation,
  shader sampling ABI, inherited state and remaining render traversal. The
  native shadow output itself no longer publishes a post/UI tile chain, but
  the main scene and downstream compatibility paths still do.
- General native frame ownership, later scenery/text correctness, full-height
  VR scene content and stereo depth, and the full desktop completion gate.

## Build and checks

The devloop skill was used with current AGENTS target/configuration rules.
All 22 standalone CTests pass (21 texture/state/view tests and one material
test). Six scene-boundary and three reflection lock-order source guards pass.
The two new guards cover explicit shadow source/output ownership, retained
output lifetime, restore-before-release order, empty-pass clear and the absent
shadow post-chain publication. These are structural checks, not independent
GPU, concurrency or original-ABI comparisons. The lifecycle is not executed
twice to compare resource effects.

Host-only `reblue` Vulkan build succeeded; codegen reported its module up to
date and no guest translation unit rebuilt. The shared device-header signature
change rebuilt host consumers. Binary: 47,358,464 bytes, 07:56:29 EDT, source
`688b76916` dirty. No dependency changed. Desktop pixel results follow below.

## Normal desktop sequence

Process 22152 ran 07:56:48-07:58:51 with that binary and the exact original
five-setting profile: autoplay/perf on, capture delay 60, minimum 600 draws,
120 frames. All five settings audited in `reblue_734.log`; 1673 archives /
119346 names mounted. No error/critical/VK_ERROR or upload-exhaustion entries.

All 120 1920x1080 captures were preserved in `out/verification/native_shadow_flat`,
from `frame_1788609470_0.raw` to `frame_1788609474_119.raw`, frames 2839-2958,
07:57:50.948-07:57:54.289. There are 0/119 jumps over 6%, zero cyan patches,
median cyan 0.011%, maximum 0.02%. Actual first/last previews were inspected:
Shu and solid village scenery, with moving shadows and no broad bands/cyan.
The preview tool's `--mono` label is not a separate legacy control run.

Last shadow totals: 6301 begins / 6300 ends, 6300 explicit outputs, 873 empty
clears and 38674 matching ownership checks. Compatibility begin/end, refusals,
wrong ownership and null outputs are all zero. Each begin still made one
engine camera snapshot and one light-fit call: 6301 of each. Periodic counters
are sampled mid-scope, not a balanced shutdown/leak audit. Main-scene ownership
also has no mismatches/fallbacks; native views have no imports/fallbacks and
host culling reports no missing native volume. All analysis completed after
the renderer stopped. This is short-field correctness evidence, not an FPS
claim, later-scene qualification, VR qualification or full desktop acceptance.

## Normal final-eye sequence

The code and flat evidence were committed and pushed as `2a7c288`. Process
19872 ran 08:01:00-08:02:48, log `reblue_735.log`, using the same 07:56:29
binary. The vrsim skill was used; all 15 temporary profile settings audited:
original autoplay/perf/delay/count, minimum 450 draws, native shadow passes on,
VR on, legacy stereo off, multiview and layered textures on, scene-array capture
and mirror off, camera mode 2, diorama height 0, XR render scale 1.0. The
process-only environment named the absolute simulator manifest and its absolute
31232-byte runtime DLL, with width 1440, height 1584, head height 0. Full archive
mount succeeded. XR eye coordinates differ from the game camera, confirming
the override is active. No Quest or Thor was used.

The 120 final stacked 1440x3168 captures are in
`out/verification/native_shadow_vr`, from `frame_1788609722_0.raw` through
`frame_1788609731_119.raw`, frames 13142-13261, 08:02:02.672-08:02:11.102.
There are 0/119 jumps over 6%, no cyan patches and zero measured cyan. Actual
first/last previews show both eyes with stable rocky scenery/orange sky, but
heavy blur and black bars. Both stereo tests return INCONCLUSIVE: useful
bands 44/52%, disparities -1/-2 pixels, spread 1. Stable pixels do not prove
depth, comfort, complete vertical scene content or headset performance.
The 4096x4096 shadow map and scene depth/colour each have two layers. Final
eyes are 1440x1584 each, but scene content remains 1440x808 during capture.

Last shadow totals: 15301 begins / 15300 ends, 15300 explicit outputs, 10450
empty clears, 102251 matching ownership checks and zero compatibility, refusal,
wrong ownership or null outputs. Remaining camera snapshots and light fits
are 15301 each. Main-scene ownership has 117551 matches and no fallback;
native views have 35589 updates and no imports/fallbacks, while host frustum
walks report no missing volume. No error/critical/VK_ERROR or exhaustion
entries were found. All analysis completed after the renderer stopped; the
original five-setting profile is restored and no renderer remains running.

The new sun-shadow lifecycle now has normal flat and final-eye short-sequence
evidence on the same binary. Null output, shadows-off/layer-policy refusal,
setting changes during a live scope, deeper nesting and secondary shadow GPU
coverage were not exercised. Known later rock-wall/text failures were not
requalified. The full desktop completion gate remains open.
