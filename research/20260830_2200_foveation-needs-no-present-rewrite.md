# Foveation does not need the present rewrite. The Quest says so itself.

2026-08-30.

## The blocker that is not one

`docs/VR_PORT_PLAN.md` and CLAUDE.md both say fixed foveated rendering needs the scene rendered
*into the XR swapchain image*, which it is not - the guest owns its surfaces and present composites
into the runtime's image. That has been recorded as the prerequisite for foveation since the port
began, and it is why Track B1 (direct-to-swapchain) was sequenced ahead of Track B2 (foveation).

**That is true of `XR_FB_foveation` and false of foveation in general.** `XR_FB_foveation` decorates
an XR swapchain, so it can only foveate what is rendered into one. The underlying Vulkan mechanism -
a fragment density map - applies to **any render pass**, including the guest's own scene target.

## The Quest confirms it, unprompted

`adb logcat` while reblue runs, from the runtime's own foveation layer:

```
Foveation: Forcing on dynamic foveation
Foveation: Vulkan foveation tile offset enabled: 0
Foveation: Vulkan foveation tile turn off enabled: 1
Foveation: Vulkan FFR is supported, with density map size 32x32
```

**"Vulkan FFR is supported, with density map size 32x32."** The device supports a fragment density
map at a 32x32 tile granularity, and the runtime is already forcing dynamic foveation on for its own
compositing.

## Why this matters now

Per-pass GPU timing measured today
(`research/20260830_2100_where-the-gpu-time-actually-is.md`) puts **the scene pass at ~45 ms of a
56 ms frame - 81% of all GPU time**, in a full-resolution two-layer target. That is precisely the
pass foveation exists to make cheaper, and it is a two-layer target, so every fragment it saves is
saved twice.

So the sequencing in the plan can change: **foveation no longer waits on B1**. Attaching a fragment
density map to the scene render pass acts directly on the 81%, and does it without restructuring
present - which was the riskiest change on the board, explicitly accepted as "replace outright, no
fallback", and would have put the working stereo path at risk to get here.

## What it needs

- `VK_EXT_fragment_density_map` on the device and enabled at device creation, which plume owns.
- A small density image, 32x32 tiles per the runtime's own report, attached to the scene render
  pass as `VkRenderPassFragmentDensityMapCreateInfoEXT`.
- plume's render pass and framebuffer descriptions to carry it - the same shape as the `viewMask`
  work already carried in the fork for multiview.

Note the survey that was going to confirm the extension list from inside the app broke the build on
a backslash: `"...%s\n"` written through a Python edit became a literal newline inside the C string
literal. That trap is already in CLAUDE.md and was walked into anyway. The runtime's own log
answered the question without it.
