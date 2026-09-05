# Explicit per-draw native skin bindings

2026-09-05, Windows Vulkan desktop. Follow-up to the late-scene failure in
`20260905_0010_native-draw-late-scene.md`. Full host rendering and desktop/both-eye
qualification remain incomplete.

## Source finding

The previous replay path stored one joint-slot table on `NodeTemplate`, recovered
after the entire interpreted node had finished. Direct-node slots were inferred
by finding equal pose matrices; deferred entries supplied explicit slot indices,
but only the first matrix was checked. Every sub-draw then gathered from that
same table. Equal poses do not identify joints, and one final table cannot
represent sub-draws with different bindings. The replay verifier additionally
skipped c60-c155, so its earlier reports did not qualify skin palette equality.

Exact source read via the guest-source skill:

- `bdSceneNodeDrawSingle`, `generated/reblue_recomp.40.cpp`, especially
  0x82280D8C-0x82280EF0: the 0x02xx command replaces the current joint-index
  table; matrix gathering copies each indexed 64-byte palette slot. Direct
  commands refuse more than 24 matrices.
- `sub_82286BC0`, `generated/reblue_recomp.23.cpp`: publishes `count * 4`
  registers at the requested base unless the engine-mode byte is 1. It does
  not infer identity or transpose the gathered matrix.
- `config/hooks/render_list.toml`, `gpu/hooks/scene_node.cpp` and the native
  deferred consumer: deferred entries carry the table at +800, count at +289
  and palette at +268. The existing consumer adapter validates up to 49 matrices.
- `config/hooks/frame_interp.toml`: the separate object palette interpolation
  hook is in `bdSceneNodeProcessRenderCmds`, not the direct-node token gather.

No generated source, binary/decompiler output or hook addresses were changed.

## Native boundary

`NativeSkinBinding` contains bounded model-local joint indices, not guest
addresses or pose-derived identities. The shared command decoder keeps a
binding on each geometry range; unknown and explicitly unskinned are distinct.
The standalone gather is independent of the SDK, guest memory, matrix packing
and graphics API, and leaves its destination untouched on a failed source load.

The foundation and regression tests are pushed as `ef35cc7`. Tests cover equal
initial poses that later diverge, multiple bindings for one geometry range,
explicit reset, unknown inputs, capacity and truncated-command failures, and
transactional gather failure. The existing native-material suite passes; all
11 separate texture/lifetime/upload/state/packet tests also pass (0.51 seconds).

Renderer integration imports explicit model-command bindings for supported
direct phase-0 draws and entry joint tables for deferred draws. Each sub-draw
keeps its own binding. Capture checks every supplied matrix against that draw's
actual input; unsupported/ambiguous cases remain a counted compatibility boundary.
Replay preflights and owns the current gathered poses before issuing any draw.
The final-node table and matrix-value search are removed. The verifier no longer
zeros or skips the bone-register range. It still compares composition rather
than proving the entire downstream consumer or inherited engine state correct.

The desktop `reblue` target linked successfully in the existing build tree;
codegen reports zero writes/deletes and the guest module up to date. No guest
objects were rebuilt. The devloop skill kept verification on this desktop path.

This is not complete host animation/skinning: skeleton evaluation and pose
sources remain engine-produced, list storage and discovery remain adapters,
and packing still targets the existing vertex shader register ABI. Persistent
skeleton/skin scene assets and a dedicated native GPU palette remain required.

## Replay-off late-scene control

Before the integration build, `reblue_686.log`, PID 25516, ran 00:12:52-00:18:57
EDT using the same binary as the prior `d2824b2` verification. Six settings all
passed audit: the five late-scene values (autoplay/perf CSV, delay 270 seconds,
minimum 30 draws, 120 captures) plus `bd_host_draw=false`. This was a correctness
control only; disabling replay is not the conversion target.

The isolated `out/verification/native_skin_replay_off_late` sequence contains
120 1920x1080 final captures, frames 14675-14794 at 00:18:17.708-00:18:21.126.
Analysis finds 100/119 jumps over 6%, zero cyan patches (median/max 0%). Inspected
sequences 1/2 and 43/44: the character stays intact instead of stretching across
the frame, and the background rocks remain visible in the latter pair. Text
still appears damaged; motion and remaining changes mean this is not a complete
scene pass. A downscaled preview of the actual 43/44/diff image was used because
the image viewer could not decode the larger diagnostic PNG directly.

The final consumer report has 3799984 entries, zero replayed and 3724826 direct
draws, zero fallback/refusals. The process was stopped only after its complete
capture and the original five-setting profile restored before the next run.

## Normal replay: late-scene check

`reblue_687.log`, PID 24596, 00:24:10-00:30:20 EDT, all five late-scene settings
audited. Replay stayed enabled; there was no sampled verifier override. The
120 final 1920x1080 captures at `out/verification/native_skin_late_flat` cover
frames 14705-14824, 00:29:36.140-00:29:39.521.

There are **110/119 jumps above 6%** and no cyan patches (median/max 0%). This is
not an aggregate stability improvement over either prior failed sequence.
Inspected pairs 1/2, 43/44 and 118/119: the earlier extreme character stretching
is absent and the character remains intact through the sampled motion. However,
background rock surfaces still disappear between frames and text remains damaged.
The later scene therefore still fails correctness. Near-coincident characters
and other unsampled animation paths are not qualified by these checks either.

Last skin report: 787878 imported palette checks, **zero mismatches**, 46561
unsupported inputs; 481158 replayed palettes / 4437535 joints. Unsupported counts
are draw attempts, not distinct assets. Final consumer: 3479083 entries, 2767312
replayed, 698855 direct draws, zero consumer fallback/refusals. This establishes
that both native skin gathering and ordinary replay were exercised, not that the
frame has no engine dependencies. No error/critical, overflow/exhaustion,
retirement-race, VK_ERROR or Vulkan-failure messages were found.

## Normal replay: final-eye multiview check

`reblue_688.log`, PID 24808, 00:31:13-00:33:24 EDT. Used vrsim with the existing
absolute runtime manifest, process-local environment (1440x1584 recommended eyes,
head height 0), no registry changes and no headset. All 13 standard profile
settings audited: autoplay/perf CSV, 60-second delay/minimum 450 draws/120
captures, VR on, legacy stereo off, multiview/layered textures on, scene-array
capture/mirror off, camera mode 2 and diorama height 0. Instance/session creation
and different game/eye positions confirm XR camera composition.

Actual final eyes remain 936x1030 each. The isolated `native_skin_vr` sequence
contains 120 stacked 936x2060 captures, frames 20898-21017 at
00:32:15.768-00:32:21.049: **0/119 jumps above 6%, no cyan patches**. Inspected
both eyes at sequences 0 and 119: no broad horizontal corruption; blur and large
letterboxes remain. Both stereo checks return INCONCLUSIVE with the 44% and 52%
textured bands at -1 pixel each and zero disparity spread. This is not target
resolution, stereo-depth, full-game or headset-performance qualification.

Last skin report: 167154 checks, zero mismatches, 7740 unsupported inputs,
1222675 replayed palettes / 5606458 joints. Final consumer: 5853342 entries,
5147280 replayed, 699459 direct draws, zero fallback/refusals. No error/critical,
overflow/exhaustion, retirement-race, VK_ERROR or Vulkan-failure matches.
Stopped only the completed test process and restored the original five-value
flat profile. No renderer remains running. The next correctness work remains
the disappearing late-scene surfaces/text and their retained scene/pass inputs;
Quest is still deferred until the full desktop host-renderer gate.
