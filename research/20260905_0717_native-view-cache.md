# Native camera/frustum-cache producer

2026-09-05, Windows Vulkan desktop, EDT. Camera/frustum producer conversion,
not complete frame ownership or Quest qualification.

## Source and ownership

The guest-source and devloop guides were used with the current desktop-first
scope in AGENTS. Read `render_tweaks.toml`, `stereo.toml`,
`output_resolution.toml` and the exact generated functions:

- `sub_82186840`, file 24: seven-slot view cache, force-refresh setting at
  renderer +7128, canonical shape and six-plane publication.
- `sub_822873E0`, file 18, and `sub_821CCC78`, file 84: projection inverse,
  six clip-point unprojections, side/depth division and convention conversion.
  Vertical slopes have an authored -1.1 multiplier, not merely a -1 flip.
- `sub_82287478`, file 53: general view inverse, camera origin, normalized
  direction, roll-free angles and orientation quaternion. The transformed
  up-vector calculation has no consumed output.
- `bdMatrixInverse4x4`, file 63, and `sub_82491748`, file 105: ordered float
  cofactors and twice-refined reciprocal. `bdVec3TransformByMatrix`, file 63,
  delegates a non-perspective vec3 transform to `sub_824911F8`, file 22.
- `sub_824906C8`, file 97; `sub_82277198`, file 30; `sub_82277968`, file 38;
  `sub_8217A8D0`, file 90: normalization, direction angles, scalar asin
  approximation and ordered sine/cosine polynomials.
- `sub_827355C0`, file 93: initialization clears all seven validity words.
  The debug visualization in file 26 traverses the same cache extent.

`native_view.h` owns general inverse/unprojection, native clip points, the
10% vertical guard band, camera orientation and an address-free view cache.
Its cache is not limited to seven views. Singular/nonfinite inputs preserve
IEEE results instead of inventing an identity camera. No PPC context, engine
addresses, GPU SDK or original math helper calls enter the native core.

The complete `sub_82186840` replacement consumes values from the existing
native transform producer, builds and retains native shapes, and publishes
the current host culling volume directly. It does not recover that volume
from engine planes. Scene begin's optional cached-view selection can also
use the native record. Cache hits use host-owned shapes; an absent native
record with an already-valid engine slot is an explicit counted bootstrap.
The reset replacement clears native ownership and publishes validity getters.

The temporary bridge still reads engine settings/invalidation and publishes
big-endian shape/cache/plane getters for unconverted clients. It admits the
seven engine view IDs, has mapped-range checks, and counts unsupported
original calls. Missing native transform ownership is a counted matrix import,
not an unreported stale native camera. The original clip table is checked only
in comparison mode; normal production uses native constants. Coefficients were
read from the owned executable's rdata; runtime comparison confirmed the six
clip points. A PE raw-section offset calculation did not establish runtime
data-table contents, so it was not used as evidence for those points.

`bd_native_views` defaults on; `bd_native_views_verify` defaults off. The
comparison executes the original cache producer once before native getter
writes, then checks the return value, all 13 shape/cache floats, validity and
all 24 plane coefficients at 1e-5 absolute/relative tolerance with matching
NaNs. No GPU lifecycle body is executed twice for comparison.

## Failures and corrections

1. Initial binary 07:17:13, 47,346,176 bytes. Process 24132, log 729,
   started 07:17:40 and exited at a comparison error at 07:17:57, before
   captures. A generic row-expanded inverse produced far -20068.44 versus
   -20020.547. Reordering algebraically equivalent float operations changes
   cancellation in the far-point divisor. Native column-paired cofactors now
   retain the producer's order; the comparison tolerance was not widened.
2. Corrected inverse binary 07:22:30, 47,346,176 bytes. Process 25296,
   log 730, started 07:23:02 and exited at 07:23:22. Shape checks passed,
   but one translated-plane offset was 1.288569 versus 1.2886009. Native
   scalar sine/cosine now retain the ordered polynomial instead of library
   trig, preventing tiny orientation differences from being amplified by
   camera translation.
3. A new test initially associated the first run's far value with a different
   projection logged in run two. That incorrect fixture failed. It was
   corrected to the second projection's result; tests now exit noninteractively
   on failure instead of opening a Windows Debug CRT assertion dialog. The
   stalled owned test was stopped; a second attempt was terminated by CTest's
   explicit timeout. Neither was treated as a successful test/build.

## Corrected comparison

Binary 07:28:42, 47,346,688 bytes, source version `b55bd4df8` dirty. The
host-only build linked successfully; codegen was up to date and no generated
guest translation unit or dependency rebuilt. All 21 texture/state/camera
CTests, the separate material CTest, and seven scene/reflection source guards
pass. Tests cover independent double inverse products for 1000 general
matrices, 20001 angle samples, off-centre projection, camera translation,
roll-free orientation, distant/singular projection and native cache ownership.
Source guards are not independent runtime/GPU tests.

Process 15172, log 731, ran 07:29:05-07:30:54. All six profile settings
audited: the original autoplay/perf/capture-delay-60/minimum-600/count-120,
plus view comparison enabled. Full 1673 archives / 119346 names mounted.
GPU: RTX 3060, Vulkan 1.4.341, MSAA4, no fragment-density-map support.

Final sampled counters: 15341 produced/rebuilt/checked views and clip checks,
all using native transform values; zero mismatches, compatibility calls,
refusals, matrix imports or cache bootstraps. Cache-hit, selected-cache-view
and reset counters on the reporting thread are zero; those paths are not
qualified by this GPU run. Native culling has no missing volume. The original
comparison also invokes the already-native plane helper, so plane-production
counters include diagnostic work and are not normal-path performance numbers.

The isolated 120-frame 1920x1080 sequence is
`out/verification/native_view_compare`, `frame_1788607808_0.raw` through
`frame_1788607811_119.raw`, 07:30:08-07:30:11, starting at frame 2839.
0/119 pairs exceed 6%; no cyan patches, median 0.011%, maximum 0.02%.
Actual first/last images show Shu in the village, solid scenery and animated
shadows, without broad banding/cyan. All analysis completed after stopping
the renderer. No normal-path performance claim or full-game coverage follows.

## Normal comparison-off flat

Implementation and comparison evidence were committed/pushed as `b257079`.
The unchanged 07:28:42 binary ran as process 24108, log 732,
07:35:57-07:38:01. The original five-setting profile was restored exactly;
all five audited. Full archives/names mounted. Native view totals: 18054
produced/rebuilt, all native matrix values, zero checked/clip-check calls,
compatibility, refusal, matrix imports or cache bootstraps. Native culling
has 604783 walks and no missing volume. Scene begin/end have no fallbacks,
44959 matching ownership checks and 12600 explicit outputs. No error,
critical, VK_ERROR or upload-exhaustion entries.

Captures: `out/verification/native_view_flat`, 120 1920x1080 frames 2817-2936,
`frame_1788608219_0.raw` through `frame_1788608223_119.raw`,
07:36:59.849-07:37:03.227. All 119 pairs stay below 6%, no cyan patches;
median cyan 0.011%, max 0.02%. Actual first/last images show stable village
scenery and Shu, with moving shadows and no broad banding. Analysis finished
before the VR renderer launched; no capture/control setting was retained.

## Normal final-eye desktop VR

The vrsim guide was read and used. Same binary, process 24548, log 733,
07:38:42-07:41:16. All 15 temporary settings audited: autoplay/perf,
delay 60, minimum 450, count 120, native views on, VR on, legacy stereo off,
multiview and layered textures on, scene-array capture and mirror off,
camera mode 2, diorama height 0, XR scale 1.0. The absolute runtime manifest
and its absolute 31232-byte DLL were checked. Process-only simulator
environment: 1440x1584 recommended eyes, height 0. Full archives/names mounted.
Logged XR eye and game positions differ, confirming the native view override.

Final sampled view totals: 41174 produced/rebuilt with native matrices,
zero compatibility, refusal, matrix imports, bootstraps or comparison calls.
Native culling has 691810 walks and no missing volume. Scene lifecycle has
17401 begins / 17400 ends, 34800 explicit outputs, 11174 empty-pass clears,
132975 matching ownership checks and no fallback/refusal. No error/critical,
VK_ERROR or upload exhaustion. The remaining state-308 adapter has 34802
calls and parameter adapters 191411; these are not fully host-owned frames.

The scene colour/depth attachments initially use 1440x808, briefly change
to 1280x720 during the transition, and return to 1440x808 at 07:39:11.430.
All have two layers. Final swapchain/captured eyes are 1440x1584 per eye;
this does not fix the shorter scene-content height.

Captures: `out/verification/native_view_vr`, 120 stacked 1440x3168 frames
13900-14019, `frame_1788608384_0.raw` through
`frame_1788608416_119.raw`, 07:39:44.904-07:40:16.672. All 119 pairs stay
below 6%, no cyan patches (median/max measured 0). Actual first/last images
show both eyes with stable rocky scenery/orange sky but substantial blur and
letterboxing. Both stereo checks return exit 2, INCONCLUSIVE: usable bands
44/52%, disparity -1/-2 pixels, spread 1. This is not a stereo-depth or
framing pass, headset comfort/performance evidence, or full VR qualification.

The renderer stopped before analysis. Analysis completed successfully with
the inconclusive stereo outcomes retained, and the exact original five
profile settings were restored. No renderer, test or analyzer remains live.
The follow-up source edit only corrects an obsolete camera-cache comment;
all three successful runtime checks use the same functional binary.

## Remaining work

Engine camera/object sources, inherited transform imports, invalidation/debug
settings, other-view clients/getters, scene traversal, frame scheduling,
state 308, animation, effects/post/UI and resource adapters remain. This does
not qualify the known later scenery/text failure, VR framing/depth, battles,
cutscenes, reloads or the whole desktop gate. No Quest or Thor run is authorized
by this component checkpoint.
