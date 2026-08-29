# Research: the desktop build was never actually blocked

Date: 2026-08-29 05:00
Topic: getting a Windows Vulkan build, and with it the Meta XR Simulator route.

CLAUDE.md said "a *full* Windows link additionally needs vcpkg, which is not installed here" and
recommended syntax-checking individual sources instead. That has shaped months of work - it is why
every measurement went to the headset, and why the simulator route the same file recommends was
never used. **It was wrong.** Everything needed is installed:

| | |
| --- | --- |
| vcpkg | `C:/vcpkg`, with `vcpkg_installed/` already populated |
| clang | 22.1.8, `C:/Program Files/LLVM/bin` |
| ninja, cmake | on PATH |
| SDK slice | `out/sdk/win-amd64`, complete with import libraries |
| `default.xex` | present, 8,040,448 bytes |
| game data | `out/game`, 8.9 GB, fully extracted |

## The one real trap

`find_package` results cache, and this tree had been configured for Android, so **six packages in
the Windows cache pointed at `out/sdk-android-install`**: `rexglue_DIR`, `fmt_DIR`, `spdlog_DIR`,
`SDL3_DIR`, `VulkanHeaders_DIR`, `VulkanMemoryAllocator_DIR`.

An Android install has no Windows import libraries, so the failure is
`IMPORTED_IMPLIB not set for imported target "rex::runtime" configuration "Release"`, which reads
like a corrupt SDK slice and is not. The fmt one is worse: it fails much later, at the link of the
*host* tool `XenosRecomp`, as undefined `fmt::v12::vformat` symbols.

`VulkanHeaders` and `VulkanMemoryAllocator` are header-only and platform-neutral, so those two are
fine left alone. The other four need repointing:

```sh
export PATH="/c/Program Files/LLVM/bin:$PATH"
export VCPKG_ROOT="C:/vcpkg"
R="$PWD/out/sdk/win-amd64"
cmake --preset win-amd64-release -DREBLUE_D3D12=OFF -DREBLUE_OPENXR=OFF -DREBLUE_BUILD_INSTALLER=OFF \
  -Drexglue_DIR="$R/lib/cmake/rexglue" -Dfmt_DIR="$R/lib/cmake/fmt" \
  -Dspdlog_DIR="$R/lib/cmake/spdlog" -DSDL3_DIR="$R/cmake" \
  -Dutf8cpp_DIR="$R/share/utf8cpp/cmake"
cmake --build --preset win-amd64-release --target reblue
```

With `REBLUE_D3D12=OFF` the target is `reblue`, not `reblue_vk` - that name only exists when both
backends are configured. The output is still `reblue_vk.exe`.

## A real bug this found

`present.cpp` did not compile without OpenXR. `EnsureOffscreen`, `g_offscreen` and `g_offscreen_fb`
sat inside `#if defined(REBLUE_OPENXR)` while the use site in `Present` did not - it is gated at run
time on `XrCompositorPacing()`, which is a plain `false` in that configuration rather than an `#if`.
Android always builds with OpenXR on, so the desktop build has been broken since the offscreen
target landed and nothing noticed. The definitions reach no OpenXR header, so they moved out of the
guard.

**`reblue_vk.exe` links: 44.6 MB.**

## Where it stops, honestly

The executable builds and runs - 100 seconds without exiting - but **does not load the game**, and
this is not yet solved:

- `--help` prints `cvar: CLI11 parse error: This should be caught in your main function`, so
  command-line parsing is failing before anything is applied. `--game_data_root=...`,
  `--game_data_root ...` and the positional `game_directory` were all tried; none took.
- `EarlyInstallRoot()` therefore returns nullopt, `OnPostInitLogging` returns early at
  `reblue_app.cpp:542`, and no log file is ever opened - which is why the process looks silent
  rather than broken.
- It was configured `REBLUE_BUILD_INSTALLER=OFF`, so the install-registry fallback is not there
  either.

### What was tried

`install_registry.cpp` **is** compiled even with `REBLUE_BUILD_INSTALLER=OFF` (the object file is
there), so `EarlyInstallRoot`'s registry path is live and does not need the wizard. The record was
written by hand:

```powershell
$k = "HKCU:\Software\Zolawareeblue\Install"      # kInstallKey, install_registry.cpp:45
New-ItemProperty $k InstallRoot   "C:\...eblue\out" -PropertyType String -Force
New-ItemProperty $k SchemaVersion "3"                  -PropertyType String -Force  # kInstallSchemaVersion
New-ItemProperty $k Renderer      "vulkan"             -PropertyType String -Force
```

`game_data_path()` is `install_root/game`, so `InstallRoot` is `out`, not `out/game`, and
`out/game/default.xex` exists, which is the check at `install_registry.cpp:131`.

That changed the behaviour: **before the registry, every run logged
`Fatal: reblue - game not installed` and stopped. After it, no run writes a log at all.** So the
install check is being passed and it is now stopping somewhere earlier than logging init.

### Where it actually is

A window exists and is healthy, and the process is doing nothing:

```
MainWindowTitle : re:Blue      Responding : True
CPU             : 0.45s total after 100 seconds
WorkingSet      : 21.3 MB      Threads : 5
```

21 MB and no CPU is not a loading game - the guest heap alone would dwarf that - so it is **parked
on a modal dialog** before the guest starts. No log is written, which is consistent with stopping
before `OnPostInitLogging` finishes.

Both `--game_data_root=...` and `--game_data_root ...` and the positional `game_directory` were
tried, in forward-slash and backslash form. `--help` returns
`cvar: CLI11 parse error: This should be caught in your main function`, so **command-line parsing
is broken on this build regardless of the flag** - the error is caught and swallowed instead of
being reported, which would hide every bad flag on desktop and is worth fixing on its own.

The next person should attach a debugger, or run it interactively and read the dialog, which takes
about ten seconds of a human's time and is not reachable from a shell. Fixing the swallowed CLI11
error is the durable fix.

**None of `bd_render_scale`, `bd_shadows` or `bd_reflections` has been verified.** The desktop build
is the way to verify them without a headset and it is one dialog away.
