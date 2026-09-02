# The scene pass is 28 of 39 GPU ms, and a depth prepass is the modern answer

2026-09-02. Quest 2, `bd_stereo`, from yesterday evening's runs.

## Where the GPU goes, by target

The per-target census of the field frame (`out/device/quest_setmove.log`):

| ms/frame | target | draws | binds |
| --- | --- | --- | --- |
| 7.18 | 1376x720 depth (scene) | 42.6 | 0.50 |
| 7.15 | 1376x720 depth (scene) | 43.7 | 0.50 |
| 7.06 | 1376x720 depth (scene) | 43.7 | 0.50 |
| 7.04 | 1376x720 depth (scene) | 43.2 | 0.50 |
| 0.58 x4 | 1376x720 colour-only | 0.5 | 0.50 |
| 0.38 x2 | 128x72 depth (shadow) | 65.7 | 0.50 |
| < 0.35 | everything else, 28 targets | | |

Four pooled scene surfaces at half a bind each: **the scene pass runs twice a
frame - once per eye - at ~7.1 ms each, 28.4 of 34.3 attributed ms** out of
39 total. The whole post chain is under 6 ms. Everything the last week said
about post-pass bandwidth is true and small; the scene pass is the frame.

Seven milliseconds for one megapixel at ~45 attributed draws is pathological
for an Adreno 650. The known reason is overdraw shaded in full: the scene
carries ~2x (forcing depth ALWAYS doubled desktop GPU time), 64% of its draws
blend and write depth, and that disables the tiler's low-resolution Z for the
rest of the pass - which is why front-to-back sorting measured zero.

## The EDRAM seed copies: +1.35 ms, measured within one run

`bd_ab_flag=bd_seed_targets`: arm off 37.79 ms GPU, arm on 39.14 -
`gpu_resolve_ms` 0.85 -> 2.45. Real, small, and a Xenon habit (seeding a
freshly acquired surface from its predecessor to imitate EDRAM persistence).
Not taken yet: a pass that relies on inherited content renders wrongly without
it, and the seed has to become per-surface rather than blanket.

## A depth prepass on the deferred queue

The modern renderer's answer to overdraw that early-Z cannot reject is to lay
depth down first. The deferred draw queue (`bd_draw_defer`, on by default)
already holds a whole pass's draws with their pipeline state, so:

- at record time, a draw that writes depth with a LESS/LEQUAL test and no
  stencil gets two extra pipelines from the same cache: colour writes off
  (the prepass) and depth writes off + LEQUAL (the colour pass);
- at flush, the prepass draws go first, near to far, then every draw in
  submission order with its colour pipeline.

Blended draws that write depth are included in both passes: in the prepass
they lay depth like anything else, in the colour pass they blend only where
they are the nearest depth-writer - which is what they did before whenever a
nearer opaque draw preceded them, and is the same image for opaque-looking
splat layers at equal depth. `bd_depth_prepass`, off until measured.

Cost: the scene's vertex work twice (~320k vertices, cheap) and one more
pipeline bind per prepassed draw on a render thread that now has 20 ms of
headroom. Expected gain: the overdraw half of the scene pass - up to ~7 ms of
the 28 - which is the 33.3 ms boundary.

## Measured: the prepass is null on the Quest

Built, verified on the desktop (91.5% non-black, mean RGB 67/61/47, stereo
verdict OK, `depth prepass: 134 of 134 draws` on the scene flush), then a
within-run A/B on the Quest 2 with the side-by-side path now on the queue:

| | prepass off | prepass on |
| --- | --- | --- |
| `gpu_draw_ms` | 32.50 | 32.61 |
| `gpu_total_ms` | 37.73 | 38.94 |
| `us/draw` (CPU) | 95.6 | 96.7 |

Nothing. Laying the nearest depth down first and shading only what passes
LEQUAL buys zero milliseconds, which means **the scene pass is not paying
for opaque overdraw**. Either there is little of it behind the nearest
depth-writer (the ~2x came from the desktop's depth-ALWAYS test and was never
run on the headset), or the cost is in the fragments that survive: the
blended stacks that shade whatever the depth says, the pixel shaders
themselves, or the 16-bit-float target's tile traffic. `bd_depth_prepass`
stays off; it is correct and free of image change, and it is not a lever.

A side result worth keeping: the side-by-side path now defers, so the queue,
its sort and anything else built on it apply to the shipping stereo route.
Every flush reports `0 opaque` and `depth 0..0` - the blended classification
says every scene draw blends, and the view-distance key is never set on this
path - so the sort has nothing to work with there either.

## Depth ALWAYS on the Quest: nothing. There is no overdraw to reject.

`bd_ab_flag=bd_debug_depth_always`, within one run: `gpu_draw_ms 35.76 ->
35.70`. Forcing every fragment to pass the depth test costs nothing, so the
scene pass was not rejecting anything to begin with - not because rejection is
broken, but because there is nothing hidden to reject. The desktop's "depth
ALWAYS doubles GPU time" was measured with reflections and shadows on and a
different cull; on the headset those are off and the visible set is the whole
cost. The prepass result above is the same fact from the other side.

So the scene pass is ~7 ms per eye of *visible* fragment work at ~1 Mpix, and
the levers are the pixel shaders, the 16-bit-float target's tile traffic, or
the vertex/binning side - which is what the on-device GPU profiler splits.

## The on-device GPU profiler: a texture-bound scene sampling mip 0

`tools/gpu_metrics_quest.sh` samples `ovrgpuprofiler -r` during the field
scene. Steady values, 20 fps, `bd_stereo`:

| metric | value |
| --- | --- |
| % time shading fragments / vertices | **99.0 / 1.0** |
| fragments shaded / s | 929 M (= ~46 M a frame, ~6.6 per scene pixel per eye) |
| % shader ALU capacity utilised | 22 |
| % shaders stalled | 21 |
| **% texture pipes busy** | **66** |
| % texture L1 miss | 25.6 |
| textures / fragment | 2.3 |
| ALU / fragment | 53 |
| **% non-base-level texture fetches** | **0.96** |
| % nearest filtered | 51 |
| read / write bandwidth | 2.8 / 1.9 GB/s (not the limit) |
| preemptions / s, avg delay | 100, ~1.7 ms |

Vertices are 1%. The ALUs are a fifth used. The texture units are the busiest
block in the GPU and a quarter of their fetches miss L1 - because **99% of
fetches come from the base mip level**. A 3D scene sampled without mipmaps is
the textbook texture-cache thrash, and it matches the fragment cost being
"proportional to fragment count" without any of it being depth-rejectable:
each of the ~6.6 fragments per pixel pays full-rate base-level fetches.

Two other things to keep: ~100 compositor preemptions a second at ~1.7 ms
each is up to a sixth of the GPU's time, and the shaders are stalled 21% of
the time, consistent with waiting on texture.

`native_texture_mirror.cpp` builds a mip chain only when the fetch constant
asks for one (`mip_max_level >= 1`) and the guest supplies mip data; a
histogram of that decision now prints as `[mips]`. If the guest's textures
have no chains, the modern fix is to generate them on upload - plume has no
blit, so that is a small downsample pass per level.

### The mip histogram, desktop

```
[mips] 512 2D mirrors: 316 with a mip chain, 196 with mip_max_level=0,
       0 mip_filter=baseMap, 512 mip_filter=point
```

Most textures carry a chain and every fetch constant asks for point mip
selection, which still selects a level; all 987 fetches in the dumped shaders
are implicit-LOD `Sample`. So the 99% base-level figure is not a missing
chain or a clamped sampler. The remaining readings are that the scene's
textures are simply magnified at this resolution (terrain tiles at texel:pixel
of one or less legitimately sit at mip 0), or that the profiler's metric means
something narrower. Either way the fragment count is the lever that does not
depend on the answer: fixed foveated rendering is being A/B'd next.

## Fragment density map foveation: negative again, in a fragment-bound frame

`bd_ab_flag=bd_foveation, bd_foveation_strength=0.3`, within one run:
`gpu_draw_ms 33.78 -> 34.80 (+3.0%)`, `gpu_total 39.74 -> 41.64`. The device
reports `fragment density map yes` and the arms flipped on schedule. In a
frame that is 99% fragment shading, a density map that shaded the periphery
at 30% would have to show; it shows a cost and no reduction, so the
attachment is not reducing shading in this pass on this driver - the same
verdict as 2026-08-30, now in the configuration where it could not hide.
`XR_FB_foveation` through the runtime's own swapchain remains the route.

## The render-stage trace: NOTHING is tiled. Every pass runs in direct mode.

`tools/gpu_drawtrace_quest.sh` (`ovrgpuprofiler -e com.reblue`, then `-t`
during the field scene) lists every surface the GPU executed in a 0.1 s
window, with its rendering mode and bin count:

```
Surface 7  | 1376x720 | color 64bit, depth 32bit | Mode: 0 (Direct) | 1 1376x720 bins | 24.49 ms | Render 22.879ms ... Preempt
Surface 4  | 1376x720 | color 64bit             | Mode: 0 (Direct) | 1 1376x720 bins |  0.09 ms
Surface 28 | 3664x1920| color 32bit             | Mode: 0 (Direct) | 1 3664x1920 bins|  3.19 ms
... 26 more, all Mode: 0 (Direct), one bin each
Surface 5  | 128x72   |                          | Mode: 2 (SwBinning)
Surface 8  | 1376x720 |                          | Mode: 3 (HwDirect) | 0.25 ms
```

**The Adreno is not tiling.** One bin the size of the surface is system-memory
rendering: every blended fragment of the scene pass does its read-modify-write
against a 64-bit colour buffer in DRAM instead of on-chip tile memory, there
is no low-resolution Z (it lives in the binning pass that never runs), and
the shading is exposed to memory latency - which is the 21% shader stall, the
66% busy texture pipes, and the ~6.6 fragments per pixel each costing full
price. It also explains the day's null results at once: depth ALWAYS, the
prepass, foveation - none of them touch a direct-mode pass's cost structure.

Adreno drops a render pass to direct mode when something in it forbids
binning, and one documented trigger is a timestamp query inside the pass.
`gpu_timing.cpp` writes `vkCmdWriteTimestamp` inside the active render pass
on every category or target change - the per-segment split
(`gpu_draw/resolve/inter`) and the per-target census that this whole
investigation has leaned on. The instrument was shaping the measurement.
`bd_gpu_timing_segments` now gates those marks and defaults off on Android;
the frame begin/end pair stays, so `gpu_total_ms` survives.

## With the in-pass timestamps off: the scene pass is STILL direct

`bd_gpu_timing_segments=false`, same trace: the 1376x720 scene pass reads
`Mode: 0 (Direct), 1 bin, 24.66 ms` exactly as before, while a second
1376x720 colour+depth pass in the same frame now reads `Mode: 1 (HwBinning),
16 384x224 bins, 3.03 ms` with `Binning / LoadColor / Render / StoreColor /
LoadDepthStencil / StoreDepthStencil` stages - the shape the scene pass
needs. So the timestamps were *a* trigger, not *the* trigger, and the driver
decides per pass: something about the scene pass itself forbids binning.

What that pass has that the binned one does not, under side-by-side: every
draw twice (~1000 draws, ~640k vertices), a viewport and scissor change on
every draw (the per-eye loop), and ~300 blended depth-writing draws. Two
traces meant to separate those did not land: the multiview trace captured no
surfaces at all twice (the profiler may not see layered passes), and the
cull-150 trace caught a transition rather than the field scene. A mono trace
(no per-eye loop, half the draws) is running.

## Mono on the Quest: 33.7 ms GPU, so stereo is only +5

`bd_stereo=false`, field-scene mean from the CSV: `gpu_total 33.68 | dt 49.79`
against side-by-side's 38.9. One eye's worth of draws in the pass and no
per-draw viewport changes buys 5 ms, not the 12 a straight halving would.
Whatever keeps the scene pass out of tiled rendering is present in mono too.
The 0.6 s trace windows of that run caught lighter frames (present blits at
3.4 ms each, 1376x720 colour+depth passes at 2.3 ms in `HwDirect` - a binning
stage run and then direct chosen anyway), so a 2.5 s window is being taken
before autoplay starts walking.

## Mono's scene pass is direct too: 20.5 ms

The 2.5 s window landed on field frames: `1376x720 c64 d32 Direct, 1 bin,
20.46 ms` and `20.74 ms` in two windows, with no Binning stage at all. So
the doubled draws and the per-draw viewport churn of side-by-side are not
what forces direct mode; the pass forces it in mono, with ~500 draws. The
trigger is in the pass's content. Two probes bisect it: `bd_debug_max_draws=64`
(does a pass of 64 draws bin?) and `bd_blend_off_when_opaque=true` (does a
pass with no blending on depth writers bin?).

Side result: the deferred queue can emit side-by-side eye-major (all left-eye
draws, then all right), verified image-identical on the desktop with stereo
intact. `bd_draw_eye_major` is on by default; it removes ~1000 viewport and
scissor changes from the scene pass, whatever the tiler makes of them.

## Bisecting the direct-mode trigger, one probe per run

Each row is the scene pass in the 2.5 s render-stage trace of a mono field
scene, plus the field-scene GPU mean where a CSV was taken:

| probe | scene pass | mode |
| --- | --- | --- |
| baseline mono | 20.5 ms | Direct, 1 bin |
| in-pass timestamps off | 20.7 ms | Direct (a second full-size pass bins) |
| `bd_blend_off_when_opaque=true` (no blend on depth writers) | 20.3-21.1 ms | Direct |
| `bd_seed_targets=false` (no pre-pass copy) | 20.3-21.6 ms | Direct |
| `bd_debug_max_draws=64` | removed the scene pass itself | - |
| side-by-side (draws x2, viewport per draw) | 24.7 ms | Direct |

RenderDoc on the desktop says the scene passes contain only draws - no
in-pass clears or copies - and the binned full-size pass in the same frame
has the same attachment formats. What the scene draws have that the binned
pass's draws may not is their shaders: every guest vertex shader declares
`SV_ViewID` and reads it (`g_ViewIndex = iViewID`) whether or not the pass is
multiview. A probe shader cache without it in vertex shaders
(`XENOS_RECOMP_NO_VS_VIEWID=1` at cache generation) is being traced.

| no `SV_ViewID` in vertex shaders (probe cache) | 20.5-21.6 ms | Direct |

The vertex-stage ViewIndex is not it either. In the same frame a *small*
1376x720 colour+depth pass bins into 16 tiles with full load/store stages at
1.9 ms, so the surface type is fine and the driver's decision is about what
the big pass contains or how much of it there is. Next split: the draw count
and primitive count of the pass (cull distance 120), then the guest draws'
state - bindless descriptor sets with update-after-bind, per-draw dynamic
uniform offsets, 16-stream vertex input - against the host pipelines the
small pass uses.

| `bd_cull_distance=120` (fewer draws) | both windows at the title screen | - |

Twice now a run with a tighter cull distance was not in a field scene at
134 s (154 surfaces a window, all 3664x1920 present blits at 60 fps). The
draw-count question is being asked a different way instead:
`bd_pass_split_draws=100` ends and reopens the render pass every 100 draws
inside the deferred flush, with the same cull and the same scene.

| `bd_pass_split_draws=100` | 20.5-20.6 ms, one surface record | Direct |

The queue already flushes the scene in 60-134 draw chunks, so the pass is
several render pass instances of ~100 draws each and the profiler reports them
as one surface execution; splitting finer changed nothing. The draw count of
a render pass instance is not the trigger. Image usage and tiling are the
same for the surface that bins (plume gives every render target
TRANSFER|SAMPLED|COLOR_ATTACHMENT, optimal tiling). The verbose trace
(`ovrgpuprofiler -t -v`) is the next instrument, for whatever per-surface
detail it adds before another blind probe.

## Where this stands, and the headset dropped off adb

Eliminated as the direct-mode trigger, each by a render-stage trace of a
mono field scene: in-pass timestamps, side-by-side's doubled draws and
per-draw viewport churn, blending on depth writers, the EDRAM seed copy,
the vertex-stage `SV_ViewID`, and the draw count per render pass instance
(the scene already flushes in 60-134 draw chunks; splitting at 100 changed
nothing). Image usage and tiling match the pass that bins.

Not yet run, built and ready in the APK: `bd_debug_no_alpha_test=true`
(discard in 86 pixel shaders) and `bd_debug_no_stencil_bias=true` (stencil
and depth bias stripped from every pipeline), plus the verbose trace
(`VERBOSE=1 bash tools/gpu_drawtrace_quest.sh ...`). The Quest disappeared
from adb during the verbose run and did not come back after a server
restart; those three are the next commands once it is attached again.

If all three come back direct, the remaining differences between the scene
draws and the binned pass's draws are in the guest pipelines themselves:
16-stream vertex input with SNORM/UINT formats, per-draw dynamic uniform
offsets on an update-after-bind set, and the recompiled shaders' size. A
Turnip build in the app's lib directory (`EXTRA_LIBS`) would name the reason
outright - Mesa's driver logs why it falls back to sysmem - and is the
instrument to reach for before more blind probes.

## Second batch, after the headset came back

| probe | result |
| --- | --- |
| `bd_debug_no_alpha_test=true` | `gpu_total 33.67` (unchanged); the render-stage trace failed to start for that run |
| `bd_debug_no_stencil_bias=true` | the run never reached VR frames (`Swap chain resize failed`, no `[xr]` lines, no CSV) - invalid, not a result |
| verbose trace (`-v`) | adds per-bin stage timing only: the scene pass is `Render 11.05 / Preempt 1.68 / Render 5.80 / Preempt 1.69 / Render 0.80`, one 1x1 logical bin, no reason given |

So the vendor profiler will not say why. `bd_vulkan_icd=turnip` now loads
Mesa's Turnip driver directly (plume `icdLibraryPath` ->
`volkInitializeCustom`, the .so packaged with `EXTRA_LIBS`), and its trace is
running on the flat path first, where no OpenXR loader interop is involved.

## The "black" resolve attempts were contaminated

`bd_mv_resolve` defaulted back to true late on 2026-09-01 (for the Quest's
277 ms), and every desktop multiview test since - the `Texture2DMSArray`
attempt, the view-index colour probe, the per-eye slice views - ran with the
obsolete resolve chain ON. With `bd_msaa=0` (no MSAA resolve shader involved
at all) multiview with layered textures is black too, so the blackout was the
chain, never the shaders. The per-eye slice-view resolve is being re-tested
with `bd_mv_resolve=false` set explicitly; the flat path with the same
shaders already renders correctly.

## The multiview pair survives the resolve now - read from the capture

The "black" capture, replayed with `tools/rdc_layer_diff.py`, is the proof
the screen could not give while the resolve chain was on:

```
1649  157 draws  1920x1080 RGBA16F  differ 72.7%   (scene)
1675    1 draw   1920x1080 RGBA16F  differ 72.7%   (MSAA resolve: reads ::1598 through
                                                   slices 0+1 and slices 1+1)
1737 ..2017      post chain          differ 50-97%
2223, 2255       1920x1080 RGBA8     differ 61.2%   (present-side targets)
```

Before this change the same draw (3551 in the 2026-08-31 capture) came out
IDENTICAL and every pass after it too. The per-eye slice views on the
multisampled scene, selected by `SV_ViewID` in `resolve_msaa_color/depth`
(`ps_6_1`), keep both eyes; the post chain and present carry them through.
The black screen in those runs was `bd_mv_resolve` defaulting on, which is
now Android-only.

## Turnip loads; the loader's surface extensions block the flat path

With `bd_vulkan_icd=turnip` plume opens `libvulkan_freedreno.so` through its
`HMI` module (Mesa's Android builds export nothing else) and volk initialises
from the HAL device's `GetInstanceProcAddr`. Instance creation then fails:

```
Missing required extension: VK_KHR_android_surface.
Missing required extension: VK_KHR_surface.
```

Those are implemented by Android's platform loader, not by the driver, so a
directly loaded ICD can never present through a flat swapchain. The OpenXR
path needs no Vulkan surface - the runtime owns the swapchain images - but
under `XR_KHR_vulkan_enable` (the binding the session uses) the runtime calls
Vulkan on our instance through the system loader, and a driver-native handle
is not loader-dispatchable. The route that works, and is the modern one, is
`XR_KHR_vulkan_enable2`: the runtime creates the VkInstance and VkDevice
itself through the `vkGetInstanceProcAddr` we pass in (`xrCreateVulkanInstanceKHR`,
`xrCreateVulkanDeviceKHR`), and plume adopts them instead of creating its
own. That is the same seam `XR_FB_foveation` and runtime-composited swapchains
need, so it is the next piece of the port rather than a probe.

## Multiview stereo, on screen

Desktop, `bd_stereo_multiview=true, bd_mv_layered_textures=true,
bd_mv_resolve=false`, MSAA 4 (the default):

```
non-black 92.3%  mean RGB 73/66/49
halves: mean abs diff 14.15, 31.8% of pixels differ
stereo_check: OK, crossed disparity, near separating more than far
```

Looked at: two views of the field scene with clear parallax on the fence
posts and the character. One difference to keep in mind: the shadow map is
rendered once, so the character's ground shadow differs between the eyes
(the right eye's is shorter). That is approximation the owner has accepted,
and a per-eye shadow projection is not where the time should go now.

So the multiview path is complete on the desktop: one submission, two layers,
the array heap for every read, the per-eye MSAA resolve keeping the pair, and
present flattening it. `bd_mv_layered_textures` can default on. On the Quest
the same path measures 59 ms GPU with the obsolete chain on and 277 ms with
it off, and both are the tiling question - the scene pass in direct mode -
which `XR_KHR_vulkan_enable2` plus Turnip is being built to answer.

## XR_KHR_vulkan_enable2 works on the Quest

`bd_xr_vulkan2=true` (the Android default now): plume builds its create-infos
as before and hands them to `xrCreateVulkanInstanceKHR` /
`xrCreateVulkanDeviceKHR` through two callbacks on `VulkanInterfaceOptions`;
the physical device comes from `xrGetVulkanGraphicsDevice2KHR`.

```
[xr] runtime created the VkInstance (enable2), VkResult 0
[xr] runtime created the VkDevice (enable2), VkResult 0
GPU caps: Vulkan 1.1.284 on Adreno (TM) 650 ...
dt 49.87 | gpu_total 40.43 | elsewhere 12-13 ms    (unchanged)
```

The runtime now dispatches through the `vkGetInstanceProcAddr` we pass, which
is the precondition for driving the headset with a directly loaded ICD.

## Update-after-bind is not the trigger (2026-09-02 10:05)

`bd_debug_no_uab=true` creates the bindless texture and sampler sets without
`UPDATE_AFTER_BIND` on the bindings, the layout and the pool (plume
`VulkanInterfaceOptions::noUpdateAfterBind`; partially-bound and the variable
count stay). The log confirmed it took (`[vk] bd_debug_no_uab: bindless sets
without update-after-bind`, `[config]` audit live=true), the frame rendered
(30.0 fps in the light stretch, `gpu_total_ms` 38.5, 557 draws), and the
render-stage trace is unchanged: the 1376x720 c64 d32 scene pass is
`Direct, bins=1`, Render 20.4 ms, Preempt 5.0 ms, in both windows. The same
trace still shows the 1920x3664 c32 pass in `HwBinning bins=25`, so the driver
is choosing per pass, not per process. Descriptor-set flags eliminated.

Remaining suspects, in the order they will be probed: the recompiled vertex
shaders (Adreno builds a position-only binning variant; if the position
depends on the whole fetch-constant/skinning body, the driver may judge the
visibility pass dearer than direct rendering), the 16-stream vertex input, and
per-draw dynamic offsets. Turnip through `XR_KHR_vulkan_enable2` remains the
only route to an authoritative reason, and it stalls at
`xrCreateVulkanInstanceKHR` = `XR_ERROR_VALIDATION_FAILURE`; the next step there
is a logging `vkGetInstanceProcAddr` trampoline that prints what the runtime
asks of the ICD.

## Turnip through enable2 reaches the instance and no further (10:20)

A logging `vkGetInstanceProcAddr` handed to `xrCreateVulkanInstanceKHR`
(`bd_xr_vulkan2_trace`) named the validation failure: the runtime appends
`VK_KHR_surface` and `VK_KHR_android_surface` to every instance it creates,
Turnip returns `VK_ERROR_EXTENSION_NOT_PRESENT` (-7), and the runtime reports
`XR_ERROR_VALIDATION_FAILURE`. The wrapper now drops extensions the ICD does
not expose; the runtime then creates the VkInstance through Turnip (VkResult
0). The next call, `xrGetVulkanGraphicsDevice2KHR`, takes only the instance
handle and the runtime resolves it through `/system/lib64/libvulkan.so`,
which faults in `GetInstanceProcAddr` on a handle the platform loader never
created. With the session enumerating the physical device itself,
`xrCreateVulkanDeviceKHR` answers `XR_ERROR_HANDLE_INVALID` (-12) before
calling the ICD: the runtime validates handles against the platform loader.
The session binding and swapchain import would go the same way. **A directly
loaded ICD cannot reach the XR session on this runtime; the Turnip route is
closed** short of a forwarding layer under the platform loader. The wrapper and
the extension filter stay (they cost nothing under the blob).

## No pass of ours bins. The binned surfaces are the compositor's (10:30)

The full surface list of the `bd_debug_no_uab` trace: every surface at the
guest resolutions (1376x720, 688x360, 344x180, 172x90, 128x72) is `Direct`,
including one-draw passes; the `HwBinning` surfaces are 1920x3664 c32 (25
bins) and 1024x1024 c32 d24s8 MSAA 4 (66 bins). `ovrgpuprofiler -t 1` with
`com.reblue` force-stopped lists exactly those two, so they are the
compositor's. The CLAUDE.md line "the choice is per pass" was wrong and is
replaced: **the trigger is a property of the whole process**, which narrows
it to what every pass shares - device features and extensions, the pipeline
layout, image creation, or the pass structure. Plume enables every queried
core feature wholesale (`pEnabledFeatures = &deviceFeatures.features`),
robustBufferAccess included, plus robustness2, buffer device address, scalar
block layout, sample locations, load/store-op-none and fragment density map.
Attachment layouts are the OPTIMAL ones (not GENERAL, which Turnip would
treat as a feedback loop). SPIR-V capabilities in the host dump: Shader,
RuntimeDescriptorArray, MultiView, SampledBuffer - nothing exotic. Probes
added: `bd_vulkan_no_robust` (robustness off), `bd_vulkan_no_ext` (optional
extensions treated as unsupported).

## Robustness and every optional extension eliminated; the capture shows the pass structure (10:45)

`bd_vulkan_no_robust=true` (robustBufferAccess, robustBufferAccess2,
robustImageAccess2 off; log confirmed): scene pass `Direct`, 25.45 ms,
`gpu_total_ms` 38.8 - robustness is not a lever on this frame either.
`bd_vulkan_no_ext` with fragment density map, sample locations,
load/store-op-none, buffer device address, scalar block layout, mirror clamp,
present id and present wait all treated as unsupported: `Direct`, 25.1 ms,
38.3 ms total. Device features and extensions are eliminated.

`tools/rdc_outline.py` now prints each pass's attachment load/store ops (from
the `vkCreateRenderPass` chunks). The 2026-08-31 desktop side-by-side capture:

```
[ 0] 4096x4096 draws=221 [D32_SFLOAT_S8_UINT x1 C/S]            shadow map
[ 2]  480x270  draws= 90 [RGBA16F x1 L/S; D32S8 x1 C/S]         reflection
[ 3] 1920x1080 draws=  0 [RGBA16F x4 C/S]                       clear-only pass, colour-only fb
[ 4] 1920x1080 draws=314 [RGBA16F x4 L/S; D32S8 x4 C/S]         the scene, colour LOADED
[ 5] 1920x1080 draws= 14 [RGBA16F x4 L/S; D32S8 x4 L/S]         restart
[ 6] 1920x1080 draws=  4 [RGBA16F x4 L/S; D32S8 x4 L/S]         restart
... 22 one-draw post passes, all L/S
```

The guest binds the colour target, clears it, then binds the depth target.
Plume's deferred clear is per framebuffer, so the switch flushed it as a
zero-draw pass (3) and the scene pass (4) loads the colour it just cleared.
`VulkanCommandList::setFramebuffer` now carries a pending clear over to the
new framebuffer when it holds the same attachments. Verification: a desktop
capture must show pass 3 gone and pass 4 as `C/S` on colour; then the Quest
trace says whether that was the direct-mode trigger.

## Held clears: the scene pass now begins with CLEAR, and it is still Direct (11:05)

The framebuffer trace (`PLUME_FB_TRACE=<path>`, plume) showed the real order:
the host's `RequestClear` clears the scene colour through a colour-only
framebuffer and unbinds; the shadow and reflection passes run; the scene binds
colour+depth; and the draw queue's rebind of the same framebuffer flushed the
deferred depth clear too. Plume now holds a deferred clear per texture across
framebuffer switches (`HeldClear`), folds it into the load op of the next pass
that binds the texture, and flushes it as a pass of its own only when a
barrier or copy touches that texture. The host keeps the cleared target out
of the speculative SHADER_READ flip (`held_clear_rt`). Desktop capture
(`held.zip.xml`): the scene pass is `[RGBA16F x4 C/S; D32S8 x4 C/S]` with 639
draws and the zero-draw passes on it are gone; the frame is a correct stereo
pair with shadows. Quest: surfaces 34 -> 30, `gpu_total_ms` 38.5 -> 37.5,
and the scene pass is **still `Direct`** (Render 19.5 ms). Load ops are not
the trigger. One leftover: the shadow map's depth clear still flushes as a
zero-draw pass because a barrier touches it first (being traced).

The positive control from the 10:40 run: the surface right before the post
chain - the scene target's last pass instance, the ~60 effect draws after the
mid-frame resolve - ran `HwBinning` with 16 bins. Same attachments, same
formats, same layouts as the main instance. What differs is the content:
the main instance carries the opaque geometry through the recompiled vertex
shaders. Next probe: `XENOS_RECOMP_POS_ONLY_VS` zeroes every vertex output
but position so DXC strips the rest; if the pass bins with small vertex
shaders, the driver's choice is a cost model over vertex work.

## Position-only vertex shaders: still Direct (11:20)

`XENOS_RECOMP_POS_ONLY_VS` (55 guest vertex shaders with every output but
position zeroed, DXC stripping the rest): scene pass `Direct`, Render 19.4 ms,
`gpu_total_ms` 37.5 - identical to the full shaders. So the vertex work is
neither what keeps the pass direct nor a measurable share of its time.
Eliminated. Next: the depth format (ours D32S8, the compositor's binned
surface D24S8), stencil/depth bias in the scene pipelines (valid run pending).

## Stencil and depth bias eliminated (11:35)

`bd_debug_no_stencil_bias=true` (valid run this time, audit live=true): scene
pass `Direct`, Render 19.6 ms, `gpu_total_ms` 37.6. Eliminated. The
queue-flush reorder in `BindDrawFramebufferLocked` (queued draws now leave
before the speculative SHADER_READ flip; they used to be emitted into a
target already flipped) is in this build; frame unchanged.

## Depth writes eliminated (11:50)

`bd_debug_no_depth_write=true`: scene pass `Direct`, Render 23.3 ms (more
overdraw, as expected), `gpu_total_ms` 39.9; the effects instance binned
again (16 bins, 2.3 ms). Sixteen properties of the pass eliminated. The
remaining explanation is the environment: every trace shows the compositor
preempting the scene pass (Preempt 4-6 ms), and a driver that must preempt
at draw granularity for a higher-priority context has a reason to keep a
long pass out of GMEM. Test: the flat path on the headset, no VR session.

## The shadow-map flush is a desktop artefact; flat path untraceable (12:00)

With every host barrier site named in the trace (`NoteBarrierCall` appends
the site), the barrier that flushes the held shadow-map clear reports no
site: it comes from a caller outside the counted ones, and the only such
caller that reads depth before the shadow draws is the sun-occlusion pass,
which runs on the desktop and is dropped on Adreno. The Quest measurement
configuration renders no shadow map (`bd_shadows=false`), so this stays
noted, not chased. `bd_vr_enabled=false` on the headset: the app ran (503
draws, 50.8 ms GPU) but `ovrgpuprofiler -t` listed only the compositor - the
render-stage trace sees the VR foreground context only. Preemption stays
untested. Probe running: `bd_cull_distance=20`, the amount of geometry.

## Instrument trap: a duplicated value option voids args.txt (12:15)

Two `bd_cull_distance=20` runs came back with every setting at its default
(`[config]` audit: 11 of 15 did not take effect, no `args.txt added` line).
The cause is CLI11: a value option given twice (`--bd_cull_distance 350`
from the script defaults, then `20` from the caller) throws, and the whole
file is dropped. Boolean flags given twice were tolerated, which is why the
`bd_stereo=false` runs worked. `tools/verify_quest.sh` now dedupes keys with
the caller winning.

## Geometry amount eliminated; the render-mode hunt is parked (12:30)

`bd_cull_distance=20` (audit: all 13 took effect): 332 draws, scene pass
`Direct`, Render 12.3-14.4 ms, `gpu_total_ms` 16.5 - and a frame at the 30 fps
boundary, with most of the world culled away. Eighteen properties of the
pass, the pipelines, the device and the process have now each been changed
with the render-stage trace watching, and none moved the Adreno blob off
`Mode: 0 (Direct)` for the scene pass; the effects instance of the same
target bins. What could not be tested: the compositor's preemption (the
profiler sees only the VR foreground) and the driver's own reasons (Turnip
cannot reach the XR session). Parked, not closed: the fixed-foveation lever
depends on it (a fragment density map on a direct-mode pass measured +17 ms
for nothing on 2026-09-01, which is what a direct-mode FDM would do).

What the same traces show that IS ours to change: the present pass into the
XR swapchain runs at 3664x1920 - the panel's maximum, both eyes - for a
1376x720 scene, at 2.5-2.7 ms Render plus 1.4-1.8 ms Preempt every frame,
and the guest's post chain is ~8 ms of one-draw full-screen passes.

## The headset frame composed at 1466x768: GPU 37.5 -> 34.4 ms (12:50)

The XR swapchain and the offscreen present target were the window's size -
the whole panel, 3664x1920 - for a 1376x720 game frame, and the XR quad path
created the swapchain on the frame the session turned Running, before any
offscreen target existed, so the size was locked to the window forever.
`bd_xr_present_scale` (default 0.4 of the window, aspect preserved; the quad
path sizes the swapchain the same way and skips a mismatched frame) puts the
present at 1466x768: that pass went from 4.5-5.0 ms (Render 2.7, Blit 0.6-0.8,
Preempt 1.4) to 2.0 ms (Render 0.57, Blit 0.10), and `gpu_total_ms` from 37.5
to **34.4**, 1.1 ms above the 33.3 ms boundary; the lighter stretch of the
run reads 33.4 ms, 30 fps. Audit: all 13 settings took effect.

## Two bugs behind the resized present, both found by looking at pixels (13:10)

The first headset capture after the resize was the top-left 40% of the game
magnified: `RecordPresentPass` took its viewport, fit and capture extent from
the window, not from the target it renders into. It now reads the target's
own extent. The next runs then wrote no capture at all: the guest-surface
capture site evaluates `CaptureDue()` before checking whether it can record,
and in mono it consumed the one-shot latch with nothing written, so the
composite site never fired. The latch is now asked only when that site can
record. Verified on the desktop under the headless runtime: the frame composed
at 768x432 is the whole scene, correctly framed (`frame_1788365213.png`).

Headset verification (13:15): the pulled 1466x768 capture is the whole field
frame, correctly framed, normal brightness; `gpu_total_ms` 34.8, audit all 13
settings took effect.

## Side-by-side stereo with today's changes: 37.5 ms GPU (13:30)

`verify_quest.sh` defaults (bd_stereo=true, the shipping route): `gpu_total_ms`
**37.5** (39.2 yesterday evening), 20 fps tier, 4.2 ms above the boundary;
CPU elsewhere 15 ms. The pulled 1466x768 capture is a correct stereo pair
(733x768 per eye, visible parallax). Next: the multiview path's Quest cost
under the render-stage trace, resolve chain on and off.

## Multiview on the Quest, resolve chain on: 62.7 ms, and the reason is pixels (13:45)

Render-stage trace (`bd_stereo_multiview=true`, `bd_mv_layered_textures=true`,
resolve on): the scene pass is `Direct`, Render **35.7 ms** (mono 19.6), frame
62.7 ms, 15 fps. Multiview renders two full 1376x720 layers; side-by-side
gives each eye 688x720 - half the fragments. The fragment-bound pass costs
what its pixels cost, so the multiview target has to be a 688x720 two-layer
image before its number means anything against side-by-side. That is the
next piece: layered targets at half width, the per-layer viewport at x=0,
the present composing each layer into its half.

## Multiview without the resolve chain: 424 ms, and the trace sees none of it (14:05)

`bd_mv_resolve=false`: `gpu_total_ms` 424, 2.3 fps, `[xr] fence 357 ms`. The
render-stage trace (2.5 s window) lists twelve compositor surfaces and not one
of ours, so whatever the driver is doing in that frame is not a render pass
it reports - a fence that long reads as the driver serialising or waiting,
not rendering slowly. Not shipped (resolve is on by default on Android), so
parked behind the half-width multiview measurement; a longer trace window
and the realtime metrics are the instruments when it is picked up.

## Half-width multiview: 41.7 ms, and the frame is black (14:20)

`bd_mv_half_width` halves the width the guest asks for in
`bdSceneResolutionScaleHook` under multiview: the scene target became
688x720 with two layers and the frame went 62.7 -> 41.7 ms - but the guest's
front buffer stays 1376 wide (sized from the output fit, not the scene), the
present still reads a 1376x720 layered rt, and the pulled capture is black.
Default off until the present-side chain follows the narrower scene; to be
read out of the code, not probed.

## The post chain, recorded per frame (14:45)

`bd_dump_post_draws` names every post draw by pixel shader hash. A desktop
field frame at 1280x720, 15 full-screen quads, all through the tile + resolve:

```
quoter    1280x720 -> 640x360      (bilinear downsample)
quoter     640x360 -> 320x180
quoter     320x180 -> 160x90
quoter     160x90  ->  80x45
quoter      80x45  ->  80x45
ms_weight  640x360 in place, 13 taps   (blur)
ms_weight  320x180, 160x90, 80x45, 80x45 likewise
dof       1280x720: scene, depth, blur640, blur320; c27=(2.4, 0.075, 0.5, 0.997)
brightpass 1280x720 -> 320x180; c27=(0.25 threshold, 10 intensity)
ms_weight  320x180 x2 (13 taps each)
ms_tex    1280x720: scene + bloom320, 2 weights
```

The 160x90 and 80x45 levels are computed and not read by anything in this
scene. Host plan: downsample 1/2 and 1/4, blur each, DoF composite, bright
pass at 1/4, two blurs, composite - nine passes, no resolves, the guest's
parameters read from its live constant block at the draw it would have made.

## The host-owned post chain, first working (15:00)

`src/gpu/post_chain.cpp`, `bd_host_post` (default on). Read from the chain
recording: the two composites (`dof`, `ms_tex`) are full-screen draws that
sample fixed slots - depth 0, scene 1, five blurred levels 2-6 at 1/2, 1/4,
1/8, 1/16, 1/16 for `dof`; scene 0 and the 320x180 bloom mask 1 for
`ms_tex`. Everything that produced those textures (five quarter downsamples,
seven 13-tap blurs, one bright pass, each through the tile plus a resolve)
is replaced: the draw path hands post draws to `HostPostIntercept` right
after the framebuffer bind and before any state flush; producers into
non-fullscreen targets are dropped, and at each composite the host first
fills the sampled guest textures itself - a box downsample and a separable
9-tap gaussian through two host scratch textures per size, the bright mask
with threshold and intensity read from the guest's live c27. The composites
then run as the guest wrote them, so no host DoF or composite shader exists.
No host pass touches the tile; nothing is resolved. Desktop capture
(`frame_1788368105.png`): depth of field on the distance, the character
sharp, bloom on the sky - the frame reads the same as the guest chain's.

Quest, side-by-side defaults, host chain on: `gpu_total_ms` **38.0** (37.5
before), draws 534 (541), capture a correct stereo pair with the same depth
of field and bloom. The pyramid passes cost what the guest's small passes
cost; the saving has to come from fewer passes - one dual-filter pass per
level, and one host composite for the two full-resolution guest composites
and the resolve between them.

## The host composite: one full-resolution pass for both guest composites (15:45)

`bd_host_post_composite` (default on): at the dof draw the host builds the
five levels with one 13-tap downsampling blur each (Jimenez 2014) and drops
the guest draw; at the ms_tex draw it builds the bloom mask and runs
`post_composite_ps` into the frame - bd_pe_ps_dof's level cascade and
bd_pe_ps_ms_tex's weighted sum in one pass - and drops that draw too. The
guest's parameters ride a host-filled block through the pixel-constant
binding (`UploadHostConstants`): c27 from the dof draw, c13/c14 from ms_tex
(w0 = 4 is the resolve exponent-bias restore). Two things the captures
taught: depth sits at 0.99+ everywhere with this projection, so the guest's
level stays below one across the distance and the look is entirely the
first level's blur - at the nominal kernel width the distance stayed sharp;
at twice the width (`bd_host_post_blur=2`) the far buildings and cliffs blur
as the guest's did, the character stays sharp (`frame_1788369916.png`). Nine
host passes replace fifteen guest quads, two of them full resolution, and
every resolve between them.

Quest, side-by-side defaults, folded composite: `gpu_total_ms` **37.6** (37.5
before the host chain, 38.0 with the first form); the capture is a correct
stereo pair. The post chain's GPU share on the Quest was small all along; the
rewrite removes the Xbox 360 tile-and-resolve structure and fifteen guest
draws, and buys no frame time there. The frame is the scene pass.

## Two thirds of the texture data has no mip chain (16:20)

The mirror histogram with texel totals (desktop, 512 textures): 316 ship a
chain and hold 21.0 M texels; 196 ship none and hold **43.1 M** - and they are
the world textures, 1024x1024 and 2048x1024 in DXT1 (0x12), DXT3 (0x13) and
DXT5 (0x14). Every fragment of those samples the base level, which is what
the Quest's texture pipes at 66% with 25% L1 misses look like. Host mip
generation at upload - decode the blocks, box-filter, re-encode with
stb_dxt (added to thirdparty/stb, public domain) - is the next piece.

## Host mip chains, first working (16:50)

`src/gpu/host_mips.cpp`, `bd_host_mips` (default on): for a DXT1/3/5 texture
the guest ships without a chain, the mirror decodes the base blocks,
box-filters each level and re-encodes with stb_dxt; DXT1's punch-through
alpha and DXT3's explicit alpha block are done by hand (stb has neither),
and the levels feed the same chain builder the guest's own mips use. Desktop:
chains of 4-8 levels build at upload, the capture shows cliffs, ground,
buildings and foliage cutouts intact (`frame_1788370957.png`). Quest
measurement follows.

Quest, side-by-side defaults, host mip chains on: `gpu_total_ms` **37.6** -
no change. The sampler LOD range is unbounded, so the chains are sampled;
the texture pipes' load is fragment volume, not minification misses. The
chains stay for the image. A fresh render-stage trace of the shipping
configuration with everything landed follows.

## Opaque cutouts for alpha-tested blended depth-writers (17:30)

The census says every blended depth-writing scene draw blends SRC_ALPHA over
INV_SRC_ALPHA, and about half of them alpha-test (desktop: 315 of 621). A
blended draw that writes depth invalidates the tiler's low-resolution Z and
rejects nothing behind it; a cutout - alpha test deciding the pixel, no
blend, depth written - is what a modern engine draws foliage and fences as.
`bd_cutout_opaque` makes the conversion in `SanitizePipelineState`: blend
off, ONE/ZERO, for pipelines with alpha test, depth write, depth test and
that blend pair. Desktop capture (`frame_1788371951.png`): foliage, fence
and rope edges read the same as blended. Quest measurement follows; the
half without alpha test stays blended for now.

Quest, side-by-side defaults, `bd_cutout_opaque=true`: `gpu_total_ms`
**37.5**, 193 of 321 blended depth-writers converted, capture a correct pair.
No change - the fourth work-reducing change today that leaves the
frame-wide number where it was (post chain, mip chains, cutouts), while the
two pass-structure changes moved it. The frame's GPU span includes the
compositor's preemption and any idle gaps; per-pass render time from the
render-stage trace is the only honest gauge of our own work. A mono trace
with everything landed, against the morning's 19.5 ms scene pass, is
running.

## THE SCENE PASS IS DRAW-BOUND, NOT FRAGMENT-BOUND (18:00)

Mono render-stage trace with everything landed (held clears, present size,
host post chain, host mips, cutouts): scene pass Render **19.2-19.7 ms**,
535 draws - the morning's 19.5 ms exactly. The second point is the morning's
`bd_cull_distance=20` run: 332 draws, Render 12.3-14.4 ms. Both sit on one
line: **~36 us of GPU time per scene draw**, and the count is what changed.
Everything that reduces fragment work moved nothing: depth ALWAYS, the depth
prepass, the density map, position-only vertex shaders, opaque cutouts, mip
chains, the host post chain. The realtime counters that said "99% shading
fragments, 6.6 fragments per pixel" described where the GPU sat, not what it
waited on. The scene pass costs what its draws cost: the command stream and
the state each carries - a descriptor set bind with three dynamic offsets,
vertex buffer binds for up to sixteen streams, viewport and scissor, the
pipeline. Fewer draws (instancing on the bdSceneNodeDrawSingle seam) and
cheaper draws (constants indexed by a push constant instead of a per-draw
descriptor bind, only the streams a declaration uses) are the levers - the
"submit less" work the mandate names. `gpu_total_ms` 34.8 mono.
