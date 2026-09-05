<img width="1480" height="662" alt="Untitled-1" src="https://github.com/user-attachments/assets/1779fdfd-bc3a-416d-8b6c-38874d8eae93" />



> [!CAUTION]
> **This is a personal, vibe-coded fork. It is AI-driven experimentation for my own amusement.**
>
> I am not looking for users, testers, bug reports, feature requests, support questions, or
> Discord pings about it. Nothing here is supported, nothing here is promised, and most of it
> is written by an AI under loose supervision and pushed without ceremony. It will break. It
> will stay broken for a while. That is fine, because it is a toy.
>
> **Please do not bother me about it.** If you want something out of this: fork it and do the
> work yourself. That is genuinely the intended workflow, and the license permits it.
>
> If you want a real, working, supported build of re:Blue, go upstream:
> **[zolaware/reblue](https://github.com/zolaware/reblue)**. All the credit for this project
> belongs there. Issues and pull requests on *this* repo may be closed unread.

> [!IMPORTANT]
> re:Blue is an unofficial project, not affiliated with or endorsed by Microsoft, Xbox, Mistwalker,
> Artoon, or Sega. It ships no game data. You supply that from your own discs.


# re:Blue (personal fork)

re:Blue is a static recompilation of *Blue Dragon*: the original PowerPC program
is translated ahead of time into C++ and compiled as a native application. This
personal fork is moving the game's rendering out of the Xbox 360 model and into
a host-native Vulkan renderer, while gameplay remains recompiled.

## The goal: desktop host renderer, then Quest 2 VR

Move **all rendering** to the host: scene and material data, animation and GPU
skinning, shadows, reflections, particles/effects, post-processing, UI, frame
scheduling and presentation. The finished frame must no longer depend on guest
rendering execution, per-draw Xenos/D3D state translation or EDRAM emulation.

Preserve Blue Dragon's recognizable art style and readability, with freedom to
change assets, materials, lighting and geometry. Desktop asset conversion,
generated LODs, merged statics, impostors, offline texture mips and compression
are part of the work. The renderer is being built around multiview stereo,
occlusion/frustum culling, instancing, indirect draws and modern Vulkan features.

**Desktop comes first.** Fields, battles, cutscenes, menus, scene transitions,
reloads and both eyes must be verified before Quest 2 qualification and
optimization resume. The eventual Quest target is **72 Hz at 1440x1584 per eye**,
with shadows and foveation; it is not a result this fork has achieved. AYN Thor
is not an active test target.

Optional character cel shading and tourist mode remain side features. They do
not replace the host-renderer goal.

## Current state

Snapshot: 2026-09-04.

This is an unfinished renderer migration, not a fully native-rendering or
Quest-ready release.

| Area | Implemented | Still required |
| --- | --- | --- |
| Native mesh assets | Versioned persistent `.bdmesh` cache, triangle lists, shared host GPU buffers and existing generated LOD support; enabled by default | Asset-level discovery/loading, independent native layouts/materials, dynamic geometry and cache streaming/eviction |
| Material properties | Shared, content-keyed `.bdmat` assets for supported diffuse/specular/reflection recipes, independent cooker/loader and bounded residency; enabled by default | Native texture/lighting definitions, asset-level scene bindings, remaining draw recipes and replacement of the shader-register compatibility boundary |
| Receiver shadows | Host policy composition from current node visibility and model controls for supported direct-tree draws; enabled by default | Native pass/visibility producers, persistent feature policy and remaining draw recipes |
| Texture assets | Persistent `.bdtex` assets, independent mip cooking, shared host GPU ownership, direct immutable material bindings and native stable samplers; enabled by default | Asset-level scene associations, dynamic/inherited inputs, remaining imports and headset-specific formats |
| Resource uploads | Bounded host staging pages, fence-safe reuse/retirement, separate from shader constants | Complete native dynamic-geometry producers and asset streaming/backpressure |
| Deferred work | Live host depth from explicit bounds/fixed policies, host ordering, bounded whole-batch planning and corrected entry relocation | Native scene/pass inputs, remaining entry fields, engine storage and the guest list consumer |
| Scene submission | Host traversal and draw replay, frustum/occlusion culling, instancing, vertex pulling and indirect submissions | Replace retained guest draw templates and material/constant producers; remove remaining guest resource dependencies |
| Frame and VR | Host targets/post-processing, layered multiview presentation and desktop OpenXR test runtime | Complete host frame scheduling, effects/UI/animation ownership and representative full-game visual checks |
| Desktop verification | Native mesh tests pass; a 120-frame flat-view correctness sequence showed no jumps over 6% or cyan patches | A 64-frame lighting defect persists in the retained template path with native meshes enabled or disabled; the distant diorama captures do not establish stereo depth |
| Android / Quest 2 | ARM64 build/APK and OpenXR/controller foundations exist from earlier work | Full desktop completion gate, then fresh device qualification and optimization |

The mesh/capture evidence and its limits are recorded in
[the native mesh research note](research/20260904_1713_native-mesh-assets-and-capture-ownership.md);
[the native material note](research/20260904_1748_native-material-properties.md)
records material-source checks and flat/multiview correctness comparisons.
The persistent material contract and standalone cooker are documented in
[Native material assets](docs/NATIVE_MATERIAL_FORMAT.md).
The [native texture contract](docs/NATIVE_TEXTURE_FORMAT.md) covers texture
files, the independent mip cooker, native sampling and the remaining resource bridge.
The [binding checkpoint](research/20260904_1946_native-material-texture-bindings.md)
passed its short flat capture. [Host upload pages](docs/HOST_UPLOAD_ARENA.md)
remove the subsequent texture/constant-buffer wrapping hazard, but the later
scene still has unqualified dark/missing-geometry frames; see
[the upload evidence](research/20260904_1959_host-upload-pages.md).
The [receiver-shadow checkpoint](research/20260904_2041_native-shadow-receiver-inputs.md)
passes its short flat sequence and input checks, but reproduces the 64-frame
multiview defect. Its longer baseline also confirms the later scene remains
broken after the upload lifetime fixes.
The [host deferred-work checkpoint](research/20260904_2055_host-deferred-work.md)
removes guest allocation/sorting execution from its normal path, with passing
standalone and short flat checks; the multiview defect remains reproducible.
The [live depth checkpoint](research/20260904_2122_live-native-deferred-depth.md)
also replaces initial depth execution and stale replay keys with host calculation
from current transforms. Its input comparison and normal flat capture pass;
the multiview defect and incomplete stereo-depth qualification remain.
Passing this desktop slice does not establish full-game coverage or headset
performance.

## Project documentation

- [Host renderer transition](docs/HOST_RENDERER_TRANSITION.md): active scope,
  completion checklist and remaining dependencies.
- [AGENTS.md](AGENTS.md): canonical instructions for coding agents, including
  build/verification rules and frequent, scoped commits and pushes.
- [CLAUDE.md](CLAUDE.md): thin import of `AGENTS.md`, so shared instructions are
  maintained in one place.
- [Research](research/): dated experiments and evidence, not current promises.
- [Original VR plan](docs/VR_PORT_PLAN.md) and
  [archived project notes](docs/archive/CLAUDE_2026-09-04.md): historical context;
  superseded wherever they conflict with the current transition.

## Desktop verification

The main desktop loop uses the Vulkan executable and
[the repository's headless OpenXR runtime](.claude/skills/vrsim/SKILL.md).
It exercises the VR path without a headset; it cannot prove Quest performance,
device-only foveation or comfort.

Desktop settings go in `profiles/default/reblue.toml` under the install root.
Use `bd_xr_autoplay` for field-scene bring-up and `bd_capture_after_s`,
`bd_capture_min_draws` and `bd_capture_frames` for capture sequences. Verify the
live settings in the log and inspect the actual images.

- `tools/capture_seq.py` flags neighbouring-frame changes.
- `tools/capture_cyan.py` checks a known visual artifact.
- `tools/stereo_check.py --raw <capture> --stacked` examines layered stereo.
  Featureless black bars/sky are inconclusive, not proof of depth.

Multiview is the target stereo path. Do not enable legacy side-by-side
`bd_stereo` alongside `bd_stereo_multiview`. Capture the final presented eyes
when qualifying presentation; `bd_mv_capture_array` selects a scene target
instead.

## Table of Contents

- [Renderer Goal](#the-goal-desktop-host-renderer-then-quest-2-vr)
- [Current State](#current-state)
- [Project Documentation](#project-documentation)
- [Hardware Requirements](#hardware-requirements)
- [How to Install](#how-to-install)
- [Features](#features)
- [FAQ](#faq)
- [Building](#building)
- [Credits](#credits)
- [License](#license)

## Hardware Requirements

Requires all three retail Blue Dragon discs or their disc images. The desktop requirements below are inherited upstream baselines, not a fresh qualification of this experimental renderer. Android and Quest remain unsupported development targets.

### Minimum

- OS: Windows 10 version 1909 or later, Ubuntu 24.04 / Fedora 40 / SteamOS 3.6 or later, or macOS 13.3 Ventura or later
- Processor: Intel Core i5-4460 3.2 GHz 4 Core or AMD Ryzen 3 1200 or Apple M1, or equivalent
- Memory: 8 GB RAM
- GPU: Nvidia GTX 1050 Ti or AMD RX 570, or equivalent performance & VRAM. DirectX 12 with Shader Model 6.0, or Vulkan 1.2, or Metal
- Storage: 15 GB available space

### Recommended

- OS: Windows 11, SteamOS 3.6, or macOS 14 Sonoma or later
- Processor: AMD Ryzen 5 5600X or Intel Core i5-12400 or Apple M2, or equivalent performance, 6 physical cores minimum
- Memory: 16 GB RAM
- GPU: Nvidia RTX 2060 or AMD RX 5700, or equivalent performance & VRAM. 8 GB VRAM for 4K with MSAA
- Storage: 15 GB available space

## How to Install

This fork publishes no releases. [Download the latest upstream release for your platform](https://github.com/zolaware/reblue/releases/latest) or [build yourself](#building).

1. Blue Dragon shipped on three DVDs, and you will need a disc image of each one from your own copy of the game.

2. Run the executable. A setup wizard will guide you through the rest. You will be asked to point it at each of the three disc images in turn, and it will check each one before letting you continue. Once you pick where to install, the program copies itself there and restarts from that location, so you can delete the folder you extracted the zip into.

3. Pick a graphics quality preset. The wizard copies the game files out of the discs, and you are done. You may also install DLC from this installer or from the main menu under the config menu

The wizard only needs to run once. If something later goes missing from your install, launching with `--repair` reopens it on your existing install and copies back only what it needs.

## Features

These features are inherited from upstream re:Blue. The host-renderer transition is still in progress; this list is not a claim that every feature has been reverified in this fork.

### Graphics

- Resolutions up to 4K, windowed or fullscreen, on whichever monitor you pick
- Aspect ratios 16:9, 4:3, 16:10, 21:9, 32:9, plus auto and stretch
- Four quality presets, Low through Ultra
- MSAA up to 8x or SSAA up to 4x
- Anisotropic filtering
- Shadow quality and draw distance
- Depth of field adjustment
- Unlocked FPS with optional caps and VSync

### Quality of Life

- Unlocked frame rate, with optional caps at 30, 60, 90, or 120
- Save from the camp menu anywhere instead of only at save points
- Field of view adjustment, 45 through 120 degrees
- Skip the in-game tutorial pages
- Full area map on the world map screen, with zoom, floor switching, and a legend
- Optional map markers for the hidden items, chests, and barriers a floor still has, plus per-floor counts, carried onto the field compass
- The field HUD can fade out once you stop pressing anything, or stay off entirely
- Achievement list viewable in game, with eight new re:Blue achievements alongside the original ones
- Master volume control
- Separate center, rear, and subwoofer levels for 5.1/7.1 tuning
- Fully native keyboard and mouse support with cursor and look modes supported by mouse
- Every controller button rebindable to a key, with mouse sensitivity and cursor opacity of your own
- Menus take the mouse directly: hover a row to move the cursor, click to confirm, wheel to scroll
- Custom input based icons/glyphs for hud elements, following the device you last used or pinned to Xbox, PlayStation, Switch, or Steam Deck
- UI language and voice language chosen separately


### Mods and DLC

- Built-in mod manager
- Official DLC is supported

### Platforms and Languages

- Windows on DX12 or Vulkan
- Linux AMD64 and ARM64, including the Steam Deck and other handhelds
- macOS AMD64 and ARM64
- Custom menus in English, French, German, Italian, and Spanish

## FAQ

### Where is my save data and configuration stored?

Everything lives under the folder you installed to:

- Saves and settings: `profiles\default\`
- Your configuration file: `profiles\default\reblue.toml`
- Game files copied from your discs: `game\`
- Mods: `mods\`

### I want to update the game. Will I lose my save data?

No. Copy a newer build over your existing installation and your saves, settings, and mods are left alone. You do not need to reinstall or point the wizard at your discs again.

### How do I install mods?

Use the mod manager in the config menu. It accepts a mod folder or a zip file and puts everything in the right place for you

### Can I keep more than one set of saves?

Yes. Each profile is its own folder under `profiles\`, holding that profile's saves, settings, achievements, and DLC toggles. Launch with `--profile <name>` to pick one, and anything but `default` starts out fresh.

## Building

The build needs CMake, Ninja, a C++23 Clang toolchain, the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk), the checked-out submodules
and `assets/default.xex` from your own game disc. Windows also needs vcpkg
(including DXC). Game executables, generated guest code and cooked game assets
are not distributed in this repository.

Read the [dev-loop guide](.claude/skills/devloop/SKILL.md) for SDK/bootstrap
details and [AGENTS.md](AGENTS.md) for current rules. Some older setup passages
in the guide are historical; in particular, a Vulkan-only build's target is
`reblue`, not `reblue_vk`.

For the **already configured workspace** used by this fork:

```powershell
$env:PATH = 'C:\Program Files\LLVM\bin;' + $env:PATH
$env:VCPKG_ROOT = 'C:\vcpkg'
cmake --build --preset win-amd64-release --target reblue -j 4
```

That tree is configured with `REBLUE_D3D12=OFF`, `REBLUE_OPENXR=ON` and PCH on;
its output is `out/build/win-amd64-release/reblue_vk.exe`. These are local
configuration choices, not the untouched preset defaults. Reuse an existing
tree rather than rebuilding the guest to test host changes.

For a fresh desktop configure, bootstrap the SDK/codegen and dependencies first,
then select a Vulkan-only preset and configure OpenXR headers/loader as described
in the [vrsim guide](.claude/skills/vrsim/SKILL.md):

```sh
cmake --preset win-vk-release -DREBLUE_OPENXR=ON
cmake --build --preset win-vk-release --target reblue
```

[CMakePresets.json](CMakePresets.json) also includes Linux/macOS and
`android-arm64` presets. Android requires a cross-built SDK plus host-native
codegen/shader tools; `tools/build_apk.sh` packages the APK. Their existence is
not a claim that this revision has been qualified on each platform. Quest runs
remain deferred until the complete desktop host-renderer gate passes.

Standalone checks for the current mesh, material, texture/lifetime and stereo work:

```sh
cmake -S tools/native_mesh_test -B out/native_mesh_check -G Ninja
cmake --build out/native_mesh_check
ctest --test-dir out/native_mesh_check --output-on-failure
cmake -S tools/native_material_test -B out/native_material_test -G Ninja
cmake --build out/native_material_test
ctest --test-dir out/native_material_test --output-on-failure
cmake -S tools/native_texture_test -B out/native_texture_test -G Ninja
cmake --build out/native_texture_test
ctest --test-dir out/native_texture_test --output-on-failure
python tools/stereo_check_test.py
```

Use the configured Clang toolchain (on Windows, supply `CMAKE_CXX_COMPILER` and
`CMAKE_RC_COMPILER` if needed). The Python stereo tests require Pillow.

## Credits

Huge thanks to everyone who has put time into this. re:Blue would not be where it is without you.

**None of these people work on this fork, and none of them should be contacted about it.** The
credits below are upstream's, kept because they earned them and because the license says to keep
them. Everything re:Blue actually is came from [zolaware/reblue](https://github.com/zolaware/reblue);
everything broken in this repo came from me and a language model.

### re:Blue Development Team

- **[crack](https://github.com/tomcl7)** project lead and developer

- **[rcold](https://github.com/RC0ld)** developer and has done an absurd amount for this project. A lot of re:Blue looks the way it does because of him.

### Playtesting and Support

- **[infernozotza](https://github.com/Zotza)** - Playtester 
- **baus.98** - Playtester
- **[wolfaeterni](https://github.com/Zolawolf)** - Playtester and French Translations 
- **[griever666.](https://github.com/grv666)** - Playtester
- **[fungus](https://github.com/fungoid-creature)** - Playtester
- **[graine25](https://github.com/Graine25)** - macOS and Linux Development Support
- **[zhyxeryz](https://github.com/Zhyxeryz)** - Playtester and German Translations
- **[Azar42](https://github.com/Azar42)** - Playtesting
- **[ZolaKluke](https://github.com/ZolaKluke)** - Playtester
- **[emersed](https://github.com/RaphyEmersed)** - Playtester
- **[mrcmunir](https://github.com/mrcmunir)** - Spanish Translations
- **[mystixor](https://github.com/mystixor)** - German Translations
- **[toby](https://github.com/TbyDtch)** - Graphic Design

### Special Thanks

- The **[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk)** team, for the toolchain this project is built on.

- The **[hedge-dev](https://github.com/hedge-dev)** team, for [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) and for blazing the trail for Xbox 360 recompilations with [Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp).

- The wider **Xbox 360 emulation scene**, and the [Xenia](https://github.com/xenia-project/xenia) project in particular. A lot of the hardest problems were solved long before this project started.

## License

See [LICENSE](LICENSE).
