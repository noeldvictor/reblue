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
`bd_native_scene_textures_verify=true`. Its results are pending at this first
checkpoint; no runtime or visual qualification is claimed from the unit tests.
