# Research: the levers, measured on the Quest at last

Date: 2026-08-29 10:00
Topic: `bench_quest.py all` on a Quest 2, and the question it was built to settle.

```
configuration                                        frame    fence     else    fps   draws
baseline                                           170.1ms  108.6ms   53.2ms    5.9    2834
render_scale=50, reflections=false                 129.0ms   68.7ms   52.0ms    7.8    2761
  + shadows=false                                  129.4ms   65.4ms   55.6ms    7.7    2777
  + stereo (sep 0.06, conv 0.03)                   164.3ms   91.5ms   63.9ms    6.1    2766
```

## The fill work transfers

**GPU fence 108.6ms -> 68.7ms, a 37% cut, and 5.9 -> 7.8 fps.** Comfortably outside the +/-30%
cross-restart band, so it is a result rather than noise, and it is the first time anything verified
on the desktop build has been confirmed on the device. The desktop loop was worth building.

`bd_shadows=false` is **not** a result: 65.4ms against 68.7ms is inside noise. The per-surface census
could never see the shadow pass because it hooks the colour target, so this flag went to the device
unverified and has now measured as approximately nothing. It should be described that way rather
than as a lever.

## The back-pressure hypothesis is dead, and that is the useful part

`research/20260829_0900_guest-call-census.md` raised the possibility that part of the ~62ms of
`elsewhere` was the guest thread **waiting** on the render thread rather than computing - which
would have meant the fill work shrank the CPU floor for free, and that culling was not worth
building. The test was to free the GPU and watch `elsewhere`.

**43ms of GPU time was freed and `elsewhere` moved 53.2ms to 52.0ms.** Nothing. The floor is real
computation.

So:

- **The CPU floor is genuine** and caps this port near **19 fps** on its own, independent of
  everything the GPU does.
- **Culling is worth building.** The census target stands: `bdSceneNodeDrawSingle`, 420 calls a
  frame in a village and 130 in a wasteland, first place in both scenes.
- The two halves of the frame are as separable as the earlier notes assumed. The factor-of-three
  anomaly in the old `bd_debug_max_draws` number remains unexplained, but it is not back-pressure.

## Stereo runs on the headset

The fourth row is the first time `bd_stereo` has been on a Quest. It did not crash, and it costs
what it should: **fence 65.4 -> 91.5ms** for a second view of a half-scale scene, and **`elsewhere`
55.6 -> 63.9ms** for the doubled draw recording, which is the ~8ms the design predicted. Frame
129 -> 164ms, 7.7 -> 6.1 fps.

Worth stating plainly: **stereo at half scale costs less than mono at full scale** - 91.5ms of fence
against the baseline's 108.6ms. Two eyes are genuinely cheaper than today's single eye, which is
what `bd_render_scale` was for.

**Not visually confirmed in the headset.** The frame cost says it drew twice; nobody has looked
through the lenses. That is the next thing, and it needs a person wearing it.

## Where this leaves the port

| | |
| --- | --- |
| best mono | **7.8 fps** (129ms: 68.7 GPU, 52.0 CPU) |
| best stereo | **6.1 fps** (164ms: 91.5 GPU, 63.9 CPU) |
| CPU floor alone | ~52ms, i.e. **19 fps** ceiling |
| target | 72 fps, i.e. 13.9ms |

Still an order of magnitude out, and the shape of the remaining work is now unambiguous: the GPU has
a proven lever with more room in it (render scale below 50, and foveation, which was dismissed on
the false "not fill-bound" reading and should be reassessed), and the CPU needs the node-submission
cost attacked directly, with culling as the first candidate.

## Foveation is not the lever it looks like, in this architecture

`bd_render_scale` working reopened fixed foveated rendering, which had been dismissed on the
since-disproved reading that the frame was not fill-bound. It should stay closed, for a different
and better reason.

`XR_FB_foveation` reduces the shading rate across an **OpenXR swapchain image**. This port does not
render the scene into a swapchain image: the guest draws into its own surfaces - the ones
`D3DDevice_CreateSurface` hands out and `bd_render_scale` resizes - and present composites the
finished frame into the runtime's image. The 68.7ms of fence is spent in the guest's surfaces.
Foveating the swapchain would apply to a single full-screen composite blit that costs almost
nothing.

So foveation is worth exactly nothing **until the guest scene renders directly into per-eye XR
swapchain images**, which is the same piece of work stereo needs next. After that it is free
performance and Quest 2 supports it (fixed only - there is no eye tracking). Before that it is
effort against a blit.

The session currently requests two extensions, `XR_KHR_vulkan_enable` and
`XR_KHR_android_create_instance`, and adding the foveation pair to that list would compile and do
nothing measurable.

## Render scale below 50: the GPU can be taken to zero

```
configuration                          frame    fence     else    fps   draws
render_scale=50, reflections=false   129.4ms   62.7ms   58.0ms    7.7   2768
render_scale=35, reflections=false   115.8ms   50.6ms   57.0ms    8.6   2775
render_scale=25, reflections=false    73.0ms    0.1ms   64.1ms   13.7   3045
```

**13.7 fps at quarter scale, against 5.9 at baseline - 2.3x.** And the GPU fence is **0.1ms**: the
scene is drawn, the levers are on, and the GPU has nothing left to wait for. The fall from 50.6ms to
0.1ms between 35 and 25 is the same super-linear tiler behaviour the scissor sweep showed - below
some size whole tiles contain nothing and are skipped rather than shaded cheaply.

So the GPU half of this port is **solved**, in the sense that it can be taken to zero whenever the
image quality is worth trading. What is left at 73ms is 64ms of CPU and 8ms of `xrWait`.

**The frame is now entirely CPU-bound**, which is what the earlier back-pressure test predicted and
this confirms from the other direction: freeing the GPU completely leaves `elsewhere` at 64ms, up
slightly from 58ms rather than down.

### What that means for the target

72 fps needs 13.9ms. The CPU floor is ~58-64ms. **Nothing on the GPU side can get there** - it is
already at zero. Every remaining frame of headroom has to come out of the CPU, which is guest
simulation plus node submission, and the census names `bdSceneNodeDrawSingle` as the top consumer in
both scenes it has been run in.

`bd_cull_bias` is the first attempt at that, hooked before the visibility test in
`bdSceneNodeCullTraverse` so the guest culls its own nodes more aggressively without any control
flow being redirected.
