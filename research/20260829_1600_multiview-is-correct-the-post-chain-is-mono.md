# Multiview is correctly configured. The post-process chain is what flattens it.

2026-08-29, third note today, and it corrects the second one.

`20260829_1530_the-shader-cache-was-stale.md` ended with "layer 1 is entirely black, nothing
rasterises into view 1" and a list of everything that had been checked and was correct. The
conclusion drawn from that - that view 1 was not being rendered - **was wrong**, and the way it was
settled is worth recording because it needed a tool this project did not have.

## Vulkan validation layers now run on the device

There were none in the NDK, so the Khronos Android build is fetched from the
Vulkan-ValidationLayers releases and packaged into the APK. Android only loads layers from the
app's own lib directory, so `tools/build_apk.sh` grew an `EXTRA_LIBS` hook - space-separated paths
copied into `lib/arm64-v8a/`. The same hook is the route for a replacement Turnip driver later.

```sh
EXTRA_LIBS=/path/to/libVkLayer_khronos_validation.so bash tools/build_apk.sh
adb shell settings put global enable_gpu_debug_layers 1
adb shell settings put global gpu_debug_app com.reblue
adb shell settings put global gpu_debug_layers VK_LAYER_KHRONOS_validation
adb shell settings put global gpu_debug_layer_app com.reblue
```

Turn it back off afterwards - it is slow enough to change what the frame timings mean.

## What validation said about multiview: nothing

Across a full run into a field scene, **not one multiview, view-mask, render-pass-compatibility or
array-layer error**. The only VUID raised at all was unrelated (below). Vulkan considers the
multiview setup entirely valid: two-layer targets, `2D_ARRAY` attachment views with `layerCount 2`,
`viewMask 3` on both the framebuffer and pipeline render passes, and `vkCmdBeginRenderPass` using
`targetFramebuffer->renderPass`, which is the multiview one.

So the scene almost certainly *does* render both views. The empty layer 1 is downstream.

## The presented surface is a post-process output, and post passes are mono

The framebuffer log makes the split obvious once the layer counts are printed:

```
framebuffer 344x180 rtLayers=2 dsLayers=0 -> viewMask=3     <- scene, stereo
framebuffer 688x360 rtLayers=2 dsLayers=0 -> viewMask=3     <- scene, stereo
framebuffer 172x90  rtLayers=1 dsLayers=0 -> viewMask=0     <- post, mono
framebuffer  86x45  rtLayers=1 dsLayers=0 -> viewMask=0     <- post, mono
```

A mono pass writes layer 0 and nothing else. What `bd_stereo_debug_layer=1` samples at present time
is the *end* of that chain, so its layer 1 being black is the expected consequence of a mono post
chain - not evidence that the scene never rendered view 1. This was already a known open item
("multiview post-process chain still mono"); it just was not connected to the symptom.

**So the remaining stereo work is the post-process and present path, not the scene render.** Either
the post chain becomes view-aware, or the present resolves per-eye from the two-layer scene target
before the mono passes collapse it. That is a different and much better-defined job than "find out
why multiview does nothing".

## Two real fixes made while chasing this

**`viewMask` was read off the wrong surface.** `GetOrCreateFramebuffer` computes
`container = ds ? ds : rt` because the cache entry lives on the depth surface, and then took the
layer count from `container`. The *pipeline's* multiview flag comes from the colour render target
(`hooks/state.cpp`), so a single-layer depth surface paired with a two-layer colour target would
have given the framebuffer `viewMask 0` while every pipeline drawn into it had `viewMask 3` - a
render-pass incompatibility, which is undefined and on Adreno renders view 0 silently. It now reads
`rt ? rt->layers : container->layers`. Not the cause here (every scene framebuffer has no depth
attachment, `dsLayers=0`), but it was a live trap.

**DXC targeted vulkan1.0.** No `-fspv-target-env` flag, so the default applied, and the `MultiView`
capability does not exist there - `SV_ViewID` lowers to `gl_ViewIndex`, which needs it. Now pinned
to `vulkan1.1`. Also not the cause, and also wrong on its own terms.

## The one validation error that did fire, and it is worth chasing

```
VUID-VkShaderModuleCreateInfo-pCode-08740
vkCreateShaderModule(): SPIR-V Capability Int64 was declared, but one of the following
requirements is required (VkPhysicalDeviceFeatures::shaderInt64).
```

Fourteen times, on shader module creation. The shader constants are read through
`vk::RawBufferLoad` at a `uint64_t` device address - see
`20260829_0030_shader-constants-are-global-loads.md` - so `Int64` is declared by design. The feature
being unsatisfied means that path is formally undefined on this device, and it is the hottest path
in the renderer.

plume passes the queried `VkPhysicalDeviceFeatures2::features` straight to `vkCreateDevice`, so if
the Adreno 650 reported `shaderInt64` it would already be on. **It presumably does not.** Worth
confirming and then deciding between `PhysicalStorageBuffer64` without `Int64`, or moving the
constants to a UBO - which
`20260829_0030_shader-constants-are-global-loads.md` already argues for on performance grounds.

Not chased today; recorded so it is not lost.

## Method note

Three conclusions in a row about stereo were wrong - "94% of pipelines are mono", then "view 1 never
rasterises" - and each was corrected by adding one measurement: the census layer counts, then the
HLSL grep, then validation layers. The pattern holds: **the conclusions drawn without a number were
the ones that were wrong.**
