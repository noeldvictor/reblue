# Native full-eye geometry, independent of the authored UI canvas

2026-09-05, Windows Vulkan desktop, EDT. Source base `b0c2d93` plus this
checkpoint. This removes the observed 1440x808 scene / letterboxed final-eye
restriction, not the remaining blur, weak stereo depth or guest frame producers.

## Ownership change

`Output::LatchedFit` explicitly fitted the entire runtime eye to the HUD's
16:9 design aspect. The resulting 1440x808 scene was presented inside the
1440x1584 layer. This was a host sizing policy, not a runtime limitation.
Existing HUD fitting already scales uploaded 2D geometry independently.

`native_output_geometry.h` now supplies SDK-independent scaled eye extents,
authored-canvas scale and full-eye viewport policy. Scene sizing and
`XrPresentSize` share the exact same dimension calculation. No aspect fit,
packed-eye squeeze or eight-pixel console alignment applies to this native
extent. Overflow/nonfinite input is rejected. At scale 1.0 the whole scene
and final layer are 1440x1584; 0.65 gives 936x1030, not 936x520.

`Output::DesignScaleX/Y` preserve equal horizontal/vertical pixel density for
authored UI. Projection-mode VR ignores the desktop stretch/aspect choice;
cinema still composes the authored flat aspect. Direct layered projection
presentation covers the full runtime viewport, including odd dimensions.
Cinema/movie and non-native packed-eye fitting remain distinct policies.
No full HUD/cinema/movie GPU qualification is claimed by the math tests.

The existing output hook map, output callbacks, scene pass allocation,
2D upload fitting and XR camera/presentation sources were inspected. No
instruction site, generated source, shader, dependency or asset was edited.
The devloop/vrsim skills kept builds in the existing tree and verification
on the desktop simulator. This is native output geometry; engine texture
constructors/getters, post/scene producers, native UI ownership and complete
frame scheduling remain conversion boundaries. Current latched-size behavior
is retained; resize/late-runtime/mode-change GPU coverage is not established.

## Build and independent checks

All 24 CTests pass: 23 in `out/native_texture_test` (including the new
`host_output_geometry`) and one material test. Ten scene/three reflection
source guards and both stereo analyzer tests pass. The new output test covers
full/square/odd/scaled extents, clamping, zero/NaN/infinity/overflow rejection,
canvas pixel proportions and all sixteen viewport policy combinations.
Those checks do not independently prove GPU projection, UI or threading.

The existing Vulkan-only `reblue` target linked successfully at 10:18:34:
`reblue_vk.exe`, 47391232 bytes, embedded base `b0c2d93` with modifications.
Adding the header triggered CMake's source-glob reconfigure; codegen reported
the module up to date and rebuilt no guest translation unit. Existing settings
initializer-order warnings remain. A final comment-only clarification rebuilt
only output.cpp and relinked at 10:25:48, same byte count; rendering logic was
unchanged between the VR and subsequent flat runs.

## Full-size normal desktop OpenXR run

PID 20476 ran hidden 10:19:40-10:22:39.273, log `reblue_748.log`, using the
10:18:34 binary. All sixteen settings audited: autoplay/perf on; capture
delay 60, minimum 450 draws, 120 frames; native sun camera/shadow passes on;
VR on; legacy stereo off; multiview/layered textures on; scene-array capture
and mirror off; camera mode 2, diorama height 0, XR scale 1.0. Diagnostic
comparisons are off. The full 1673-archive / 119346-name install mounted.

The process-only runtime manifest and its 31232-byte DLL paths are absolute;
`XRSIM_WIDTH=1440`, `XRSIM_HEIGHT_PX=1584`, `XRSIM_HEIGHT=0`. OpenXR creates
1440x1584 eyes. The log reports native scene 1440x1584, actual two-layer scene
targets of that size, and a final presentation from 1440x1584 to 1440x1584
with viewport +0,+0 and source/destination pixel ratio 1.00.

The 120 stacked final-eye captures are isolated in
`out/verification/native_full_eye_vr`, `frame_1788618042_0.raw` through
`frame_1788618053_119.raw`, frames 7903-8022, 10:20:42.656-10:20:53.402.
Both full-resolution eyes from the first and last captures were inspected:
they fill the frame, with no black bars. All 1584 rows of each eye have over
2% nonblack RGB pixels (>8/255). This verifies the letterbox removal in pixels,
not just counters. Heavy blur remains in the rocky scenery/orange-sky view.

Sequence analysis flags 10/119 pairs over 6%, from 53-54 through 62-63, with
maximum 59.84% at 56-57. Inspected jump 57 shows the same kind of large
dark/blue foreground passage as the earlier run; this is not a proven geometry
or shadow regression, and not a stable-sequence pass. There are no cyan patches,
with zero measured median/maximum cyan. Both stereo checks are INCONCLUSIVE
(exit 2): bands 44/52/62/72% give -1/-1/-2/-2 pixels, spread 1. Proper near/far
framing and character-shadow visibility still need work before enabling the
native sun camera. No complete VR, comfort or headset-performance claim follows.

Last sampled totals: 12901 native sun fits/snapshots, zero refusal/inactive;
95907 matching shadow and 95906 matching main-scene ownership checks, no
lifecycle fallback. Original camera snapshot/light-fit/cull-comparison calls
are zero. Native views have 33169 productions, no refusal/import/bootstrap.
Counts include loading and are sampled mid-scope, not balanced shutdown or
complete-frame guest-removal proof. No error/critical/assertion/fatal/device-lost,
VK_ERROR or exhaustion marker was found in checked patterns.

## Normal flat control

The original five-setting profile was restored exactly: autoplay/perf on,
delay 60, minimum 600 draws and 120 frames. Native sun, VR and diagnostics
are off. PID 23948 ran hidden 10:25:48-10:27:36.524, log `reblue_749.log`,
using the final 10:25:48 binary. All settings audited, the full archive/name
mount succeeded, and output remained 1920x1080.

Captures are isolated in `out/verification/native_full_eye_flat`,
`frame_1788618411_0.raw` through `frame_1788618414_119.raw`, frames 2840-2959,
10:26:51.340-10:26:54.638. There are 0/119 jumps over 6%, no cyan patches,
median cyan 0.012%, maximum 0.02%. Actual first/last full-resolution frames
show the village, Shu and his cast silhouette. This is a short flat control,
not later scenery/text or full-game qualification.

Final samples: 5401 shadow begins / 5400 ends, 33269 matching shadow and
38669 matching main-scene ownership checks, no lifecycle fallback; native
views have 15343 productions with no refusal/import/bootstrap. Default camera
snapshots/light fits remain 5401 each. No error markers from the same checked
patterns were found. Both processes were stopped by verified path/PID/start
time before analysis. Raw sequences were isolated using hard links, preserving
original captures without duplicate storage. No renderer/replay process remains
running; the original profile stays restored. No Quest/Thor run occurred.
