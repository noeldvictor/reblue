# Complete native DoF preparation and submission

2026-09-05, Windows Vulkan desktop, EDT. Source base `caa0e4e` plus this
checkpoint. This removes the supported DoF producer bodies, not the complete
post scheduler or the remaining Xbox 360 rendering model.

## Exact source and ownership

`sub_82217108` at 0x82217108 (`generated/reblue_recomp.22.cpp`) forces the
selected buffered mode to 2, writes four DoF parameters, selects/copies the
buffered world focus point, transforms it through current view/projection,
clamps a resolution divisor to [2,16], maintains two five-texture caches and
walks the downsample/blur producers. The complete body was inspected, including
the PPC comments. `sub_82179E50` and `sub_82188840` establish the thread-bank
selection; `sub_82216D90` confirms the property offsets at initialization.

The matching consumer, 0x82217DE8 in `.43.cpp`, is misleadingly named
`bdShadowStencilDrawIndexed`. It binds depth, scene and five blur levels,
allocates an intermediate colour target, draws a fullscreen quad and resolves/
unbinds those resources. It is the DoF composite, not evidence of stencil use.
The stale format comments were corrected without changing any depth formats.
The symbol name stays unchanged to avoid an unrelated guest regeneration.

`native_dof_bridge.cpp` now replaces both whole bodies. Its adapter supplies
typed `DofParameters`, the current native view/projection, authored properties
and explicit scene/depth images to `HostPostPrepareDof`. The host builds its
existing layered atlas directly; the matching consumer validates its owner,
source and frame and returns the original success value without a quad,
intermediate allocation, seed or resolve. The later combined composite reads
that atlas and retained scene. No normal native DoF input comes from PS c27.
Queues flush before layout changes and framebuffer caches are invalidated
after the native producer; skipped bindings cannot leave a falsely bound pass.

The scalar constants were checked against the owned XEX's in-memory decoded
PE image, without writing another image or changing assets: mode-2 factor
1.25 at 0x8208EEE8, blur multiplier 0.001 at 0x82061FB4, range multiplier
0.010001 at 0x8208EEE4, divisor limits 2/16 at 0x82058924/0x82062B1C.
The world-to-view-to-projection order and square-root strength setting are
preserved. IEEE exceptional focus values are not silently replaced with an
arbitrary focus distance. This conversion does not claim to cure VR blur.

## Build, tests and producer comparison

The existing Vulkan-only `reblue` target linked at 12:49:02, then again at
12:52:44 after interface/comment corrections. Both binaries are 47405568
bytes, embedded base `caa0e4e` with local changes. Codegen reported its module
up to date; no guest translation unit rebuilt. The second build emitted six
existing `getenv`/`fopen` deprecation warnings in resource/framebuffer code.
No generated source, shader, dependency or hook TOML changed.

All 26 CTests pass (25 texture/state/camera/post tests and one material test).
The new SDK-free test covers authored scaling, focus projection/translation,
exceptional clip w and wrong/repeated/cross-frame preparation consumption;
assertions remain enabled in release builds. Existing ten scene and three
reflection source guards pass, plus three new post source guards. Source guards
are not independent GPU or full ABI verification.

Diagnostic PID 21076 ran 12:49:51-12:52:06.212, `reblue_753.log`, with the
original five profile settings except capture delay 600, plus
`bd_native_dof_verify=true`. All six settings audited and the full install
mounted. This diagnostic deliberately executes original preparation/submission
and compares the native values against its publication. Last sampled total:
6901 parameter checks, zero mismatches/refusals. Field values were aperture
2.4, blur 0.075, range 0.50005, focus 0.987549305. No capture was requested
within that run's duration. This is a flat parameter comparison, not a VR
parameter comparison or native execution qualification.

## Normal flat capture

PID 24032 ran 12:53:33-12:55:57.052, `reblue_754.log`, using the 12:52:44
binary. Autoplay/perf on, delay 60, minimum 600, capture count 32; all five
settings audited, 1673 archives / 119346 names mounted. Native DoF defaults
on; verification, VR, native sun and other diagnostic overrides are off.

32 isolated hard-linked raw frames in `out/verification/native_dof_flat`:
`frame_1788627276_0.raw` through `frame_1788627277_31.raw`, frames 2837-2868,
12:54:36.128-12:54:37.116. Each is 1920x1080 / 8294420 bytes. Sequence analysis
has 0/31 jumps above 6%, maximum 2.82%; no cyan patch frames, median 0.011%.
The actual full-resolution first/last images were inspected: Shu, his cast
silhouette, foliage, distant scenery and moving windmill shadows remain.
This is about one second of capture, not a 120-frame stability qualification.

Last sampled totals: 7498 native preparations, 7497 consumed (sampled before
the matching consumer), three original preparations/draws/refusals during
startup/scene changes, no later field increase. Those three are not yet
attributed to a specific failed preflight check. No checked error/critical,
assertion/fatal/device-lost/VK_ERROR/exhaustion markers appeared.

## Normal desktop OpenXR capture

PID 23528 ran 12:57:21-13:00:29.569, `reblue_755.log`, the same 12:52:44
binary. All sixteen settings audited: autoplay/perf on, delay 60/minimum 450/
32 frames; native sun and shadow passes on; VR on, legacy stereo off;
multiview/layered textures on; scene-array capture and mirror off; camera
mode 2, diorama height 0, XR scale 1.0. Native DoF is on by default and its
diagnostic comparison is off. The full archive/name install mounted.

The process-only runtime manifest referenced the checked absolute 31232-byte
`reblue_xrsim.dll`; width 1440, height 1584 and simulated eye height 0 were
set only for the launched process. No global runtime setting changed.
Final layers/viewport are 1440x1584 with a 1:1 +0,+0 presentation rect.

32 final stacked captures in `out/verification/native_dof_vr`:
`frame_1788627503_0.raw` through `frame_1788627505_31.raw`, frames 8158-8189,
12:58:23.921-12:58:25.891. Each is 1440x3168 / 18247700 bytes. Sequence
analysis has 0/31 jumps above 6%, maximum 0.45%, no cyan patches/median/maximum.
Both first and last stereo analyses report correctly crossed depth: bands
44/52/62/72/82/90/95% give -1/-2/-3/-5/-6/-8/-9 pixels, near-far -8, spread 8.
Actual full-resolution first/last left/right images were all inspected:
village stairs, foreground ground, rocks, orange sky and moving windmill
geometry fill the eyes without letterboxing. Distant blur remains; Shu's
shadow is not qualified in this framing. This two-second window does not
supersede the longer coverage requirements or earlier late-scene failure.

Last sampled native DoF preparations/consumers are 14698/14697, three original
preparations/draws/refusals, no nonfinite focus or verification calls. Counters
were sampled before the matching consumer, not at balanced shutdown. Scene
ownership checks reached 108557 without mismatch/fallback. No checked error
markers appeared. Field focus logs now provide useful evidence: e.g.
1.01353812 and 1.00175095, outside the normalized visible depth interval.
That is a lead for investigating authored focus versus the composed VR camera,
not proof of the complete blur cause or permission to silently disable DoF.

All owned processes were stopped with checked path/PID/start-time identities.
The original five-setting profile was restored exactly (120 capture frames,
minimum 600, delay 60). Native sun remains off and XR scale 0.65 by default.
Free space was 623128576 bytes before the final PNG exports. No Quest/Thor run
occurred, and no headset performance claim follows from these desktop checks.

## Remaining boundaries

Engine property storage, thread banks, focus/camera sources, image containers,
source-surface aliases/scales and outer filter traversal remain adapters.
The bloom producer and its constant-register inputs still execute; the combined
composite still enters via the old ms_tex draw. Alternate DoF/host-post settings
and unavailable inputs retain counted compatibility execution. This checkpoint
does not qualify all views, nesting, animated focus, cutscenes or the later
scene failure, and it is not full native frame ownership.

The guest-source skill directed the exact producer investigation; devloop/vrsim
kept verification on the existing desktop build and desktop OpenXR. Disk space
required 32-frame checks instead of 120. Existing captures/logs are preserved;
hard links isolate current evidence without duplicating the raw payloads.
