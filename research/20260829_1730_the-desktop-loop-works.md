# The desktop build runs the game, and it is the fast verification loop

2026-08-29. The Quest went offline mid-session, which turned out to be worth it: it forced the
desktop route that this project has assumed was unavailable for weeks, and the desktop route is
**better than the device for everything except performance numbers**.

## It just works

Everything needed was already on this machine. No downloads, no new dependencies.

```sh
export PATH="/c/Program Files/LLVM/bin:$PATH"
export VCPKG_ROOT="C:/vcpkg"
cmake --preset win-amd64-release -DREBLUE_D3D12=OFF -DREBLUE_OPENXR=OFF \
      -DREBLUE_BUILD_INSTALLER=OFF \
      -Drexglue_DIR="$PWD/out/sdk/win-amd64/lib/cmake/rexglue"
cmake --build --preset win-amd64-release --target reblue
```

Then the three pieces of setup that are not obvious, all of which the existing notes get slightly
wrong:

- **The binary is `reblue_vk.exe`, not `reblue`.** `CLAUDE.md` says the opposite - that with
  `REBLUE_D3D12=OFF` the Vulkan executable takes the `reblue` target name. The *target* is `reblue`;
  the *output* is still `reblue_vk.exe`.
- **The registry record must name the directory holding the exe.** `HKCU\Software\Zolawareeblue\Install`,
  `InstallRoot` plus `SchemaVersion`=3. Anywhere else raises a modal about running outside the
  install directory, with no log line and no exit - it looks exactly like a hang.
- **Game data goes at `<InstallRoot>/game`**, and a junction to an existing extraction is fine:
  `New-Item -ItemType Junction`. The 8.9 GB already extracted for the device needed no copy.
- **Cvars go in `<InstallRoot>/profiles/default/reblue.toml`**, flat TOML, one key per line. Command
  line flags do not work - `--help` returns a swallowed CLI11 parse error.

## Measured against the device loop

| | device | desktop |
| --- | --- | --- |
| build `src/` change | ~10s | ~10s |
| package + install | ~20s (62 MB APK) | **none** |
| launch to a field scene | ~130s | **~75s** |
| capture resolution | 344x180 upscaled | **1920x1080 native** |
| needs the headset awake, prox broadcast, adb | yes | no |

**About 2.5 minutes saved per iteration, and no headset to keep awake.** The device is still the
only place performance numbers mean anything - different GPU, different driver, different pacing -
but every question about *correctness* is cheaper and clearer here.

## It independently confirmed the stereo fix

Same `tools/stereo_check.py` analysis, run on a desktop capture at 1920x1080:

```
band   32%   44%   52%   62%   72%   82%   90%   95%
disp    +4    +2    +1    +1    +4    -3    -4    -5

far +4, near -5, near - far = -9px  ->  OK crossed
```

Crossed and correctly signed on a completely different GPU, driver and resolution. That is a real
independent check of the fix rather than a re-run of the same conditions.

It also **explains the constant offset seen on the Quest**. There, far sat at +21px rather than near
zero; here it is +4. The Quest's scene target is 344 wide at `render_scale=25`, so the two
half-width viewports land on 172 and 172 with rounding either side; 1920 halves evenly into 960.
So that offset was viewport rounding, exactly as suspected, and not a depth error.

And the picture is worth having: the desktop capture is Blue Dragon's opening village at full
resolution, with Shu standing in it - near fence rail separating strongly, distant cliffs barely
moving. The Quest captures at quarter scale were too soft to judge anything by eye.

## What it cannot do yet

`REBLUE_OPENXR=OFF` here, so there is no eye pose, `ViewOverrideActive()` is false, and the camera
modes are not exercised. **The character anchor cannot be verified this way as configured.**

The fix is a second configure with `REBLUE_OPENXR=ON` against a PC OpenXR runtime. The OpenXR-SDK
source is already checked out under `out/xr-loader-android/` and its headers are platform-neutral,
so a Windows loader is a second build of the same tree rather than a new dependency. That plus Meta
XR Simulator or SteamVR would make the anchor, the camera modes and the projection layer all
checkable without hardware. **That is the highest-value next step for this loop**, and it is the
thing the original plan wanted all along.

## The correction worth recording

`CLAUDE.md` has said for weeks that the desktop targets do not link on this machine because there is
no vcpkg, and that this is what kept the simulator route closed. It then says, further down and
added later, that vcpkg *is* installed. Both statements were in the file at once.

The route was open. Nobody tried it. The whole VR port was built on-device with printf because of a
stale sentence.
