# The desktop Vulkan build: what is actually known

**CORRECTION, appended after the fact.** The original title and conclusion of this note -
"the desktop renders black" - is NOT established, and the evidence below that says so is weak.
Read this section first.

2026-08-30. Open at time of writing. Recorded so the eliminations are not repeated.

## What is true

- **The Quest 2 renders correctly** and is unregressed through all of today's work:
  15.0 fps, `dt 66.77ms`, `gpu_total 56.28ms`, stereo crossed and correct, verified from a
  two-layer capture that shows both eyes with textures, shadows, character and parallax.
- **The desktop Vulkan build (`reblue_vk.exe`) renders black.** Confirmed with an OS-level
  screen capture (PowerShell `CopyFromScreen`), which is independent of every capture path in
  this repo and cannot fail the same way.
- The last desktop run known to be good was **2026-08-29**, before the guest constant rewrite
  landed. So this is most likely a regression from that work, which the Quest tolerates.

## The one informative frame

Before the Windows shader cache was force-regenerated, an OS screenshot showed Blue Dragon's
camp menu - "Medals / Gold / Help / Time" - rendered as 2D text at **hugely wrong scale**, over a
black 3D scene. Wrong-scale 2D plus black 3D is the signature of shaders reading the wrong
constants: the 2D path takes its ortho transform from the same constant file the 3D path takes
its view-projection from.

**After** deleting `generated/shader_cache.cpp` and rebuilding, the frame went *fully* black -
the 2D text disappeared too. So the freshly generated Windows cache renders strictly less than
the stale one did, while the identical generation on Android renders correctly.

That asymmetry is the strongest lead and is not yet explained. Note Windows emits **both** caches
("Compressing DXIL cache..." then "Compressing SPIRV cache...") where Android emits one.

## Eliminated, with evidence - do not re-run these

- **Not the host binding.** `[constants] bound guest constant buffer ... ranges 4096/3584/352`
  and `[constants] first non-zero dynamic offsets 4152320 4156416 4160512` - the buffer is
  created, published to all three descriptors, and the per-draw offsets are non-zero, sensibly
  spaced and 256-aligned.
- **Not the backend TU split**, for *this* configure. Three shared TUs really were compiling
  `TextureDescriptor()` with the wrong backend's offset (`gpu/bindless.cpp`,
  `gpu/texture_upload.cpp`, `gpu/sampler_cache.cpp`) and that is a genuine bug now fixed, with a
  `REBLUE_COMMON_TU` guard on `bindless_allocator.h` so the next one fails to build. But this
  configure is `REBLUE_D3D12=OFF` - only `reblue_vk.exe` is built and everything compiles
  Vulkan-only - so it cannot have been the cause here.
- **Not MSAA, stereo or multiview.** Black at `bd_msaa=0` with stereo and multiview both off, and
  black with `bd_stereo_multiview=true`. Three screenshots twelve seconds apart, all `0.0%`
  non-black.
- **Not the capture instrument.** That was independently broken and is now fixed (below); the
  OS-level screenshot agrees with it.
- **Not a config problem.** `[config] all 6 settings in reblue.toml took effect`.
- **The GPU is doing real work**: 830 draws, `gpu_total 5.49ms`. Constants reading as zero
  collapses geometry and costs almost nothing (the earlier failed Android attempt went
  6.37 -> 0.24ms), so the vertex path is probably not degenerate. Fragments are shading black.

## Two real bugs fixed on the way here

Both were found chasing this and are worth having regardless.

**1. Copying from a swapchain image was invalid usage.** plume created swapchain images
`COLOR_ATTACHMENT | TRANSFER_DST | SAMPLED` - no `TRANSFER_SRC`. Vulkan silently drops such a
copy and leaves the readback zero, with no error unless the validation layers happen to be on.
That is why `bd_capture_after_s` returned an all-black image on every path for as long as it has
existed. Fixed, masked against `surfaceCapabilities.supportedUsageFlags`.

**2. `CaptureDue()` latches, and two sites were calling it.** The composited capture consumed the
one-shot before `bd_mv_capture_array` could use it, so the array capture photographed nothing -
which reads exactly like a black scene target and cost two runs of wrong conclusions. The
composited grab now stands aside when a specific guest surface was requested.

The composited capture also moved into `RecordPresentPass`, which is the only point where the
game plus the ImGui overlay both exist. That is how the VR options panel was finally seen.

## Where to go next

The bisect that was not run, and should be: build the commit **before** the constant rewrite in a
git worktree and run the desktop. That settles "did the rewrite break it" in one step, which no
amount of reasoning here has.

Second lead: find out which cache `reblue_vk.exe` actually loads on Windows, given both a DXIL and
a SPIR-V cache are generated into the same build tree, and whether the SPIR-V one is complete.


---

## Correction, 2026-08-30 16:10: "the desktop renders black" was not established

Three things undermine the conclusion this note was originally written to record.

**1. The instrument was focus-dependent.** The OS screenshot used
`[Screen]::PrimaryScreen` and captures whatever is in front. One "99.3% non-black"
reading that looked like a clean pre-rewrite render was a photograph of two terminal
windows. `tools/shot_window.ps1` now finds the reblue window by process, restores and
foregrounds it, and captures only its client rect - use that, never a whole-screen grab.

**2. The desktop reaches field scenes and does real GPU work.** From one 9,846-frame run
on current `main`:

```
draws: min 19  median 817  max 2187
frames with >= 1500 draws: 993
gpu_total_ms median 9.30  max 13.10
```

2187 draws is a field scene at the scale this repo has always recorded for the desktop
(~2070). The GPU is doing 9-13ms of work. That is not the profile of a renderer producing
nothing.

**3. What was actually photographed were menus.** Two successful window captures showed
Blue Dragon's camp menu ("Medals / Gold / Help / Time") and its status-effect icon
glossary - both with correct textures, correct colours, correct alpha. So 2D rendering,
texture binding, the bindless heap and the pixel path all work on desktop.

`bd_xr_autoplay` lands somewhere different on each desktop run - menus, loading, and
occasionally a field scene - so a screenshot at a fixed wall-clock time is close to a coin
toss, and six black samples in a row is weak evidence rather than proof.

**What remains genuinely unknown:** whether a desktop *field scene* renders correctly. No
capture has caught one. That is the question to answer, and the way to answer it is to gate
the capture on draw count rather than on elapsed time - photograph the first frame with
>= 1500 draws, not the frame at t=140s.

**What is settled:** the pre/post-rewrite comparison this note proposed was run and produced
nothing usable, because of (1). It should be re-run with the window capture, and only after
the draw-count gate exists.
