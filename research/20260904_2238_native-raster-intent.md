# Native raster intent, 2026-09-04

## Ownership change

`native_raster.h` defines named host depth, cull/fill, colour-write and front
stencil intent. Draws copy those fields from live native memory without reading
the engine render-state cache or converting its 15 values per draw. The intent
is separate from the last bound pipeline and replay snapshots, so replay or a
temporary draw override cannot silently become the source of subsequent state.
The copy preserves unrelated shader, blend, target and multiview fields.

The `bdSetRenderState` hook now replaces the 15 recognized raster setters and
their guest dispatch/cache-update loop. `native_raster_bridge.cpp` checks the
device range and exact dispatch identity before any effects. It produces native
intent and explicitly publishes temporary getter-cache, register-shadow and
dirty-bit effects for the engine readers that remain. Known unchanged writes
skip device-shadow effects, preserving the original cache shortcut. Unknown
states/setters still execute the original and have separate counters.

Bootstrap imports the current cache once; `bdEngineInit` invalidates it after
the engine's getter-seeding loop. An unsupported/disabled raster setter also
invalidates the imported state. Ordinary blend/other-state calls do not force
new raster imports. A mutex protects the live record across startup/render
threads, independently of the draw pipeline lock.

`bd_native_raster` defaults on and `bd_native_raster_verify` off. Verification
runs the original setter once, comparing the entire first 12188 bytes of device
storage and the updated cache slot against predicted publication. At every
ordinary draw it also compares all 15 cache words with the host import record,
counting untracked writers without silently repairing the native state. These
are diagnostic guest calls, not zero-guest-execution evidence. Replayed draws
that bypass ordinary state flush still use their existing pipeline recipes.

## Source contract

Used the repository guest-source skill: inspected hook TOML references first
(there is no instruction-site render-state hook), read the existing state hook
and complete translated `bdSetRenderState` in module 24, then all 15 complete
translated setters. Read the complete `bdEngineInit` in module 93, including
the 388-byte getter-seeding loop. Cache-base references in modules 4, 6, 8, 20,
40, 46, 65, 87 and 99 are save/read uses around setter calls; the similarly
spelled module-60 offset has another high-address base. Checked comparison and
stencil enums against the SDK header, and the prior draw-time conversions.
Generated source and hook TOML are unchanged.

Supported offsets: 40/44/48 depth enable/function/write, 52 fill, 56 cull,
108/112 stencil enable/two-sided, 116/120/124 fail/depth-fail/pass, 128 function,
132/136/140 reference/read-mask/write-mask, and 212 colour-write.

The adapter preserves the old import-only defaults for uninitialized depth
function and zero stencil masks. Native intent itself can express NEVER,
disabled depth writes and zero masks without sentinel meanings. Engine enable
gates affect only the compatibility register shadows, not native draw intent,
matching the previous cache-based renderer. This is not a new stencil/default
correctness claim.

## Tests and build

Core checkpoint `0144034` was committed and pushed before integration. Eight
standalone tests pass (0.45 seconds total). The new test checks native field
copy/dirty behavior, unrelated-pipeline preservation, replay independence,
import offsets/defaults, native zero-mask/NEVER values and all 15 publication
contracts across 30000 randomized cases with both hardware gates and existing
neighbor/dirty bits. Release builds explicitly keep assertions enabled.

The configured Vulkan-only desktop `reblue` target linked with OpenXR/PCH on.
The first build found two missing settings-declaration semicolons; corrected
and rebuilt successfully. Final incremental build linked with zero codegen
writes and no rebuilt guest translation units. No build tree was deleted.

## Limits

This removes raster execution and per-draw raster-cache translation, not the
complete X360 state model. Blend still imports device registers because engine
sites write them inline. Alpha-test, sampler and other state producers remain;
remaining-state telemetry logs their offsets. Engine materials/pass decisions,
getter storage, counter-clockwise stencil behavior, native scene/pass assets,
shader ABI and retained replay assumptions still need conversion. The known
multiview flashing and damaged later scene are not assumed fixed by this work.
Full-game/both-eye qualification remains required before any Quest work.

Local profiles, generated code, game data, binaries, logs and captures are
excluded from commits.

## Desktop verification

### Comparison, log 672

PID 23476 launched 22:38:14 EDT with the original five-setting profile plus
`bd_native_raster_verify = true`; all six settings applied. Native raster was
on by default. Capture delay 60 seconds, minimum 600 draws and 120 frames.
Sequence 119 completed 22:39:20.688 at frame 2953, 1920x1080. Output isolated
in `out/verification/native_raster_verify`: 0/119 jumps over 6%, no cyan patches
(median 0.011%, maximum 0.02%). First and last images were inspected: character,
terrain, vegetation, buildings and shadows intact.

Last report at 22:40:00.089: 1491692 native setter updates, 1295286 unchanged;
1491692 comparisons, zero mismatches. There were 3070903 ordinary native draw
checks with zero cache drift, one bootstrap import and no refused setters or
legacy raster draws. Other-state compatibility calls were 6337413: these are
not eliminated by the raster conversion. The field exercised depth, fill, cull,
stencil-enable and colour-write entry points, not the stencil operation/mask
setters. Standalone coverage is not GPU qualification for those unexercised
paths. No error/critical, Vulkan error/failure, overflow, exhaustion or
retirement-race matches. Exact-path validated process stopped and confirmed
exited at 22:40:04. The diagnostic override was then removed.

### Normal flat, log 673

PID 20476 launched 22:40:23 with the original five-setting profile; all five
settings applied. Same final binary, native raster on and comparison off by
default. Sequence 119 completed 22:41:29.911 at frame 2953, 1920x1080. Isolated
output: `out/verification/native_raster_flat`. 0/119 jumps over 6%, no cyan
patches (median 0.011%, maximum 0.02%). First and last images were inspected:
character, terrain, vegetation, buildings and shadows intact.

Last report at 22:41:55.655: 1295107 native setter updates, 1125343 unchanged,
2625039 ordinary native draw flushes; zero diagnostic calls/checks, refusals
or legacy raster draws and one bootstrap import. Other-state compatibility
calls: 5492473. These counters do not count replayed draws as newly composed
native raster recipes. No error/critical, Vulkan error/failure, overflow,
exhaustion or retirement-race matches. Exact-path validated process stopped
and confirmed exited at 22:41:58. This short field check does not requalify the
known damaged later scene or all stencil paths.

### Normal desktop multiview, log 674

Integration `d8166ec` was committed and pushed before this run. PID 19944
launched 22:43:26 EDT with the same final source binary. All 13 temporary
settings applied: autoplay/perf CSV, 60-second capture delay, minimum 450
draws, 120 frames; VR on, legacy stereo off, multiview/layered textures on,
scene-array capture off, mirror off, camera mode 2 and diorama height 0.
The process-local simulator requested 1440x1584 per eye, head height 0.
OpenXR created a session and actual 936x1030x2 swapchain; eye and game cameras
differed. This is not a full-resolution target or headset performance check.

Sequence 119 completed 22:44:34.287 at frame 20836, 936x2060 stacked eyes.
Isolated output: `out/verification/native_raster_vr`. 10/119 pairs exceeded
6%, at destination frames 30, 31, 33, 34, 36 and 94, 95, 97, 98, 100. The
known 64-frame cadence persists. No cyan patches (median/maximum 0.000%).
Both eyes in the first/last frames and the 29-to-30 jump pair were inspected:
blur/banding and large black bars remain, with the jump changing scene
sharpness/appearance in both eyes. The stereo check exited 2, INCONCLUSIVE,
with only one bounded textured band; its output image was inspected too.
The native raster conversion does not resolve the existing VR failure.

Last report at 22:45:15.081: 5246909 native setter updates, 4597555 unchanged,
14357554 ordinary native draw flushes, one bootstrap import; zero diagnostic
calls/checks, refused setters or legacy raster draws. Other-state compatibility
calls: 28513657. No error/critical, Vulkan error/failure, overflow, exhaustion
or retirement-race matches. Exact-path validated process stopped and confirmed
exited at 22:45:19. All renderer processes are stopped; the original five-setting
profile is restored. The final eight-test rerun passed in 0.48 seconds.

## Next boundaries

Remaining-state telemetry in the normal flat run observed offsets 60, 72, 76,
96, 100, 104, 304, 308, 324, 328 and 332. These identify the next setter/input
sources to inspect, not permission to drop their effects. Replace blend/alpha/
sampler and material/pass production with native records, including inline
writers, and retire the temporary cache/register publication adapter as its
readers disappear. Retained replay recipes and the known multiview/later-scene
failures still need correction. No later-scene capture was repeated here.
The repository skills kept testing on desktop and required sequence and eye
inspection; no Quest or Thor work was performed. The all-rendering/full-game
acceptance gate remains open.
