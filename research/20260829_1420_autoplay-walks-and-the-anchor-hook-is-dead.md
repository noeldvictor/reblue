# Autoplay never walked, and the player-anchor hook never fires

2026-08-29. Two separate things, found together because the first hid the second.

## Autoplay pressed START and A and nothing else

`ApplyAutoplay` in `src/xr/xr_pad_driver.cpp` drove START once at 6.0s and then A for 200ms out of
every 1.2s, for ever. It never touched a stick. So every unattended run this project has ever
measured - the fill sweeps, the culling sweeps, the 28.9 fps result - was taken with the character
**standing still**.

That matters twice over:

- It is not a representative frame. A stationary character streams nothing and spawns nothing.
- The guest only calls its field-movement code when the character is actually moving, so every hook
  hung off the player object silently never fired. That read for a long time as "the anchored
  camera modes do not work".

Fixed: after `kWalkStart` the left stick traces a slow circle (`kWalkTurnRate` rad/s). A circle
rather than a straight line, so the character stays in one region instead of walking into geometry
and stopping.

**`kWalkStart` has to be late.** At 26s the game is still on the file menu, and a stick deflection
there moves the selection - the run then sits in a menu for ever, which measures as 18 draws/frame
with the camera pinned at (0, 1000, 500) and reads exactly like a hang. Launch to a field scene is
about 130s on this device, so it is set to 150s.

Verified by the camera tracing the circle:

```
cam: game (155.9, 149.1,  76.6)
cam: game (116.8, 150.9, 162.2)
cam: game (157.0, 152.2, 194.2)
cam: game (238.3, 150.2, 158.2)
```

## Walking triggers random encounters, which ends the measurement

Both walking runs left the field within about a minute - into an encounter, then a battle, then a
menu (18 draws/frame, 47 fps, which is a menu and not gameplay). So a walking benchmark is not
stable for long, and lengthening the settle time makes it *more* likely to have wandered off.

This is the first concrete argument for **tourist mode's encounter suppression as a measurement
tool** rather than only a player feature: a walking character with encounters off is the
representative steady-state field frame this port has never actually measured.

## bdPlayerFieldMovementUpdate (0x82207858) is never called

With walking confirmed and the probe log moved above every gate, `bd_vr_player_probe` still prints
**nothing**. So the hook does not fire while the character walks.

What was ruled out along the way, so none of it gets re-checked:

- `src/xr/xr_player_anchor.cpp` **is** compiled - `src/CMakeLists.txt` uses `GLOB_RECURSE`, and the
  static from the hook body (`bdPlayerFieldMovementUpdate::last`) is in the shipped `.so`.
- The hook **is** registered - `0x82207858` is named in `config/functions.toml:745`, and
  `REX_HOOK_RAW` is plain `extern "C" REX_FUNC`, i.e. function replacement.
- `args.txt` **is** read - 16 arguments, confirmed in logcat, `bd_vr_player_probe` among them and
  not rejected.
- `bd::mem::load` is correct here: it reads through `be<T>` and returns host order, so this is not
  the double-swap mistake.
- The probe was gated behind `bd::xr::Settings::Get().Enabled()`; moving it above that changed
  nothing, but it is left moved. A diagnostic gated on the feature it exists to bring up makes a
  silent probe ambiguous between "never fired" and "VR was off", and those need different fixes.

`generated/` has exactly **one** direct call site, in `bdPlayerFieldUpdateMain`
(`generated/reblue_recomp.13.cpp:8152`), and it is not obviously conditional. So either
`bdPlayerFieldUpdateMain` is itself not called on the walking path, or the name at `0x82207858` is
attached to a mode this scene does not use.

**Next seam to try is `bdFieldCameraSetupFollow` (0x821B1A58)**, already named, which runs every
frame in the field and whose argument should reach the followed character. The candidate offsets
`{5704, 5716, 7436}` and the `bd_vr_player_pos_offset` cvar stay as they are - the probe mechanism
is right, it is hung on the wrong function.

Until then `CharacterAnchor` stays invalid and `ThirdPerson` / `FirstPerson` keep falling back to
the game's own camera, exactly as `CLAUDE.md` says.

## tools/build_apk.sh could ship an APK that cannot start

Unrelated, found because it cost half an hour of this. The OpenXR loader was staged only if
`OPENXR_LOADER` happened to be exported in the environment. Forget the export and the script prints
`signature verified`, `adb install` prints `Success`, and the app then dies in `dlopen` before
`main()`:

```
dlopen failed: library "libopenxr_loader.so" not found: needed by .../libreblue.so
```

**No reblue log file is written at all**, because that happens before logging starts - the only
evidence is one line in logcat under a tag the app never gets to use. The newest log on device is
then the *previous* run's, so reading it reports the previous build's numbers under the new build's
name. That is the stale-log trap with an extra step.

Now the script finds the loader itself under `out/xr-loader-android/`, and refuses to package at all
when `libreblue.so` names the soname and the file is not staged.
