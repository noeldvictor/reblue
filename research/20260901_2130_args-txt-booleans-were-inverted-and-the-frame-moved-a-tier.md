# args.txt booleans were inverted, and fixing that moved bd_stereo a tier

2026-09-01. Quest 2 (`1WMHH830AY1165`), three runs, and one desktop multiview
run.

## The config trap, named by the audit

The first `verify_quest.sh` run of the day was configured through both
`args.txt` and the profile TOML, and `bd::AuditProfileConfig` said:

```
[config] 'bd_reflections' did not take effect: file says 'false', live value is 'true'
[config] 'bd_shadows' did not take effect: file says 'false', live value is 'true'
[config] 'bd_stereo_multiview' did not take effect: file says 'false', live value is 'true'
[config] 3 of 14 settings in reblue.toml did not take effect
```

`bd_stereo_multiview` defaults to false, both files said false, and it was
live true. Only `args.txt` could have done that, and the SDK's parser says how:
`rex::cvar::Init` registers every boolean cvar with CLI11 as a **flag**
(`--name,!--no-name`). A flag does not consume the next token. So

```
--bd_stereo_multiview
false
```

is "set the flag" followed by a positional `false` that `allow_extras()`
swallows. **Every boolean written as `--name` on one line and `false` on the
next was set to true.** That is the format `tools/bench_quest.py`,
`tools/verify_quest.sh` and `tools/stereo_check.py` all write, and have written
since they were created.

Consequences for the record:

- The run above measured `bd_stereo=true` **and** `bd_stereo_multiview=true` at
  once: `gpu_total 61.79ms`, `dt 66.98`, the both-paths-on configuration this
  repo already knows rasterises everything four times. Discarded.
- Every earlier device number whose configuration turned a boolean *off*
  through `args.txt` is suspect. "`bd_shadows` is not a lever (65.4 vs 68.7ms)"
  was measured with shadows on both times. The `bench_quest.py levers` table's
  `reflections=false` and `shadows=false` rows were the same. Runs configured
  through the profile TOML are unaffected, and the audit line says which a run
  was.

**Fixed in `ReblueActivity.java`**: a `--name` line followed by a value line is
joined into `--name=value` before it reaches argv, which CLI11 parses for flags
and options alike. `verify_quest.sh` now writes the profile TOML as well, and
prints the `[config]` lines and the logcat `args.txt added N argument(s)` line
instead of grepping the app log for a string that was only ever in logcat.

Confirmed on the next run: `args.txt added 14 argument(s)`,
`[config] all 14 settings in reblue.toml took effect`.

## The frame moved a tier

Same scene, same build, `bd_stereo=true`, and for the first time on a headset
`bd_reflections=false` and `bd_shadows=false` genuinely in effect:

| | 2026-08-31 (TOML, reflections/shadows at default true) | 2026-09-01 (all 14 in effect) |
| --- | --- | --- |
| `dt_ms` | 66.88 | **50.26** |
| `gpu_total_ms` | 39.67 | 38.22 |
| `[xr] xrWait` | 6-10 | 5-7 |
| `[xr] elsewhere` (CPU) | ~60 | **43-45** |
| draws | 577 | 505 |
| us/draw | 114.5 | 98.2 |

**20 fps, the 3-slot tier, from a setting.** The GPU barely moved; the CPU
did - the reflection pass re-renders the scene and the shadow pass renders it
again, and on the side-by-side path every one of those draws is submitted
twice. `bd_reflections` and `bd_shadows` are **CPU** levers here, which nobody
could have seen while the flag that turned them off was turning them on.

This does not say which of the two did it, and it is not a within-run A/B.
It is one run against the previous day's, which this repo does not accept as
a measurement under ~30% - but 66.9 -> 50.3 is a pacing-tier boundary, and the
`[xr]` line's CPU term fell by a quarter.

## `other_ms` cannot tell CPU from compositor wait

Yesterday's note concluded "both stereo paths are CPU-bound at 66ms" from
`other_ms`. The `[xr]` log line for the same runs disagrees:

| | `gpu_total` | `xrWait` | `elsewhere` |
| --- | --- | --- | --- |
| `bd_stereo` | 39.67 | 6-10 | ~60 |
| `bd_stereo_multiview` | 60.83 | 17-41 | 27-39 |

`other_ms` is `dt - (fence + submit + drain + pace)` and `xrWaitFrame` is not
subtracted, so a GPU-bound frame whose cost is absorbed by the compositor wait
reads as 66ms of "CPU" with a fence of zero. `bd_stereo` is CPU-bound;
`bd_stereo_multiview` is GPU-bound - and its GPU figure was taken with the
five-pass resolve chain still on by default, which is now off. Multiview's
~30ms CPU with a ~40ms GPU would be a 50ms frame: the same tier `bd_stereo`
reached today, with the CPU half free.

## The thread split, field scene, 150s

```
SDLThread        100%   guest main, pinned to the big cluster
Audio Worker      78%
Main Thread x5  55-70%  guest workers, efficiency cores
Draw Thread       28%
```

The guest main thread is the saturated one, and the audio worker at 78% of a
core is worth a look on its own.

## The profile said nothing, and now says what thread

`bd_sample_profiler`'s ring holds the last 65536 samples - 16 seconds across
the sampled threads - and dumped every 600 frames, so the file on disk
described whatever the run was doing at the last tick. The first profile:

```
libc.so          77.9%   (syscall 71%, nanosleep 18% of it)
libllvm-qgl.so   17.7%   the Adreno shader compiler
libreblue.so      0.4%
```

It now dumps once at `bd_capture_after_s` and stops, so it describes the same
moment as the capture. The second profile, pinned to 150s in a field scene,
read almost the same - 77% libc, 17.5% shader compiler, 1.1% libreblue - and
that is still not an answer, because a worker blocked in a futex and the main
thread spinning at 100% both sample as libc `syscall`, and the flat histogram
mixes seven threads.

So each sample now carries its thread id and link register. The dump adds
`# TID` lines and a `# SAMPLES2` section (`tid pc lr count`), and
`tools/symbolize_profile.py` prints a per-thread section: module shares, top
functions, and for every leaf outside `libreblue.so` the caller that reached it.
Other modules resolve when a copy sits in `out/device/` - `libc.so` from
`/apex/com.android.runtime/lib64/bionic/`, `libllvm-qgl.so` from
`/vendor/lib64/`.

The 17.5% in the shader compiler during a steady field scene is its own
finding: the driver is compiling in a frame that should have compiled
everything minutes ago. The log has three `CreateHostGraphicsPipeline failed:
backend pipeline null` lines and nothing caches a failure, so the same
pipelines may be re-attempted every frame - the Thor's 21,615-line failure at a
smaller scale. The per-thread profile will say which thread pays it.

## Multiview: one real bug fixed, and it was not the flatten

RenderDoc's count of 24 single-layer views over two-layer images pointed at
guest-texture creation, and the bug was there: `D3DDevice_CreateTexture_hook`
assigned `texture->layers` after `BindTextureSRV`, which rebuilds the view with
`arraySize = layers` and read 0. Hoisted, and `textureViewOf` set so the view
built there is kept. The obsolete resolve chain's companion surfaces and slice
views are no longer created (`bd_mv_resolve` and `bd_mv_redirect_srv` default
off), which removes the other 48 single-layer views.

Desktop, `bd_stereo_multiview=true, bd_mv_layered_textures=true`, all eight
settings in effect, 39 two-layer targets, present rt `layers=2`:

```
halves: mean abs diff 0.00, 0.0% of pixels differ
```

Unchanged. Three sessions have now verified every view, descriptor, view mask
and copy on this path and the pair still collapses somewhere between the scene
array (which measures correct stereo) and present. The next instrument reads
the *pixels* of every pass instead: `tools/rdc_layer_diff.py` replays a capture
under RenderDoc's interpreter and reads both array layers of every two-layer
target back after every pass, printing which passes hold a pair and which hold
one layer twice. The first flattened target after a differing source is the
pass at fault, whatever its bindings say.

## Per-thread profile: the render thread is a driver memcpy

With thread ids and link registers on every sample, the field-scene profile
reads (`out/device/profile_20260901.txt`):

| thread | samples in | what it is |
| --- | --- | --- |
| `Draw Thread` (the guest's render thread) | **56% one `memcpy` called from `vulkan.adreno.so`**, 16% futex, ~17% our draw code plus recompiled guest | the critical path |
| `Main Thread` x5 at 55-75% of a core each | **75% `libllvm-qgl.so`** | the Adreno shader compiler, still compiling in a steady field scene |
| `SDLThread` at 100% of a big core | `SDL_PumpEvents`, `ANDROID_JoystickDetect`, `clock_gettime`, mutex churn | SDL's event loop spinning; not guest code |
| the present thread (`Main Thread`, registered with XR) | 85% futex wait, 9% `nanosleep` under the VR runtime | waiting on the render thread |
| every `XThread` and the other `SDLThread`s | 100% futex | blocked |

## The memcpy is the driver copying the descriptor set, and it scales with the set

The per-draw work on the render thread is one `vkCmdBindDescriptorSets` with
three dynamic offsets - the guest constants, re-based per draw - on the set
that also holds the bindless texture array. If the driver copies the set to
patch the offsets, the copy is the size of the array. The probe: shrink the
Android array from 4096 to 1024 entries, change nothing else.

| | 4096 entries | 1024 entries |
| --- | --- | --- |
| render thread samples in the driver memcpy | **56%** | **6%** |
| `[xr] elsewhere` (CPU) | 43-45 ms | **19 ms** |
| `[xr] xrWait` | 5-7 ms | 30 ms |
| `gpu_total_ms` | 38.2 | 36.4 |
| `dt_ms` | 50.3 | 50.1 |

The copy scaled with the array, the CPU term fell by 25 ms, and the frame did
not move because it is now **GPU-bound at 36.4 ms** - 3 ms above the 33.3 ms
boundary of the 30 fps tier. No `Bindless texture heap full` in the run, so
1024 slots held a field scene, but a probe is not a fix.

**The fix: move the three dynamic constant ranges out of the texture set and
into the sampler set** (256 entries), which becomes the set re-based per draw.
Adreno allows four bound sets and all four are taken - spaces 0/1/2 are one
physical texture set bound three times, space 3 the samplers - so the constants
cannot have a set of their own. Shader side (`shader_common.h` in the
XenosRecomp fork, and every host shader that names the heaps): cbuffers at
`vk::binding(0..2, 3)`, texture heaps at `binding(0, N)`, samplers at
`binding(3, 3)`. Host side: the sampler set builder leads with the three
dynamic ranges, `SamplerDescriptor(slot)` applies the shift where
`TextureDescriptor()` used to, and the per-draw dynamic bind moves to set 3 in
`draw.cpp`, `draw_queue.cpp` and `frame_ring.cpp`; imgui mirrors the layout.
Needs the shader cache regenerated - the two manual XenosRecomp steps.

## Multiview on the Quest with the resolve chain off: 277 ms

`bd_stereo=false, bd_stereo_multiview=true`, first run since `bd_mv_resolve`
defaulted off: `gpu_total 277ms`, `fence 313ms`, 2.7 fps. The per-target
census puts **75.6 ms/frame on the 1280x720 two-layer scene depth target at 60
draws** - the scene pass alone is 7x what it measured yesterday with the
resolve on (60.8 ms for the whole frame). Not investigated. The likeliest
difference is that the resolve pass used to issue the layout barriers
(`SHADER_READ` on the array, `COLOR_WRITE` back) around every layered target,
and without it the post chain samples layered images that are still in the
colour-attachment layout. Nothing on the desktop shows this; RenderDoc's
Android build or the validation layer on device is the instrument.

## The move, measured: CPU 44 -> 13 ms, and stretches at 30 fps

Desktop first: the side-by-side frame renders with correct textures and
samplers and `stereo_check --raw` reads `far +4, near -7, OK`. Then the Quest,
same `verify_quest.sh` defaults, bindless array back at 4096:

| | before (morning) | 1024 probe | constants in the sampler set |
| --- | --- | --- | --- |
| render thread samples in the driver memcpy | 56% | 6% | **1.3%** |
| `[xr] elsewhere` (CPU) | 43-45 ms | 19 ms | **12-14 ms** |
| `gpu_total_ms` (field mean) | 38.2 | 36.4 | 39.2 |
| `dt_ms` (field mean) | 50.3 | 50.1 | 50.0 |
| `[xr]` tail of the run | 50.3 ms | 50.1 ms | **33.3 ms, 30 fps** |

The frame is GPU-bound now. The field-scene mean sits at the 50 ms tier
because the GPU averages 39 ms; the lighter stretches at the end of the run
come in under 33.3 ms and the compositor gives them 30 fps. The CPU has
roughly 20 ms of headroom under the GPU, which is what the multiview path and
instancing were always going to need.

Two `CreateHostGraphicsPipeline failed: backend pipeline null` lines in the
log, and the pipeline counter (now printing every 200th creation) reads:

```
13:57:18  1001 mono pipelines so far
13:57:38  1201
13:58:01  1401
13:58:22  1601
```

**Ten pipelines a second, in a field scene, minutes after load.** That is the
five driver threads at 60% each. Something in the pipeline cache key varies
continuously; it is the next thing to find, and it is probably why the GPU
time jitters across the 33.3 ms boundary.

## The five compiler threads are our PSO precache, and they cost the frame nothing

The threads at 55-75% each are `pso_precache` workers - `hardware_concurrency
- 3` of them, demoted to background priority, which is why `top` shows them at
nice 10 - and the ~10 pipelines a second are `pso_predictor` expanding every
shader technique into cores x cull modes x spec constants x skinning. The
desktop compiles the same 5,000+ in twenty seconds; Adreno takes minutes.

`bd_pso_precache=false`, same run otherwise: `dt 50.08 | gpu_total 39.09 |
elsewhere 16.5`, no compiler threads, and 41 pipelines compiled lazily over the
whole run. Identical frame. So the precache neither helps nor hurts a 170 s
run on this device; it stays on by default, and the thread policy's "13 guest
workers" count is mostly these. Not a lever.

## Validation on the Quest: one layout violation, no per-frame errors, no frame

`tools/validate_quest.sh` with multiview on reported exactly one thing, twice
(once per pipeline layout - the renderer's and imgui's):

```
VUID-VkDescriptorSetLayoutCreateInfo-descriptorType-03001
vkCreateDescriptorSetLayout(): pBindings[3] has VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT flag, but ...
```

03001 says a set layout with any update-after-bind binding may not also hold a
dynamic uniform buffer. The sampler set now does (constants at 0-2, the
update-after-bind sampler array at 3) - and the texture set did before today,
for the same reason, so this is not new; it is the shape Adreno's four-set
limit forced and the driver has accepted throughout. It is still a spec
violation and the honest fix is a set without update-after-bind for the
constants, which needs a free slot: collapsing spaces 0/1/2 into one physical
set with three bindings, the same work the sun-occlusion set has been waiting
on.

The run itself never printed an `[xr]` frame line or a `[perf]` census: under
the layer the app did not reach a field scene in 160 s, so validation says
nothing yet about the 277 ms multiview frame. The next probe is cheaper:
multiview with `bd_mv_resolve=true` again on today's build, which restores the
barriers the resolve issued. If the GPU returns to ~60 ms the regression is the
resolve's side effects.

## The 277 ms is the resolve chain's absence, not the array heap's presence

Multiview with `bd_mv_resolve=true, bd_mv_redirect_srv=true` on today's build
(constants in the sampler set): `dt 66.8 | gpu_total 59.3 | elsewhere 13.5`,
the same 59-60 ms GPU as yesterday, 15 fps. So the chain's side effects are
load-bearing on Adreno, and multiview's CPU is now 13.5 ms - the same as
side-by-side's, because the descriptor copy was the cost of a second
submission and it is gone.

Two things the resolve does that the array-heap path does not: it issues
`SHADER_READ` / `COLOR_WRITE` barriers around every layered target, and it
points every downstream read at a single-layer companion instead of the
two-layer array. A run with `bd_mv_resolve=true, bd_mv_redirect_srv=false`
keeps the barriers and samples the array, and separates them.

## Separated: it is not the array sampling

`bd_mv_resolve=true, bd_mv_redirect_srv=false` - the resolve pass runs and
issues its barriers, but nothing ever samples the companion; the post chain
reads the two-layer array directly through the array heap:

| | `gpu_total` | `dt` | `elsewhere` |
| --- | --- | --- | --- |
| resolve off | 277 | 277 | 38-40 |
| resolve on, companion sampled | 59.3 | 66.8 | 13.5 |
| **resolve on, array sampled** | **59.2** | 66.8 | 14.5 |

Sampling the array costs nothing. Whatever the resolve pass does *besides*
providing a companion - the `SHADER_READ` / `COLOR_WRITE` barriers around each
layered target, or simply ending the guest's render pass and forcing a
framebuffer rebind - is what Adreno needs, and without it every pass in the
frame runs ~4.5x slower, which looks like the GPU leaving its tiled path
altogether. `bd_mv_resolve` defaults back to **true** for now: the five extra
passes cost less than what their absence costs. Naming the mechanism needs a
GPU-side view (ovrgpuprofiler or RenderDoc's Meta fork), or a probe that
issues the resolve's barriers without its draws.
