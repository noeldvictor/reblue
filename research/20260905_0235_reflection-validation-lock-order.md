# Reflection validation lock ordering

2026-09-05, Windows desktop. Follow-up to
`20260905_0144_native-reflection-selection.md`, code checkpoint `f60aef5`.

## Observed failure and source

The normal late run, `reblue_701.log`, PID 21460, ran 02:21:31-02:33:46 EDT.
All five settings were audited: autoplay/perf on, capture delay 270 seconds,
minimum 30 draws and 120 frames. No replay comparison override was enabled.
It stopped advancing at frame 10087 during loading, before any capture files.
The process remained alive and the audio timer continued logging Sleep calls.
Its last reflection report had 440992 checks, zero mismatches, five unsupported
draws, no lookup/null refusals and 1246229 composed bindings, all native.
Those counters do not qualify a stalled frame or the later scene.

A non-invasive CDB thread snapshot at approximately 02:31 EDT was saved to
`out/verification/reflection_701_stacks.txt`. The debugger detached normally.
The relevant stacks were:

- Draw thread 23600 (`5C30`): `DispatchDraw -> HostDrawCapture ->
  ResolveReflectionBinding -> ResolveGuestTexture`, waiting for the mirror mutex.
- File01 thread 21180 (`52BC`): `GetOrCreateNativeMirror -> Build2DMirror ->
  BuildNativeMipTexture -> BuildNativeTexture -> AcquireNativeTextureGpu`,
  waiting for `VideoState::mutex`.
- Other file threads waited for the mirror mutex; the main game thread waited
  in `bdEnqueueRead` while loading a visual/model.

The source closes the lock cycle: `src/gpu/hooks/draw.cpp` takes the video mutex
before calling capture; `native_texture_mirror.cpp` holds its registry mutex
through mirror creation; `native_texture_gpu.cpp` takes the video mutex for
upload. The new capture-time registry lookup inverted that existing order.
This was a confirmed deadlock, not an observation timeout or a GPU-error log.
Only the owned renderer process was stopped after the evidence was collected.

## Correction

Capture now snapshots the selected logical address and actual native binding
while draw state is protected. It does not look up the registry. Node commit,
after the draw hook releases the video mutex and before taking the template
store lock, resolves that captured address and performs the source comparison.
Failure still refuses the complete node before template publication. It does
not disable checks, guess a previous image or turn lock contention into a
successful binding. Pending checks are cleared at commit and new-node reset.

The table selection is fixed at draw time, not recalculated at node commit.
Native handles preserve the actual image through comparison. Dynamic wrappers
remain the existing short-lived compatibility/lifetime boundary; this change
does not replace that boundary or the complete native scene association work.
Replay preflight still resolves current bindings outside the video lock.

## Initial verification

- The host-only renderer built and linked successfully; codegen wrote/deleted
  nothing and no guest objects rebuilt. Version-dependent host objects also
  rebuilt after CMake refreshed the revision stamp.
- Extended material CTest: 1/1 passed (0.04 seconds), including a table-row change
  after selection that must not mutate the captured address.
- Existing texture/upload/state/verification/lighting CTests: 13/13 passed
  (0.62 seconds).
- `python tools/reflection_lock_order_test.py`: two checks passed. These are
  explicitly source-boundary regression guards, not a runtime concurrency proof.

The first two sandboxed build jobs made no compiler progress and were stopped;
the separately authorized builds above completed. At code checkpoint `29d492e`,
a fresh normal late-scene run was in progress. Its completed evidence follows;
the full desktop host-renderer goal remains open.

## Corrected normal late run

`reblue_702.log`, PID 24972, 02:38:24-02:44:53 EDT. Same five audited settings
as the failed normal run; no replay sampling/comparison override. Binary linked
02:37:17, reporting `f60aef5` dirty, contains the code subsequently pushed as
`29d492e`. No code edits occurred between that link and either normal run here.

The renderer advanced beyond the previous deadlock, kept drawing during loading
and held capture while only 20 draws were present. It then produced all 120
1920x1080 captures: frames 14193-14312, 02:43:41.737-02:43:45.098, isolated in
`out/verification/native_reflection_late_flat`. Raw endpoints are
`frame_1788590621_0.raw` and `frame_1788590625_119.raw`.

Final reflection report: **1214021 checks, zero mismatches**, six unsupported
draws, no lookup/null refusals; 2500252 composed bindings, all native. All
selections were pass-default; 6701 checked draws enabled reflection. That adds
some enabled-input coverage, not a qualified reflection effect or table/dynamic
path. Lighting: 46727 host publications, no compatibility/reset/refusal calls;
1192168 direct shadow-input checks, zero wrong. Consumer: 3310475 entries,
2589480 replayed, 706931 direct draws, no fallback/refusals. Skin: 1101986 checks,
zero wrong, 60185 unsupported; 557726 replayed palettes / 5056816 joints.

Sequence analysis completed: **108/119 changes over 6%**. No cyan patches,
median/max 0%. Inspected first/last images and adjacent pairs 1/2, 28/29,
79/80 and 103/104. The sequence starts with a dark transition, then follows
villagers on a wooden platform during a camera move. Characters no longer
stretch, but later pairs still show large rock-wall surfaces appearing and
disappearing and damaged text. Early pairs looked intact; they were not a
substitute for inspecting the later pairs. Camera motion also contributes to
the jump metric, so its count alone is not a flicker diagnosis or a speed result.

This verifies progress through the previously deadlocked loading transition,
not long-session deadlock freedom or visual correctness. The prior scenery/text
failure remains open; the binding-source counters do not establish its cause.

## Normal desktop multiview

The vrsim guide supplied the process-scoped runtime setup and final-eye checks;
no headset or system-runtime changes were used. `reblue_703.log`, PID 22592,
02:44:58-02:46:59 EDT. All 13 profile settings audited:

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
```

`XR_RUNTIME_JSON` named the absolute `out/xrsim-build/reblue_xrsim.json`;
`XRSIM_WIDTH=1440`, `XRSIM_HEIGHT_PX=1584`, `XRSIM_HEIGHT=0`. The runtime and
session initialized, and the logged eye differed from the game camera. Actual
swapchain eyes were still **936x1030**, not the 1440x1584 target.

All 120 final-eye captures, 936x2060 stacked, are isolated in
`out/verification/native_reflection_vr`: frames 20754-20873,
02:46:01.256-02:46:06.443. Raw endpoints are `frame_1788590761_0.raw` and
`frame_1788590766_119.raw`. Sequence analysis: **0/119 jumps over 6%**;
no cyan patches, median/max 0%. Both first and last stacked eye images were
inspected: no broad banding, but persistent blur and large black bars.
`stereo_check.py --raw ... --stacked` was **INCONCLUSIVE** for both endpoints:
the only usable bands (44%/52%) were both -1 pixel, with zero disparity spread.
Stable pixels do not qualify stereo depth, eye sizing or readability.

Final reflection report: 118347 checks, zero mismatches/unsupported/refusals,
7917245 composed bindings, all native; pass-default, disabled selection only.
Lighting: 54384 host publications, no compatibility/reset/refusal calls;
115872 matching direct inputs. Consumer: 5168252 entries, 4572040 replayed,
590372 direct draws, no fallback/refusals. No error/critical, VK_ERROR, overflow
or exhaustion matches were found in either normal-run log.

Only the owned test processes were stopped, after their complete captures.
The original five-line desktop profile was restored: autoplay/perf true,
capture delay 60 seconds/minimum 600 draws/120 frames. Logs, raw captures,
previews, profiles and binaries stay local and uncommitted. Quest testing and
optimization remain deferred until the full desktop host-frame gate passes.
