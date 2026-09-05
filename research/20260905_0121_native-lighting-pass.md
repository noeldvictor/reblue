# Host lighting producer and explicit shadow sampling parameters

2026-09-05, Windows Vulkan desktop. Follow-up to the recurring diagnostics in
`20260905_0053_recurring-draw-verification.md`.

## Source and ownership

The guest-source guide kept this trace in the exact generated C++ and PPC
comments, after reading `config/hooks/render_tweaks.toml` and
`config/hooks/render_list.toml`. `sub_8227FDC8` only configures blend/alpha
policy; it does not produce the mismatching shadow vector.

The actual lighting producer is `sub_82174CE8`, starting at line 3690 of
`generated/reblue_recomp.21.cpp` in this tree. Its input descriptor supplies
feature bytes at +13/+16/+17/+18, light count at +24, ambient at +28,
colour scale at +44 and shadow bias/threshold at +60/+64. Camera position
comes from the existing engine camera record. Scene-origin/range values come
from the existing scene globals. These remain engine data sources.

It resets the shared 412-byte material staging block through `sub_821982C8`,
then fills named lighting values. For shadow sampling it searches the texture
list for the current slot and queries that entry's level-zero width and height
through `sub_82182180`, `sub_821821D0`, `sub_8246D1C8` and
`D3DTexture_GetLevelDesc`. The latter's existing host implementation establishes
the metadata and zero-dimension clamping used by the new importer.

PS c9 is **not just bias**: it contains bias, threshold, and texture dimensions
multiplied by the kernel scale at `(uint32_t(-32250) << 16) + 3208`.
The live source value is **0.25**, not 0.5. The first integration comparison
caught that mistake: `reblue_692.log`, PID 8248, 01:17:39-01:18:47 EDT,
reported 8138 publication mismatches, first at staging offset 232 (actual
480 versus expected 960). That run is not qualification evidence. The core
now carries the kernel scale as an explicit input and the bridge imports it
from its actual source. The correction was independently tested before rerun.

`native_lighting.h` now defines address-free input/pass records.
`lighting_shader_bridge.h` separately packs the temporary engine staging ABI.
`native_lighting_bridge.cpp` replaces the complete lighting producer and its
reset/dimension helper execution on valid inputs. It reads host texture metadata
without constructing SDK descriptors. Invalid/unsupported input falls back
before native effects and is counted. An independent reset call invalidates
the native record, preventing reuse after a compatibility reset.

Supported direct phase-0 nodes consume the producer's current shadow sampling
record before interpretation. Replay no longer takes this value from retained
sub-draw constants or the visual's last interpreted draw. Source comparisons
remain independent of full replay comparisons. Deferred entries and other
recipes are not silently redirected to the current pass record.

This is not a complete native lighting system or frame. Engine descriptors,
camera/scene data, texture-list discovery, material mutation/flush and the
big-endian shader staging adapter remain. Texture slot 5, colour-write recipes,
animated UVs, other retained lighting inputs and native scene/pass scheduling
still require conversion. The source trace does not prove that shadow sampling
causes all disappearing scenery or damaged text.

## Standalone and build checks

The initial SDK-independent core checkpoint, `383abaa`, was committed and
pushed before renderer integration. Tests exercise separate pass records,
changing/absent/odd extents, configurable kernel scale, feature suppression,
light counts, complete staging reset/packing and nonfinite loading inputs.
All 13 tests in `out/native_texture_test` pass after the scale correction
(0.51 seconds). The devloop guide kept the build in the existing desktop tree,
target `reblue`; host objects compiled and linked, with codegen reporting zero
writes/deletes and one module up to date. No guest objects were rebuilt.

## Corrected short comparison

`reblue_693.log`, PID 24420, 01:19:54-01:21:36 EDT. Binary: `383abaa` plus
the corrected integration. All seven settings audited: autoplay/perf CSV on,
capture delay 60 seconds, minimum 600 draws, 120 captures,
`bd_native_lighting_verify=true`, `bd_host_draw_verify_every=31`.
The lighting verifier executes the original producer for comparison, then
publishes the native result. Replay sampling substitutes interpretation for
the sampled candidates; it is not a normal-path performance run.

Final lighting report: 13538 publications/comparisons, **zero mismatches**;
200650 captured shadow-input checks, **zero mismatches**; 548100 composed
replay inputs; zero compatibility/refusals and zero missing extents. The 13538
reset-hook calls here are the diagnostic original executions, not normal-path
reset dependencies. Full replay comparison still has other mismatches:
40688 nodes / 57563 draws, 19818 draws flagged, declared VS/PS 64/475,
state 0, geometry 129 and draw-count nodes 0. Its 20 PS c9 differences are not
covered by the zero direct-node source-comparison claim; other recipes remain.

The isolated `out/verification/native_lighting_short_verify` has 120
1920x1080 final captures, frames 2838-2957, 01:20:56.906-01:21:00.181.
Sequence analysis: **0/119 jumps over 6%**, zero cyan patches (median 0.011%,
maximum 0.02%). Inspected the actual first/last images: character and terrain
remain intact. Their separate endpoint preview is not a consecutive-frame
metric. No error/critical, overflow/exhaustion or VK_ERROR matches were found.
The owned process was stopped only after all 120 captures were present.

## Normal late scene

`reblue_694.log`, PID 10032, 01:22:48-01:28:47 EDT. Same renderer code as the
corrected short run. All five settings audited: autoplay/perf CSV on, delay
270 seconds, minimum 30 draws, 120 captures. Native lighting and normal replay
are enabled; neither lighting comparison nor replay sampling is active.
The loading interval stayed at 20 draws, so capture correctly waited for the
later scene. The owned live PID was observed throughout, not restarted.

Final lighting report: **43580 host publications**, zero compatibility,
refusals or reset calls; **700323 direct-node input checks, zero mismatches**;
2159795 replayed shadow inputs. Last skin check: 612405 checked, zero wrong,
31363 unsupported, 446054 replayed palettes / 4109655 joints. Final consumer:
3433919 entries, 2747303 replayed, 674397 direct draws, zero fallback/refusals.
These are component counts, not fully host-owned frame counts.

`out/verification/native_lighting_late_flat`: 120 1920x1080 final captures,
frames 14723-14842, 01:28:14.598-01:28:17.901. **107/119 jumps over 6%**, zero
cyan patches (median/max 0%). Inspected pairs 1/2 and 43/44: character geometry
stays intact, but the latter still loses major rock surfaces and has damaged
text. This scene still fails. The small difference from the prior 110/119
count does not establish improvement: cutscene motion and capture placement
also change that metric. No error/critical, overflow/exhaustion, retirement-race,
VK_ERROR or Vulkan-failure matches. Stopped the owned process after the complete
capture and isolated only files newer than its start time.

## Normal desktop multiview

The vrsim guide kept this check on the repository's headless desktop OpenXR
runtime. Its manifest and absolute DLL path were verified. Only process-local
environment variables were set: `XR_RUNTIME_JSON` to that manifest,
`XRSIM_WIDTH=1440`, `XRSIM_HEIGHT_PX=1584`, `XRSIM_HEIGHT=0`.
The final host-only rebuild also compiled fallback-counter reporting; codegen
again wrote/deleted nothing and did not rebuild guest objects.

`reblue_695.log`, PID 23980, 01:29:57-01:31:39 EDT. All 13 settings audited:
autoplay/perf on, capture 60 seconds/minimum 450 draws/120 frames, VR and
multiview/layered textures on, legacy side-by-side/array-target capture/XR
mirror off, camera mode 2 and diorama height 0. No lighting comparison or replay
sampling. The runtime initialized successfully and requested 1440x1584, but
the existing render scale produced **936x1030 per eye**; this is not the target.

Final lighting report: **50806 host publications**, zero compatibility,
refusals or reset calls; **92967 direct-node checks, zero mismatches**;
6216180 replayed shadow inputs. Last skin: 115514 checks, zero wrong, 5375
unsupported, 844009 palettes / 3852958 joints. Consumer: 4128372 entries,
3647364 replayed, 476363 direct, zero fallback/refusals. No error/critical,
overflow/exhaustion, retirement-race, VK_ERROR or Vulkan-failure matches.

`out/verification/native_lighting_vr`: 120 stacked 936x2060 final-eye captures,
frames 20732-20851, 01:31:00.321-01:31:05.775. **0/119 large frame jumps**,
zero cyan patches (median/max 0%). Inspected both eyes at sequence 0 and 119:
no broad banding, but blur and large black bars remain. Both stereo checks
return **INCONCLUSIVE**, actual exit code 1: the 44% and 52% bands both give
-1 pixel disparity, zero spread. This framing cannot qualify stereo depth.
Desktop analysis ran concurrently with this correctness capture; no performance
claim is made. Stopped only the owned renderer after all captures were complete.

Final standalone rerun: all 13 texture/upload/state/lighting tests pass (0.52
seconds), plus the material/skin test (0.03 seconds). The original five-setting
flat profile is restored and no test renderer remains running. Full desktop/
both-eye acceptance remains open. No Quest or Thor tests were performed.
