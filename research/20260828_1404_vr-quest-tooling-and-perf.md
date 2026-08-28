# Research: tooling, prior art, and VR performance for the Quest port

Date: 2026-08-28 14:04
Topic: AI-agentic ports of old games to Quest — tooling and advice; Vulkan multiview and culling.

---

## 1. Prior art: AI-driven recompilation ports

**`SansNope/UnleashedRecomp-Android`** is the closest thing to a template for this project — an
unofficial Android port of Unleashed Recompiled, itself a static recompilation of an Xbox 360 game,
built with AI doing most of the code writing under human direction.

Their stated workflow, which is worth copying almost verbatim:

> The maintainers set the direction, test every build on real devices, debug with testers, and
> decide what ships; the AI does much of the code writing, crash analysis, and reverse engineering
> under that guidance.

Points that transfer directly:

- **Device testing is not optional and not substitutable.** They validate every build on real
  hardware, because GPU-specific breakage does not reproduce in simulators. Their bug list is
  almost entirely driver-specific: Adreno 750 corrupts with MSAA on, Adreno 6xx needs sysmem render
  mode to avoid corruption and hangs, Mali is experimental at best.
- **Structured logging and hang-detection watchdogs** are how they debug remotely without a
  debugger attached. Worth building in early rather than retrofitting — on Quest especially, where
  attaching a debugger is a chore.
- **Iteration counts are high.** They cite nine rebuild cycles to root-cause a single "ring crash".
  Budget for that; a cross-compiled Android build loop is slow and the answer is usually not in the
  first hypothesis.
- **They ship a driver-selection mechanism**, not a fixed driver: a bundled Mesa Turnip plus the
  ability to import another driver as a plain `.so` or an AdrenoTools/ExynosTools `.zip`. Given how
  much of their bug list is driver-shaped, this looks like a requirement rather than a nicety.
- They are candid that AI collaboration "means occasional artifacts, freezes, or audio issues can
  slip through", and that detailed bug reports with logs attached are what actually gets things
  fixed. Our README takes the opposite stance on bug reports, so the logging has to be good enough
  that *we* can self-diagnose.

Their build setup: Windows host, VS 2022 Build Tools, CMake, Ninja, Android SDK/NDK r29, JDK 17,
vcpkg. Host recompilation tools are built for Windows first, then the Android ARM64 target is
cross-compiled. That matches what re:Blue would need, and confirms the host-tools/target split.

---

## 2. VR injection frameworks — what to steal conceptually

**UEVR (praydog)** is the reference implementation for retrofitting 6DOF onto a flat game. It does
not apply to us directly — it works by forcing Unreal's *own* built-in stereo path on and hooking
engine-level objects, and Blue Dragon has no such path — but three of its design decisions are
worth copying:

1. **Three rendering modes, not one:** Native Stereo, Synchronized Sequential, and
   Alternating/AFR. Different games break under different ones. Offering a fallback mode is how a
   universal mod survives contact with engines it was not written for, and our own two-pass vs
   multiview choice (§4) is the same idea.
2. **Automatic UI projection into 3D space** is treated as a first-class feature, not polish. This
   confirms the plan's Phase 4 sizing: HUD and menus are where flat-game VR ports actually die.
3. **6DOF is opt-in per game via object hooks.** Even a mature universal injector does not get true
   6DOF controllers for free — it needs per-game setup. We are doing one game by hand, which is
   strictly easier, but it is a warning against expecting anything to fall out automatically.

There is no equivalent injector for a custom PowerPC-recompiled renderer. We are writing the stereo
and camera path ourselves; nothing off the shelf does it. The upside is that we control
`bdCameraViewSetMatrices` directly, which is a cleaner interception point than UEVR gets.

---

## 3. Tooling for Quest native development

| Tool | What it gives us | Notes |
| --- | --- | --- |
| **RenderDoc Meta Fork** | Frame capture plus tile-level render stage traces and per-draw-call metrics (up to 59) from the Adreno tile renderer | The important one. Standard RenderDoc does not expose the tiler data, and on a tile-based GPU that data is the whole story. Windows and Mac installers. |
| **ovrgpuprofiler** | Real-time GPU metrics and render stage traces from a CLI | Already on the headset — ships with the Meta Quest runtime, nothing to install. Lowest-friction first look. |
| **OVR Metrics Tool** | Frame rate, heat, CPU/GPU throttling, tears, stale frames, as an in-headset HUD | The one to leave running during ordinary play testing. Thermal throttling is invisible without it and will otherwise look like a mysterious performance regression twenty minutes in. |
| **Snapdragon Profiler** | CPU, GPU, DSP, memory, power, thermal | Qualcomm's, so it sees things Meta's tools do not. Useful for the CPU side — XMA decode, the recompiled guest code itself. |
| **Monado** | Open-source OpenXR runtime, can present a simulated HMD on a desktop | Lets the XR code path run on a Linux dev box with no headset attached. Good for smoke-testing session lifecycle in CI. |

For desktop-phase development (Phases 0–5 of the plan), SteamVR or Monado plus the Quest over Link
gives a real headset with a real debugger, which is the entire argument for building the camera on
desktop first.

---

## 4. Vulkan multiview

`VK_KHR_multiview` renders both eyes in one pass, with shaders branching on the `ViewIndex`
built-in. Meta's own guidance calls instanced stereo / multiview **the best option** for optimising
Quest applications, and the reasoning is specific to tile-based GPUs: without multiview the
implementation runs multi-pass with single-layered tiles; with it, multi-layered tiles, so the
tiler's binning work is done once rather than twice.

The saving is on CPU-side draw submission and on binning, **not** on fragment shading — you still
shade every pixel of both eyes. So it helps when submission-bound, which a game with Xbox 360-era
draw call counts running through a recompiler plausibly is.

**Cost for us:** the translated shaders have to index a per-view matrix. That means touching
XenosRecomp's output rather than just host code, which is why the plan defers it to Phase 8. The
right sequence is unchanged:

1. Two passes first. Correctness, on desktop, where it is debuggable.
2. Measure on device. If submission is not the bottleneck, multiview buys nothing.
3. Multiview only if the measurement says so.

Also noted while reading: **`VK_QCOM_tile_shading`** exists on Adreno and exposes tile-level
control. Probably too exotic for this project, but worth remembering if bandwidth turns out to be
the wall.

---

## 5. Culling in stereo

This matters more than usual for us, because the plan already has to widen
`bdCameraViewFrustumTest` (`0x82135030`) so head-turning does not pop geometry. Doing that naively
means culling almost nothing, so it is worth knowing what the established answers are.

**The core problem:** an object occluded for one eye may be visible to the other. Culling per-eye is
correct but doubles the work.

**Established approaches, cheapest first:**

- **Single combined volume.** Run one query against a volume enclosing both eyes rather than two
  per-eye queries. Umbra's "Stereo Camera" does exactly this — one occlusion query for a spherical
  volume covering both viewpoints, correct for both eyes, roughly half the cost of doing it twice.
  **This is the right default for us**, and it composes neatly with the frustum expansion the plan
  already calls for: expand once, to a volume that covers both eyes plus a head-motion margin,
  rather than expanding a single-eye frustum by a fudge factor.
- **Round-robin occlusion.** Alternate which eye is queried each frame, saving a full frame of
  queries at the cost of one frame of latency in the visibility data. A good fallback if combined
  volumes prove too conservative.
- **Hybrid mono rendering.** Render distant geometry once, monoscopically, and only render near
  geometry in stereo — stereo separation is imperceptible past a certain distance anyway. Meta has
  written this up for UE4 and Unity. For a JRPG with large outdoor areas and a skybox this could be
  a significant win, and it is worth flagging as a candidate even though it is not in the plan yet.

**Relevance to re:Blue specifically:** `src/gpu/occlusion.cpp` already exists and the guest uses
occlusion queries (`bdCameraCheckAllPointsVisible`, `pfx_occlusion_count_ps`). Those queries will
double under two-pass stereo and are a likely source of both cost and per-eye divergence — if the
two eyes disagree about whether a lens flare is occluded, it will flicker. Worth checking early.

---

## 6. Conclusions for the plan

- Copy the UnleashedRecomp-Android workflow: human directs and tests on device, AI writes; build
  structured logging and a hang watchdog in from the start; ship driver selection, do not trust the
  stock Adreno blob.
- Get RenderDoc Meta Fork and OVR Metrics Tool set up before the first on-device build, not after.
- Keep two-pass stereo as the correctness path and treat multiview as a measured optimisation,
  not a starting assumption.
- Change the culling approach in the plan from "expand the frustum by a factor" to **"cull once
  against a combined two-eye volume with a head-motion margin"**. Same amount of work, better
  answer, and it is what the VR industry converged on.
- Add hybrid mono rendering to the Phase 8 candidate list.

**Sources:** [UnleashedRecomp-Android](https://github.com/SansNope/UnleashedRecomp-Android) ·
[UEVR](https://github.com/praydog/UEVR) ·
[RenderDoc Meta Fork](https://developers.meta.com/horizon/documentation/native/android/ts-renderdoc-for-oculus/) ·
[ovrgpuprofiler](https://developers.meta.com/horizon/documentation/spatial-sdk/ts-ovrgpuprofiler/) ·
[OVR Metrics Tool](https://developers.meta.com/horizon/documentation/native/android/ts-ovrmetricstool/) ·
[VK_KHR_multiview](https://www.khronos.org/registry/vulkan/specs/1.1-extensions/man/html/VK_KHR_multiview.html) ·
[Sascha Willems on multiview](https://www.saschawillems.de/blog/2018/06/08/multiview-rendering-in-vulkan-using-vk_khr_multiview/) ·
[Adreno best practices](https://docs.qualcomm.com/bundle/publicresource/topics/80-78185-2/mobile_best_practices.html) ·
[Hybrid mono rendering](https://developers.meta.com/horizon/blog/hybrid-mono-rendering-in-ue4-and-unity/) ·
[Occlusion culling in VR](http://vrarwiki.com/wiki/Occlusion_culling) ·
[Monado](https://monado.freedesktop.org/)
