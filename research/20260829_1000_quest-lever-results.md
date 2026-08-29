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

## Cull bias: mechanically working, win inside noise

```
configuration                                   frame    fence     else    fps   draws
render_scale=25, reflections=false             71.4ms    0.1ms   62.9ms   14.0   3041
  + cull_bias=0.6                              64.2ms    0.1ms   55.7ms   15.6   2931
  + cull_bias=0.35                             66.4ms    0.2ms   57.9ms   15.1   2735
```

The draw count falls with the bias - 3041, 2931, 2735 - so the hook is working: shrinking the
bounding radius before `bdSceneNodeCullTraverse`'s visibility test does make the guest cull its own
nodes and skip their draws.

The CPU win is **11%**, which is inside the +/-30% cross-restart band this file keeps insisting on,
so it is directional rather than proven. Roughly proportional to the draws removed, which is at
least consistent with node submission being a large share of the CPU.

**0.35 is not better than 0.6.** It removes another 200 draws and the frame does not improve, so the
extra nodes it culls are cheap ones - small or simple - while the cost is concentrated somewhere the
bias does not distinguish. A radius bias culls by *size*, and what matters is presumably *what is in
the node*, so this is the wrong axis to keep pushing.

**Best measured configuration so far: 15.6 fps** (`render_scale=25, reflections=false,
cull_bias=0.6`), against a 5.9 fps baseline. **2.6x.**

## Where the port stands after a day on the device

| | |
| --- | --- |
| baseline, as it was this morning | 5.9 fps |
| best now | **15.6 fps** |
| GPU | **solved** - the fence is 0.1ms and can be traded for image quality at will |
| CPU | ~56ms, and it is the entire frame |
| 72 fps needs | 13.9ms |

The GPU half is finished as an engineering problem. Everything from here is the ~56ms of CPU, and
the two things that have moved it are draw count (a little) and nothing else yet. The census points
at `bdSceneNodeDrawSingle`; the bias experiment says size is the wrong way to select what to cut.

## Distance culling: the CPU floor comes down 43%

Measured with the repaired loop - the one that waits for the app to actually die and refuses to
report a run that produced no new log:

```
configuration                                    frame    fence     else    fps   draws
render_scale=25, reflections=false              75.3ms    0.1ms   65.8ms   13.3   3713
  + cull_distance=1200                          73.2ms    0.1ms   64.8ms   13.7   3649
  + cull_distance=600                           47.0ms    0.2ms   37.4ms   21.3   1731
```

**At 600 the draw count halves and the CPU falls from 65.8ms to 37.4ms - 43% - for 13.3 -> 21.3
fps, a 1.60x.** Far outside the +/-30% band, and the largest single win in the port so far.

Against this morning's 5.9 fps baseline that is **3.6x**.

It also confirms the census: node submission *is* the CPU floor. Halving the nodes halved the CPU,
which is what a cost that is per-node and nothing else looks like.

### It worked the whole time

`bd_cull_distance` was reported as "no effect" twice. Both reports were invalid, and for the same
reason: **`am force-stop` does not reliably kill a VR app the Oculus runtime is holding.** The
process survived, `am start` became a no-op, the app carried on with its previous args, and every
configuration in the sweep measured the same unchanged live process - producing tables of identical
numbers that read as "this setting does nothing".

Two conclusions were published from that: that the cull did nothing, and that a second call path
must be bypassing it. Both wrong. The second path, `sub_82282608`, was then hooked, hung the guest,
and was reverted with a further wrong explanation about addresses.

A site probe settles it: **the second path never executes in this scene at all** - an inert hook on
its compare never fires once. Every one of the ~2000 `bdSceneNodeDrawSingle` calls a frame comes
through `bdSceneNodeCullTraverse`, the path that was hooked correctly from the start.

**The tooling was the bug, and it cost three wrong conclusions.** A measurement loop that silently
reports the previous run is worse than no measurement, because it is indistinguishable from a real
negative result. `bench_quest.py` now polls `pidof` until the process is gone, records the newest
log before launch and discards the run if it has not changed, and warns when every configuration
produces an identical frame time - which is the check that finally caught it.

### Where the frame is now

| | |
| --- | --- |
| this morning | 5.9 fps |
| now | **21.3 fps** at `render_scale=25, reflections=false, cull_distance=600` |
| GPU | 0.2ms - still nothing |
| CPU | 37.4ms, still the whole frame |
| 72 fps needs | 13.9ms |

The CPU is still 2.7x too slow, and it is still node submission. `bd_cull_distance` is a blunt
instrument - things pop in at the boundary - so the next move is either a gentler curve (fade rather
than cut) or attacking the per-node cost itself, which is `bdSceneNodeDrawSingle` at 7,740 bytes of
recompiled PowerPC per node.

## The target is not 72 fps, and we are closer than the frame rate suggests

Sweeping the cull further:

```
configuration                                 frame    fence     else    fps   draws
cull_distance=600                            46.6ms    0.2ms   37.0ms   21.4   1754
cull_distance=450                            42.8ms    0.2ms   32.1ms   23.4   1238
cull_distance=350                            41.8ms    0.1ms   19.2ms   23.9    545
```

The CPU keeps falling - 37.0 to 19.2ms - but the frame rate stops moving. The breakdown says why:

```
frame 41.6ms = xrWait 21.0 + fence 0.3 + elsewhere 19.9
```

**`xrWait` is now the largest item in the frame.** 41.6ms is 72/3 Hz to within a tenth of a
millisecond, so the compositor has locked the app to a third of the display rate and `xrWaitFrame`
is absorbing the slack. The *work* is only about 20.6ms.

### Which changes what "fast enough" means

A Quest 2 compositor paces to submultiples of 72Hz: **13.9ms for 72, 27.8ms for 36, 41.7ms for 24**.
At ~20.6ms of work the app is already comfortably inside the **36Hz** tier and is being paced at 24
anyway - so there is a whole tier being left on the table for reasons that are not the frame's cost.

And **Blue Dragon is natively 30 fps**. 36Hz is not a compromise for this game, it is above the rate
the original ran at. 72 was never the right target; it is the panel's rate, not the game's.

So the remaining work splits into two very different problems:

1. **Why is a ~20.6ms frame being paced at 24Hz rather than 36?** Candidates: the app never requests
   a display refresh rate (`XR_FB_display_refresh_rate`), so the runtime assumes 72 and drops to a
   third when it cannot be met; or the pacing decision has hysteresis and is reacting to the
   pre-cull frame times; or occasional long frames pull the average past the threshold. This is
   worth more than any further CPU work - it is a whole tier for no rendering change at all.
2. **Getting under 13.9ms**, which is the only reason to keep cutting CPU, and which matters only if
   72Hz is actually wanted for a 30fps game.

The honest reading: the port is one pacing question away from running at above its native frame
rate, and the CPU work that looked like the whole problem this morning has already done its job.

## 28.9 fps: the game's native rate, in VR, on a Quest 2

```
configuration                                              frame    fence     else    fps   draws
render_scale=25, reflections=false, cull_distance=350,
  xr_refresh_rate=60                                      34.6ms    0.3ms   19.0ms   28.9    519
  xr_refresh_rate=72                                      41.8ms    0.1ms   19.2ms   23.9    536
```

**Identical work, 5 fps apart.** 34.6ms is the 60/2 tier; 41.8ms is the 72/3 tier. The only
difference is which rate the app asked the runtime for.

Blue Dragon shipped at 30fps on an Xbox 360. **28.9 is that rate.** Against this morning's 5.9,
the port is **4.9x** faster.

### Why asking matters more than it sounds

A Quest compositor paces to submultiples. Inheriting the system's 72Hz means the tiers are 13.9,
27.8 and 41.7ms - and a frame of ~20ms of work misses 13.9, so it is thrown all the way down to
41.7. Requesting 60Hz moves the tiers to 16.7, 33.3 and 50, and the same ~20ms frame lands on 33.3.

Nothing rendered differently. The port was being paced against a target it was never going to hit,
and 72Hz is the panel's number rather than the game's.

`bd_xr_refresh_rate` requests it, defaulting to 0 (leave the runtime alone). The extension is
appended only when the cvar is set, and `xrCreateInstance` is retried without it on failure.

### Two mistakes on the way in, both worth recording

**Requesting the extension unconditionally killed the session.** An unsupported extension name fails
`xrCreateInstance` outright, and the app then died before it could log why.

**Probing for it first was worse.** Calling `xrEnumerateInstanceExtensionProperties` before
`xrCreateInstance` looks like the careful thing to do, and on Android it is not: the loader has to be
initialised first, so the app launched to a splash screen with a 4MB native heap, no guest, and no
log at all. The symptom was identical to the first mistake, which made it look like the fix had not
worked.

**Append and retry needs no call before instance creation**, which is why that is what it does now.

### Where the frame is

| | |
| --- | --- |
| this morning | 5.9 fps |
| now | **28.9 fps** at 60Hz, render_scale 25, reflections off, cull 350 |
| CPU | 19.0ms against the 33.3ms the tier allows - comfortably inside |
| GPU | 0.3ms |
| next tier | 16.7ms at 60Hz, which needs the CPU roughly halved again |

The cull is blunt at 350 - things pop in - so the honest next step is a gentler distance curve, or
per-node cost, now that there is real headroom inside the tier to spend on quality.

## Correction: "the GPU is solved" is true at quarter scale and nowhere else

Spending the tier's headroom on resolution does not work, and the reason corrects a claim made
several times above.

```
refresh 60, cull 350, render_scale=100
  frame 66.8ms = xrWait 48.4 + fence 0.1 + elsewhere 17.8

refresh 60, cull 400, render_scale=75
  frame 50.5ms = xrWait 23.5 + fence 0.2 + elsewhere 26.3
```

At full scale the app does ~18.5ms of *measured* work and is paced at 60/4. Nothing in the
breakdown accounts for the other 48ms - `fence` reads 0.1ms.

**Because `fence` does not measure the GPU.** It measures the CPU waiting on a fence, and when the
CPU is far enough ahead there is nothing to wait for. The compositor still sees a frame that arrives
late and paces down accordingly. CLAUDE.md already records this - "a near-zero GPU fence does not
mean the GPU is idle, it means the GPU was not the last thing waited on" - and it was still read as
headroom.

So the earlier statement that the GPU half is "solved, and can be traded for image quality at will"
is wrong as written. It is solved **at `render_scale=25`**. The fill cost is still there at higher
scales; it is simply invisible in the column being read, and it surfaces as the compositor dropping
a tier.

### The best known configuration

```
bd_xr_refresh_rate  60
bd_render_scale     25
bd_reflections      false
bd_cull_distance    350
```

**28.9 fps**, frame 34.6ms in the 60/2 tier, CPU 19.0ms. That is Blue Dragon's native rate.

Raising quality from here needs the *real* GPU cost measured, not inferred from `fence` - which
means a GPU timestamp around the scene pass, or the scissor trick from
`20260829_0230_the-frame-is-fill-bound.md` applied at each candidate scale. Guessing from `fence`
will keep saying resolution is free right up until the compositor halves the frame rate.
