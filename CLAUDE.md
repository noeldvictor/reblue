# CLAUDE.md

Guidance for Claude Code working in this repository.

## What this repo is

A personal fork of [zolaware/reblue](https://github.com/zolaware/reblue), a static recompilation of
the Xbox 360 game *Blue Dragon*. The original PowerPC XEX is translated ahead of time into C++ by
the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) and compiled natively — there is no
interpreter and no JIT. Everything in `src/` is host-side code that either implements the console's
services or reaches into the recompiled guest through hooks.

**Fork goals**, in the owner's stated priority order:

1. **VR, with 6DOF head tracking as the critical item.** Third-person (head-anchored orbit),
   first-person, and diorama camera modes, plus a world-scale control. Character movement stays on
   the controller; the head drives the camera independently.
2. **ARM64 Android** — an AYN Thor first, then Quest 2 natively.
3. **Cel shading on characters**, optional and toggled from the in-game options menu.
4. **Tourist mode** — infinite HP, 999 stats, encounter suppression, for sightseeing in VR.

Blue Dragon boots into VR on a Quest 2 at its native 30 fps, with working Touch controllers and
the head pose driving the game's own camera. What is missing is genuine stereo: the scene renders
once, from one eye, onto a world-locked quad. Cel shading and tourist mode are untouched.
**[docs/VR_PORT_PLAN.md](docs/VR_PORT_PLAN.md) is the working plan** and the place to look before
starting anything; the section below is the condensed version.

The fork is explicitly low-stakes and AI-driven. Prefer working, understandable changes over
polish. Do not add support infrastructure (issue templates, contribution guides, changelogs) unless
asked — the README tells people to fork rather than file issues, and that is intentional.

## Modern VR technique is mandatory, and is not the thing to measure

**Multiview, foveated rendering and per-eye render targets are requirements here, not options.**
They are settled practice across the VR industry. Do not benchmark them against the path they
replace, do not build an A/B to decide whether they are worth adopting, and do not report a result
saying "the old way was about the same". That question is already answered, and running it spends
headset time, build cycles and attention that the actual problem needs.

**The actual problem is finding where in the static recompilation the guest has to change.**
Blue Dragon is 2006 code written for a Xenon: a hardware command processor made draw calls nearly
free, so the engine submits about a thousand individually placed scene nodes a frame and never
batches. Every modern technique has to land somewhere in that recompiled code, and *where* is the
research:

- **Multiview** needs the scene in a layered target and every vertex shader reading `SV_ViewID`.
  The seams were `D3DDevice_CreateSurface` for the target, `bdSceneNodeCullTraverse` for what gets
  submitted, and XenosRecomp's vertex epilogue for the per-eye geometry.
- **Foveation** needs the scene rendered *into the XR swapchain image*, which it currently is not:
  the guest owns its surfaces and present composites into the runtime's image. Until that changes,
  foveating the swapchain applies to a single blit that costs nothing.
- **Anything that reduces work per object** has to be found in the guest first. `bd_guest_census`
  exists to name the function, and it names `bdSceneNodeDrawSingle` - 23x everything else on device.

**Point measurement at two questions only**: *where is the seam*, and *is the implementation
correct*. Sampling layer 1 to confirm both eyes actually render is the right kind of check and it
found two real bugs. Whether the technique itself is a good idea is not a question this project
needs to ask.

## Build

```sh
cmake --preset win-amd64-release        # or linux-amd64-release, linux-arm64-release, mac-arm64-release
cmake --build --preset win-amd64-release
```

Prerequisites: CMake 3.25+, Ninja, Clang 20+ (the presets hardcode `clang`/`clang++`), vcpkg on
Windows, and the ReXGlue SDK. CI downloads a prebuilt SDK slice per platform; locally you can point
`REXSDK_DIR` at a `rexglue-sdk` source tree instead.

**The build is not self-contained**, but both missing pieces are obtainable in about a minute; the
`devloop` skill has the bootstrap.

- The SDK is a public release. `v0.10.0` is what `generated/rexglue.cmake` pins, and slices exist
  for win/linux/mac on amd64 and arm64. **There is no `android-arm64` slice**, which is what gates
  the Android target.
- `assets/default.xex` is the game executable from your own discs, ~8 MB inside a 7.8 GB ISO.
  `tools/extract_xex.py` walks XDVDFS and reads only the sectors it occupies, locally or straight
  off an adb-connected device. Under a second.

With both, `rexglue codegen` emits 219 files in ~7 seconds and is deterministic. A full Windows
*link* additionally needs vcpkg, since `find_package(directx-dxc CONFIG REQUIRED)` is unconditional
on WIN32 even for the Vulkan-only target.

Useful targets:

- `reblue_codegen` — runs `rexglue codegen` over `reblue_manifest.toml`, emitting the recompiled
  guest sources into `generated/`. Incremental via `generated/codegen.d`.
- `reblue_shader_cache_gen` — runs `XenosRecomp` over the Xenos shaders in `assets/`.
- `reblue_shader_hlsl_dump` — dumps the intermediate HLSL for inspecting shader translation.
- `reblue_prelink` — D3D12 only; DXC-links every spec-constant variant at build time.

Options worth knowing: `REBLUE_D3D12` (OFF selects Vulkan; forced OFF off Windows),
`REBLUE_VULKAN_EXE`, `REBLUE_BUILD_INSTALLER`, `REBLUE_PROFILING` (Tracy zones, never in Release),
`REBLUE_PCH`, and `REBLUE_OPENXR` (OFF by default, Vulkan-only, builds the VR session).

### Measuring on the desktop, which is now the first stop

A desktop run is ~90 seconds, needs no headset, no adb and no APK, and prints the same `[perf]`
per-surface census the device does. Set up once:

- The registry record has to name **the directory holding the exe** - running from anywhere else
  raises a modal "running outside of its install directory" with no log line and no exit, which
  looks exactly like a hang. `HKCU\Software\Zolaware
eblue\Install`, `InstallRoot` plus
  `SchemaVersion` = 3, and the game data at `<InstallRoot>/game` (a junction is fine).
- Command-line flags **do not work** - `--help` returns a swallowed CLI11 parse error. Cvars go in
  `<InstallRoot>/profiles/default/reblue.toml`, which is a flat TOML: `bd_xr_autoplay = true`.
- `bd_xr_autoplay` drives it into a real scene on desktop too.

**Read `other_ms` from `bd_perf_csv`, not the frame rate.** The desktop sits vsync-locked at exactly
16.67ms with the GPU wait at 0.03ms, so `dt_ms` measures the monitor and nothing else. `other_ms` is
CPU work per frame - 5.77ms in a field scene - and it is the desktop analogue of the Quest's ~62ms
floor, which makes CPU changes measurable here. The CSV lands in `logs/perf/`.

Averaging the whole file is wrong: loading frames drag the mean to 21.7 fps against a steady state
of 60. Take the last few hundred rows.

### Building for the desktop, which does work

The one non-obvious part is that `rexglue_DIR` caches to whichever slice was configured last, and an
Android install has no Windows import libraries - the symptom is
`IMPORTED_IMPLIB not set for imported target "rex::runtime"`, which reads like a broken slice and is
not. Point it at the right one explicitly:

```sh
export PATH="/c/Program Files/LLVM/bin:$PATH"
export VCPKG_ROOT="C:/vcpkg"
cmake --preset win-amd64-release -DREBLUE_D3D12=OFF -DREBLUE_OPENXR=OFF -DREBLUE_BUILD_INSTALLER=OFF   -Drexglue_DIR="$PWD/out/sdk/win-amd64/lib/cmake/rexglue"
cmake --build --preset win-amd64-release --target reblue
```

With `REBLUE_D3D12=OFF` the *target* is `reblue`, but **the binary it produces is still
`reblue_vk.exe`**. This file said otherwise; it is the target name that changes, not the output.

**This works, it was verified on 2026-08-29, and it is the fastest correctness loop available** -
about 2.5 minutes faster per iteration than the device, because there is no APK, no install, and a
field scene arrives in ~75s instead of ~130s. It also captures at 1920x1080 instead of a
quarter-scale 344x180, which is the difference between judging an image and guessing at it. The
device remains the only place performance numbers mean anything.

Three pieces of setup, all of which look like a hang when wrong:

- The registry record must name **the directory holding the exe** (see above).
- Game data at `<InstallRoot>/game` - a junction to an existing extraction is fine, so the copy
  already made for the device needs no duplication: `New-Item -ItemType Junction`.
- Cvars in `<InstallRoot>/profiles/default/reblue.toml`, flat TOML. Command-line flags do not work.

**`REBLUE_OPENXR=OFF` means no eye pose, so `ViewOverrideActive()` is false and the camera modes are
not exercised** - stereo geometry is checkable this way, the anchor and the camera modes are not.
**The Windows OpenXR loader is built and reblue links against it** - `out/xr-loader-win`, from the
`out/xr-headers/openxr` source, and it must be **static** (`-DDYNAMIC_LOADER=OFF`): an earlier tree
left a 23 KB stub DLL whose import lib links with `undefined symbol: xrEndSession`, which reads like
a missing dependency and is not. A real static loader is 3.1 MB.

What is missing is a **runtime**. Oculus and SteamVR are both installed here and neither works
without hardware - `OpenXR: no usable runtime (-4)`. **Meta XR Simulator is the headless one and is
not installed**; it is the single remaining piece for testing camera modes with no headset, and
`XR_RUNTIME_JSON` overrides the active runtime per process so installing it changes nothing globally.
See `research/20260829_1730_the-desktop-loop-works.md`.

`REBLUE_OPENXR=ON` additionally needs `REBLUE_OPENXR_INCLUDE` and `REBLUE_OPENXR_LOADER`. The
OpenXR-SDK source is already checked out under `out/xr-loader-android/` (that build tree is the
Android loader; the headers in its `include/` are platform-neutral), so a Windows loader is a second
configure of the same source rather than a new dependency. **That is the route to Meta XR Simulator,
and it is the only way to work on stereo without the headset.**

## Layout

```
CMakeLists.txt          hand-edited; "rexglue migrate" will not touch it
cmake/                  shader compilation, codegen wiring, embedding, build info
config/                 the manifest's function map and per-feature hook tables (TOML)
generated/rexglue.cmake SDK boilerplate — auto-generated, do not edit
docs/                   VR_PORT_PLAN.md, the working plan for this fork
reblue_manifest.toml    entrypoint, hook includes, recompiler flags
research/               dated research notes - see Research notes below
src/                    all host code
thirdparty/             plume (render backend), XenosRecomp, implot, stb, zstd, miniz, ...
```

`src/` subsystems, each its own `bd::` namespace:

| Path | Namespace | What it holds |
| --- | --- | --- |
| `src/core/` | `bd` | settings, i18n, threading, logging, encoding, app paths, shutdown |
| `src/gpu/` | `bd::gpu` | the renderer: device, draws, pipelines, shaders, present, resources |
| `src/engine/` | `bd::engine` | game-specific logic reached through hooks: battle, field, menus, saves, HUD |
| `src/platform/` | `bd::platform` | window, input, file dialogs, crash handling, HTTP, packaging |
| `src/vfs/` | `bd::vfs` | the guest filesystem over the installed game data |
| `src/ui/` | `bd::ui` | ImGui-based overlays and dialogs |
| `src/audio/` | `bd::audio` | audio settings and debug |
| `src/installer/` | `bd::installer` | the first-run disc wizard (desktop-only, `REBLUE_BUILD_INSTALLER`) |
| `src/xr/` | `bd::xr` | VR: head pose to per-eye matrices, camera modes, world scale, stereo culling, XR settings |

`src/xr/` splits deliberately. `xr_math.h`, `xr_camera.*`, `xr_cull.*` and `xr_settings.*` reach no
OpenXR header, so they compile in every configuration and can be reasoned about without a headset —
the coordinate conversion and the camera composition are the parts most likely to be subtly wrong,
and keeping them dependency-free is what makes them checkable. The session, swapchain, frame loop
and input go in `reblue_openxr_only` in `src/CMakeLists.txt` and build only under `REBLUE_OPENXR`.

**Keep it that way.** `xr_camera` and `xr_cull` take their settings pushed in (`CameraTuning`,
a margin argument) rather than reading `bd::xr::Settings`. That is not incidental — it is the only
reason `tools/xr_math_test` can exercise them, and it has already caught two real bugs. Do not
reintroduce a reach into the cvar singleton from either file.

**Handedness lives in one place.** OpenXR is right-handed with -Z forward; Blue Dragon is D3D9-era
left-handed with +Z forward, row-vector, row-major. They differ by a mirror on Z. Everything
crossing that boundary goes through `FromOpenXRPose` in `xr_math.h`. Do not convert at a call site.

## Android / Quest state

The Android target builds and runs on a Quest 2. `tools/build_apk.sh` packages it, game data comes
from `tools/extract_game_data.py`, and `args.txt` beside the game data appends cvars without
rebuilding the APK - use it, a 62 MB reinstall to change a log level is not a dev loop.

It runs: XEX loaded, 154 kernel imports patched, guest heap up, VFS mounting **1673 archives /
119346 record names** of real game data, Vulkan 1.1 on the Adreno 650, the guest rendering, Touch
controllers reaching the guest as a pad, and OpenXR compositing it at 30 fps - the game's native
rate. Those two VFS numbers are the quickest sanity check that the install is complete; see the
game data section above.

The one shortcut still in place is the sun occlusion descriptor set, which is **dropped** on Android
rather than fixed. Adreno exposes `maxBoundDescriptorSets = 4` where a desktop GPU reports 8 or 32,
and this renderer wants five. Dropping it costs four pipelines that still declare bindings there, so
they fail to create. The real fix is to collapse sets 0/1/2, which are one physical descriptor set
bound three times to satisfy three HLSL register spaces. See
`research/20260828_1720_quest-bindless-blocker.md` - and note that note's own correction: the
65536-entry bindless heap was a wrong first hypothesis, since the Adreno 650 does report
`descriptorIndexing`.

Diagnostics that exist now and should be used before guessing: the SDK logs to logcat on Android
(spdlog android_sink), `crash_handler.cpp` unwinds with `_Unwind_Backtrace` and reports the faulting
PC from the signal context, and the on-device log lives under the app's external files directory.

## Git Bash mangles device paths. This has cost hours, three times.

Under MSYS (the Bash tool here), **any argument that looks like a Unix path is rewritten to a
Windows one** before the program sees it. `/storage/emulated/0/...` becomes
`C:/Program Files/Git/storage/emulated/0/...`. `MSYS_NO_PATHCONV=1` disables that - and then
disables it for *every* argument in the command, including the local ones, which is the trap.

Three ways this has already gone wrong, all of which looked like something else:

| Symptom | Actually |
| --- | --- |
| `adb push` reports success, file never changes on device | Local path stayed `/c/Users/...`, adb could not stat it. The `&&` did fire, because the *echo* was unconditional. |
| Three consecutive perf experiments all measure "no change" | Every `args.txt` push had silently failed; the app was reading a stale file |
| `extract_game_data.py --adb-serial` dies with `short read: wanted 2048, got 0` | The device-side ISO path was rewritten to `C:/Program Files/Git/storage/...` |

**The rule: `MSYS_NO_PATHCONV=1` for the command, Windows-style paths for anything local.**

```sh
MSYS_NO_PATHCONV=1 adb push "C:/Users/.../args.txt" /storage/emulated/0/Android/data/com.reblue/files/args.txt
```

And verify, every time, because all three failures above reported success:

- Print the `adb push` result instead of discarding it. It says `1 file pushed` or it says `cannot stat`.
- After a launch, check `adb logcat -s reblue | grep 'args.txt added'` - the count must match the
  file. A stale `args.txt` is indistinguishable from a setting that does nothing.
- `adb shell cat` the file back when it matters.

Two more silent no-ops in the same family, worth knowing before trusting a measurement:

- **`bd_msaa` only accepts 0/2/4/8.** `--bd_msaa 1` is rejected by the validator and leaves it at 4.
  A run that "proves MSAA does not matter" may be a run with MSAA still on.
- **Cvars with `kRequiresRestart` need a `force-stop`**, not just a relaunch of the activity.
- **An APK missing `libopenxr_loader.so` installs cleanly and then cannot start.** It dies in
  `dlopen` before `main()`, so **no reblue log file is written at all** and the newest log on device
  is the *previous* run's - which then reports the old build's numbers under the new build's name.
  The only evidence is one logcat line. `tools/build_apk.sh` now finds the loader itself and refuses
  to package without it, but if a launch ever produces no new log, check logcat for `dlopen` before
  anything else.

## Game data: use `--all`, not the manifest

`tools/extract_game_data.py` was driven by `res/embed/installer/manifest.txt`, the desktop
installer's copy list. **That list does not name a single locale-specific file** - grep it for `_us`
and you get nothing - so a manifest-only extraction produces a build that boots, shows its title
screen, accepts input, and then dies the moment a new game starts:

```
[disc] file-load fatal, failed file: 'D:\database\camp\ene_dic_us.u16'
Fatal: File Load Error - Failed to load a required game file
```

All four of those records live in `pack/packmem_us.ipk`, a 2 MB archive **on disc 1** that the
manifest never mentions - and 1107 files on disc 1 alone are in the same position, mostly locale
packs and locale sound banks. So this is not a "you only extracted one disc" problem, which is what
it looks like at first; extracting all three discs through the manifest still misses it.

**Use `--all`.** It ignores the manifest and takes every file on the disc, still honouring
`--skip-media`. The manifest path only remains for reproducing what the installer does.

Nothing before "new game" touches any of those records, so the install looks completely healthy
until it isn't - which is why this got chased through the renderer first. If a fatal file load
appears, check the VFS counts in the log before suspecting code: a full three-disc `--all` install
mounts **1673 archives / 119346 record names**, where a manifest-only one mounts 1274 / 70008.

`--skip-media` is fine for iterating (it drops `movie/` and `snd_stream*`, most of the bytes) but it
does mean no intro movie.

The discs never have to be copied to the PC. The reader walks XDVDFS and pulls only the sectors it
needs, straight off an adb-connected device:

```sh
MSYS_NO_PATHCONV=1 python tools/extract_game_data.py   "/path/on/device/Blue Dragon ... (Disc 2).iso"   "/path/on/device/Blue Dragon ... (Disc 3).iso"   --adb-serial <serial> -o out/game --all --skip-media
```

**Push directories, never files in a loop.** One `adb push` per file, with a `mkdir -p` round trip
each, managed 3 files in 90 seconds. Pushing the changed subtrees instead moved 5 GB in under a
minute, at 120 MB/s - four orders of magnitude apart, and the only difference is where the loop
lives.

## Forked dependencies

Both patches outgrew `patches/` and now live as real history in the owner's forks. **Push changes
to these, do not add new patch files.**

**Work on `main` in every fork.** No feature branches, matching how this repo itself is worked.

| Submodule | Fork | Branch | Carries |
| --- | --- | --- | --- |
| `thirdparty/plume` | `noeldvictor/plume` | **`main`** | `VulkanInterfaceOptions` so an OpenXR runtime can name its extensions and physical device, plus Vulkan multiview: `viewMask` on the pipeline and framebuffer descs, and the `VkPhysicalDeviceMultiviewFeatures` enable without which a view mask is silently ignored |
| `thirdparty/XenosRecomp` | `noeldvictor/XenosRecomp` | `reblue` (see below) | `SV_ViewID` on every vertex shader and the per-eye off-axis skew that makes multiview stereo work, plus `vs_6_1` because `SV_ViewID` is a shader model 6.1 semantic |
| ReXGlue SDK (not a submodule) | `noeldvictor/rexglue-sdk` | **`main`** | The Android ARM64 port |

**XenosRecomp is the exception, and it needs a decision.** Its fork carries three branches:
`main` is the *original upstream* (hedge-dev), `zolaware-main` is the full history the reblue fork
was taken from, and `reblue` is the working branch our commits sit on. `main` and `reblue` have
**unrelated histories**, so the work cannot simply be merged onto `main` - moving it there means
force-pushing `main` to the reblue lineage, which discards the upstream history that branch
currently holds. `zolaware-main` preserves what matters, so this is safe to do, but it is
destructive and has not been done.

Until then `reblue` is the branch, and it is where the multiview work is pushed.

The ReXGlue SDK is **not** a submodule - it is cloned to `out/rexglue-src` and pointed at with
`REXSDK_DIR`, because no `android-arm64` release slice exists. Its Android port is
`noeldvictor/rexglue-sdk` branch **`android-arm64`**: fibers via vendored libucontext (bionic
aarch64 has no `getcontext` family at all), `ASharedMemory_create` for the guest address space,
an activity harness, an Android filesystem path, and logcat logging.

Two things to know before touching the SDK tree:

- **Never commit `thirdparty/libmspack`.** Its symlinks check out as plain text on Windows, so the
  submodule permanently reads as modified. Committing that corrupts the pointer.
- This repo vendors **XenosRecomp**, the Xenos *shader* recompiler. **XenonRecomp** is a different
  project - the PowerPC *CPU* recompiler - and is also forked, but nothing here uses it. Check which
  one a change actually needs.

## Dev loop

**Invoke the `devloop` skill** (`.claude/skills/devloop/`) before building, running, deploying, or
diagnosing a slow build. The short version:

- **`tools/stereo_check.py` answers "does stereo have depth" in one command.** It captures a frame
  with `bd_capture_after_s`, matches the two eyes band by band and prints a verdict: **FLAT** (the
  eye offset is proportional to `clip.z` and divides out to a constant slide), **INVERTED** (crossed
  the wrong way, so the world renders pseudoscopic), or **OK**. It is checked against the three real
  captures that produced those states, so it is a regression test and not just a report. Run it
  after anything touching the camera, the projection or the shader recompiler.
- **Sweep with `tools/bench_quest.py`, never adb by hand.** Every performance knob reaches the game
  through `args.txt`, so a whole matrix costs no build and no reinstall.
  `python tools/bench_quest.py levers` runs render scale, shadows and reflections one variable at a
  time and prints a comparison table; `fill` runs the scissor sweep; `--config "k=v,k=v"` runs
  anything else and `--targets` adds the per-surface draw census. It encodes the traps that have
  each cost hours here: every push result is checked rather than assumed, the proximity-sensor
  broadcast is sent twice, and it prints the reminder that a difference under ~30% is
  cross-restart noise rather than a result.
- **Know the four loop lengths, and pick the shortest one that answers the question.** Measured
  here: a cvar experiment via `args.txt` is **~30s and builds nothing**; a `src/` change is
  **~10s to compile**, ~15s to package, ~5s to install, ~30s to the title screen; a codegen flag
  change is **~8s for codegen plus ~77s** because all 54 recompiled TUs rebuild; and a full
  disassembly measurement out of `libreblue.so` needs no device at all. Most questions asked in this
  port were answerable at one of the shorter lengths than the one reached for.
- **The device loop is about 60 seconds, and that turned out to be fine.** Measured, on this
  machine: incremental `src/` build **~10s**, `tools/build_apk.sh` **~15s**, `adb install -r`
  **~5s**, launch to title screen **~30s**. The whole VR port was built this way. The advice below
  used to say "do not go to the device, use Meta XR Simulator" - that is still the better loop *if*
  a desktop Vulkan toolchain exists, but this machine has no vcpkg so the desktop targets do not
  link, and the simulator was never needed. Do not treat going to the device as defeat.
- **Change a cvar without building anything.** `args.txt` beside the game data is read at launch,
  one argument per line. A settings experiment is a push and a relaunch, about 30 seconds, with no
  compile and no 62 MB reinstall. Check the `args.txt added N argument(s)` line in logcat, because
  a failed push looks exactly like a setting that does nothing.
- **Push directories, never files in a loop.** One `adb push` per file with a `mkdir -p` round trip
  each managed **3 files in 90 seconds**. Pushing the changed subtrees instead moved **5 GB in under
  a minute**, at 120 MB/s.
- **A XenosRecomp change needs two manual steps or it never reaches the device.** `REBLUE_XENOSRECOMP`
  points at `out/host-xenosrecomp/`, a separate host build tree that the Android build never
  rebuilds - and the shader cache does not depend on that binary either, so `ninja` reports "no work
  to do" and keeps the old `generated/shader_cache.cpp`. A shader-recompiler change therefore
  compiles, commits, deploys, and changes nothing at all. This is how the per-eye stereo skew sat in
  0 of 55 shaders for a day while the C++ around it looked correct.

  ```sh
  cmake --build out/host-xenosrecomp --target XenosRecomp
  rm -f out/build/android-arm64-release/generated/shader_cache.cpp
  cmake --build --preset android-arm64-release --target reblue
  ```

  **Verify in the emitted HLSL, not the source.** `--target reblue_shader_hlsl_dump` then grep
  `out/build/<preset>/hlsl_dump/` for whatever was added. Nothing else can tell the difference.
- **Do not rebuild the guest.** `reblue_recomp` and `reblue_generated` are separate OBJECT libraries
  so a change in `src/` never touches the 54 recompiled TUs or the multi-megabyte shader cache. If
  they rebuild, a codegen input changed — find out which.
- **Never wipe the build directory.** Reconfigure in place.
- **Build one target**, e.g. `--target reblue_vk`, not the default that links two executables.
- **PCH or compiler cache, never both.** They fight — no compiler cache caches a PCH compilation,
  which is why CI sets `REBLUE_PCH=OFF`. Locally: PCH on while editing, cache on while
  reconfiguring.
- **Bootstrap first, then most of this works.** The SDK is a public release and `default.xex` can
  be extracted from a disc image in under a second with `tools/extract_xex.py` — codegen then runs
  in ~7 seconds and is deterministic. See the `devloop` skill. A *full* Windows link additionally
  needs vcpkg. **It is installed** - `C:/vcpkg`, with `vcpkg_installed/` already populated, and
  LLVM lives at `C:/Program Files/LLVM/bin`. The desktop build **was configured, built and run on
  2026-08-29** and it works. This file claimed for weeks that it did not, while also stating further
  down that vcpkg was present - both sentences were in the file at once, and the whole VR port was
  built on-device with printf because nobody tried the route the file said was closed. Never report
  a build success that did not happen, and never leave a "this does not work" claim in place without
  a date and a reason.
- **But the maths is testable.** `tools/xr_math_test/` compiles `xr_math.h` standalone against a
  stub `rex/types.h` and runs assertions on it. It caught the off-centre projection sign on its
  first run. Keep new maths in the dependency-free files so it stays reachable from here, and extend
  the test when it lands.

## Reading the guest: the recompilation already did it

**Do not install a decompiler.** `generated/` holds the entire XEX translated to C++ - 223 files,
about 110 MB, **18,777 functions** - with the original PowerPC interleaved as comments and every
address from `config/functions.toml` applied as a real name. It is exact rather than reconstructed,
it is named, and it answers the question a decompiler is usually opened for: struct offsets.

```sh
grep -rln "DEFINE_REX_FUNC(bdPlayerFieldMovementUpdate)" generated/
```

```c
	// mr r31,r3
	r31.u64 = ctx.r3.u64;                     // r3 is the first argument
	// lwz r11,7224(r31)
	r11.u64 = REX_LOAD_U32(r31.u32 + 7224);   // and 7224 is a field in it
```

`ctx.r3` is the first argument, `r4` the second - the same register names `config/hooks/*.toml`
lists, so a hook body and the generated source read against each other directly. **Invoke the
`guest-source` skill** before going looking for anything in the original binary.

It is a build product: never edit it, never commit it, and if it rebuilds unexpectedly a codegen
input changed.

## Rewriting the recompilation is in scope

The owner has explicitly put the generated guest code on the table for performance work, and that
matters, because **the in-game bottleneck is not the GPU**. A field scene costs ~180ms of CPU
against ~1ms on the GPU fence, and that CPU is the recompiled PowerPC. Shaders, foveated rendering
and texture formats cannot touch it - they are all GPU-side, and the GPU is asleep.

So the expensive options are allowed: changing what codegen emits, hand-writing hot functions as
host `REX_FUNC` implementations (the mechanism exists and `config/functions.toml` already names 1608
candidates), or cutting work a VR port does not need the guest to do at all.

**Measure before rewriting anything.** Two tools exist and both work:

- **`bd_perf_csv`** - a per-frame CSV, no rebuild, set it in `args.txt`. This is the cheapest real
  measurement in the project and went unused for a long time. A field scene reads:
  `dt_ms 182.6 | fence_ms 110.0 | other_ms 72.2 | draws 2957 | pso_switches 1121 | fb_binds 23`.
  **2,957 draws per frame** is the number that reframes everything below.
- **`tools/profile_quest.py`** - simpleperf, no instrumentation, names all 27,080 recompiled
  functions directly.

Treat `gpu_total_ms` in that CSV with suspicion: `MarkDraw` writes a timestamp per draw into a
512-entry pool, so at 2,957 draws the pool saturates early and the column measures a fraction of the
frame, not the frame.

### What reading the code already found

`research/20260828_2100_guest-cpu-cost.md` has the detail. Three things worth knowing before
touching guest performance:

- **`non_argument_as_local` miscompiles the guest. Do not enable it.** It cuts `ctx.` accesses
  across `generated/` from 2,049,797 to 1,306,101 - a 36% reduction - and builds cleanly, which is
  exactly why it looked like a free win and was briefly reported as one. On device the game dies
  0.2s after the VFS mounts with `[disc] file-load fatal, failed file: '<unknown>'` - no preceding
  read failure, so the guest's own IO path is being miscompiled. Bisected against identical game
  data. **The static metric was completely disconnected from correctness**, which is the lesson: the
  flag is a promise about register liveness, and this codegen has no liveness pass to verify it
  with. `skip_msr` is the remaining unset flag and is left alone for the same reason.
- **Every guest memory access is `volatile`**, which costs real instructions, but unevenly. Built
  both ways and disassembled: `bdBuildViewMatrix` goes 1000 -> 818 instructions and loses **42% of
  its loads**, while `bdCameraRender` moves 1.5% and `bdPlayerFieldMovementUpdate` 0.8%. The win is
  in maths-heavy code with redundant loads to remove, not everywhere. `REBLUE_RELAXED_GUEST_MEMORY`
  turns it off and is **OFF by default**: `volatile` is presumably what stops a guest spin-loop's
  load being hoisted, and nothing has established Blue Dragon has no such loop.
- **Two things that look expensive and are not**, recorded so they are not investigated twice:
  FPSCR flush-mode switching is guarded by a cached compare and only writes FPCR on an actual
  change, and indirect dispatch is a range check plus a table lookup. Neither is a bottleneck.

### The vector unit is the big ARM64 opportunity

`research/20260828_2200_arm64-neon-vector-path.md`. **NEON is mandatory on ARMv8-A**, so the Quest 2
has it and the port already uses it: Xenon's VMX is translated to SSE intrinsics through SIMDe,
which maps to NEON. The problem is not instruction selection, it is that **the vector register file
lives in memory**. Counted out of the shipped `libreblue.so`:

| | instructions |
| --- | --- |
| Vector register memory traffic (q-reg `ldr`/`str`/`stur`/`stp`) | ~129,800 |
| Endian swizzling (`tbl`, `rev32/64/16`, `ext`) | ~18,600 |
| Actual float SIMD arithmetic (`fmul`/`fadd`/`fsub`/`fmla`/`fdiv`) | ~8,600 |

**Roughly fifteen memory operations per useful arithmetic one**, and byte-swizzling costs more than
double the maths. Every VMX op loads from `ctx.vN` and stores straight back, so a value written by
one instruction and read by the next round-trips through memory instead of staying in a register.

The GPRs do not have this problem **because there is a flag for them and it is on**. The SDK's eight
codegen flags cover LR, CTR, XER, CR, reserved, non-argument and non-volatile GPRs, and MSR - **not
one touches VMX**. The same optimisation that removed 743,696 context accesses for integer registers
has never been applied to the vector unit, in a game whose maths is mostly vector.

Three changes, in order, all in the forked SDK's codegen rather than `src/`:

1. **Vector registers as locals** - emit `simde__m128 v12;` and let the compiler allocate it,
   spilling to `ctx.vN` only at call boundaries. The large one.
2. **Hoist the endian conversion** so a register staying in host byte order across a function does
   not get swapped in and out for nothing.
3. **`fmla`** - only 248 fused multiply-adds against 719 `vmaddfp` in the source. SIMDe's
   `add(mul(a,b),c)` is not being contracted; `-ffp-contract=fast` is worth testing, since guest
   results are already not bit-exact with a Xenon.

Ruled out, so nobody re-runs it: **LSE atomics** (only ~11 LSE and ~24 exclusive-loop instructions
in 5.8 million - atomics are not hot, despite the build targeting baseline `armv8-a` with no
`-mcpu`), **fp16 and dotprod** (zero uses, and recompiled PowerPC would not produce them).

**These are static instruction counts, not a profile.** They say where the instructions are, not
where the time goes.

## Research notes

Findings from web research go in `research/` as `YYYYMMDD_HHMM_<slug>.md`, dated at the time of
writing, with the sources linked at the end. They are a log, not a wiki: write a new file rather
than editing an old one, so what was believed when a decision was made stays legible. When research
changes the plan, say so in the note *and* update `docs/VR_PORT_PLAN.md`.

## How the guest is patched

Two mechanisms, and it matters which one a change belongs in:

1. **Function replacement.** `config/functions.toml` names guest addresses; the SDK emits
   declarations, and host code defines `REX_FUNC(name)` to take the call over entirely.
   `src/core/hooks.h` adds `BD_NOOP` / `BD_NOOP_RETURN` for calls the host deliberately ignores.
2. **Midasm hooks.** `config/hooks/*.toml` places a callback at a specific instruction address, with
   named registers passed through, optionally redirecting control flow via `jump_address_on_true`.
   Bodies live alongside the feature in `src/`.

Hook TOMLs carry the guest addresses and a comment explaining what the surrounding guest code does.
Read the TOML before touching a hook body — the address comments are the only map of the original
binary you have. `REX_HOOK*` symbols live in `OBJECT` libraries and never `STATIC` archives, because
the linker drops the registration symbols from an archive.

## Render backends

`plume` (`thirdparty/plume`) abstracts D3D12 and Vulkan. Which one is compiled in is a build-time
choice, not runtime:

- Windows builds both `reblue.exe` (D3D12) and `reblue_vk.exe` (Vulkan) from one configure.
- Everything else is Vulkan only. macOS goes through MoltenVK (`REBLUE_MVK`); Linux builds its
  surface through the SDL3 window plume already owns.

`src/CMakeLists.txt` splits sources three ways: `reblue_backend_only` (compiles once per exe),
`reblue_d3d12_only`, and everything else (compiled once into the `reblue_common` object library).
Backend-conditional headers `#error` if included from a common TU — respect that split when adding
files.

**Shaders.** Guest Xenos shaders are translated to HLSL by `XenosRecomp` at build time, then
compiled by DXC to DXIL (D3D12) or SPIR-V (Vulkan, compressed with smol-v). The Vulkan path uses
real spec constants and needs **no runtime shader compiler**. Only D3D12 links shaders at runtime
via `src/gpu/shaders/dxc_link.cpp`, and `reblue_prelink` pre-bakes those variants anyway. This is
load-bearing for the port: a Vulkan-only Android build needs DXC on the *host* toolchain only.

## Conventions

There is no `.clang-format`. Match the surrounding file.

- Headers open with a Doxygen block: `@file` (path-relative), `@brief`, `@copyright`, `@license`.
  Keep the existing copyright lines on files you edit.
- `PascalCase` for functions and types, `snake_case` for locals, trailing underscore for private
  members (`install_root_`), `k`-prefixed constants (`kProgramFiles`).
- Namespaces close with a trailing comment, e.g. `} // namespace bd::gpu`.
- Comments explain *why*, not what — the CMake files are the house style for this, and it is worth
  imitating. A comment that restates the code is noise; a comment that records the guest-side reason
  a workaround exists is the whole value.
- C++23, no compiler extensions. `u32`/`u64` style integer aliases come from `rex/types.h`.
- Byte-swapping guest data uses `__builtin_bswap*` (`_byteswap_ulong` appears once, under MSVC).
  Guest memory is big-endian; host is not. Every read of a guest structure swaps.

## Porting notes: ARM64 / Android / Quest

Research current as of 2026-08. Correct this section when reality disagrees with it.

### Where the port actually is

Blue Dragon runs on a Quest 2 and renders in VR. Most of the section below this one was written
while that was still a plan, and is kept because the reasoning is still worth having — but read this
table first, because it is the part that gets stale.

| Piece | State |
| --- | --- |
| SDK cross-built for android-arm64, APK, install, launch | Works |
| XEX load, kernel imports, guest heap, VFS over real game data | Works |
| Vulkan on Adreno 650, swapchain, pipelines | Works |
| OpenXR session, quad layer, world-locked and correctly placed | Works, seen |
| Touch controllers as a guest gamepad | Works, drives the game |
| Head pose driving the guest's own view matrix | Works, seen moving |
| Full three-disc game data, a new game starts | Works |
| Title screen at 30 fps | Works |
| **In-game frame rate** | **4-8 fps, and ~180ms of it is CPU** |
| Mono projection layer - the "in the world" mode | Works, captured and looked at |
| Character-anchored camera modes | Anchored off the follow camera; unit-tested, untuned |
| Stereo, per-eye render | **Works.** Crossed disparity, correctly signed, 24.5 fps |
| Cel shading, tourist mode | Not started |
| Sun occlusion descriptor set on Adreno | Dropped, not fixed |

Three of those rows need saying plainly rather than being read past:

- **The anchored camera modes now have an anchor**, derived from the game's own follow camera
  (`CharacterFromFollowCamera` in `xr_camera.cpp`, fed from `ComposeView`). Blue Dragon's field
  camera sits behind the party leader and looks at them, so the leader is on the camera's forward
  axis at the follow distance and the camera's yaw is their facing - approximate, but built on data
  that provably updates every frame. Tuned by `bd_vr_anchor_distance` (0 restores the old fall-back)
  and `bd_vr_anchor_eye_height`; **the defaults are estimates and want one pass of tuning against a
  capture.** The direct route still does not work - `xr_player_anchor.cpp` hooks
  `bdPlayerFieldMovementUpdate` and that guest function never fires even with the character walking;
  see `research/20260829_1420_autoplay-walks-and-the-anchor-hook-is-dead.md` for what is already
  ruled out.
- **The projection layer produces an image**, and there is now a capture of it to point at. This
  entry said the opposite for weeks on the strength of one black grab taken before a NaN fix.
- **30 fps is the title screen.** Gameplay is 4-8 fps.

What the player sees is still a flat image on a world-locked screen: the game renders once, from one
eye. The camera modes in `src/xr/xr_camera.cpp` are composed and delivered every frame, so the
plumbing for 6DOF is live — but genuine stereo needs the guest's scene rendered twice per frame, or
per-view matrices reaching shaders whose constants XenosRecomp already baked. That is a renderer
change, not an XR one, and it is the next real piece of work.

**Keeping the headset awake unworn.** The proximity sensor suspends the app the moment nobody is
wearing it - `onResume` then straight to `onPause`/`onStop`, no log file, which looks exactly like a
startup hang. `adb shell am broadcast -a com.oculus.vrpowermanager.prox_close` makes it behave as if
worn, which is what makes unattended measurement possible at all.

**Regenerating `reblue_pch.h` does not invalidate the precompiled header.** Any codegen flag change
then fails with "file has been modified since the precompiled header was built" until the
`cmake_pch.hxx.pch` files are deleted.

**Verify the pixels, not a proxy.** Got wrong repeatedly here, so it is a rule now. "13 input
actions attached" is not "controllers work". "Swapchain format 37" is not "the colour is right". A
clean build is not a working feature. Each of those was reported as success on the strength of a log
line, and each was wrong or unverified. A VR claim is verified when a screenshot has been looked at
or a number read off a run - and `bd_xr_autoplay` plus `bd_capture_after_s` mean that needs nobody
in the headset. **`bd_capture_after_s` writes the composited frame to `logs/capture/`** as raw RGBA
with a one-line header; pull it and convert with PIL. Seconds rather than a bool because `args.txt`
is read once at launch, so a bool only ever catches the title screen - 143 lands in a field scene,
after the field comes up at ~130s and before autoplay starts walking at 150s. It cost one plume fix
(`copyTextureRegion` had no texture-to-buffer case and crashed on a null `dstTexture`) and it has
already paid for itself twice: it confirmed the projection layer, and it showed stereo has no depth.
Note the Quest system-screenshot intents complete and write nothing here, and `adb screencap` does
not see compositor layers, so this is the only route. Say
"built, unverified" otherwise. That is not a weaker claim, it is an accurate one.

Three lessons from getting this far, all of which cost hours and all of which recur:

- **Make it visible before debugging it.** Two multi-hour hunts were output Android discards; both
  fixes were one line once readable. Recorded in `.claude/skills/devloop`.
- **A symmetric test case cannot see a sign error.** Both the projection matrix and the quad's
  facing yaw shipped wrong signs that are zero in the obvious test pose. Test off-axis.
- **An involution that round-trips looks like a no-op and is not.** The OpenXR/game-space mirror is
  its own inverse, so mixing the two conventions in one expression compiles cleanly and is wrong.

See `research/20260828_1900_vr-camera-and-input.md` for the detail, including the hook seams
(`bdBuildViewMatrix` at 0x82286C40, `bdBuildProjectionMatrix` at 0x82168E18) and why Quest
controllers are invisible to SDL.

### Every measurement before 2026-08-29 was taken standing still

`bd_xr_autoplay` pressed START and then A, for ever, and **never touched a stick**. So the fill
sweeps, the culling sweeps and the 28.9 fps result were all measured with the character stationary -
not a representative frame, and the reason every hook on the player object silently never fired.

It now walks a slow circle. Two things follow, and both bite:

- **`kWalkStart` is 150s and has to be late.** At 26s the file menu is still up and a stick
  deflection moves the selection; the run then sits in a menu for ever, measuring 18 draws/frame
  with the camera pinned at (0, 1000, 500), which reads exactly like a hang. Launch to a field
  scene is ~130s on a Quest 2.
- **Walking triggers random encounters**, so a run leaves the field within about a minute and a
  longer settle makes it *more* likely to have wandered into a battle or a menu. Check the draw
  count before believing any number: a field scene is ~500-600 draws, a menu is 18. This is the
  first concrete reason to want tourist mode's encounter suppression as a *measurement tool*.

### Vulkan validation layers run on device, and should be used before guessing

There are none in the NDK. Fetch the Khronos Android build from the Vulkan-ValidationLayers
releases and package it - Android only loads layers from the app's own lib directory, which is what
`EXTRA_LIBS` in `tools/build_apk.sh` is for (also the route for a replacement Turnip driver).

```sh
EXTRA_LIBS=/path/to/libVkLayer_khronos_validation.so bash tools/build_apk.sh
adb shell settings put global enable_gpu_debug_layers 1
adb shell settings put global gpu_debug_app com.reblue
adb shell settings put global gpu_debug_layers VK_LAYER_KHRONOS_validation
adb shell settings put global gpu_debug_layer_app com.reblue
```

**Turn it off again** (`enable_gpu_debug_layers 0`) - it is slow enough to change what a frame time
means. It settled the multiview question in one run after three sessions of inference, and it
immediately surfaced an unrelated live bug: `Int64` is declared by every shader (the constant path
uses `vk::RawBufferLoad` at a `uint64_t` address) while `shaderInt64` is not enabled, which makes
the renderer's hottest path formally undefined on this device.

### Measurement precision: +/-30% across restarts. Read this before trusting an A/B.

Within a single run the frame is stable to about 3% (200.5-207.1ms observed). **Across restarts it
is not**: two runs at essentially the same draw count (2848 and 2834) measured 159ms and 205ms.
`bd_xr_autoplay` presses buttons on a fixed schedule, but load times vary, so the character is not
in the same place at the same elapsed time and the scene genuinely differs.

So any effect smaller than ~30% cannot be resolved by comparing two runs, and several results in
this session sit inside that band and must be treated as inconclusive rather than negative. The
conclusions that survive are the ones with effects far larger than the noise: capping draws
(112.8ms -> 0.1ms), VR on versus off, resolution 720p versus 360p, and capping PSO switches
(90ms -> 235ms).

To measure something smaller, do it **within one run** - a cvar that can be toggled live, or two
code paths alternating per frame - not by restarting.

### The bottleneck: guest shader constants are global memory loads

**This is the answer to why the port is slow**, and it is a property of the shader translation, not
of Blue Dragon and not of VR. See `research/20260829_0030_shader-constants-are-global-loads.md`.

`XenosRecomp/shader_recompiler.cpp:1345` emits every guest constant register as

```cpp
#define cN vk::RawBufferLoad<float4>(g_PushConstants.VertexShaderConstants + off, 0x10)
```

`VertexShaderConstants` is a `uint64_t` device address in a push constant, so **every `c[n]` read is
a raw global memory load, per invocation**. The indexed form used by skinning (line 1340) adds a
compare and a `min` on top. On a Xenos these were constant registers - the fastest read a shader
had. On an Adreno 650 a UBO read goes through the constant path and is usually hoisted at wave
launch, while a buffer-device-address load is ordinary global memory, per vertex.

A skinned vertex shader reading four bone matrices plus a WVP is 20-40 global loads per vertex. At
~400,000 vertices a frame that is 8-16 million uncached loads, and it measures as **~225ns per
vertex** where single-digit nanoseconds would be normal.

Measured from the dumped HLSL (`--target reblue_shader_hlsl_dump`): **86-89 raw loads per vertex
shader**, and in `bd_toon_vs_env` **24 of them are `a0`-relative** - `exMatrix(0 + a0)` and friends,
which is bone-matrix lookup for skinning. Those are the expensive ones: the bounds check cannot
fold, and the address is *divergent across the wave*, so the driver cannot use scalar loads and must
gather per-lane from uncached global memory. On a Xenos these hit the constant file and were nearly
free.

**The fix** is to bind the constant blocks as a uniform buffer instead of pushing a device address:
`shader_recompiler.cpp` (lines ~1340-1395) and `shader_common.h` on the fork
`noeldvictor/XenosRecomp`, plus `src/gpu/constant_buffers.cpp` and `src/gpu/draw.cpp` to bind a UBO
descriptor rather than passing `gpuAddress` through `setGraphicsPushConstants`. The data, the upload
heap and the alignment all stay; only how the shader reaches it changes. Needs the shader cache
rebuilt.

**Scope it before starting.** The constants change every draw, which is exactly why a device address
in a push constant was chosen - it needs no descriptor update. A UBO needs dynamic offsets (plume
does not expose them yet) or a per-draw descriptor write, which would be worse than today. And there
is no spare set: Adreno allows 4 and sets 0-3 are taken, so the UBO has to become a *binding* on the
shared texture set, with DXC shift flags to dodge `t0` and the variable-count array kept last.

### The bottleneck is not VR

**Measured, VR switched off entirely**: the same field scene costs 2925 draws, 108.9ms on the GPU
fence, 183ms a frame - identical within noise to VR on. **Blue Dragon runs at 5.5 fps on a Quest 2
with the flat renderer.** The session, the layer, the camera composition and the pad cost
essentially nothing. The port is slow; VR was never the reason.

### The bottleneck is fragments

**Proven fill-bound.** `bd_debug_fill_scale` shrinks the *scissor* to N percent of the viewport
without touching the viewport, so vertex work, draw count, pipeline state and every upload stay
bit-identical and only the surviving fragment count moves. A field scene, MSAA off:

| `bd_debug_fill_scale` | fragments | fence | frame | draws |
| --- | --- | --- | --- | --- |
| 100 | 1.0x | **141ms** | 210ms | 2812 |
| 50 | 0.25x | **17ms** | 98ms | 2902 |
| 25 | 0.0625x | **0.1ms** | 71ms | 3095 |

**3095 draws and 450,000 vertices cost the GPU 0.1ms.** Draws and geometry are free; fragments are
the entire GPU cost. The fall is super-linear because a tiler skips tiles that contain nothing.

**Two earlier conclusions were wrong, and both failed the same way - one experiment, two variables.**

- *"Draw-bound"*, from `bd_debug_max_draws` taking the fence from 112.8ms to 0.1ms. Removing a draw
  removes its fragments too, so that test can never separate the two. The scissor test holds draws
  fixed and still empties the GPU. There is no 60µs-per-draw anomaly and no binning problem.
- *"Insensitive to resolution"*, from `bd_max_render_height` moving nothing. It was resizing the
  wrong surface. A per-target draw census (`NoteDrawTarget`, keyed on the surface, in the `[perf]`
  line) at `bd_max_render_height=360` shows `1280x720: 2434 draws` **unchanged** beside
  `688x360: 22 draws`. The scene renders into a surface pinned to the design canvas
  (`kDesignCanvasWidth/Height`), created by the guest through `D3DDevice_CreateSurface`; the cvar
  sizes the output fit. Foveated rendering was dismissed on this evidence and should be reassessed.

`bdOutputResViewScaleHook` only ever scaled *up*, so the canvas was a floor - but removing that
guard is **inert**, the scene surface does not come through that hook. The seam is
`D3DDevice_CreateSurface_hook` (`src/gpu/hooks/resource.cpp`), which is where `bd_render_scale`
acts.

**There is also a hard CPU floor of ~62ms.** At `fill_scale=25` the GPU is idle and the frame is
still 71ms, so the CPU alone caps the port near **14 fps**: ~46ms guest simulation plus ~14ms draw
recording. Fixing fill completely gets a field scene to roughly 10-14 fps, not to 72. Both halves
have to be solved, and only the GPU half currently has a proven lever.

**Do not trust a static count of the emitted HLSL.** A pixel shader body reads like 143-158 global
loads per fragment because the constants are `#define`s that re-expand at every use. Hoisting them
to entry-initialised statics was implemented and **changed the compiled SPIR-V by +0.2%** - DXC
already CSEs them. Measured by rebuilding the shader cache and comparing size, no device needed.
Reverted. The same idea was separately tried and reverted for vertex shaders.

### Measured on a Quest, 2026-08-29

`python tools/bench_quest.py all`, which is the first thing to run and builds nothing:

```
configuration                            frame    fence     else    fps   draws
baseline                               170.1ms  108.6ms   53.2ms    5.9    2834
render_scale=50, reflections=false     129.0ms   68.7ms   52.0ms    7.8    2761
  + shadows=false                      129.4ms   65.4ms   55.6ms    7.7    2777
  + stereo (sep 0.06, conv 0.03)       164.3ms   91.5ms   63.9ms    6.1    2766
```

Four things this settles:

- **The fill levers transfer.** 37% off the GPU fence and 5.9 -> 7.8 fps, outside the +/-30% band.
  The desktop verification loop was worth building.
- **`bd_shadows` is not a lever.** 65.4ms against 68.7ms is inside noise. It reached the device
  unverified because the census hooks the colour target and cannot see a depth-only pass, and it has
  now measured as approximately nothing.
- **The CPU floor is real computation, not back-pressure.** 43ms of GPU time was freed and
  `elsewhere` moved 53.2ms to 52.0ms. It caps the port near **19 fps** on its own, so the
  node-submission cost has to be attacked directly and culling is worth building.
- **Stereo runs on the headset and costs what it should**: a second view of a half-scale scene plus
  ~8ms of doubled draw recording. **Stereo at half scale is cheaper than mono at full scale** -
  91.5ms of fence against the baseline's 108.6ms - which is what `bd_render_scale` is for. Not yet
  looked at through the lenses.

Best mono is **7.8 fps**, best stereo **6.1 fps**, against 13.9ms for 72 fps. Foveated rendering was
dismissed on the since-disproved reading that the frame was not fill-bound, and should be
reassessed.

### The CPU floor, attributed

At `bd_render_scale=25` the GPU fence is **0.1ms** and the frame is 71ms, so the CPU is the entire
port. Measured on device at that configuration:

| | of ~56ms |
| --- | --- |
| our draw recording | ~13ms (mutex 0.4, bindFB 2.0, flushState 10.5) |
| guest code | **~43ms**, overwhelmingly one function |

`bd_guest_census` counts calls into named guest functions per frame. On device:

```
bdSceneNodeDrawSingle   2084 calls   16130160 bytes of guest code
bdAnimBoneEvaluate       126 calls     706608
bdAnimationUpdate        112 calls     203840
everything else          0-3 calls    negligible
```

**23x the next consumer**, where the desktop build showed 1.9x - so tune this on the device, not on
the desktop. And the scene is **about a thousand individually placed objects**, not instanced
geometry: 2083 calls take 1270 distinct first arguments. So the fix is removing objects, and the
axis is distance.

Two culling knobs exist, both hooked into `bdSceneNodeCullTraverse` (`0x82282490`) so the guest
skips the draw on its own path with no control flow redirected:

| cvar | how it selects | measured |
| --- | --- | --- |
| `bd_cull_distance` | view-space distance; the centre at `r3` is view space with the camera at the origin, so it is a plain length | the intended lever |
| `bd_cull_bias` | bounding radius | 11% CPU, inside noise - it culls small objects and the cost is per object |

### The three fill levers

All three scale a surface the guest asks for, on the seam `bd_supersampling` already uses, so the
guest computes its own viewports, resolve rects and post-process chain from the smaller size. None
of them suppress draws - draws are free, fragments are not.

| cvar | default | what it does |
| --- | --- | --- |
| `bd_render_scale` | 100 | Scene colour and depth at N% per axis. 50 quarters the fragment cost. |
| `bd_shadows` | true | Off renders the sun shadow map at 64x64 instead of 1024x1024. |
| `bd_reflections` | true | Off pins the planar reflection to its 128-wide floor. The reflection re-renders the scene, so this is the largest single pass to remove. |

Bodies in `src/gpu/hooks/tweaks.cpp`, addresses in `config/hooks/render_tweaks.toml`. **Off forces
the smallest legal surface rather than skipping the pass**, because the receivers sample those
textures and a pass that never runs leaves them reading whatever was there before.

Do not reach for `D3DDevice_CreateSurface_hook` for this. Scaling there means lying to the guest
about a size it still computes viewports and texel offsets from, which needs a ratio fixup at every
site that reads back. It was written that way first and replaced.

See `research/20260829_0230_the-frame-is-fill-bound.md` for the full trail and
`research/20260828_2330_the-real-bottleneck.md` for the eliminations it corrects. Note that
`src/xr/xr_cull.cpp` is **dead code** - written, unit-tested, never connected to anything.

### The Quest frame budget

**The GPU is 97% idle.** At the Android defaults a frame is 32ms, of which 1ms is the GPU fence and
17ms is guest simulation and command recording. This inverts the usual mobile-VR advice, so before
optimising anything, read `research/20260828_2010_quest-frame-budget.md`:

- Shaders, fixed foveated rendering, ASTC transcoding and MSAA replacements all save **nothing**
  right now. They are GPU-side, and the GPU finishes in 1ms.
- LOD and culling are the exception, because they cut draw calls, which is CPU.
  `bdCameraViewFrustumTest` (0x82135030) is the seam and is already named.
- Fixed foveation becomes worth having *after* stereo, when the scene is drawn twice. Quest 2 has no
  eye tracking, so `XR_FB_foveation` is fixed-only.

Two defaults exist because of that note, and both are Android-only: `bd_max_render_height` 720 (the
renderer otherwise sizes the scene to the 3664x1920 headset panel, costing 119ms a frame for a
1280x720 game) and `bd_shadow_dimension` 1024.

**Measure on gameplay, not on the title screen.** Every number in that note was taken at the title
screen, and the title screen is not representative of anything - it is a static 2D image with the
GPU 97% idle. In an actual field scene, at the same 720p cap, the same build costs:

```
frame 237ms = xrWait 7.3 + submit 0.2 + present 0.0 + fence 108.3 + drain 1.6 + elsewhere 119.3
```

**Both ends are loaded**: ~108ms GPU and ~119ms CPU, against 1ms and 17ms at the title. So the
conclusion drawn there - that the GPU is idle and only CPU work matters - is true of the title
screen and false of the game. The MSAA default was chosen on that basis and is the first thing to
re-test: a real scene runs a multi-pass post chain that a static title screen never touches, and 4x
MSAA on a tiler pays for every one of those passes.

The honest state: the title screen runs at its native 30 fps, and gameplay does not. Do not quote
the title-screen numbers as the port's performance.

**A near-zero GPU fence does not mean the GPU is idle.** It means the GPU was not the last thing
waited on. A blocking present upstream hides the entire cost.

### What already works

ARM64 is not a new frontier here. Upstream ships a `linux-arm64` AppImage from CI and a `mac-arm64`
app, the ReXGlue SDK publishes a `linux-arm64` slice and has a dedicated ARM64 contributor, and the
recompiled guest code is ordinary architecture-neutral C++. The CPU side of "make it run on ARM" is
already done. `src/core/threading.cpp` is the only file with an x86 intrinsic (`_mm_pause`), and it
already falls back to `std::this_thread::yield()`.

**So the AYN Thor problem is not an ARM problem — it is an Android problem.** The Thor runs
Android 13 on a Snapdragon 8 Gen 2 (Adreno 740; the Lite is an SD865/Adreno 650). There is no
official Linux for it.

**Decision: native Android APK.** The proot/Termux route (running the existing `linux-arm64`
AppImage under a Debian rootfs) was considered and set aside — Vulkan on Adreno inside proot means
Turnip/AdrenoTools or Zink, and both performance and driver stability are a coin flip. The APK path
is also the one that shares work with Quest.

### What an Android target actually needs

Roughly in dependency order:

1. **ReXGlue SDK cross-built for `android-arm64`.** No such release slice exists;
   `.github/scripts/fetch-deps.sh` knows only `win-amd64`, `linux-amd64`, `linux-arm64`,
   `mac-arm64`, `mac-amd64`. The SDK source is public and already builds for aarch64 Linux, so the
   delta is bionic vs glibc, no GTK, Android's signal and threading rules, and its SDL3/ImGui/Tracy
   dependencies. This is the gate on everything else. Build it with `REXSDK_DIR` pointed at a source
   tree rather than chasing a release.
2. **Guest address space.** Xbox 360 recompilations reserve a fixed large virtual region so guest
   pointers are base + offset. Verify the reservation succeeds under bionic, and check page-size
   assumptions: Android 15+ requires 16 KB page support, while Horizon OS on Quest 2 is Android 12
   era at 4 KB.
3. **Vulkan through plume, on Adreno.** Prior art (`SansNope/UnleashedRecomp-Android`, an AI-built
   Android port of Unleashed Recompiled) needed a replacement Mesa Turnip driver for several Adreno
   generations, sysmem render mode on Adreno 6xx to dodge corruption and hangs, and MSAA disabled on
   Adreno 750. Expect the same class of problems. **Check BC texture support first** — Xbox 360
   textures are DXT1/3/5, and if `vkGetPhysicalDeviceFormatProperties` says no BCn, you need CPU
   decompression or an ASTC transcode, which is a real perf and VRAM cost.
4. **Toolchain.** NDK r29-ish, a CMake toolchain file, and a new `android-arm64` preset. Note that
   host tools (`rexglue`, `XenosRecomp`, `dxc`, `reblue_prelink`) must build for the *host* and run
   there; only the runtime cross-compiles. `REBLUE_D3D12` is already forced OFF off Windows, so the
   Vulkan-only path comes free.
5. **Platform layer.** `src/platform/` assumes a desktop: GTK/SDL file dialogs, desktop shortcuts, a
   crash handler, WinHTTP/libcurl. Android needs SAF for file access, an activity/APK harness, and
   probably Oboe for audio latency even though SDL3 audio should function.
6. **The installer.** `src/installer/` is a desktop wizard that wants three DVD images and produces
   roughly 15 GB of extracted output. On Android this becomes either a SAF-driven import flow or a
   "put the files here yourself" path under `Android/data/`. Set `REBLUE_BUILD_INSTALLER=OFF` early
   and side-load a pre-extracted `game/` directory to unblock the rest.
7. **Input.** SDL3 gamepad covers handhelds with physical controls. Quest has none — Touch
   controllers are OpenXR input, not HID gamepads, so Quest needs either a paired Bluetooth pad or
   an OpenXR-to-action-map shim.

### Hardware acceleration on device

Not a footnote — an Android build that ignores this is unplayable regardless of how good the camera
is. Prior art (`SansNope/UnleashedRecomp-Android`) needed a replacement Mesa Turnip driver for
several Adreno generations, sysmem render mode on Adreno 6xx to dodge corruption and hangs, and MSAA
disabled on Adreno 750. Plan for driver selection (bundled Turnip plus AdrenoTools import) rather
than trusting the stock blob.

**Check BC texture support before anything else.** Xbox 360 textures are DXT1/3/5. If
`vkGetPhysicalDeviceFormatProperties` reports no BCn, you need CPU decompression or an offline ASTC
transcode — a real cost in VRAM, load time, and install flow. `src/gpu/format.cpp` and
`src/gpu/texture_upload.cpp` are where it lands.

Also on the list: MediaCodec hardware video decode behind `bdMovieFrameConvert` / `bdMovieYuvQuadDraw`,
Oboe for audio latency, XMA decode cost (`XMAPlaybackConsumeDecodedData`), and on Quest specifically
fixed foveated rendering (`XR_FB_foveation`, large and nearly free) and Application SpaceWarp
(`XR_FB_space_warp`, Vulkan-only, needs motion vectors — and the game is natively 30 fps against a
72 Hz display, so this may be the difference between shipping and not).

### The VR camera

**The guest camera pipeline is already fully named in `config/functions.toml`.** This is the single
luckiest fact about the project — these addresses do not need finding:

- `bdCameraViewSetMatrices` (`0x82135228`) — the interception point; sets view and projection together
- `bdCameraViewFrustumTest` (`0x82135030`) — culling. Test once against a volume enclosing *both*
  eyes plus a head-motion margin, not twice per eye and not a single eye's frustum scaled up; that
  is what the VR industry converged on and it costs about half what per-eye queries do
- `bdBuildProjectionMatrix` (`0x82168E18`) — already hooked by `config/hooks/output_resolution.toml`
- `bdFieldCameraSetupFollow` (`0x821B1A58`), `bdCameraLookAt*`, `bdScriptOpCameraControl` — the
  game's own camera logic, which third-person mode suppresses
- `bdCameraRenderMotionBlur` (`0x8213CE90`) — disable in VR, non-negotiable
- `bdPlayerFieldMovementUpdate` (`0x82207858`) — locomotion, must become head-relative

Hardware reality: Quest 2 is an Adreno 650 (roughly SD865 class) with ~6 GB RAM and a practical
per-app ceiling around 4 GB, weaker than the Thor's 8 Gen 2, and stereo doubles raster cost at
72–90 Hz. The game being natively 1280x720 at 30 fps is the saving grace. Quest 2 was discontinued in
September 2024 with support running to roughly 2026–2027 — a fixed, non-improving target, which at
least means optimising for one known device.

### The key scheduling insight, and how it actually went

The original plan said: VR is the priority but Android is the blocker, so build and debug the VR
camera on desktop Vulkan against a PC OpenXR runtime, with a real debugger and RenderDoc, and port
to Android only once the camera is right. **That is still the right instinct, and it is not what
happened** - this machine has no vcpkg, so the desktop targets never linked, and the entire port was
built on-device with a 60-second loop and printf.

What made that survivable was not tooling, it was making the machine answer questions instead of
guessing. Every hard bug this port has hit was found by adding one line of output and reading it:

- 124ms of a 150ms frame in the flat present - found by printing a frame breakdown, which also
  killed three plausible hypotheses in the same line.
- A fatal file load 25 minutes into a renderer investigation - found by reading the log rather than
  the renderer.
- A camera seam that was reached but composing wrongly - found by logging the head pose and seeing
  it move.

So the rule that generalises is not "use the simulator". It is **make it visible before debugging
it**, which is the same lesson recorded in the devloop skill.

### Order of work

Items 1-5 of the original plan are **done**: the plume OpenXR seam compiles and runs, the SDK
cross-builds for `android-arm64`, `src/xr/` is complete on both sides of the OpenXR line, and the
game composites into a headset at its native frame rate with working controllers. What is left:

1. **Stereo works.** `bd_stereo=true` gives genuine depth: measured disparity far +21px, near +5px,
   **near - far = -16px** - crossed, correctly signed, monotone with depth. Before it was flat to
   2px. Verified from a capture with `bd_capture_after_s`; nobody has to wear the headset to check.

   The bug was that both paths applied the eye offset as `sep * clip.z`, and `clip.z ~= clip.w` past
   a few metres, so it divided out to a constant sideways slide with no depth at all. A lateral eye
   translation is a **constant** added to `clip.x`. Two more things had to be right at once: the
   shared constants must be multiview-only (`SV_ViewID` is 0 in the side-by-side path, so the shader
   was adding `-sep` to *both* eyes on top of the host patch), and the **left eye takes the positive
   constant** - backwards renders the scene pseudoscopic, and that is invisible in a symmetric test.

   It uses the side-by-side path, not multiview: multiview renders both layers correctly and then
   the **mono post chain** collapses them before present. Multiview is still the better architecture
   - one draw instead of two - and is worth returning to when the post chain can be made view-aware.

   **Cost is on the GPU and `fence` does not show it.** CPU only moves 19.0 -> 20.2ms, but the frame
   goes 34.6 -> 50.0ms because the compositor drops a pacing tier. `render_scale=20, cull 250,
   shadows off` claws back to 40.8ms / 24.5 fps. Getting stereo into the 33.3ms tier is the next
   performance job, and it is a fill problem with proven levers.
   See `research/20260829_1700_stereo-has-depth.md`.

2. **The occlusion descriptor set**, which is dropped rather than fixed on Android and costs four
   pipelines. Collapse sets 0/1/2 - one physical set bound three times to satisfy three HLSL
   register spaces - and the layout fits Adreno's four without dropping anything.
3. **Fixed foveated rendering** (`XR_FB_foveation`; Quest 2 has no eye tracking, so fixed only).
   Worth nothing today and genuinely useful the moment stereo lands.
4. **Tourist mode** whenever a quick win is wanted. It touches nothing else, and
   `src/engine/character.h` already exposes `PlayableCharacter::SetHP()` with
   `src/engine/stat_breakdown.h` documenting exactly which block to write for maxed stats.
5. **Cel shading last**, and only once there is a real frame budget to measure against.

Do not start with cel shading. Do not optimise the GPU before re-measuring - see the frame budget
section, where the intuitive moves all save nothing.
