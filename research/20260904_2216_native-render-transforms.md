# Native object/pass transform production, 2026-09-04

## What moved and what remains

`native_transform.h` composes named world/view/projection inputs and the
view-projection product in ordinary host memory. It is independent of engine
addresses, SDK types, GPU registers and OpenXR. The strict entry point rejects
nonfinite inputs/results; the arithmetic layer preserves IEEE exceptional
values for the explicitly tracked temporary engine import path. Neither API
persists an old draw's numeric world/view-projection result.

The existing `bdBuildViewMatrix` hook now feeds a host producer after camera
interpolation and XR composition. The normal path does not execute the original
producer, its default callback, matrix transpose/multiply helpers, palette
publisher or D3D constant setter. Native publication updates the temporary
engine matrix cache, packs the current shader ABI and marks the existing host
constant/capture bookkeeping. Interpolated/XR views go directly from native
memory to this producer; a guest-addressed view scratch is only allocated for
diagnostic or compatibility execution.

`bd_native_transforms` defaults on. `bd_native_transforms_verify` defaults off;
when on, it runs the original producer once with the same inputs, compares
cache/constants/dirty masks, then publishes the native result. `checked`
therefore counts diagnostic original calls separately from `compatibility`.
Non-default callbacks and invalid memory or cross-aliasing cache inputs still
have an explicit pre-effect fallback, with categorized counters. A zero
compatibility count is not zero guest execution while comparison is on.

This is not native scene/pass ownership in full. The object transforms and
camera/projection sources still originate in the engine. Null arguments import
inherited engine cache values, and the temporary backend still writes VS20..35
in the old shader ABI. Asset-level scene loading, native animation/GPU skinning,
material/lighting/state producers, frame/pass scheduling and other transition
requirements remain. The deferred consumer's `world` bridge count includes this
now-host-replaced entry point; use `[native-transforms]` to distinguish native
production from actual compatibility/diagnostic execution.

## Source contract

Read `config/hooks/frame_interp.toml` and the camera/world interpolation hook,
then the complete translated `bdBuildViewMatrix` in module 9 (0x82286C40).
Read the complete transpose helper `sub_824915F8` in module 50, multiply helper
`sub_82491418` and gated matrix publisher `sub_82286BC0` in module 23, and the
default callback in module 46. Module 58's renderer initialization installs that
callback. Camera call sites in modules 16/47 pass view at +160 and projection
at +224; direct node/deferred call sites pass world only. Generated code and
hook TOML are unchanged.

- r3/r4/r5 optionally supply world/view/projection. A null argument inherits
  the corresponding matrix. Non-null matrices update the engine cache at
  renderer-state +54656/+54720/+54784.
- The callback at +54848 defaults to `bdDefaultRenderFunc` (0x820F7068), which
  returns zero and has no side effects. Unknown callback identities are not
  silently discarded.
- World-only updates publish four vectors beginning at VS20. Updating view or
  projection publishes 16 vectors: transposed world, view, projection and
  view*projection. The fourth matrix is not world*view*projection.
- The engine suppression byte at +54852 equals exactly 1 to skip GPU constant
  publication; cache updates still occur. The native bridge preserves that
  distinction and counts suppression separately.
- Matrices use row-major, row-vector convention. Multiply uses paired dot
  sums with Clang contraction disabled. Transpose moves values without changing
  the world matrix's numerical meaning.
- Comparison checks the engine cache bitwise, all 64 floats of the affected
  VS window (including untouched values) numerically, and the dirty mask
  exactly. Finite tolerance is `1e-5 * (1 + abs(expected))`. Two NaNs count as
  numerically equivalent even if payloads differ; this is not bit-identical
  floating-point/denormal qualification.

## Tests and initial evidence

Core `0c18d03` was committed and pushed before live integration. All seven
standalone tests pass after adding exceptional-value coverage (0.54 seconds
total). Transform tests cover identity, transpose/involution, noncommuting
translation/scale order, independence of world from view-projection, 2000
random affine/projective matrices against independent double-precision sums,
nonfinite values in every input position, finite overflow and zero matrices.
They also check NaN-payload/signed-zero preservation through transpose and
IEEE infinity/NaN arithmetic while the strict native-asset API still rejects
these values. These tests do not prove full scene, Vulkan or VR correctness.

All incremental desktop builds linked the configured Vulkan-only `reblue`
target with OpenXR/PCH on and Clang 22.1.8. Codegen reported zero writes and no
guest translation units rebuilt. No build directory was deleted.

Initial comparison run, log 667: PID 18000 launched 22:09:23 EDT. Original five
profile settings plus `bd_native_transforms_verify = true`, all six applied.
Capture delay 60 seconds, minimum 600 draws, 120 frames. Sequence 119 completed
22:10:30.039 at frame 2644, 1920x1080; isolated output
`out/verification/native_transform_verify`. 0/119 jumps over 6%, no cyan patches
(median 0.012%, maximum 0.02%). First and last images inspected: character,
terrain, foliage, buildings and shadows intact. Last report: 633164 native
updates (605506 world-only, 27658 pass), 633164 checks, zero mismatches; 203
compatibility/refusal calls, no custom callback. Exact-path validated process
stopped and confirmed exited at 22:11:22.

Diagnostic run, log 668: PID 23564 launched 22:12:28 with the same six-setting
profile and added refusal categories. All 203 refusals were nonfinite values,
not memory, callback, alias or device failures. It was stopped after exact-path
validation at 22:14:17. This was a refusal diagnosis, not a separate visual
qualification. Neither log has error/critical, Vulkan error/failure, overflow,
exhaustion or retirement-race matches.

The final implementation handles those values with host arithmetic instead of
calling the original merely because inputs are exceptional. The diagnostic
mask identifies view and derived view-projection as nonfinite during loading;
this remains an upstream input-quality issue, not permission to cook invalid
native assets. The nonfinite-update counter is separate from compatibility.

## Final producer verification

### Final comparison, log 669

PID 24968 launched 22:15:22 with the same six-setting comparison profile; all
settings applied. Sequence 119 completed 22:16:28.103 at frame 2962, 1920x1080.
Isolated output: `out/verification/native_transform_final_verify`. 0/119 jumps
over 6%, no cyan patches (median 0.011%, maximum 0.02%). First and last images
inspected: character, terrain, vegetation, building and shadows intact.

Last report: 826215 native updates, 791073 world-only and 35142 pass, 826215
checks, zero cache/constant/mask mismatches, compatibility calls, custom
callbacks or refusals. All 203 nonfinite loading updates were handled natively.
No error/critical, Vulkan error/failure, overflow, exhaustion or retirement-race
matches. Exact-path validated process stopped and confirmed exited at 22:17:40.
This is input/publication comparison evidence, not a full-frame native ownership
or all-scene qualification. Suppression was not exercised in this field run.

Local profiles, game data, generated code, binaries, logs and captures are
excluded from commits.

### Normal flat, log 670

PID 18644 launched 22:17:52 with the original five-setting profile; all settings
applied. Same final binary, native transforms on and comparison off by default.
Sequence 119 completed 22:18:58.819 at frame 2955, 1920x1080. Isolated output:
`out/verification/native_transform_flat`. 0/119 jumps over 6%, no cyan patches
(median 0.012%, maximum 0.02%). First and last images inspected: character,
terrain, foliage, buildings and shadows intact.

Last report: 472907 native updates (451256 world-only, 21651 pass), 203
nonfinite updates; zero comparison calls, compatibility calls, custom callbacks
or refused imports. Suppression was not exercised. No error/critical, Vulkan
error/failure, overflow, exhaustion or retirement-race matches. Exact-path
validated process stopped and confirmed exited at 22:19:41. This is the
normal-path short field check, not later-scene/full-game qualification.
