# Native draw packets must remain authoritative through dispatch

2026-09-04, Windows Vulkan desktop. Follow-up to the alpha checkpoint's failed
multiview capture. The full host-renderer and desktop/both-eye gates remain open.

## Controls and source finding

With the same binary as `681e44e`, changed only `bd_host_draw=false` in the
previous 13-setting desktop multiview profile. All 14 settings passed audit in
`reblue_681.log`. PID 16544 ran 23:42:17-23:44:13 EDT. Its final deferred-consumer
report has 5273606 entries and **zero replays**. The isolated
`out/verification/replay_off_vr` sequence has 120 final stacked 936x2060 captures,
0/119 jumps above 6%, and no cyan patches. Inspected both eyes in sequences 0
and 119: the large horizontal corruption is absent, and the scene consistently
resembles the brief clearer frames of the replay-on captures. Blur and large
letterboxes remain; this is a correctness control, not the implementation target
or a complete stereo qualification.

Then restored replay with `bd_host_draw_verify_every=31` (14 settings), PID
23848, 23:45:43-23:48:17 EDT, `reblue_682.log`. Final comparator report: 486900
nodes, 580491 draws, 87142 flagged draws, but **zero pipeline-state mismatches**.
Other flags include 80086 texture differences, 6472 boolean differences, 1150
pixel-constant differences, 1019 vertex-constant differences and 274 geometry
differences. These are raw comparison counts, including inherited/unused fields,
not proof that every flagged field affects an image. Captures were preserved in
`out/verification/replay_verify_vr`. This is not a passing verifier run.

The source explains a hole in that comparison:

1. `HostDrawReplay` assigns the recorded pipeline and constants before calling
   `DispatchHostNodeDraw`. The verifier records its expectation at this point,
   then executes the original node; it never dispatches the expected pipeline.
2. `DispatchDraw` classified shaders using `VideoState::pixel_shader`, the
   engine binding history, instead of the packet's pixel shader.
3. `FlushRenderStateLocked` unconditionally replaced the packet's VS, PS and
   declaration with the engine mirrors, and applied the engine-origin live
   raster/blend/alpha intent over the packet. A preceding unrelated engine draw
   could therefore determine what a native packet actually rendered.
4. Shared vertex format decoding also used the engine declaration. Fixing only
   the PSO selection would still leave the shader reading the wrong format flags.

This is a host consumer/ownership bug. The repeated refresh-time visual change
was not evidence that the native mesh or material asset formats were wrong.
The prior alpha/blend/raster notes' ordinary-draw application counts also included
replay flushes: the assumed replay bypass did not exist in that code. Their
setter-publication comparisons remain evidence for setters, not packet binding.

## Implementation

`VideoState::native_draw_pipeline` explicitly identifies a host packet, separate
from constant overrides and engine bind history. Its producer binds the packet
pipeline and clears the pointer when restoring engine state. Native packet draws
do not execute the engine-origin draw-input import. They keep their shaders,
declaration and raster/blend/alpha policy through flush. Per-dispatch topology,
strides and framebuffer formats still compose from the actual draw/target.

Pixel shader classification, post/tail handling, queue hashes, diagnostics and
the shared vertex format flags all select the same authoritative packet input.
A null native binding remains null; it never borrows an unrelated engine shader.
Engine-origin draws still consume their own setters and binding history.

The SDK-independent `draw_intent.h` contract and new regression test were pushed
as `143a9cb`. All 11 standalone tests pass (0.49 seconds), including conflicting
engine/native programs and policy, target/topology composition, null bindings,
and return to engine state. The desktop `reblue` target linked successfully;
codegen reported zero writes/deletes and the guest module up to date. No shaders,
generated guest code, game assets or dependencies were changed; the existing
build tree was reused.

This does not make packet production fully native: retained draw recipes, engine
scene/pass sources, shader-register packing, dynamic resource inputs and many
other rendering producers remain. It fixes which input the host consumer obeys.

## First normal multiview check

PID 21784, 23:53:19-23:55:08 EDT, `reblue_683.log`. All 13 baseline multiview
settings passed audit: autoplay/perf CSV, capture after 60 seconds/minimum 450
draws/120 frames, VR on, old stereo off, multiview/layered textures on, scene-array
capture/mirror off, camera mode 2 and height 0. Replay and native conversions
were enabled normally, with no sampled verifier or replay-disable override.
The same process-local xrsim runtime advertised 1440x1584; actual final eyes
remained 936x1030 each, not the target resolution.

`out/verification/native_draw_intent_vr` has 120 final stacked eye captures,
frames 20162-20281. **0/119 jumps above 6%, zero cyan patches**. Inspected actual
sequences 0 and 119 and both eyes of `stereo.png`: the broad horizontal banding
and periodic clarity changes from the previous captures are absent. Remaining
blur, large letterboxes and limited near/far content do not establish a correct
VR view. Stereo check exits 2, INCONCLUSIVE: the 44% and 52% textured bands both
match at -1 pixel, with no measured disparity variation.

Final consumer report: 4109031 entries, 3604851 replayed, 499586 direct draws,
zero consumer fallback/refusals. This demonstrates replay stayed active during
the stable sequence; disabling replay is not the fix. No error/critical,
overflow, exhaustion or retirement-race messages were found. Native state
publication counters now correctly exclude native packet flushes, which no
longer consume that engine-origin state. A passing sequence is not proof of
all-scene stability or full-frame ownership.

## Normal flat check

PID 25332, 23:55:40-23:57:33 EDT, `reblue_684.log`. Restored all five original
profile settings (autoplay/perf CSV, capture after 60 seconds/minimum 600 draws,
120 frames); the log confirms all five took effect. No OpenXR override was used.
`out/verification/native_draw_intent_flat` contains 120 1920x1080 captures,
ending at frame 2960. It has 0/119 jumps above 6% and no cyan patches (maximum
0.02%). Inspected sequences 0 and 119: the character, cutout foliage, rocks,
terrain and windmill shadows are visible and stable in this field slice.

The final consumer report records 1272372 entries, 1045957 replayed and 222038
direct draws, with zero fallback/refusals. No error/critical, overflow,
exhaustion, retirement-race or Vulkan-failure matches were found. This is a
short field check, not evidence for the previously broken later scene. The
longer baseline is being rerun with its original 270-second delay/30-draw
threshold before any claim about that scene.
