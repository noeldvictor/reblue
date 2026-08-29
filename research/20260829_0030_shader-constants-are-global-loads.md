# Research: guest shader constants are global memory loads, not uniforms

Date: 2026-08-29 00:30
Topic: why a 140-vertex draw costs ~32us of GPU on an Adreno 650.

This is the answer to "why is the port slow", after a session of eliminating everything it is not.
It is a structural property of the shader translation, not of Blue Dragon, and not of VR.

---

## The evidence trail

Measured on device, each with no rebuild:

| Observation | Conclusion |
| --- | --- |
| 720p and 360p both cost ~110ms on the fence | **not fill-bound** - eliminates every quality setting, and foveation |
| VR off costs the same as VR on | **not VR** - the port is slow flat |
| Capping draws to 500: fence 112.8ms -> 0.1ms | **draw-bound** |
| 2848 draws, 398,959 verts, **140 verts/draw** | **not geometry** - 400K verts is nothing for this GPU |
| mutex 0.5ms, bindFB 1.8ms, flushState 11.8ms per frame | **not the renderer's CPU path** |
| Capping PSO switches to 20: fence 90ms -> **235ms** | **not pipeline switching** - and the cost is *shader-dependent* |

That last row is the one that closed it. Forcing draws to reuse the wrong pipeline made the frame
nearly three times worse, so GPU cost depends strongly on **which shader runs**. But halving the
resolution changes nothing. The only stage that is both shader-dependent and resolution-independent
is the **vertex stage**.

400K vertices costing ~90ms is **~225 nanoseconds per vertex**. A vertex shader should be single-digit
nanoseconds.

## The cause

`thirdparty/XenosRecomp/XenosRecomp/shader_recompiler.cpp:1345` emits every guest constant register
as a macro:

```cpp
println("#define {} vk::RawBufferLoad<float4>(g_PushConstants.{}ShaderConstants + {}, 0x10)", ...)
```

and the indexed form, line 1340, which is what skinning uses:

```cpp
println("#define {}(INDEX) select((INDEX) < {}, "
        "vk::RawBufferLoad<float4>(g_PushConstants.{}ShaderConstants + ({} + min(INDEX, {})) * 16, 0x10), 0.0)", ...)
```

`VertexShaderConstants` is a `uint64_t` **device address in a push constant**
(`shader_common.h:37`). So every `c[n]` read in a translated shader is a **raw load from global
memory**, executed per invocation, with the indexed case adding a compare and a `min` on top.

On the Xenos these were constant registers - the fastest read a shader had. On desktop the change is
survivable, because the constant cache absorbs it and desktop GPUs have bandwidth to spare. On an
Adreno 650 it is not: a uniform buffer read goes through the constant path and is typically hoisted
into registers at wave launch, while a buffer-device-address load is an ordinary global memory
access that happens for every vertex.

A skinned character vertex shader reading four bone matrices (16 float4) plus a world-view-projection
(4) plus lighting is 20-40 global loads **per vertex**. At 400,000 vertices a frame that is 8-16
million uncached loads.

The same applies to the shared constants - `g_SwappedTexcoords`, `g_Booleans(i)`,
`g_HalfPixelOffset` and the rest are all `RawBufferLoad` macros, so every textual use is another
load.

## Why this is the right thing to fix

It explains every measurement above at once: shader-dependent (different shaders read different
numbers of constants), resolution-independent (vertex stage), per-draw rather than per-pixel, and
invisible to every quality setting.

It is also not a Blue Dragon problem. Any game translated this way pays it; Blue Dragon just draws
2848 times a frame, which was ordinary on hardware with real constant registers.

## Quantified, from the emitted HLSL

`cmake --build ... --target reblue_shader_hlsl_dump` writes all 141 translated shaders as readable
HLSL. Counting raw loads per vertex shader:

```
89  bd_normal_vs_grassland      86  bd_toon_vs_env
87  bd_normal_vs_wind           86  bd_caustics_vs
86  bd_water_vs                 86  bd_fur_vs_env
```

**86-89 raw global loads per vertex shader**, and at ~400,000 vertices a frame that is upward of 34
million uncached loads. Against ~90ms of GPU time that is ~2.6ns a load, which is the right order
for uncached global memory on this part. The arithmetic closes.

And in the shaders that matter they are all the **indexed** form:

```hlsl
#define g_mWorld(INDEX)    select((INDEX) < 236, vk::RawBufferLoad<float4>(... + (20 + min(INDEX, 235)) * 16, 0x10), 0.0)
#define g_mViewProj(INDEX) select((INDEX) < 224, vk::RawBufferLoad<float4>(... + (32 + min(INDEX, 223)) * 16, 0x10), 0.0)
#define exMatrix(INDEX)    select((INDEX) < 196, vk::RawBufferLoad<float4>(... + (60 + min(INDEX, 195)) * 16, 0x10), 0.0)
```

Matrices, read a row at a time - four loads plus four `select`/`min` pairs per matrix, per vertex.

### A cheaper fix that does not work

The scalar constants are `#define`s, so every textual use re-loads, and `c255` appears 14 times in
`bd_toon_vs_env` with `c0` and `c1` eight times each. Emitting them as `static const float4` instead
- a two-line change in `shader_recompiler.cpp` - hoists the load to shader entry.

**It changes nothing in the shaders that matter**, because those use the indexed form exclusively,
and an indexed load cannot be hoisted that way. Tried, measured against the dumped HLSL, reverted.
Recorded so it is not tried again.

## The precise mechanism: a0-indexed reads are divergent global gathers

The call sites in `bd_toon_vs_env` settle it:

```
7x  exMatrix(0 + a0)      3x  g_mWorld(0)
7x  exMatrix(1 + a0)      3x  g_mWorld(1)
7x  exMatrix(2 + a0)      3x  g_mWorld(2)
3x  exMatrix(3 + a0)      1x  g_mWorld(3)
```

`a0` is the Xenos **address register** - dynamic constant indexing, which here is **bone matrix
lookup for skinning**. Twenty-four of the 86 loads in this shader are `a0`-relative.

Those are far worse than the static ones, for two compounding reasons:

1. **The bounds check cannot fold.** `select((INDEX) < 196, ..., min(INDEX, 195) ...)` with a runtime
   index is a real compare, min and select on every access - ALU a literal index would have removed
   at compile time.
2. **The address is divergent across the wave.** Different vertices skin to different bones, so the
   lanes read different addresses, the driver cannot issue a scalar load, and it must gather
   per-lane from uncached global memory. That is the worst case this hardware has.

On a Xenos these hit the constant file and were close to free - the register file was built for
exactly this access pattern. Turning them into per-lane global gathers is what makes a 140-vertex
draw cost 32us, and it explains the shader-dependence and the resolution-independence at once.

## The fix

Bind the per-draw constant blocks as a **uniform buffer** and index it, instead of pushing a device
address and doing raw loads. Two coordinated changes:

1. **XenosRecomp** (`shader_recompiler.cpp` around lines 1340-1395, and `shader_common.h`) - emit
   `cbuffer`/`ConstantBuffer<>` accesses rather than `vk::RawBufferLoad`. A dynamically indexed UBO
   read still goes through the constant cache on Adreno, which is the entire point. The fork is
   `noeldvictor/XenosRecomp`, already repointed.
2. **The renderer** (`src/gpu/constant_buffers.cpp`, `src/gpu/draw.cpp`) - bind the upload
   allocations as a UBO descriptor rather than passing `gpuAddress` through
   `setGraphicsPushConstants`.

**Scope the descriptor problem before starting.** The constants change every draw, which is exactly
why a device address in a push constant was chosen - it needs no descriptor update. A UBO needs
either **dynamic offsets** (one descriptor, offset supplied per draw at bind time; plume does not
currently expose this) or a descriptor write per draw, which would be worse than what is there now.
Adding dynamic-offset support to the forked plume is likely the real prerequisite.

There is also no free descriptor set: Adreno's `maxBoundDescriptorSets` is 4 and sets 0-3 are taken.
Sets 0/1/2 are one physical set bound three times to satisfy three HLSL register spaces, so the UBO
can be added as a *binding* on that shared set - with DXC `-fvk-b-shift` / `-fvk-t-shift` to avoid
colliding with `t0`, and the variable-count texture array kept as the last binding.

Both the D3D12 and Vulkan paths already declare the same constants through
`DEFINE_SHARED_CONSTANTS()` for the non-SPIR-V case, so there is a precedent in the file for the
cbuffer form.

Expect this to need the shader cache rebuilt, and expect the win to be large but not total - the
frame still has ~46ms of guest simulation behind it.

## Diagnostics left behind

Both permanent, both off by default, both reached from `args.txt` with no rebuild:

- `bd_debug_max_draws` - stop submitting after N draws a frame. Proved draw-bound.
- `bd_debug_max_pso` - stop switching pipelines after N a frame. Proved *not* pipeline-bound, and
  its surprising direction is what identified the vertex stage.

Plus permanent per-frame reporting of draws, vertices, vertices per draw, and the three CPU phases
of `DispatchDraw`, in the `[perf]` log line.
