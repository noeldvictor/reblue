# Native sun camera: restore character caster admission

2026-09-05, Windows Vulkan desktop, EDT. Source base `3e5c756` plus this
checkpoint. Supersedes the unresolved missing-Shu-shadow finding in
`20260905_0914_experimental-native-sun-camera.md`, not that note's remaining
ownership/coverage limits. Native sun camera remains **disabled by default**.

## Cause and bounded change

The character producer `sub_822D3598` applies a separate light-eye-distance
cutoff after its world-sphere tests. At 0x822D3AF8-0x822D3B10 it computes
`(-light_view_z - constant) * scale` and returns if this is at least one.
The native directional camera places its eye roughly 895 units upstream;
that padding is not a meaningful character visibility distance. Replacing
the sphere culler alone therefore could not restore the character caster.

An instruction adapter at 0x822D3AF8 now jumps to 0x822D3B14 only for view 1,
an active owned depth scope at the expected native pass nesting, and a current
native sun camera record. Other views, including view 8, and the default-off
camera retain the original path. Existing authored visibility/alpha logic and
the preceding sphere tests are not removed. The join starts a fresh compare;
it does not consume the skipped f12/f13 or condition-register values.

The hook reports skipped character cutoffs separately. This is a temporary
instruction adapter, **not a host replacement of character submission**.
No bias, light-position getter, shader, material, asset or dependency changed.

The guest-source skill directed reading the hook TOML and complete translated
`sub_822D3598`/`bdCameraRender` bodies with their exact PPC comments before
choosing the site and join. `bdSceneTreeDraw` and the active-view selection
helpers were also traced. Generated source was not hand-edited or committed.
The devloop skill kept verification in the existing desktop build tree.

## GPU controls

The inherited failing capture is `logs/renderdoc/reblue_frame3027.rdc`,
546383450 bytes (log 741). Its 1920x1080 MSAA4 scene image is resource 37420,
with 446 draws. Pixel history at Shu's face/shirt/boot identifies scene
events 2568/2590/2602, VS 9955 and PS 9958. Their sampled shadow image is
resource 1125. The scene was exported and inspected: Shu renders without
his cast silhouette. Matrix/sampled-depth evidence from the earlier note
still applies, but does not establish that a character caster was submitted.

A default-camera control, log 743, PID 24704, ran 09:34:52-09:37:29.097 using
the previous 09:12:30 / 47389696-byte binary. Its eight audited settings were
the original five plus RenderDoc enabled, delay 65 seconds and one frame.
It wrote `logs/renderdoc/reblue_frame2996.rdc`, 530372681 bytes, at 09:36:01.
The full install mounted 1673 archives / 119346 names. Both RDCs are preserved.

Default GPU replay has 303 shadow draws versus 743 in the failing native fit.
Character shadow VS 11199 draws include:

| Default shadow event | Index count | Index offset | Index buffer / stride |
| --- | --- | --- | --- |
| 319 | 489 | 2853 | 19441 / 2 |
| 323 | 1143 | 9008 | 19441 / 2 |

Those ranges match character meshes in the failing capture's scene, but are
absent from its shadow actions. Resource IDs are capture-local. Index identity
alone was initially inconclusive because cooked shadow LODs can use different
buffers; this actual default-camera GPU control and the source cutoff trace
resolve that ambiguity for the investigated character. They do not classify
every caster in the game.

Reports and inspected scene exports are in ignored directories
`out/verification/native_sun_character` and
`out/verification/native_sun_character_default`. Replay PIDs 24032 and 22912
were stopped after reports reached `done`, at 09:32:11.285 and 09:42:58.631.
Replay occurred without a running game. Scratch `out/native_sun_character_probe.py`
uses the installed RenderDoc API; RGB PNG conversions were lossless QA exports.

## Build and tests

`cmake --build --preset win-amd64-release --target reblue -j 4` succeeded.
The new instruction-site config caused codegen to report one written and 218
unchanged outputs; only guest TU 34 was rebuilt. The generated callback and
conditional jump were inspected. The binary is `reblue_vk.exe`, timestamp
09:42:38, 47391744 bytes, reporting `3e5c756` with local modifications.

All 23 CTests pass (22 native texture/state/camera tests plus one material test),
as do ten scene and three reflection source guards. The new guard checks the
hook site/join/register declaration and native scope predicates; it is not an
independent GPU, ABI or threading test. An initial `tomllib` import failed on
the installed pre-3.11 Python before building; the guard now uses the existing
dependency-free source-check approach, with no new Python dependency.

## Fixed-camera flat captures

Both runs use the same new binary, autoplay/perf logging, capture delay 60,
minimum 600 draws and 120 frames, plus `bd_native_sun_camera=true`. Log 744
also enables `bd_shadow_fit_diag`; log 745 does not. Their configuration audits
confirm seven and six applied settings respectively. Both mount the full
1673-archive / 119346-name install. Processes were stopped by verified path,
PID and start time before isolating/analyzing captures.

| Run | Process interval | Captured frames | Large jumps / 119 |
| --- | --- | --- | --- |
| Diagnostic: log 744 / PID 21748 | 09:43:40-09:45:45.679 | 2830-2949 | 2 |
| Normal: log 745 / PID 22388 | 09:53:24-09:54:50.722 | 2834-2953 | 0 |

Diagnostic captures: `out/verification/native_sun_character_fix_diag`,
`frame_1788615882_0.raw` through `frame_1788615886_119.raw`, 1920x1080.
Actual first/last and jump images were inspected. Shu's cast silhouette is
restored; moving windmill shadows overlap it in some frames. Transitions 2-3
and 3-4 exceed 6%, around a one-frame foliage/shadow change. This is not a
stable-sequence qualification. Cyan median 0.011%, maximum 0.02%, no patches.
First logged character light-view depths span -894.068 to -895.140.

Normal captures: `out/verification/native_sun_character_fix_flat`,
`frame_1788616466_0.raw` through `frame_1788616470_119.raw`, 09:54:26.800-
09:54:30.515, 1920x1080. There are 0/119 jumps over 6%, no cyan patches,
median cyan 0.012%, maximum 0.02%. Full-resolution first and last images were
inspected. The last clearly shows Shu's shadow extending from his feet to
the right. This is a short normal-path control, not proof of the diagnostic
flicker's cause or its absence in longer play.

Final sampled log-745 totals: 3901 native fits/snapshots, zero inactive/refused;
3901 shadow begins / 3900 ends, 3900 explicit outputs, 871 empty clears and
28173 matching ownership checks. Original camera snapshots, light fits and
cull comparisons are zero. Native culling tests 1737178 objects and admits
1726770; the character cutoff is skipped 3027 times. Diagnostic log 744 has
6301 native fits, 44965 matching ownership checks, 3268630 cull comparisons
(677817 changed) and 5435 skipped character cutoffs. These are mid-scope
samples, not balanced shutdown counts or complete-frame ownership.

Neither log 744 nor 745 contains an error/critical/assertion/fatal/device-lost
marker in the checked patterns. No performance improvement is claimed.

## Remaining gate

Normal full-size final-eye verification is still required before enabling
the native camera. Previously documented VR blur/letterboxing, inconclusive
depth, later scenery/text failures, other shadow modes, full character/scene
submission and complete desktop mode coverage remain open. No Quest or Thor
run occurred. The previous native-camera experiment remains opt-in; the default
still executes the explicitly counted engine snapshot/light fitter.
