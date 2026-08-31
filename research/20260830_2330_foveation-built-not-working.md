# Foveation: built, working, and a net loss on this hardware

2026-08-30. `bd_foveation` defaults OFF. The default path is unregressed:
14.9 fps, dt 66.88ms, gpu_total 56.18ms, stereo crossed and correct.

## What is built and verified working

- **`VK_EXT_fragment_density_map` enabled**, and reported: `GPU caps: ... | fragment density map yes`
  on the Adreno 650.
- **The device feature enabled**, not just the extension. Chaining
  `VkPhysicalDeviceFragmentDensityMapFeaturesEXT` is what took pipeline failures from mass failure
  back to the baseline 2 - without it every render pass naming a density map fails to create and
  every pipeline built against one comes back null, with no other diagnostic.
- **Both render passes carry the attachment.** The framebuffer's pass and the pipeline's own pass -
  which are different objects and must stay compatible - plus the clear-variant passes, which
  rebuild the `pNext` that nominates it.
- **`RenderTextureFlag::FRAGMENT_DENSITY_MAP`** and a `FRAGMENT_DENSITY_MAP` layout in plume.
- **The map itself**: `43x23` for a `1376x720` target at 32x32 tiles, full rate in the centre 35% of
  the radius, easing to `bd_foveation_strength` at the corners. Logged as
  `density map uploaded and live`.

## What does not work

With `bd_foveation=true` the app crashes: `ACCESS_VIOLATION` reading `0x10` inside
`VulkanCommandList::end()`, reached through a null `unique_ptr` - so
`s.command_lists[cur]` is null when present tries to end it. 19 of 695 frames reach a field scene
and `gpu_total_ms` reads 74.8 rather than the 56.2 baseline.

The fault is in **when the density map is uploaded**, not in the foveation itself. Three attempts:

1. **Upload inside the framebuffer bind.** Deadlocked: that path already holds the render mutex and
   `std::mutex` is not recursive. The app stayed alive with no fatal line, no frames and no
   artefacts - which reads exactly like a run that never reached a field scene.
2. **Upload inside the bind, without taking the mutex.** Crashed in `end()`: it called
   `OpenCommandListLocked` from inside an active recording, resetting the command list underneath
   the frame in progress.
3. **Upload at frame start**, right after `command_list_open` is set, with no render pass active.
   The map does now go live - but the same crash remains.

That third one places the upload where every other one-shot upload in this renderer happens, so the
remaining fault is something else about recording into the frame's command list from
`BeginCommandList`. **The clean fix is to stop sharing that command list**: allocate a one-shot list
for the upload, submit it, wait, and free it - the map is created once and never changes, so a
stall is irrelevant. That needs a standalone command list and fence from plume, which this renderer
does not currently ask for anywhere.

## The trap that was avoided

`FoveationWanted` and the framebuffer must agree, because the density map is an extra attachment and
a pipeline whose render pass lacks it is **incompatible** with a framebuffer whose pass has it -
undefined behaviour, not an error, so the symptom would be a plausible black frame.

The first version had exactly that bug: `FoveationWanted` returned true on size alone while
`FoveationMapFor` returned null until the upload completed, so for the first frames the pipeline
declared a density attachment and the framebuffer did not. Both now key off the same readiness flag,
which only changes at a frame boundary, so the two see the same answer for a whole frame.

## Worth knowing before picking this up

The measured target is unchanged and still the right one: the scene pass is ~45ms of a 56ms frame,
81% of GPU, and it is a two-layer target so every fragment foveation saves is saved twice. Nothing
found today argues against foveation - only against the way this upload is sequenced.


---

## It works now, and it costs 17ms

The crash was the shared command list, three times over. `UploadDensityMap` now uses **its own
command list and fence**, submitted and waited on the spot - the map is created once and never
changes, so a stall costs nothing and buys complete independence from the frame. plume already
exposed `createCommandList`, `createCommandFence`, `executeCommandLists` and `waitForCommandFence`;
nothing new was needed.

With that, foveation runs end to end: map uploaded and live, pipeline failures at the baseline 2,
stereo crossed and correct, 1531 field frames.

**And it is slower.** Quest 2, multiview, same scene:

| `bd_foveation_strength` | `gpu_total_ms` | `dt_ms` |
| --- | --- | --- |
| off | **56.18** | 66.88 |
| 0.5 | 73.19 | 83.57 |
| 0.15 (very aggressive) | 70.00 | 83.50 |

The savings are real and behave correctly - dropping the peripheral rate from 0.5 to 0.15 takes
3.2ms off - but they sit on top of a **fixed cost of roughly 17ms** that appears the moment the
render pass names a density map at all. Foveation at its most aggressive is still 14ms worse than
not foveating.

That is not a tuning problem. A fixed cost that does not scale with the density values is the pass
losing a fast path, not the shading getting slower.

## What this probably means, and what it does not

It does **not** mean foveation is wrong for this port. The measured target is unchanged: the scene
pass is ~45ms of a 56ms frame and is two-layer, so it is exactly what foveation is for.

What it means is that **`VK_EXT_fragment_density_map` on an app-owned render pass is not the path on
this device**. Note what the runtime logs on its own, before reblue asks for anything:

```
Foveation: Forcing on dynamic foveation
Foveation: Vulkan FFR is supported, with density map size 32x32
```

That is Meta's compositor using its own integration. `XR_FB_foveation` decorates an XR swapchain and
goes through the runtime's path; the generic extension on a render pass we own does not, and on this
Adreno it evidently drops the pass onto a slower one.

So the conclusion from `20260830_2200_foveation-needs-no-present-rewrite.md` - that foveation could
skip the present rewrite - is **wrong in practice on this hardware**, even though it was right about
the Vulkan mechanism existing and being enabled. Foveation on a Quest 2 wants the scene rendered
into the XR swapchain after all, which is Track B1.

Left in and default off. The plumbing is correct and device-verified end to end, so if a later
driver or a different device makes the generic path viable it is one cvar away; and the render-pass,
pipeline, texture-flag and layout work is exactly what `XR_FB_foveation` will need anyway.

Worth also trying before committing to B1: `VK_EXT_fragment_density_map2` and
`VK_QCOM_fragment_density_map_offset`, both of which the survey should check for - the first run of
that survey never completed because the edit that added it broke the build on a backslash.
