---
name: devloop
description: The fast build-run-test loop for this fork - which target to build, which OpenXR runtime to run against, and what never to rebuild. Use when building, running, testing, deploying to Quest or an Android handheld, or when a build is taking longer than it should.
---

# Dev loop

The rule this whole file exists to serve: **do not go to the device, and do not rebuild the guest.**

Everything through Phase 5 of `docs/VR_PORT_PLAN.md` runs as a desktop executable against a
simulated headset. The Quest is for comfort checks, real performance numbers, and driver bugs.
Nothing else.

## Build

Pick the target, never the default:

```sh
# VR work. Vulkan-only configure is what REBLUE_OPENXR requires.
cmake --preset win-vk-release -DREBLUE_OPENXR=ON
cmake --build --preset win-vk-release --target reblue_vk
```

`--target reblue_vk` matters. The default target on a `win-amd64` configure links two executables;
building both to test one is pure waste.

### PCH or compiler cache, never both

They fight: no compiler cache caches a precompiled-header compilation, which is why CI sets
`REBLUE_PCH=OFF` (see `.github/scripts/configure.sh`). Locally the choice is the opposite of CI's,
and depends on what you are doing:

| Doing | Use |
| --- | --- |
| Editing a few files, rebuilding repeatedly | `-DREBLUE_PCH=ON`, no launcher. The PCH is what makes one TU fast. |
| Reconfiguring, switching branches, bisecting | `-DREBLUE_PCH=OFF -DCMAKE_CXX_COMPILER_LAUNCHER=sccache` (ccache on Linux/macOS). The cache is what makes a full rebuild fast. |

`ccache -s` / `sccache -s` before and after. A 0% hit rate almost always means the absolute build
path changed.

### Never do these

- **Never delete the build directory** to "make sure". Reconfigure in place. A wipe throws away all
  54 recompiled guest TUs and the multi-megabyte shader cache, none of which you changed.
- **Never rebuild the guest to test host code.** `reblue_recomp` and `reblue_generated` are separate
  OBJECT libraries exactly so a change in `src/` does not touch them. If they are rebuilding, an
  input to codegen changed — `assets/default.xex`, a `config/*.toml`, or a shader. Find out which
  rather than accepting it.
- **Never report a build that did not happen.** A fresh clone has no `assets/default.xex` and no
  SDK, so codegen cannot run — but both are a minute away, see Bootstrap below. Once bootstrapped,
  codegen works and sources can be syntax-checked; a full Windows *link* still needs vcpkg. Say
  which of those you actually did.

## Bootstrap: getting a tree that can actually build

Two things are missing from a fresh clone, and both are obtainable in about a minute.

**1. The SDK.** Public releases, no auth needed:

```sh
curl -sfL -o out/sdk/sdk.zip \
  https://github.com/rexglue/rexglue-sdk/releases/download/v0.10.0/rexglue-sdk-0.10.0-win-amd64.zip
unzip -q out/sdk/sdk.zip -d out/sdk    # -> out/sdk/win-amd64/{bin,include,lib,cmake}
```

`v0.10.0` is what `generated/rexglue.cmake` pins. Slices exist for `linux-amd64`, `linux-arm64`,
`mac-amd64`, `mac-arm64` too. **There is no `android-arm64` slice** — that one has to be
cross-built from source, and it gates the whole Android target.

**2. `assets/default.xex`,** from your own Blue Dragon disc. It is ~8 MB inside a 7.8 GB ISO, so do
not copy the disc:

```sh
python tools/extract_xex.py "path/to/Blue Dragon (Disc 1).iso"
# or straight off a connected handheld, without pulling the ISO at all:
MSYS_NO_PATHCONV=1 python tools/extract_xex.py --adb-serial <serial> \
    --adb "$ADB" "/storage/XXXX-XXXX/Roms/xbox360/.../Blue Dragon (Disc 1).iso"
```

It walks XDVDFS and reads only the sectors the file occupies. Under a second over USB.

> **`MSYS_NO_PATHCONV=1` is not optional in Git Bash.** MSYS rewrites any argument that starts with
> `/` into a Windows path before Python sees it, so an on-device path silently becomes something
> like `C:/Program Files/Git/storage/...` and `dd` returns zero bytes. The symptom is an
> unexplained short read, not an error.

Then codegen, which takes about 7 seconds:

```sh
out/sdk/win-amd64/bin/rexglue.exe codegen reblue_manifest.toml
```

219 files into `generated/`. It is deterministic — a re-run on the same xex reports
"0 written, 219 unchanged", which is also the quickest way to confirm an extraction was correct.

Both `assets/` and `generated/` carry their own `.gitignore` containing `*`, so the game
executable and the recompiled guest code can never be committed. Do not weaken that.

## Compile-checking without a full build

A full Windows build additionally needs vcpkg (`find_package(directx-dxc CONFIG REQUIRED)` is
unconditional on WIN32, even for the Vulkan-only target). Short of that, individual sources can be
syntax-checked against the real SDK headers, which catches nearly everything:

```sh
CLANG="C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/Llvm/x64/bin/clang++.exe"
"$CLANG" -std=c++23 -fsyntax-only -I. -Isrc -Iout/sdk/win-amd64/include -Igenerated src/xr/xr_settings.cpp
```

## Verify without the SDK

A full build is impossible in this tree, but `xr_math.h` depends on nothing but the integer
aliases, so the piece most likely to be silently wrong *can* be compiled and run:

```sh
cmake -S tools/xr_math_test -B out/xr_math_test -G Ninja
cmake --build out/xr_math_test
./out/xr_math_test/xr_math_test
```

On this machine there is no compiler on `PATH`, but VS 2022 Build Tools ships clang 19:

```
C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/Llvm/x64/bin/clang++.exe
```

Pass it as `-DCMAKE_CXX_COMPILER=` with **forward slashes** — CMake reads `\P` in a Windows path as
an invalid escape and the configure fails with a confusing message.

This earns its keep: it caught the off-centre projection sign on its first run. Extend it whenever
new maths lands, and prefer moving logic *into* the dependency-free files so it stays reachable from
here.

## Run

The OpenXR loader picks its runtime from `XR_RUNTIME_JSON`. That is the whole switch — no rebuild,
no registry edit, and the effect is scoped to the process:

```sh
# Meta XR Simulator: a virtual Quest on the desktop. The default for all VR work.
XR_RUNTIME_JSON=<install>/meta_openxr_simulator.json ./reblue_vk

# Real headset over Link, when comfort or performance is the question.
# (Unset to fall back to the system default runtime.)
./reblue_vk
```

The simulator implements OpenXR at the API level, so the same code path runs unchanged in the
simulator, over Link, and on-device. There is no simulator-specific branch, and if you find yourself
writing one, something is wrong.

Input is simulated from keyboard, mouse, or an Xbox pad, so locomotion and bindings are testable
without touching a Touch controller.

## Building the SDK for Android

No `android-arm64` release slice exists, so it builds from source with the patch in
`patches/rexglue-sdk-android.patch`. See `patches/README.md` for what the patch contains and
`research/20260828_1600_android-arm64-bringup.md` for how it was arrived at.

```sh
git clone https://github.com/rexglue/rexglue-sdk.git out/rexglue-src
git -C out/rexglue-src submodule update --init --recursive --depth 1 --jobs 8
git clone --depth 1 https://github.com/kaniini/libucontext.git out/rexglue-src/thirdparty/libucontext
git -C out/rexglue-src apply ../../patches/rexglue-sdk-android.patch

NDK="$ANDROID_HOME/ndk/30.0.15729638"          # r30 or newer, see below
cmake -S out/rexglue-src -B out/sdk-android30 -G Ninja   -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake"   -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29   -DCMAKE_BUILD_TYPE=Release -DREXGLUE_BUILD_TESTS=OFF
cmake --build out/sdk-android30
```

**Use NDK r30 or newer.** r29's libc++ has no floating-point `std::from_chars`, which
`rex/string/numeric.h` calls with a `chars_format`; the error is a confusing "requires 3 arguments,
but 4 were provided" as overload resolution falls back to the integral form.

**On Windows, materialise libmspack's symlinks first** or the build fails with what looks like a
corrupt source file — see `patches/README.md`.

**Do not trust a wrapper's exit code.** `cmake --build ... | tail` reports the exit status of
`tail`, not of ninja. Capture ninja's own status (`echo $?` immediately after, or `${PIPESTATUS[0]}`)
and grep the log for `FAILED:`. A build that "succeeded" in 40 seconds did not.

## Building reblue for Android

Three host tools must exist first, because they *run* during the build and a cross-compiled one
cannot execute:

| Tool | Where from |
| --- | --- |
| `rexglue` (codegen) | the host SDK slice, `out/sdk/win-amd64/bin/rexglue.exe` |
| `XenosRecomp` | build `thirdparty/XenosRecomp` for the host, see below |
| `dxc` | vendored: `thirdparty/XenosRecomp/thirdparty/dxc-bin/bin/x64/dxc.exe` |

Host XenosRecomp, with VS Build Tools' clang:

```sh
CLANGDIR="C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/Llvm/x64/bin"
cmake -S thirdparty/XenosRecomp -B out/host-xenosrecomp -G Ninja -DCMAKE_BUILD_TYPE=Release   -DCMAKE_C_COMPILER="$CLANGDIR/clang.exe" -DCMAKE_CXX_COMPILER="$CLANGDIR/clang++.exe"   -DCMAKE_RC_COMPILER="$CLANGDIR/llvm-rc.exe"
cmake --build out/host-xenosrecomp --target XenosRecomp
```

`CMAKE_RC_COMPILER` is not optional: clang in GNU mode on Windows has no resource compiler and the
configure dies pointing at `Windows-Clang.cmake` rather than saying so.

**Install the Android SDK; a build tree is not enough.** `rexglueConfig.cmake` expects
`rexglue_helpers.cmake` beside it, which only the install step produces:

```sh
cmake --install out/sdk-android30 --prefix "$PWD/out/sdk-android-install"
```

Then configure:

```sh
export ANDROID_NDK="$ANDROID_HOME/ndk/30.0.15729638"
cmake --preset android-arm64-release   -DREXSDK_DIR=   -DCMAKE_PREFIX_PATH="$PWD/out/sdk-android-install"   -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH   -DREBLUE_XENOSRECOMP="$PWD/out/host-xenosrecomp/XenosRecomp/XenosRecomp.exe"   -DREBLUE_DXC="$PWD/thirdparty/XenosRecomp/thirdparty/dxc-bin/bin/x64/dxc.exe"
```

Two of those flags are load-bearing and non-obvious:

- **`-DREXSDK_DIR=`** (empty) overrides the preset. Pointing it at the SDK *source* makes CMake
  `add_subdirectory` the SDK, which cross-compiles `rexglue` and leaves codegen with a binary that
  cannot run on the build machine.
- **`-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH`.** The NDK toolchain sets this to `ONLY`, so
  `find_package` searches only inside the sysroot and will not see the installed SDK no matter what
  `CMAKE_PREFIX_PATH` says. The error just claims the SDK was not found.

## Device, once Phase 6 exists

Not yet buildable — the ReXGlue SDK has no `android-arm64` slice. When it does:

- **`adb push` the `.so`, do not `adb install` the APK.** Install re-verifies and re-optimises the
  whole package; push copies a file. The app should `System.load()` from
  `/sdcard/Android/data/<pkg>/files/` behind a debug flag so the loop is push-and-launch.
- The ~15 GB of game data lives on the device permanently and is never part of a deploy step.
- Host tools (`rexglue`, `XenosRecomp`, `dxc`, `reblue_prelink`) build for and run on the host. Only
  the runtime cross-compiles.

## Profiling

Desktop first: Tracy is already wired in behind `REBLUE_PROFILING` (never in Release). Use it before
guessing.

On device, in increasing order of effort:

| Tool | Use it for |
| --- | --- |
| **OVR Metrics Tool** | Leave running during play. Thermal throttling is invisible without it and looks like a mystery regression twenty minutes in. |
| **ovrgpuprofiler** | Real-time GPU metrics from a CLI. Already on the headset, nothing to install. First look. |
| **RenderDoc Meta Fork** | Tile-level render stage traces and per-draw metrics. Plain RenderDoc cannot see the tiler, and on Adreno the tiler is the whole story. |
| **Snapdragon Profiler** | CPU side — XMA decode, the recompiled guest code itself. |

## Performance rules specific to this project

From `research/20260828_1414_fast-dev-loop-and-quest-perf.md`, each of which contradicts something
the code currently does:

- **MSAA through `pResolveAttachments`, never a resolve shader, for the XR eye buffers.** On a tiled
  GPU the MSAA samples then never leave on-chip memory. The shader resolve in `src/gpu/resolve.cpp`
  exists to emulate Xbox 360 EDRAM resolves and should stay for that — but it is the wrong path for
  eye buffers and costs about 3 ms on Quest.
- **Prefer MSAA over render scale.** If fill-bound, scale resolution down and add MSAA, not the
  reverse.
- **FFR and subsampled layout together, never FFR alone.** Most "FFR did nothing" reports are a
  missing subsampled layout.
- **Measure before reaching for multiview.** It halves submission and binning, not shading. A
  composite-heavy renderer is more likely fill-bound, where multiview buys nothing.
