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

re:Blue rebuilds Blue Dragon as a native application through static recompilation, translating the original code into something your machine runs directly rather than emulating a console around it. That opens the door to things an emulator cannot reach: higher frame rates, modern resolutions, and real mod support.

This fork exists to poke at one question: **how far down does it go?** Upstream already runs on 64-bit
ARM under Linux and macOS. I want Blue Dragon in VR with a real 6DOF camera, on an ARM64 Android
handheld (an AYN Thor and friends), and on a Meta Quest 2. Also cel shading. Also a cheat mode so I
can wander around and look at it instead of fighting things.

## What this fork is trying to do

Roughly in priority order. **Blue Dragon renders in stereo 3D on a Quest 2, with head tracking and
Touch controllers, and a new game starts.** Stereo has real depth - measured off a captured frame as
crossed disparity, near geometry separating 16px more than distant. It is **not playable yet** - roughly 8-10 fps at full
render scale against the game's native 30 - and the quarter-scale setting that reaches 30 reads as
blurry through the lenses, so it is not a fix. The bottleneck is the recompiled guest running on the
CPU, not the GPU and not VR: the GPU executes a whole frame's command buffer in about 2ms, and
switching VR off entirely changes nothing. A clone needs two things before it can
build — the ReXGlue SDK (a public release) and `default.xex` from your own disc, which
`tools/extract_xex.py` lifts out of an ISO in under a second without copying it.

| Goal | State | Notes |
| --- | --- | --- |
| **VR with 6DOF head tracking** | Foundations built and tested | Head drives the camera, controller drives the character. |
| — camera maths | **Done, 49 checks passing** | Handedness, per-eye projection, all four camera modes, world scale, recentre, stereo culling volume. Compiles and runs standalone. |
| — third-person, head-anchored orbit | Composition done, anchor derived | Default mode. The party leader's position comes from the game's own follow camera - the leader sits on its forward axis at the follow distance. `bd_vr_anchor_distance` tunes it; the defaults are estimates and want one pass on hardware. |
| — first person | Composition done | Animations are authored third-person, so expect a novelty. |
| — diorama + world scale | Composition done | Tabletop view. Probably the mode that ends up working best. |
| — OpenXR session | **Working on a Quest 2** | Instance, session, reference space, swapchain, frame loop. The plume seam compiles and runs. |
| — head pose driving the game camera | **Working** | Composed onto the game's own view matrix at `bdBuildViewMatrix`, the same seam frame interpolation uses. One frame of latency, by construction. |
| — stereo rendering | **Working, with real depth** | `bd_stereo`. Every scene draw is submitted twice into left/right viewports with per-eye constants, and the two halves reach the compositor as per-eye `imageRect`s. Verified from a device capture: far +21px, near +5px, **near - far = -16px** - crossed and correctly signed. `tools/stereo_check.py` re-runs that check in one command. The eye offset must be a *constant* added to `clip.x`; the original `separation * clip.z` divides out to a flat sideways slide with no depth at all, which is what shipped for three sessions. |
| **ARM64 Android (AYN Thor, etc.)** | **Builds** | `libreblue.so`, 140 MB, ELF64 AArch64, linking against stock platform libraries only. The SDK cross-builds too. |
| — shaders | **Solved** | 142 cache entries. They live in `pack/!necessity.ipk`, zlib-compressed, which is why raw scans found nothing. `tools/extract_ipk.py` unpacks them. |
| — APK | **Installs and runs on a Quest 2** | `tools/build_apk.sh`, 62 MB. Six steps, no Gradle. |
| — game data on device | **Done** | Extracted with `tools/extract_game_data.py --all`, straight off an adb-connected device without copying the ISOs. **Use `--all`**: the install manifest names no locale files, so a manifest-only extraction boots, shows its title screen, and dies the moment a new game starts. The VFS mounts 1673 archives / 119346 records with `--all`; those two numbers are the quickest check that an install is complete. |
| — **the game renders on a Quest 2** | **Working** | Title screen up: "press START", the 2007 Mistwalker/Microsoft copyright lines. Guest code executing, VFS serving the discs, shaders compiling, frames presenting. |
| **Quest 2 VR** | **Stereo 3D, in a projection layer** | The scene is drawn once per eye with a real eye offset, so it has depth rather than being a screen hanging in front of you. |
| — performance, title screen | **30 fps, the game's native rate** | Was 6.7. The renderer was drawing a 1280x720 game at the 3664x1920 headset panel resolution, and the flat Android present - a surface a headset never shows - cost 124ms of every 150ms frame. |
| — performance, in game | **Not playable, and the diagnosis was wrong** | Around 8-10 fps at full render scale, ~20 at quarter. This row used to say the frame was fill-bound, on the strength of a scissor test. It is not: measured with real GPU timers the **GPU executes the whole command buffer in ~2ms of a ~100ms frame**, forcing every tile load and depth store to `DONT_CARE` changes nothing, and culling almost the entire scene away still costs 73ms against 12ms of our own work. The cost is the recompiled guest on the CPU. See [research/20260829_2300_the-gpu-is-not-the-bottleneck.md](research/20260829_2300_the-gpu-is-not-the-bottleneck.md). |
| — projection layer | **Working, captured** | Replaces the floating quad so the world surrounds you. Confirmed by capturing the frame handed to the compositor and looking at it. |
| — character-anchored camera | **Does not work** | `SubmitCharacter()` is never called, so third-person and first-person quietly fall back to the game's own camera. |
| — controllers | **Working** | Touch controllers are not Android gamepads — they exist only as OpenXR actions, which is why SDL reported no pad and `adb input keyevent` did nothing. 13 actions, Touch bindings, presented to the guest as a 360 pad. |
| **Cel shading on characters** | Not started | Post-process outlines and banded lighting. Optional, toggled in the options menu. |
| **Tourist mode** | **Works** | `bd_tourist_mode` keeps the party at full HP and MP so a field can be looked at rather than survived. Encounter suppression still to do. |
| Windows / Linux / macOS, x86-64 and ARM64 | Works (upstream) | Untouched here. Use upstream builds. |

### Settings that matter for VR performance

All reach the game through `args.txt` beside the game data on Android, or the profile's
`reblue.toml` on desktop - no rebuild, no reinstall.

| cvar | What it does |
| --- | --- |
| `bd_render_scale` | Scene at N% per axis. Still the largest single lever on frame time, though not for the reason first recorded - the GPU is nearly idle either way, so the effect is most likely memory bandwidth shared with a guest that is itself memory-bound. **25 reads as blurry gibberish through the lenses**, which is the first thing a wearer said about it, so do not treat it as a free win. |
| `bd_reflections` | Off pins the planar reflection, which re-renders the scene, to its floor. **Verified.** |
| `bd_shadows` | Off renders the sun shadow map at 64x64. Unverified - the draw census cannot see a depth-only target. |
| `bd_stereo` | Submits every scene draw twice, one viewport per eye. This is what gives depth. |
| `bd_stereo_separation` / `bd_stereo_convergence` | Eye offset and the distance at which parallax is zero. Check the result with `tools/stereo_check.py`. |
| `bd_capture_after_s` | Writes the composited frame to `logs/capture/` N seconds in. The only way to see what a headset is actually being shown. |
| `bd_vr_anchor_distance` | Game units from the follow camera to the party leader, for the anchored camera modes. |
| `bd_tourist_mode` | Full HP and MP while walking the field. |
| `bd_debug_fill_scale` | Diagnostic. Shrinks the scissor without touching the viewport, so only the fragment count moves. It was once read as proving the frame fill-bound; re-measured, cutting fragments to a sixteenth barely moves the frame. |

`python tools/bench_quest.py all` runs the verified levers on a connected Quest, one variable at a
time, and prints a comparison table. It builds nothing.

**There are no patch files any more.** Both outgrew `patches/` and now live as real history in
forks: `noeldvictor/plume` branch `reblue-openxr` carries the OpenXR device-creation seam, and
`noeldvictor/rexglue-sdk` branch `android-arm64` carries the Android cross-build. The submodules
point at those. `noeldvictor/XenosRecomp` is forked and repointed too, unmodified so far.

**[docs/VR_PORT_PLAN.md](docs/VR_PORT_PLAN.md)** is the actual plan, with the guest camera addresses,
the phase breakdown, and an honest risk register. [CLAUDE.md](CLAUDE.md) is the condensed version
plus the codebase notes. [research/](research/) is the dated log of what was found and when.

The approach, in one line: **make the machine answer the question instead of guessing, because the
device loop turned out to be about 60 seconds anyway.** That was not the original plan - the plan was
to build the camera on a desktop against Meta XR Simulator and go near hardware only for comfort and
driver bugs - and it is not what happened. The whole port was built on-device with printf.

What made that work was instrumentation rather than tooling. Every hard bug here was found by adding
one line of output and reading it, and the ones that took longest were the ones reasoned about
instead: stereo was diagnosed wrongly four times in a day, and each wrong answer fell to one new
number. Three things carry that load now, and all three run with nobody in the headset:

- `bd_capture_after_s` writes the frame the compositor is handed to disk. Quest's own screenshot
  intents do nothing on this Horizon build and `adb screencap` cannot see compositor layers, so
  before this every VR claim rested on a log line.
- `tools/stereo_check.py` turns "does stereo have depth" into one command, and is checked against the
  three real captures that produced its three verdicts. `--raw` reads a capture already on disk, so
  it runs on a desktop build with no headset attached.
- `bd_sample_profiler` plus `tools/symbolize_profile.py` - **a sampling profiler that works on a
  Quest**, which `simpleperf` does not: Horizon OS refuses shell perf on this device whatever
  `perf_event_paranoid` says, so `tools/profile_quest.py` had never produced a profile. A process
  may always signal its own threads, so this one samples itself and resolves the PCs on the host
  against the unstripped binary. It works on the desktop build too, where a run costs 170s and no
  device at all.
- `bash tools/verify_quest.sh` - install, run, and pull the frame breakdown, per-frame CSV, profile,
  capture and per-thread CPU split, in one command.
- Vulkan validation layers, packaged via `EXTRA_LIBS` in `tools/build_apk.sh`.

## Table of Contents

- [Hardware Requirements](#hardware-requirements)
- [How to Install](#how-to-install)
- [Features](#features)
- [FAQ](#faq)
- [Building](#building)
- [Credits](#credits)
- [License](#license)

## Hardware Requirements

Requires all three retail Blue Dragon discs or their disc images. Steam Deck is supported. 64-bit ARM processors are supported on Linux and macOS. Windows is x86-64 only. Android and Quest are not supported and are the whole point of this fork.

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

Everything below is new to re:Blue. All of it is configurable in game, from the title screen or the camp menu.

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

re:Blue builds with CMake and vcpkg against the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk).

```sh
cmake --preset win-amd64-release       # or linux-amd64-release
cmake --build --preset win-amd64-release
```

Presets cover `win-amd64`, `win-vk`, `linux-amd64`, `linux-arm64`, `mac-amd64`, and `mac-arm64`, each in Debug, Release, and RelWithDebInfo. A `win-amd64` preset builds both the DX12 executable (`reblue.exe`) and the Vulkan one (`reblue_vk.exe`), and a `win-vk` one builds the Vulkan executable alone. As with running the game, building requires the files from your own copy of Blue Dragon.

There is no `android-arm64` preset yet, and adding one is the open work in this fork. It needs the
ReXGlue SDK cross-built with the Android NDK, which upstream does not publish. [CLAUDE.md](CLAUDE.md)
has the notes.

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
