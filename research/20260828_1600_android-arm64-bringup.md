# Research: bringing up android-arm64, in practice

Date: 2026-08-28 16:00
Topic: what it actually took to get the tree buildable and the ReXGlue SDK configuring for Android.

This one is a log of a bring-up rather than a literature review. Everything below was established
by doing it on this machine, against a Quest 2 and an AYN Thor both connected over adb.

---

## 1. The tree can build after all

Two things were assumed missing from a fresh clone. Both turned out to be about a minute of work.

**The SDK is a public release.** `rexglue/rexglue-sdk` publishes per-platform slices on every
release and nightly, no authentication needed. `v0.10.0` is what `generated/rexglue.cmake` pins.
Slices: `win-amd64`, `linux-amd64`, `linux-arm64`, `mac-amd64`, `mac-arm64` — **no `android-arm64`**,
which is the whole reason §3 exists.

**`default.xex` does not require copying a disc.** It is ~8 MB inside a 7.8 GB ISO. XDVDFS says
exactly which sectors it occupies, so reading only those takes under a second — including straight
off an adb-connected handheld, so the discs never have to leave the device they live on.
`tools/extract_xex.py` does this; it also works on a local image.

The numbers, for the record: Blue Dragon is XGD2, so the filesystem base is `0xFD90000` (sector
129824) and the volume descriptor sits 32 sectors past it. Root table at relative sector 1783935;
`default.xex` at relative sector 411356, 8,040,448 bytes, starting `XEX2`.

With both, `rexglue codegen` emits **219 files in ~7 seconds** and is deterministic — a second run
on the same xex reports "0 written, 219 unchanged", which is the cheapest possible check that an
extraction was byte-correct.

`assets/` and `generated/` each carry a `.gitignore` of `*`, so neither the game executable nor the
recompiled guest code can be committed. That is upstream's design and it should stay.

---

## 2. Verification short of a full link

A full Windows link additionally wants vcpkg, because `find_package(directx-dxc CONFIG REQUIRED)`
is unconditional on `WIN32` even for the Vulkan-only target. Short of installing it, individual
sources can be syntax-checked against the real SDK headers:

```sh
clang++ -std=c++23 -fsyntax-only -I. -Isrc -Iout/sdk/win-amd64/include -Igenerated src/xr/xr_settings.cpp
```

`src/xr/{xr_camera,xr_cull,xr_settings}.cpp` all pass, `rex/cvar.h` macros included. Until this
point they had only ever been checked against the standalone stub in `tools/xr_math_test`.

---

## 3. The SDK for android-arm64

The gate on everything Android. Much smaller than the risk register assumed.

**What already works, unmodified:**

- Android is `UNIX AND NOT APPLE`, so the SDK's platform detection classifies it as `linux-arm64`
  and defines `REX_PLATFORM_LINUX=1`. There is no "unsupported platform" wall to climb.
- The NDK is Clang, clearing the SDK's `FATAL_ERROR` on non-Clang toolchains, and is far past its
  Clang-18 floor.
- `-mcmodel=small` and position-independent code — what the SDK already selects for anything that
  is not x86_64 — are exactly what Android needs.
- **SDL3 configures itself for Android natively.** The configure summary comes up with the `android`
  video driver, `aaudio` and `opensles` audio, `android`/`hidapi` joysticks, and both `vulkan` and
  **`openxr`** GPU backends. That last one is a pleasant surprise and worth remembering when the XR
  session lands.

**What needed patching:** one guard. `src/ui/CMakeLists.txt` demands `x11-xcb` and `wayland-client`
through pkg-config on any non-Apple UNIX. Android has neither — SDL3 hands over an `ANativeWindow`
and the surface comes from `VK_KHR_android_surface` — so the Android case takes an empty branch
ahead of the desktop Linux one. That is the entire content of
`patches/rexglue-sdk-android.patch` so far, and with it the SDK **configures** for android-arm64.

### NDK version matters, and the newer one is free

Building with **NDK r29** (Clang 20) fails in the SDK's own headers, not in anything Android-specific:

- `rex/string/numeric.h` calls `std::from_chars(..., std::chars_format::general)` on a floating
  point type. r29's libc++ has no floating-point `from_chars`, so overload resolution falls back to
  the integral one and reports "requires 3 arguments, but 4 were provided".
- `rex/chrono/chrono.h` specialises `std::chrono::clock_time_conversion`, which r29's libc++ does
  not declare.

Both are libc++ maturity gaps, and the SDK already knows about the first one: there is a
`#if REX_PLATFORM_MAC` branch using a hand-rolled `portable_float_from_chars`, because AppleClang's
libc++ has the same hole. Extending that guard to Android would work.

**But NDK r30 ships Clang 21, whose libc++ has `__charconv/from_chars_floating_point.h`.** Trying
the newer toolchain first is much cheaper than writing a portability patch, and it costs one
variable. Prefer that; only reach for the `REX_PLATFORM_MAC`-style guard if a target is pinned to an
older NDK.

### The trap that is not Android's fault

`thirdparty/libmspack` keeps 15 symlinks under `cabextract/mspack/` pointing at the real sources.
Git on Windows without Developer Mode runs `core.symlinks=false` and writes each out as a text file
whose entire content is its target's relative path. The compiler then says:

```
lzxd.c:1:1: error: expected identifier or '('
    1 | ../../libmspack/mspack/lzxd.c
```

which reads like a corrupt source file and is nothing of the sort. It would break a Windows build
for any target. Materialise them as copies, or re-clone with symlinks enabled.

---

## 4. Hardware on hand, and what it means

Both target devices are connected over adb:

| Device | Product | SoC / GPU | Notes |
| --- | --- | --- | --- |
| Quest 2 | `hollywood` | XR2 Gen 1, Adreno 650 | The VR target. |
| AYN Thor | `kalama` | Snapdragon 8 Gen 2, Adreno 740 | Android 13. Also reachable over WiFi adb. |

The Thor already has a working Xbox 360 emulator stack on it — XenDroid builds, `Turnip_Gen8_V33`
and a loose `libvulkan_freedreno.so`, and a `bd_frames_*.gfxr` GFXReconstruct capture of Blue Dragon.
Two things follow:

1. **The Turnip driver-replacement path is already proven on this hardware**, which is the mitigation
   the earlier research said the port would need for Adreno.
2. There is an existing performance baseline to compare a recompiled build against, and a captured
   frame trace to reason about the renderer without running anything.

The installed Android SDK carries NDK r25 through r30 and build-tools 30–36, so nothing needs
downloading to attempt an APK.

---

## 5. What this changes

- The Android target is no longer blocked on an unknown. The SDK configures; the remaining question
  is only how much of it compiles.
- Prefer NDK r30. Record the r29 failure so nobody rediscovers it.
- `tools/extract_xex.py` means any machine with a disc image can bootstrap the tree in a minute,
  which makes the "fast dev loop" goal much more achievable than it looked.
- Keep using `--adb-serial`: pulling 23 GB of ISOs to work on an 8 MB file would have been the
  single biggest waste of time available.
