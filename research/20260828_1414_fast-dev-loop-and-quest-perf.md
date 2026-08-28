# Research: a fast dev loop, and what actually makes Quest 2 fast

Date: 2026-08-28 14:14
Topic: killing build-and-wait time; Quest 2 performance facts that change our design.

---

## 1. The headline: Meta XR Simulator

**A lightweight OpenXR runtime that presents a virtual Quest on the desktop.** Not an emulator of the
device — an implementation of the OpenXR API that runs on the PC, so an app links the ordinary
OpenXR loader and talks to it exactly as it would talk to a headset.

Why this matters more than anything else in this document:

- **No headset, no deploy, no cable.** The XR code path runs in a normal desktop process under a
  normal debugger with RenderDoc attached.
- **It supports native development**, not just Unity and Unreal. That is the part that makes it
  usable for us.
- Input is simulated from keyboard, mouse, or an Xbox controller, so controller bindings and
  locomotion can be exercised without touching a Touch controller.
- **It works at the OpenXR layer**, which means the same code path runs unchanged in the simulator,
  over Quest Link, and on-device. There is no simulator-specific branch to maintain.
- Version 205 adds **Meta XR Operator**, an MCP integration explicitly built so AI coding agents can
  observe, interact with, and verify a running VR app. Given how this fork is being built, that is
  worth a serious look — it would let the agent doing the work actually see the result.

**How a native app selects it:** the OpenXR loader honours `XR_RUNTIME_JSON`. Point it at the
runtime manifest and the loader ignores the system default entirely:

```sh
XR_RUNTIME_JSON=<install>/meta_openxr_simulator.json ./reblue_vk
```

The same lever switches between the simulator, SteamVR, and Monado without rebuilding or touching
the Windows registry, and because it is an environment variable its effect is scoped to the process.
On Windows the registry entry is the permanent global setting; the env var is the per-run override,
which is what a dev loop wants.

**Consequence for the plan:** the desktop-first ordering was already right, and this makes it
sharper. The loop for Phases 0–5 is *edit, build one target, run a desktop exe*. No `adb`, no APK,
no headset, and no thermal throttling confusing the numbers. Reach for the actual Quest for
comfort checks, perf numbers, and driver bugs — nothing else.

---

## 2. What is actually slow in this build, and what already handles it

re:Blue is in better shape here than most projects, and it is worth being precise about why before
"optimising" something that is already fine.

**The expensive, stable part is the recompiled guest code.** `rexglue codegen` emits ~54 translation
units plus a multi-megabyte `shader_cache.cpp`. Those change only when the XEX, the hook TOMLs, or
the shaders change — which is to say, almost never while working on `src/xr/`.

**And it is already isolated.** `cmake/generated.cmake` builds them as separate OBJECT libraries,
`reblue_recomp` and `reblue_generated`, precisely so both executables link one set of objects
instead of paying for 54 recomp TUs twice. The comment there says so explicitly. This is the C++
equivalent of the "assembly definition" split — a change to `src/xr/xr_camera.cpp` recompiles one TU
and relinks. It does not rebuild the guest.

So the honest answer to "can we stop rebuilding everything": **we already do not rebuild everything.**
The remaining costs are (a) the link, (b) a full rebuild after a reconfigure, and (c) codegen when it
genuinely does need to re-run.

**What is already in place:**

- `ccache` on Linux/macOS CI, `sccache` on Windows CI, via `CMAKE_{C,CXX}_COMPILER_LAUNCHER`.
- Codegen is stamp- and depfile-driven (`generated/codegen.d`), so it re-runs only on real input
  changes.
- LLD on Linux (`CMAKE_LINKER_TYPE`), and `line-tables-only` debug info on the generated sources.
- A precompiled header behind `REBLUE_PCH`.

**The PCH / cache tension, which the CI scripts already document:** neither ccache nor sccache caches
a precompiled-header compilation, so CI sets `REBLUE_PCH=OFF`. The right local choice is the
opposite of CI's:

| Situation | Setting |
| --- | --- |
| Day-to-day editing of a few files | `REBLUE_PCH=ON`, no compiler launcher. The PCH is what makes a single-TU rebuild fast. |
| Reconfiguring, switching branches, bisecting | `REBLUE_PCH=OFF` plus ccache/sccache. The cache is what makes a *full* rebuild fast. |

Do not enable both and expect both benefits; they actively fight.

**Practical rules that follow:**

- Build one target: `cmake --build --preset win-vk-release --target reblue_vk`. Do not build the
  default target and link two executables when you are only testing one.
- Never delete the build directory to "make sure". Configure again in place; CMake handles it, and a
  wipe throws away every object file including the 54 you did not change.
- `ccache -s` before and after tells you whether the cache is actually working. A 0% hit rate
  usually means the absolute build path changed, which is the classic ccache trap.

---

## 3. When the loop does reach the device

Everything above avoids Android entirely. Once Phase 6 lands and it cannot be avoided:

- **`adb push` the shared library, do not reinstall the APK.** `adb install` re-verifies and
  re-optimises the whole package; `adb push` copies a file. The usual arrangement is to have the app
  `System.load()` its native library from app-specific external storage
  (`/sdcard/Android/data/<pkg>/files/`) when a debug flag is set, so the loop becomes push-and-launch
  in seconds rather than a full reinstall. **Design for this from the start** — retrofitting it
  means changing how the app loads its own code, which is exactly the kind of change that is
  annoying later and free now.
- Keep the ~15 GB of game data on the device permanently and out of the APK. It should never be part
  of any deploy step.
- Build a structured log and a hang watchdog in early. The prior-art Android port
  (see `20260828_1404_vr-quest-tooling-and-perf.md`) leans on exactly that, because attaching a
  debugger to a headset is a chore and most bugs get diagnosed from logs.

---

## 4. Quest 2 performance facts that change our design

These are not general advice; each one contradicts something re:Blue currently does or assumes.

### 4a. MSAA is nearly free — but only through resolve attachments

On a tile-based GPU the MSAA buffer is *transient*: the GPU renders each tile entirely in on-chip
memory and the extra samples never reach main memory. 4x MSAA at Quest 2's 1440x1584 per eye saves
roughly 66 MB per eye, 132 MB total, versus materialising it.

That saving only happens if MSAA is set up as a 4x colour and depth attachment with a **non-MSAA
image in `pResolveAttachments`**, so the resolve happens at end-of-tile inside the GPU. Meta are
blunt about the alternative: going through `vkCmdResolveImage` stores the 4x data to memory and
resolves it back, and "could easily add 3ms on the GPU". At a 72 Hz budget of 13.9 ms, 3 ms is over a
fifth of the frame.

**re:Blue currently does neither.** `src/gpu/resolve.cpp` resolves with a full-screen shader pass —
`GetOrCreateResolveMSAAPipeline`, backed by the `resolve_msaa_color_2x/4x/8x` and matching depth
shaders compiled in `cmake/generated.cmake`. That reads a `Texture2DMS`, which forces the MSAA buffer
out to main memory, which is the expensive path plus a shader on top.

This is defensible on desktop and it exists for a reason: it is emulating the Xbox 360's EDRAM
resolve, which is a guest-driven operation and not a render-pass-end event. So the conclusion is not
"replace it" — it is:

> **Separate the two.** Keep the shader resolve for the guest's EDRAM resolve semantics. For the
> XR eye buffers, use a render pass with `pResolveAttachments` so the stereo path gets tile-memory
> MSAA. They are different problems that currently share one code path.

Also worth recording: Meta's guidance is that some MSAA is **almost always preferable to raising
render scale**. If we are pixel-bound on Quest, the first lever is resolution scale down plus MSAA,
not the other way around.

### 4b. Fixed foveated rendering wants subsampled layout

FFR is the large, cheap win on Adreno for anything fill-bound, and a stereo JRPG at 72 Hz will be
fill-bound. Two caveats found:

- It "works best when rendering directly into the eye textures" in a forward rendering mode. Anything
  that renders to an intermediate and blits afterwards throws away part of the benefit — relevant,
  because re:Blue composites through its own chain (`src/gpu/output.cpp`, `present.cpp`).
- **Subsampled layout is strongly recommended for any Vulkan app using FFR.** It keeps the rendered
  content at its true resolution inside a subregion of the buffer rather than stretching it to fill,
  which is where the bandwidth saving actually comes from. Enabling FFR without it leaves most of the
  win on the table — there are developer reports of "no performance gain with FFR" that come down to
  this.

### 4c. Draw submission

Covered in the previous note: multiview halves submission and binning, not shading. Given the
composite chain above, we may well be fill-bound rather than submission-bound, in which case FFR and
render scale matter far more than multiview. **Measure before choosing.**

---

## 5. Changes this makes to the plan

1. **Add Meta XR Simulator to Phase 0** as the primary run target. Desktop-first was already the
   plan; this removes the headset from the loop as well.
2. **Add `XR_RUNTIME_JSON` switching** to the dev tooling so simulator / Link / Monado is a variable,
   not a rebuild.
3. **Split the MSAA resolve path** in Phase 1: guest EDRAM resolve keeps the shader; XR eye buffers
   get render-pass `pResolveAttachments`. Record it as a Phase 1 item, not a Phase 8 optimisation,
   because retrofitting a render pass structure later is much worse than building it right.
4. **FFR plus subsampled layout together**, never FFR alone.
5. **Design the Android library-load path for `adb push`** before writing the APK harness.
6. Local build guidance: PCH on for editing, ccache on for reconfiguring, never both; build one
   target; never wipe the build directory.

**Sources:**
[Meta XR Simulator overview](https://developers.meta.com/horizon/documentation/unity/xrsim-intro/) ·
[XR Simulator getting started (native)](https://developers.meta.com/horizon/documentation/native/xrsim-getting-started/) ·
[Faster iteration with XR Simulator](https://developers.meta.com/horizon/blog/boost-iteration-efficiency-meta-xr-simulator-experimental/) ·
[OpenXR loader design and operation](https://registry.khronos.org/OpenXR/specs/1.0/loader.html) ·
[Fixed foveated rendering](https://developers.meta.com/horizon/documentation/native/android/os-fixed-foveated-rendering/) ·
[Vulkan for mobile VR rendering](https://developers.meta.com/horizon/blog/vulkan-for-mobile-vr-rendering/) ·
[Showdown on Quest: optimising for Quest 2](https://developers.meta.com/horizon/blog/showdown-on-quest-part-2-how-we-optimized-the-pc-vr-demo-for-meta-quest-2/) ·
[sccache](https://github.com/mozilla/sccache) ·
[adb](https://developer.android.com/tools/adb)
