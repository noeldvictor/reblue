---
name: vrsim
description: Run reblue's VR path on the desktop with no headset, using the project's own headless OpenXR runtime. Use for verifying camera modes, the character anchor, stereo depth, the projection layer, or anything else that needs an eye pose.
---

# Running VR without a headset

`tools/xrsim/` is a headless OpenXR runtime written for this project. It reports two views, hands
out real `VkImage`s so the renderer has somewhere to draw, and returns a head pose it makes up. It
composites nothing and displays nothing.

**Use it for anything that needs an eye pose.** Without a runtime, `ViewOverrideActive()` is false,
the camera modes never compose, and the character anchor is never read - so those are simply not
testable on the flat build. With it they are, in about three minutes, with nobody in a headset.

**The pose is a function of frame index, not wall-clock time.** Two runs that reach frame N see the
same head. That is the whole point: a real headset cannot give you a reproducible capture, and
without reproducibility an A/B on the camera is noise.

## Why not something off the shelf

All three alternatives are closed, and each was tried:

| | |
| --- | --- |
| SteamVR | Its runtime will not initialise without an activated HMD - confirmed with the null driver enabled and `vrserver`/`vrcompositor` running. **Never leave `forcedDriver:null` in `steamvr.vrsettings`; it breaks a real headset.** |
| Oculus runtime | Wants a Quest over Link |
| Meta XR Simulator | The right tool. Its binary is behind a developer login that cannot be scripted - the npm package is a 31 KB Unity wrapper, and the CDN URL 404s without a session token |

## Build it

Once. It does not depend on reblue and reblue does not depend on it.

```sh
export PATH="/c/Program Files/LLVM/bin:$PATH"
cmake -S tools/xrsim -B out/xrsim-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build out/xrsim-build
```

Produces `reblue_xrsim.dll` and `reblue_xrsim.json` beside it. The manifest carries an **absolute**
`library_path` on purpose: the loader resolves a relative one against its own working directory, and
the failure is opaque (`Failed to open dynamic library reblue_xrsim.dll with error 2`).

## Build reblue with OpenXR on

The desktop Vulkan build, plus the Windows loader. See the `devloop` skill for the registry record,
the `game` junction and the TOML - all three look like a hang when wrong.

```sh
cmake -S out/xr-headers/openxr -B out/xr-loader-win -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DDYNAMIC_LOADER=OFF -DBUILD_TESTS=OFF -DBUILD_API_LAYERS=OFF -DBUILD_CONFORMANCE_TESTS=OFF
cmake --build out/xr-loader-win --target openxr_loader

export VCPKG_ROOT="C:/vcpkg"
cmake --preset win-amd64-release -DREBLUE_D3D12=OFF -DREBLUE_BUILD_INSTALLER=OFF \
  -Drexglue_DIR="$PWD/out/sdk/win-amd64/lib/cmake/rexglue" \
  -DREBLUE_OPENXR=ON \
  -DREBLUE_OPENXR_INCLUDE="$PWD/out/xr-headers/openxr/include" \
  -DREBLUE_OPENXR_LOADER="$PWD/out/xr-loader-win/src/loader/openxr_loader.lib"
cmake --build --preset win-amd64-release --target reblue
```

**The loader must be static.** An earlier tree left a 23 KB stub DLL whose import lib links with
`undefined symbol: xrEndSession`, which reads like a missing dependency and is a broken artifact. A
real one is 3.1 MB.

## Run

`XR_RUNTIME_JSON` is per process, so nothing about the machine changes.

```sh
XR_RUNTIME_JSON="$PWD/out/xrsim-build/reblue_xrsim.json" \
  ./out/build/win-amd64-release/reblue_vk.exe
```

**Do not minimise the window.** A minimised window has a 0x0 client area and the flat swapchain
fails with `Plume createSwapChain failed` before anything renders. Under PowerShell that means no
`-WindowStyle Minimized`.

Confirm it took, in the app log:

```
OpenXR: instance up, per-eye 1024x1024
OpenXR: session created
[xr] cam: game (...)  head m (-0.032, 1.600, 0.000)  eye (...)
```

`head m` showing 1.600 is the simulator's default height. **`eye` differing from `game` is the
check that matters** - it means `ViewOverrideActive()` is true and the camera modes are composing.
If they match, VR is not driving the view and whatever you are testing is not being tested.

## Knobs

All optional, all environment variables.

| | |
| --- | --- |
| `XRSIM_YAW_RATE` | rad/s the head turns. Default 0 - a fixed pose, which is what a reproducible capture wants |
| `XRSIM_HEIGHT` | eye height in metres. Default 1.6 |
| `XRSIM_IPD` | interpupillary distance in metres. Default 0.064 |
| `XRSIM_WIDTH` / `XRSIM_HEIGHT_PX` | per-eye recommended size. Default 1024 |

Combine with `bd_capture_after_s` in the profile TOML to get a frame on disk, and
`tools/stereo_check.py` to grade it.

## What it does not do

- **No compositor.** It never displays anything and never blends layers, so it cannot tell you
  whether a layer *looks* right in a headset - only what the app drew. Use `bd_capture_after_s` for
  the pixels.
- **No controller input.** Every action reads as untouched, deliberately: `bd_xr_autoplay` drives
  the game through a synthesised pad, and a simulated stick that drifted would make captures
  non-reproducible.
- **Not a performance model.** Timings here are a desktop GPU with no compositor and no vsync. The
  Quest remains the only place a frame time means anything.
- **Not conformant.** It implements the ~35 entry points reblue calls. Anything else returns
  `XR_ERROR_FUNCTION_UNSUPPORTED`, and an app that caches that null pointer and calls it will crash
  at PC 0 - which is exactly how the missing `xrGetActionStateVector2f` announced itself. If reblue
  starts calling something new, add it to the dispatch table in `xrsim.cpp`.
