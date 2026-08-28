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
- **Never assume a local build is possible.** This tree has no `assets/default.xex` and no ReXGlue
  SDK, so codegen cannot run and a full build will not complete. Say so plainly rather than
  reporting success. The dependency-free half of `src/xr/` exists partly so it can be reasoned about
  by reading.

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
