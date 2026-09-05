# Host scene-texture selection and publication

2026-09-05, Windows desktop. Follow-up to the explicit scene-target callback
boundary in `20260905_0144_native-reflection-selection.md` and the lock-order
correction in `20260905_0235_reflection-validation-lock-order.md`.

## Exact source

The guest-source guide directed inspection of hook TOML and exact generated
bodies before changes. `bdGetCurrentRenderTarget` (generated file 1) and
`bdGetNextRenderTarget` (file 97) select the scene table at
`(uint32_t(-32137) << 16) + 29804 + 4`. This is not the model reflection table.
Only when the scene table equals the active table at
`(uint32_t(-32036) << 16) - 7864 + 4` does its active offset apply. Otherwise
current/next use indices 0/1. The unsigned next index wraps at 32 bits.
An absent scene table returns zero without consulting the fallback. An index
outside the table count uses active-table state +32. In-range rows are 28 bytes,
with the image address at +24; the row-array pointer is table +4.

`sub_8221E618` (file 91) calls these selectors and binds them to shader slots 5
and 10, with no other material effect. It is both a virtual callback itself and
a helper of `sub_82454C08` (file 99). That wrapper additionally flushes constants
from visual +5084 and applies conditional blend overrides; those operations
are not replaced by this first scene-image producer checkpoint.

The existing `D3DDevice_SetTexture` host hook (`gpu/hooks/state.cpp`) resolves
the resource and calls `Video::SetTexture`; nonzero unknown resources use a debug
texture. `Video::SetTexture` treats null as a no-op and owns layout/binding
publication. The new producer preflights both resource lookups outside the
video mutex, then uses that host publication boundary directly. Unsupported
lookups leave the whole original callback as a counted compatibility path,
including its debug fallback; they cannot partially publish a new pair.

The colour-mask source remains separate: `sub_82186BA0` constructs/clears scene
surfaces and sets mask 7/15 from the active slot, while other pass callbacks
also write colour state. This checkpoint does not approximate those producers
with a blanket overwrite of native draw-packet state.

## Ownership and limits

`scene_texture_import.h` defines a checked, SDK-independent scene-role selector.
`native_scene_texture_bridge.cpp` replaces both complete selector functions and
the complete `sub_8221E618` binding callback. `bd_native_scene_textures` defaults
on; independent producer comparison defaults off. Native image handles and
live dynamic-resource adapters are distinguished, and null no-ops are counted.
The immutable-image capture boundary was factored out unchanged so scene
production and reflection/draw capture share the same lifetime criteria.

This first checkpoint removes that guest selection/binding execution, not the
entire material callback system or guest draw/pass scheduling. Scene table
production, native persistent scene associations, dynamic resource ownership,
shader-register compatibility, the wrapper's blend/constant behavior and
retained scene-image replay recipes remain to be converted. It does not fix
or qualify the existing scenery/text defects or stereo depth.

## Initial verification

- Host-only Vulkan build linked at 02:59:18 EDT. Codegen wrote/deleted nothing;
  no guest objects rebuilt. The new hooks live in the common OBJECT library.
- All 14 texture/upload/state/verification/lighting/scene-image CTests passed
  (0.51 seconds); the material CTest passed (0.03 seconds).
- The two reflection lock-order guards still pass. All new registry lookups
  happen outside the video lock; the comparison snapshots actual bindings
  under that lock without performing another registry lookup there.
- Scene selector tests exercise same/different active tables, current/next,
  empty/absent tables, fallback, unsigned wrap, explicit zero selection,
  unreadable rows, checked address overflow and independent snapshots. A bad
  second selection refuses the pair while the first independent query remains
  usable.

The producer comparison run (PID 23580, started 03:01:07 EDT) uses autoplay/perf,
180-second capture delay, minimum 30 draws, 120 captures and
`bd_native_scene_textures_verify=true`. Results were pending at the first
checkpoint (`f3d4272`); no runtime or visual qualification was claimed from the
unit tests. The completed comparison is recorded below.

## Completed producer comparison

PID 23580 ran from 03:01:07 to 03:07:47 EDT, using the 02:59:18 Vulkan binary
(logged revision `e4e47ca` dirty, containing the code subsequently committed as
`f3d4272`). Log: `out/build/win-amd64-release/logs/reblue_704.log`. The startup
audit confirms all six profile settings took effect: autoplay/perf true,
capture delay 180, minimum 30 draws, 120 frames, scene-texture verification true.
The renderer was stopped only after the complete sequence was preserved. The
original five-setting profile was restored: autoplay/perf true, capture delay
60, minimum 600 draws, 120 frames, no verification override.

Final periodic scene-texture report:

- Current 19743, next 19742: 39485 original-selector comparisons, zero wrong.
- 14 host pair publications and 14 original-publication comparisons, zero wrong.
- Zero compatibility calls or refusals. All 28 non-null inputs used native
  image handles; dynamic inputs and null no-ops were not exercised by this run.

These are separate coverage counts: the high selector count does not mean the
binding callback ran on every frame or draw. Comparison mode also executes the
original producers and is not proof of guest-free execution. No replay recipe
integration is included in this checkpoint.

The run progressed beyond the previous loading deadlock. Its final reflection
report has 1021972 matching source checks, five unsupported callback draws,
zero refusals and 3014967 composed native bindings. Lighting has 49034 host
publications, zero compatibility/refusal/reset calls and 998345 matching direct
shadow-input checks. No error/critical, Vulkan error, overflow or exhaustion
lines were found. These are correctness observations, not performance results.

All 120 flat 1920x1080 captures are isolated in
`out/verification/native_scene_texture_producer_verify`, from
`frame_1788591849_0.raw` through `frame_1788591853_119.raw`. They cover frames
10015-10134, 03:04:09.468-03:04:13.231 EDT. Inspected endpoints show the field path
and the village/title-logo transition. This is an early transition, not the
normal late scene that still has scenery/text defects. The local `--raw --mono`
preview command only converts an existing flat capture; it is not a separately
launched replay-off control.

Sequence analysis completed successfully: 111/119 pairs exceed the 6% change
threshold. Inspected pairs 064 and 101 show camera movement and title animation
without the previously observed broad rock-wall disappearance; their count
must not be interpreted as 111 confirmed rendering failures or compared with
the late-scene failure count as a performance/correctness trend. These sampled
pairs and endpoints do not qualify every frame or the full game.

The cyan detector reports 47/120 frames above 0.30%, median 0%, maximum 1.25%,
but zero patch frames in its 2-60% range and zero whole-frame detections. Cyan
matching alone cannot distinguish title art/sky from rendering defects.

Rechecked after staged review: 14/14 texture/state CTests, 1/1 material CTest
and both reflection lock-order source guards pass. Normal comparison-off flat,
later-scene and final-eye VR checks have not been rerun at this checkpoint.
