# We wrote an OpenXR runtime, and it immediately found a 100x scale bug

2026-08-29, last note of the day. The Quest went offline mid-session and never came back, which is
how this happened.

## The runtime

`tools/xrsim/` implements the ~35 OpenXR entry points reblue calls and nothing else. Two views, real
`VkImage`s allocated on the app's own `VkDevice`, a head pose it makes up, no compositor and no
display. About 700 lines.

Every alternative was closed, and each was tried properly rather than assumed:

| | |
| --- | --- |
| SteamVR | Will not initialise its runtime without an activated HMD. Confirmed with `requireHmd:false`, `forcedDriver:null`, `driver_null.enable:true` and both `vrserver` and `vrcompositor` running - still `XR_ERROR_RUNTIME_UNAVAILABLE`. **Never leave `forcedDriver:null` in `steamvr.vrsettings`; it breaks a real headset.** |
| Oculus runtime | Installed, wants a Quest over Link |
| Meta XR Simulator | The right tool. Its npm package is a 31 KB Unity wrapper, and the real binary is behind `securecdn.oculus.com/binaries-download-auth/`, which 404s without a session token. The version endpoint is public; the download is not. |

Verified end to end:

```
OpenXR: instance up, per-eye 1024x1024
OpenXR: session created
[perf] 1744 draws/frame, 339998 verts/frame
[xr] cam: game (-20.4, 205.7, 131.3)  head m (-0.032, 1.600, 0.000)  eye (88.7, 118.6, 60.1)
```

**`eye` differing from `game` is the check that matters.** It means `ViewOverrideActive()` is true,
so the camera modes are composing and the character anchor is being read. Without a runtime that is
false and none of it runs - which is why none of the bugs below were findable before today.

Three traps, each cost a run and all are in the `vrsim` skill: a minimised window has a 0x0 client
area and the flat swapchain fails with `Plume createSwapChain failed` before anything renders; the
manifest's `library_path` must be absolute, because the loader resolves a relative one against its
own working directory and reports only `error 2`; and a missing entry point **crashes the app at
PC 0** rather than returning an error, because an OpenXR client caches the pointer and calls it -
which is how the absent `xrGetActionStateVector2f` announced itself.

## The scale was wrong by 100x

`bd_vr_units_per_metre` defaulted to **1.0**. A Blue Dragon unit is a **centimetre**: the field
camera sits at y ~ 150 with the ground at 0, which is a 1.5 m eye height.

So the head's 1.6 m of height became 1.6 cm of game movement, and `bd_vr_third_offset` of
`(0, 1.5, -3.0)` - metres, by mistake - put the third-person camera **three centimetres** behind the
character. The capture is unambiguous: a dark mass filling the frame, which is the inside of the
character's head.

The offsets' ranges were `+/-50` as well, half a metre, so the bug could not even be tuned around
from a config file.

Fixed to 100 units/metre, offsets in game units, ranges `+/-2000`.

**Then a second one, only visible once the first was fixed.** With the scale right the camera sat
3.7 m up looking down, because `third_offset_y` supplied eye height that the head pose already
carries. The anchor belongs on the ground; `third_offset_y` is now 0.

```
before      eye ( 71.1,  87.1,  29.2)   inside the character
scale fix   eye (-19.7, 368.8, -24.4)   3.7 m up, looking down
offset fix  eye ( -6.7, 224.8, -27.7)   2.25 m, behind - a third-person camera
```

Each of those is a capture that was looked at, not a number that was reasoned about.

## Battles use the diorama camera

A battle is a stationary set-piece: the ranks do not move, so a follow camera has nothing to follow
and spends the fight fighting the game's own battle camera. `bd_vr_battle_diorama` (default on)
switches for the duration.

`bd::engine::BattleActive()` reads the same task liveness the battle manager does, and deliberately
**only** that half - waiting for the manager to be captured for the step would leave the camera in
follow mode for the first frames of every battle, which is when the transition is most visible.

## Multiview is still not settled, and the reason is a measurement limit

With the post chain made two-layer, both array slices are now populated where layer 1 used to be
black. Two captures at `bd_stereo_debug_layer` 0 and 1 differ - mean 23/255, luma 73 against 87.

**That difference cannot be attributed to the per-eye skew**, because the two captures come from two
separate runs and autoplay is not frame-identical across restarts: load times vary, so the scene
differs. `bd_stereo_debug_layer` is read when the surface is created, so one run can only ever
sample one slice.

Settling it needs a capture that reads **both** array slices in a single frame. That is a small
change to the capture path - `copyTextureRegion` already takes an `arrayIndex` in its subresource -
and it is the right next step for multiview, which is worth having because it is one draw instead of
two and stereo currently costs a whole pacing tier on the Quest.

Recorded rather than guessed at, because two runs comparing different scenes is exactly the kind of
measurement this project has been caught by before.
