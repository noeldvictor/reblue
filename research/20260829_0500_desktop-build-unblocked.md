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

Next step is one of: build with the installer on and let it register an install, write the profile
`reblue.toml` under `out/profiles/default/` by hand, or fix the CLI11 error properly. The last is
the real fix - a parse error being swallowed like this would hide every bad flag on desktop.

**None of `bd_render_scale`, `bd_shadows` or `bd_reflections` has been verified yet.** The desktop
build is the way to verify them without a headset, and it is one config problem away.
