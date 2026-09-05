# Native per-view ownership of tracked camera composition

2026-09-05, Windows Vulkan desktop, EDT. Source base `6f95d51` plus this
checkpoint. This fixes a host camera ownership error on top of the full-eye
geometry change; the full renderer conversion remains incomplete.

## Source evidence and change

`frame_interp.cpp` applied `ComposeView` whenever the generic matrix setter
received nonzero r4. That setter also receives light cameras and identity
resets for 2D/post work. Each call submits a game camera and advances the XR
anchor smoother; those unrelated matrices therefore changed both tracking
state and the matrices consumed by later rendering.

The original `sub_82217108` DoF producer (translated source in
`generated/reblue_recomp.22.cpp`) transforms its focus point through the
current view/projection cache, divides clip z by w, and writes c27.w.
The generated `bd_pe_ps_dof` shader and the host composite consume it as
normalized focus depth. Log 748's first three composites printed focus
0, 45.3, 64.4. This was a useful symptom of applying a scene eye to an
unrelated matrix; it is not a complete diagnosis of all remaining blur.

`bdRenderViewSubmit` reads the render camera from descriptor +8. Its
translated body is in `generated/reblue_recomp.16.cpp`; the outer
`bdCameraRenderSetup` in `.46.cpp` supplies its embedded camera at +432.
Both the scene and shadow begin producers read that camera's view at +160
and projection at +224. Their original callers and current host bodies
were inspected. The outer setup object's interpolation offsets must not
be confused with this submitted render camera.

The new SDK-independent `ViewCompositionScope` accepts only that explicit
source pair, composes it once, and returns the same native matrix to the
remaining consumers in the submission. A raw entry wrapper establishes the
scope and restores an enclosing scope on exit. It runs independently of the
interpolation setting. Other matrix writes do not invoke XR composition.
No guest camera memory is overwritten, and no generated source, hook TOML,
shader, dependency or game asset was changed.

This retains the original view scheduler and temporary descriptor tokens.
Native camera/controller data, interpolation, tracked reflection derivation,
focus production and complete frame ownership remain required. A generic
matrix write is no longer treated as an implicit scene-camera submission.

## Build and tests

The existing Vulkan-only `reblue` target linked successfully at 12:08:42:
`reblue_vk.exe`, 47391744 bytes, embedded base `6f95d51` with local changes.
CMake reconfigured for the added header; codegen reported its module up to
date and no guest translation unit rebuilt. All 25 CTests pass (24 texture/
state/camera/output tests plus one material test), as do the existing ten
scene and three reflection source guards.

The new test checks wrong/absent camera pairs, disabled tracking, exactly
one composition on repeated consumers, independent nested owners, preserved
outer results and recovery after a rejected composition. These unit checks
do not independently establish all GPU modes or thread-safety. The
guest-source skill directed the exact translated-source investigation;
devloop/vrsim kept build and runtime verification on desktop.

## Desktop OpenXR, normal effects and opt-in native sun

An initial launch, PID 22152 / log 750, was stopped at 12:10:33.456 before
capture: the config audit rejected the misspelled `bd_vr` setting and ran
flat. It is excluded from VR evidence. The corrected setting is
`bd_vr_enabled = true`.

PID 14864 ran 12:10:38-12:12:59.935, log `reblue_751.log`. All sixteen
settings audited: autoplay/perf on, capture delay 60/minimum 450/120 frames;
native sun/shadow passes on; VR on; legacy stereo off; multiview/layered
textures on; scene-array capture/mirror off; camera mode 2, diorama height
0, XR scale 1.0. Comparisons/debug overrides are off. The full 1673 archives
and 119346 names mounted. The process-only absolute runtime manifest/DLL
were verified; `XRSIM_WIDTH=1440`, `XRSIM_HEIGHT_PX=1584`, `XRSIM_HEIGHT=0`.

Scene and final eyes remain 1440x1584, with a 1:1 +0,+0 present viewport.
The 120 final stacked captures in `out/verification/scoped_camera_vr` are
`frame_1788624700_0.raw` through `frame_1788624715_119.raw`, frames
7907-8026, written 12:11:40.841-12:11:55.573. Sequence analysis has 0/119
pairs above 6% (maximum 0.64%); cyan patches/median/maximum are zero.
Both first and last full-resolution left/right images were inspected:
village stairs, foreground ground and distant rocks fill the eyes without
letterboxing. Distant scenery remains visibly blurred. The old large
foreground passage is absent in this corrected camera framing; this is
not evidence that every earlier intermittent defect is fixed.

Both first/last stereo analyzer runs exit 0: bands 44/52/62/72/82/90/95%
give -1/-2/-3/-5/-6/-8/-9 pixels, correctly crossed near/far disparity with
spread 8. This qualifies depth in this short scene, not headset comfort or
full-game stereo. Shu's cast shadow is not qualified in this VR framing.
The first three composite focus values are now 0/0/0; this startup sample
does not establish field/cutscene focus correctness. Field camera logs now
track the actual position, e.g. game (20.1,151.4,23.1), eye (16.9,151.4,23.1).

Last sampled totals: 11401 native sun fits/snapshots, 85510 matching shadow
and 85509 scene ownership checks, no original camera snapshot/light fits or
lifecycle fallback. Native views have 28567 productions with no imports or
bootstrap. Counts include loading and are sampled mid-scope, not proof of
balanced shutdown or complete guest removal. No checked error/critical,
assertion/fatal/device-lost/VK_ERROR/exhaustion markers were found.

## Original flat control and restoration

The original five-setting profile was restored exactly: autoplay/perf on,
capture delay 60/minimum 600/120 frames. Native sun, VR and diagnostics are
off. PID 17720 ran 12:14:19-12:16:00.957, log `reblue_752.log`; all five
settings audited and the full archive/name install mounted.

The 120 1920x1080 captures in `out/verification/scoped_camera_flat` are
`frame_1788624921_0.raw` through `frame_1788624924_119.raw`, frames
2837-2956, 12:15:21.553-12:15:24.854. There are 0/119 jumps above 6%, no
cyan patches, median cyan 0.011%, maximum 0.02%. First/last full-resolution
images show Shu and his cast silhouette, with the moving windmill shadow.
Last sampled shadow/scene ownership checks are 31474/36574 with no fallback;
default camera snapshots/light fits remain 5101 each. Native views have
14439 productions, no imports/bootstrap. The same error-marker checks are
clear. This short control does not supersede the earlier late-scene failure.

Processes were stopped using checked path/PID/start-time identities. Raw
sequences were isolated with hard links, not duplicated or deleted. The
original profile is restored and no owned renderer remains running. Native
sun stays off by default, XR scale stays 0.65, and no Quest/Thor run occurred.
