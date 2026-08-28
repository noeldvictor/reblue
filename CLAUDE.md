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

None of it is built yet. **[docs/VR_PORT_PLAN.md](docs/VR_PORT_PLAN.md) is the working plan** and
the place to look before starting anything; the section below is the condensed version.

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

**The build is not self-contained.** It needs `assets/default.xex` — the game executable from your
own discs — which is not in this repo (CI clones a private `zolaware/reblue-assets`). Without it,
`rexglue codegen` cannot run and `generated/` stays empty except for `rexglue.cmake`. Configure
still succeeds in that state by design; the build does not. Assume you cannot do a full local build
and reason from the source instead, unless the user says otherwise.

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

## Dev loop

**Invoke the `devloop` skill** (`.claude/skills/devloop/`) before building, running, deploying, or
diagnosing a slow build. The short version:

- **Do not go to the device.** Phases 0–5 run as a desktop executable against **Meta XR Simulator**,
  a virtual Quest presented as an OpenXR runtime on the PC. Selected with `XR_RUNTIME_JSON`, so
  simulator / Link / Monado is an environment variable, not a rebuild. The Quest is for comfort,
  real performance numbers, and driver bugs only.
- **Do not rebuild the guest.** `reblue_recomp` and `reblue_generated` are separate OBJECT libraries
  so a change in `src/` never touches the 54 recompiled TUs or the multi-megabyte shader cache. If
  they rebuild, a codegen input changed — find out which.
- **Never wipe the build directory.** Reconfigure in place.
- **Build one target**, e.g. `--target reblue_vk`, not the default that links two executables.
- **PCH or compiler cache, never both.** They fight — no compiler cache caches a PCH compilation,
  which is why CI sets `REBLUE_PCH=OFF`. Locally: PCH on while editing, cache on while
  reconfiguring.
- **A full local build is not possible in this tree** — no `assets/default.xex`, no SDK, so codegen
  cannot run. Say so rather than reporting a success that did not happen.
- **But the maths is testable.** `tools/xr_math_test/` compiles `xr_math.h` standalone against a
  stub `rex/types.h` and runs assertions on it. It caught the off-centre projection sign on its
  first run. Keep new maths in the dependency-free files so it stays reachable from here, and extend
  the test when it lands.

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

### The key scheduling insight

VR is the priority but Android is the blocker, and taken literally that puts the most important work
behind the longest pole. It does not have to be: **build and debug the VR camera on desktop Vulkan**
(`reblue_vk.exe` or the Linux Vulkan build) against a PC OpenXR runtime, wearing the Quest 2 over
Link, with a real debugger and RenderDoc attached. Port to native Android only once the camera is
right. Do not attempt to invent a VR renderer on a device with no debugger.

### Order of work

1. The Vulkan device-creation seam — **written, as `patches/plume-openxr-seam.patch`.** 69
   insertions across `plume_vulkan.h` and `plume_vulkan.cpp`, adding `VulkanInterfaceOptions` so an
   OpenXR runtime can name the instance extensions, device extensions, physical device and minimum
   API version. Backward compatible: the options pointer defaults to null and the no-argument
   `CreateVulkanInterface()` stays. It is a patch rather than a plume fork because forking a
   submodule means owning its history; see `patches/README.md`. **Not compiled** — no Vulkan SDK
   here — so building it is the first thing that happens on a real toolchain.
2. `src/xr/` — **done** for the dependency-free half: `xr_math` (handedness, view, per-eye
   projection), `xr_camera` (all four modes, world scale, recentre, snap turn), `xr_cull` (combined
   two-eye volume), `xr_settings` (15 cvars, wired into `ReblueApp::OnPostInitLogging`). 49 checks
   passing under `tools/xr_math_test`. The OpenXR half is next and needs a real toolchain.
3. Black stereo frame on the Quest over Link, from the desktop Vulkan build.
4. Stereo scene rendering, then the 6DOF camera and its modes.
5. In parallel and independently, because it is pure information and the longest pole: attempt the
   ReXGlue SDK `android-arm64` cross-build.
6. Tourist mode whenever a quick win is wanted — it touches nothing else, and
   `src/engine/character.h` already exposes `PlayableCharacter::SetHP()` with
   `src/engine/stat_breakdown.h` documenting exactly which block to write for maxed stats.
7. Cel shading last, and only once there is a real frame budget to measure against.

Do not start with OpenXR on-device. Do not start with cel shading.
