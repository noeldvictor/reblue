# Research: the frame is fill-bound, and the "draw-bound" conclusion was wrong

Date: 2026-08-29 02:30
Topic: isolating fragment cost from draw cost on a Quest 2, and correcting
`20260828_2330_the-real-bottleneck.md`.

---

## The correction

The earlier note concluded the frame was **draw-bound**, on the strength of `bd_debug_max_draws`:
capping to 500 draws took the fence from 112.8ms to 0.1ms. That inference does not hold. **Removing
a draw also removes its fragments**, so the experiment moves two variables at once and cannot
distinguish "too many draws" from "too many pixels".

Every "resolution does not matter" measurement was also void, for a different reason - see below.

## The experiment that separates them

`bd_debug_fill_scale` shrinks the **scissor** to N percent of the viewport in each axis, in
`Video::FlushViewport`. The viewport is deliberately left alone, so vertex work, draw count,
pipeline state, binning input and every buffer upload are bit-identical; only the number of
fragments that survive clipping changes.

A field scene, VR on, MSAA off:

| `bd_debug_fill_scale` | fragments | fence | frame | draws | verts |
| --- | --- | --- | --- | --- | --- |
| 100 | 1.0x | **141ms** | 210ms | 2812 | 372K |
| 50 | 0.25x | **17ms** | 98ms | 2902 | 412K |
| 25 | 0.0625x | **0.1ms** | 71ms | 3095 | 450K |

**The GPU goes from 141ms to nothing while the draw count goes up.** 3095 draws and 450,000 vertices
cost the GPU 0.1ms. The frame is fill-bound, completely, and draws and geometry are free at this
scale.

The fall is *super*-linear (0.25x the fragments gives ~0.12x the time), which is what a tiler does:
with a smaller scissor whole tiles contain no primitives and are skipped, so binning and shading
both vanish rather than just shading.

## Why every resolution measurement was void

`bd_max_render_height` did nothing because it was resizing the wrong surface. A per-target draw
census (`NoteDrawTarget`, now permanent in the `[perf]` line) at `bd_max_render_height=360`:

```
target 1280x720: 2434 draws/frame     <- the scene. unchanged by the cvar
target 688x360:    22 draws/frame     <- the output fit. this is what the cvar moved
target 320x180:   167 draws/frame
```

The scene renders into a **1280x720 surface pinned to the design canvas**
(`kDesignCanvasWidth/Height`, `src/gpu/output.h`), created by the guest through
`D3DDevice_CreateSurface` and passed straight through by
`D3DDevice_CreateSurface_hook` (`src/gpu/hooks/resource.cpp:47`). Lowering the render height moved a
surface taking 22 draws and left the one taking 2434 alone.

`bdOutputResViewScaleHook` only ever scaled *up* (`if (s > 1.0)`), so the design canvas was a floor.
Removing that guard was tried and is **inert** - the scene surface does not come through that hook.
Reverted. The scaling seam is `D3DDevice_CreateSurface_hook`, not the view-scale hook.

## What this means

Two independent problems, now cleanly separated:

| | cost | status |
| --- | --- | --- |
| **GPU, fragments** | ~141ms | fill-bound, proven. Fixable by rendering the scene smaller. |
| **CPU floor** | **~62ms** | unmoved by every GPU experiment. ~46ms guest sim + ~14ms draw recording. |

At `fill_scale=25` the GPU is idle and the frame is still 71ms, so **the CPU alone caps the port at
about 14 fps**. Fixing fill entirely gets a field scene to roughly 10-14 fps, not to 72.

Both have to be solved. Fill is the larger and the better understood of the two, and it now has a
proven lever.

## The overdraw number

GPU counters put a field scene at 94-98% of shader time in fragment shading and ~1 billion
fragments/second, so ~167 million fragments per frame against a 1280x720 target. That is **~181x
overdraw**, where 2-4x is normal. The per-target census says 2375-2434 of ~2850 draws go to that one
surface, averaging ~7.6% screen coverage each.

Halving the scene resolution divides that by four and is worth doing, but 45x overdraw would still
be pathological. **Why 2400 draws each cover a twelfth of the screen is the next question**, and it
is likely worth more than the resolution scaling.

## Diagnostics left behind

- `bd_debug_fill_scale` - scissor-only fill isolation. Permanent, defaults to 100 (off).
- `NoteDrawTarget` - per-target draw census in the `[perf]` line. Permanent.
- `bd_debug_max_draws`, `bd_debug_max_pso` - kept, but read the caveat above: neither isolates a
  single variable.

---

## Follow-up, same session: two hypotheses tested without a device

The Quest dropped off USB partway through, so both of these were settled from the emitted shader
dump and the shader cache instead. Recording them because both looked strong and both were wrong.

### "Pixel shaders do 143-158 global loads per fragment"

Counting `vk::RawBufferLoad` in the dumped HLSL bodies gives 143-158 per pixel shader invocation
against 91 for a vertex shader - the constants are `#define`s, so every textual use re-expands into
a load, and `ShadowTexture_Texture2DDescriptorIndex` appears 13 times (once per PCF tap) with
`g_ShadowPcfScale` 10 times. Unlike the vertex shaders, **pixel shaders use no `a0`-indexed form at
all** - 0 indexed macros, 116 scalar macro uses from 75 distinct constants - so the "hoist to a
`static const`" fix that provably does nothing for vertex shaders should have been a pure win here.

Implemented in the fork, in `shader_common.h` (the shared scalars) and `shader_recompiler.cpp` (the
single-register float4s and the sampler descriptor indices), leaving the parameterised forms as
macros. Rebuilt the shader cache and compared:

```
before: 2,740,507 bytes
after : 2,746,387 bytes   (+0.2%)
```

**DXC already common-subexpression-eliminates them.** The compiled output is unchanged; the loads
never existed outside the HLSL text. Reverted.

This is a cheap and reusable technique: a codegen change that should shrink the shader can be
accepted or rejected by rebuilding `shader_cache.cpp` and comparing its size, in about a minute,
with no device and no deploy.

### "The pixel shaders are 1000-line ubershaders"

`bd_normal_ps` has a 986-line body, `bd_mirror_ps` 1179, against 11 texture ops each. But the bodies
are properly branched - 40 `if`s on uniform booleans (`g_bNMap`, `g_bShadowMap`, `g_bTexture0`),
only 5 `select`s and no loops - so the paths are not flattened, and much of the line count is the
shared `tfetch2D`/`tfetch3D` helper library that every shader includes, including an unrolled
bicubic filter that costs four samples when used.

So neither the constant loads nor a flattened ubershader explains the fragment cost. **What makes a
fragment expensive here is still unknown** and needs a GPU profiler on the device, not another read
of the source.

## Status of the fix

`bd_render_scale` (25-100, default 100) scales scene surfaces at
`D3DDevice_CreateSurface_hook`, for requests at or above the design canvas only, and converts the
guest's own viewports by the bound target's actual/requested ratio in `D3DDevice_SetViewport_hook`
(`GuestTexture::requestedWidth/Height` carries it). The resolve path already handles a scaled
resolve - `CopySurfaceToTextureLocked` has a comment describing a 672x720 scene resolving to
1280x720 - so the downstream is in place.

**It compiles and has never been run.** The device left before it could be deployed. Expect
`bd_render_scale=50` to quarter the fragment cost and take the fence from ~141ms to roughly 17ms by
analogy with the scissor sweep, giving a ~98ms frame; the ~62ms CPU floor is untouched by it. The
things to check on the first run are the post-process chain, which samples the scene surface and
derives texel offsets from dimensions the guest still believes are 1280x720, and anything that reads
`bdSetViewportConstants`' (1/W, 1/H) in VS/PS c21.
