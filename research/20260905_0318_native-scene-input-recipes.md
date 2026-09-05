# Native scene-image draw recipes

2026-09-05, Windows Vulkan desktop. Follow-up to
`20260905_0301_native-scene-textures.md`.

## Source and ownership

The guest-source guide directed inspection of the render hook TOML and exact
translated source. `bdSceneNodeDrawSingle` (generated file 40) calls the active
draw callback at `((uint32_t(-32036) << 16) - 22280) + 36` before direct indexed
draws, conditionally on its material-change flag. The callback chain through
`sub_8221D530` / `sub_8221DB00` invokes visual vtable +32 after global callbacks.
`sub_8221E618` selects and binds current/next scene images, directly or through
the blend/constant wrapper `sub_82454C08`. Model reflection selection alone
cannot describe the final slot-5 input. The node also has ordinary and animated
texture writes, and can change active render-target selection between commands.

The new `SceneTextureRecipe` records named current/next roles and producer kind,
not an image address, old image handle or guessed match against another draw.
The host producer emits each semantic event only after its non-null texture
publication. An ordinary texture write clears that slot's semantic role even
if the image pointer does not change. Null selection remains a no-op. Each
draw snapshots its current role recipe; later writes do not mutate that draw.
New nodes do not infer roles from the previous node's bindings.

During capture, actual bindings are compared with the already prepared native
producer inputs. No texture-registry lookup occurs under the draw/video lock.
The node-entry logical selections must agree with the publication-time values;
otherwise the whole node remains explicitly unsupported until its intra-node
pass changes have a native sequence. Unknown callback owners are also counted
as unsupported, not silently converted by image-identity guessing.

After source verification and compatibility visual-history updates, converted
slots discard their retained native handles, wrappers, addresses and surface
inheritance flags. Replay resolves the current pair outside video/store locks
and preflights all draws before dispatch. It checks today's producer kind,
composes the requested live inputs, and supplies them to sampler composition,
packet overrides and the replay comparator. A requested null or unresolved
input refuses the complete node. Merge and material-census identity includes
the role recipe; different producers/roles cannot become the same recipe.

This removes retained scene-image selection from converted draw templates,
not all replay/template dependencies. Scene-table and persistent association
production, dynamic image ownership, native pass sequences, the wrapper's
blend/constants, shader ABI, other material inputs and full-frame scheduling
remain unconverted. Callback/global state inherited by later interpreted nodes
still requires independent qualification. Model reflection enable remains its
own contract; a scene image callback is not treated as a new enable producer.

## Initial verification

- All 15 texture/upload/state/recipe CTests pass (0.54 seconds), including the
  new SDK-independent semantic-role test. The material CTest also passes.
- Tests cover current/next mapping, repeated publication, ordinary overrides,
  null no-op, unrelated slots, independent draw/node snapshots, changing live
  native/dynamic inputs, atomic missing-input refusal and callback decoding.
- All three reflection/scene source-boundary guards pass. These are not a
  runtime concurrency proof.
- Host-only Vulkan build linked at 03:17:58 EDT. Codegen wrote/deleted nothing,
  and no guest objects rebuilt. Logged source revision is `3520ba5` dirty.

The first run uses autoplay/perf, capture delay 180, minimum 30 draws, 120
frames and `bd_host_draw_verify_every=8`. Scene producer comparison is off.
Runtime coverage and pixel inspection are pending at this implementation
checkpoint; tests alone do not qualify replay, later scenes or VR.

## Selection-path guard and first runtime evidence

Review after `cb9c342` found that comparing only selected image addresses could
miss an intra-node table/offset change when two rows currently alias the same
image. The transient source stamp now includes scene/active tables, count,
active offset, effective index, selected source word and image. The full stamp
must agree between node entry and publication. None of it enters the persisted
role recipe. Added regression cases keep both images identical while changing
the active rows or relocating their array; both changes are detected.

The strengthened build linked at 03:23:51 EDT (`cb9c342` dirty), again with
zero codegen writes/deletions and no guest object rebuild. All 15 texture/state
CTests and three source-boundary guards still pass.

The first sampled run used the 03:17:58 binary, before this stamp strengthening:
PID 19296, 03:19:33-03:23:40 EDT, `reblue_705.log`. All six settings were audited
as effective. It recorded 3414 scene-input source comparisons with zero wrong
or unsupported inputs, and 11613 composed scene-role draws / 23226 native
inputs, with zero dynamic inputs or preflight refusals. Composition counts
include diagnostic candidates, not only dispatched draws. The scene producer
recorded 1707 pairs / 3414 native inputs and zero comparison/compatibility calls.

The general sampled replay comparator still fails: its final report has
569028 draws compared, 183381 wrong, including unrelated material/texture,
constant, geometry and bool fields. The bounded examples contain no slot-5/10
difference, but that absence is not a complete per-role comparator census.
Source-role checks do not establish that all replay inputs or later inherited
state are correct. No error/critical, Vulkan error, overflow or exhaustion
lines were found.

The complete early flat sequence is isolated in
`out/verification/native_scene_roles_sampled`, from
`frame_1788592956_0.raw` to `frame_1788592960_119.raw`, 1920x1080. Its inspected
endpoints show the field path and village/title transition, not the later
scenery/text failure. Cyan analysis reports 50/120 frames above 0.30%, median
0.003%, maximum 1.23%, zero 2-60% patches and zero whole-frame detections.
Sequence-difference analysis completed after the second checkpoint: 115/119
pairs exceed 6%. Inspected pairs 064/094 show camera/title movement without
broad missing-rock flashes; this moving transition is not the normal late
failure and its high change count is not a count of confirmed rendering faults.

The strengthened normal run (PID 18528, started 03:24:29 EDT, `reblue_706.log`)
has both comparison modes off and all five settings audited: autoplay/perf,
270-second delay, minimum 30 draws and 120 frames. Its late-scene result is
recorded below; the diagnostic run above does not stand in for it.

## Completed normal late scene

PID 18528 ran 03:24:29-03:30:57 EDT with the 03:23:51 binary, code now committed
as `8bf2240`. `reblue_706.log` shows that the capture correctly held through a
20-draw loading screen, then completed all 120 frames: 14672-14791,
03:29:54.483-03:29:57.909. The isolated directory is
`out/verification/native_scene_roles_late_flat`, from
`frame_1788593394_0.raw` to `frame_1788593397_119.raw`, 1920x1080.

Normal source/composition coverage:

- Scene inputs: 34 checks, zero wrong/unsupported/refused; 13133 composed and
  dispatched scene-role draws, 26266 native inputs, zero dynamic inputs.
- Scene producer: 17 pairs / 34 native inputs; zero comparison, compatibility
  or refusal calls. Dynamic and null publication paths were not exercised.
- Reflection: 991895 checks, zero wrong/unsupported/refused; 1855 enabled
  pass-default cases, no table cases; 2780233 composed native bindings.
- Lighting: 47355 publications, zero compatibility/refusal/reset calls,
  969620 matching direct shadow-input checks.
- Deferred consumer: 3633200 entries, 2843288 replays, 776919 direct draws,
  zero fallback/refusal. Its other engine adapters remain counted.
- No error/critical, Vulkan error, overflow or exhaustion lines were found.

Sequence analysis reports 113/119 changes above 6%, with no cyan detections
(median/max 0%). The first frame is a dark transition and the last shows the
villagers/platform. Inspected pairs 080 and 104 still show large rock-wall
surfaces disappearing between frames. Thus scenery correctness still fails;
the zero source-check mismatches do not qualify these pixels. Prior text
failures also remain unqualified, not fixed by this capture. No performance
claim is made from frame timing or the difference count.

## Full-size desktop final eyes

The vrsim guide directed use of the local headless OpenXR runtime and final-eye
capture. Its existing manifest has an absolute DLL path. PID 24840 ran
03:31:01-03:33:21 EDT with the same 03:23:51 renderer, `reblue_707.log`.
Process-only environment: runtime `out/xrsim-build/reblue_xrsim.json`, width
1440, height 1584, simulated head height 0 metres. All 14 profile settings were
audited as effective:

```toml
bd_xr_autoplay = true
bd_perf_csv = true
bd_capture_after_s = 60
bd_capture_min_draws = 450
bd_capture_frames = 120
bd_vr_enabled = true
bd_stereo = false
bd_stereo_multiview = true
bd_mv_layered_textures = true
bd_mv_capture_array = false
bd_xr_mirror = false
bd_vr_camera_mode = 2
bd_vr_diorama_height = 0
bd_xr_render_scale = 1.0
```

This deliberately uses scale 1.0 instead of the existing 0.65 default; it is
not a same-settings performance comparison with earlier VR runs. The log
confirms a 1440x1584x2 layered swapchain and final direct presentation.
Scene content is still fitted to 1440x808 and letterboxed, not a full-height
native VR camera. The default scale was not changed.

All 120 captures are in `out/verification/native_scene_roles_vr_fullsize`,
`frame_1788593523_0.raw` through `frame_1788593543_119.raw`: 1440x3168 stacked,
frames 12342-12461, 03:32:03.851-03:32:23.963 EDT. Analysis reports 0/119 changes
above 6% and no cyan (median/max 0%). Both eyes in both endpoints were inspected:
no broad banding, but blur and large black bars remain. Both stereo checks are
INCONCLUSIVE: only 44%/52% bands usable, disparities -1/-2 pixels, spread 1.
Matching final layer dimensions is not depth, framing, comfort or 72 Hz proof.

This view exercised zero scene-image publications or scene-role draws; the
selectors ran without comparisons/refusals. It is a general final-eye check,
not VR GPU coverage of the new callback roles. Reflection has 112307 matching
disabled pass-default checks and 7532558 composed native bindings. Lighting
has 37328 publications and 112319 matching direct input checks, no
compatibility/refusal/reset calls. Consumer: 5037764 entries / 4456617 replays /
575454 direct draws, zero fallback/refusal. No error/critical, Vulkan error,
overflow or exhaustion lines were found. Analysis work overlapped the VR run;
no timing or performance result is claimed.

All owned renderers stopped after their captures completed. The exact original
five-setting profile is restored: autoplay/perf true, capture delay 60, minimum
600 draws, 120 frames. No game assets or saves were manually edited or included
in commits, and no device was used.
Full desktop scenes, persistent host scene/pass/material producers and the
Quest gate remain unfinished.
