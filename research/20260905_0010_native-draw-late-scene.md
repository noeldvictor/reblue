# Native packet ownership: late-scene failure remains

2026-09-05, Windows Vulkan desktop. Follow-up to
`20260904_2348_native-draw-intent.md`. The tested native packet consumer fix is
committed and pushed as `d2824b2`; its independent ownership contract/test is
`143a9cb`. No code changed between the short flat/multiview checks and this run.

## Run and evidence

`reblue_685.log`, PID 25288, ran 00:01:33-00:07:44 EDT. Used the existing desktop
binary and install, with no OpenXR override. The log confirms VR disabled and
1673 shipped archives / 119346 record names. All five profile values passed audit:

```toml
bd_xr_autoplay = true
bd_perf_csv = true
bd_capture_after_s = 270
bd_capture_min_draws = 30
bd_capture_frames = 120
```

These match the earlier late-scene baseline's delay/draw threshold. The run
waited in the 20-draw loading section, then captured frames 14623-14742 at
00:06:58.344-00:07:01.846. Only this process was stopped after the complete
sequence. The 120 final 1920x1080 raw captures were isolated by run start time in
`out/verification/native_draw_intent_late_flat`. Restored the original five-value
profile (60-second delay, minimum 600 draws). No renderer process remains.

## Pixels and limits

Sequence analysis completes successfully with **106/119 pairs above the 6%
jump threshold**, versus 73/119 in the original upload-page late baseline
(later pre-fix checkpoints also failed). This is not a stability improvement;
the exact per-frame causes still require isolation.

The scene still fails correctness. Inspected `jump_002.png` (sequences 1/2): an
intact character changes into grossly deformed geometry. The same earlier
baseline pair shows dark/missing geometry; its exact rendering differs, so this
is not a pixel-identical replay comparison. Inspected `jump_044.png` (43/44):
character geometry stretches across the frame, text is damaged, and background
rock surfaces disappear between frames. These are not ordinary camera motion.
Cyan analysis finds zero patches, with median/max 0.000%/0.00%; absence of that
one artifact does not qualify these pixels.

The final consumer report records 3450523 entries, 2731409 replayed and 706511
direct draws, with zero fallback/refusals. Engine adapter counts remain large:
visual 1230231, material 1438228, state 4506831, world 719114 and resource 3205789.
These are boundary-call counts, not a precise guest-instruction census.

No error/critical, overflow, exhaustion, retirement-race, VK_ERROR or
Vulkan-failure matches were found. The last upload report shows 8388608 reserved
bytes, a 46137344-byte total peak / 29975808-byte slot peak, 22 pages created /
20 retired and zero failures. Shader storage peaked at 3640672 / 33554432 bytes.
Allocation health is not geometry correctness, and the exact remaining visual
cause has not been isolated by this run.

## Next boundaries

The packet consumer fix removes broad banding and large jumps in its short
multiview capture, with replay enabled. It does not resolve this later scene or
replace retained packet recipes, engine scene/pass/animation inputs or the
shader-register ABI. This cutscene/transition remains an explicit desktop gate.

Separate read-only tracing explains the existing below-target VR size:
`gpu/settings.cpp` defaults `bd_xr_render_scale` to 0.65, and `gpu/output.cpp`
and `gpu/present.cpp` apply that scale to the runtime's recommended eye size.
1440x1584 consequently becomes 936x1030; the content is also fitted to the
game's aspect. This is an existing policy, not evidence the runtime refused the
target. No scale/default change was mixed into the packet correctness comparison.
Target-resolution, near/far stereo, full-scene qualification and complete native
ownership are still required before Quest work.
