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

| Submodule | Fork | Branch | Carries |
| --- | --- | --- | --- |
| `thirdparty/plume` | `noeldvictor/plume` | `reblue-openxr` | `VulkanInterfaceOptions`, so an OpenXR runtime can name the instance extensions, device extensions, physical device and minimum API version |
| `thirdparty/XenosRecomp` | `noeldvictor/XenosRecomp` | `reblue` | Unmodified so far. `zolaware-main` on the same fork carries the full upstream history it was taken from |

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
  needs vcpkg, which is not installed here; short of it, syntax-check individual sources against the
  real SDK headers in `out/sdk/win-amd64/include`. Never report a build success that did not happen.
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
| **Mono projection layer - the "in the world" mode** | **Built, never seen render** |
| **Character-anchored camera modes** | **Do not work** |
| Stereo, per-eye render | Not started |
| Cel shading, tourist mode | Not started |
| Sun occlusion descriptor set on Adreno | Dropped, not fixed |

Three of those rows need saying plainly rather than being read past:

- **`SubmitCharacter()` is never called.** `CharacterAnchor` has no source, so `ThirdPerson` and
  `FirstPerson` fall back to the game's own camera position. The character-anchored modes do not do
  what their names say.
- **The projection layer has never been observed producing an image.** It is built, committed, and
  replaces the quad in every non-Cinema mode. The one capture taken of it was black; a NaN was found
  and fixed after that, and the headset went offline before a retest.
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
or a number read off a run - and `bd_xr_autoplay` exists so that needs nobody in the headset. Say
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

### The bottleneck is not VR

**Measured, VR switched off entirely**: the same field scene costs 2925 draws, 108.9ms on the GPU
fence, 183ms a frame - identical within noise to VR on. **Blue Dragon runs at 5.5 fps on a Quest 2
with the flat renderer.** The session, the layer, the camera composition and the pad cost
essentially nothing. The port is slow; VR was never the reason.

And the GPU half is **insensitive to resolution**: 720p and 360p both cost ~110ms. So it is not
fill, not blend, not framebuffer bandwidth - which eliminates every quality setting, and foveated
rendering with them, since that only reduces shading rate.

What is left is ~2925 draws a frame on a tile-based renderer, which points at the **binning pass** -
a tiler runs all geometry through vertex processing before shading, and that scales with draw calls
and vertex count, not pixels. Not yet proven: `gpu_total_ms` reads 3.5ms against a 110ms fence, but
that column is unreliable at this draw count (one timestamp per draw into a 512-entry pool). The
next step is `ovrgpuprofiler` or Snapdragon Profiler, not another guess.

See `research/20260828_2330_the-real-bottleneck.md` for the eliminations, and note that
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

1. **Stereo.** The one thing standing between this and actual VR. Today the scene renders once, from
   one eye, onto a world-locked quad - a cinema screen rather than a world you are inside. It needs
   either the guest's scene drawn twice per frame, or per-view matrices reaching shaders whose
   constants XenosRecomp already baked. **This is a renderer change, not an XR one.** The camera
   modes are composed and delivered every frame already, so the 6DOF plumbing is live and waiting.
   Budget: the GPU is 97% idle, so drawing twice is affordable; the CPU is the constraint.
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
