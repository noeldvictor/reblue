# Foveation: the whole stack is built, and it crashes when switched on

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
