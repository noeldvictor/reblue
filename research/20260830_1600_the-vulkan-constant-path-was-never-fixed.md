# The Vulkan constant path was never fixed, and the desktop dump hid it

2026-08-30, on a Quest 2 (Adreno 650) and a desktop RTX 3060.

## The claim being corrected

CLAUDE.md says `python tools/spv_caps.py <hlsl_dump>` "reads `Int64 141 / 141`" and that
`--require-absent Int64` is the gate on the constant-heap rewrite. Run against the
**desktop** dump today it reads `Int64 0 / 141`, which looks exactly like the rewrite
having landed. It has not landed. The two dumps disagree:

| dump | Int64 | PhysicalStorageBufferAddresses |
| --- | --- | --- |
| `out/build/win-amd64-release/hlsl_dump` | **0 / 141** | 0 / 141 |
| `out/build/android-arm64-release/hlsl_dump` | **141 / 141** | 141 / 141 |

Same recompiler binary, same source, both rebuilt from clean. The difference is not
version skew - it is the backend.

## Why

`shader_recompiler.cpp` emits *both* constant paths into every shader, selected by a
preprocessor conditional in the generated HLSL:

```
:1291   out += "#ifdef __spirv__\n\n";
:1345       #define cN vk::RawBufferLoad<float4>(g_PushConstants.PixelShaderConstants + off, 0x10)
:1400   out += "\n#else\n\n";
:1402       cbuffer PixelShaderConstants : register(b1, space4)
:1506   out += "#ifndef __spirv__\n";
```

So **DXIL gets a uniform buffer and SPIR-V gets a raw 64-bit-address global load.**
The desktop `hlsl_dump` on this machine is the D3D12 variant, so it takes the `#else`
branch, declares no `Int64`, and reports a clean gate. Every Vulkan target - Quest,
AYN Thor, Linux, macOS - takes the `#ifdef` branch.

**`tools/spv_caps.py` is not wrong; it was pointed at the wrong dump.** Always name the
preset when quoting it, and prefer the android one, which is the target that cannot
compile `Int64` at all.

## What it costs, measured

Quest 2, field scene, 804 frames selected by draw count, `bd_stereo_multiview=true`:

```
dt_ms 158.74 | gpu_total_ms 155.51 | gpu_draw_ms 152.94 | gpu_resolve_ms 1.33
fence_ms 131.16 | other_ms 27.03 | draws 522.87 | pso_switches 114.18
```

The GPU is now **96% of the frame** and the CPU spends 131ms of it waiting. This inverts
every earlier note in this repo: the port was CPU-bound, the CPU work landed, and the GPU
is now the wall.

Two independent estimates of the same anomaly:

- **Per pixel against desktop.** The per-target census totals **190.4 Mpix/frame**.
  190.4 Mpix in 152.94ms is **1.24 Gpix/s**. An Adreno 650 does 4-8 Gpix/s for a cheap
  textured fragment. We are ~5x under the hardware floor.
- **Against the desktop build of the same scene.** Desktop `gpu_draw 4.54ms` at 1920x1080
  against the Quest's 152.94ms at 1376x720x2 layers - 34x the time for 2.25x fewer pixels,
  so ~75x per pixel. An RTX 3060 is maybe 10-15x an Adreno 650 in raw fill, leaving a
  **5-7x anomaly specific to Adreno**.

Both land on 5-7x, from different data. The one documented Adreno-specific per-fragment
cost in this port is the constant path: `research/20260829_0030_shader-constants-are-global-loads.md`
measured 86-89 raw loads per vertex shader and 143-158 per pixel shader, and noted that a
buffer-device-address load is ordinary uncached global memory on Adreno where a UBO read
goes through the constant path and is hoisted at wave launch. On an RTX 3060 those loads
coalesce through a large L2, which is why the desktop never showed it.

## The scissor test proved less than it was read as proving

`bd_debug_fill_scale=25` takes the frame from 155.7ms to 26.5ms at an unchanged draw
count, and that was written up here as "the frame is fill-bound". It is not evidence for
fragment *count*: shrinking the scissor removes fragments, and if each fragment carries
~150 global loads then a fragment-count experiment and a per-fragment-cost experiment
produce identical curves. **A test that varies one quantity cannot separate two factors
that are multiplied together.** The correct reading is "the cost is proportional to
fragments", which is true of both hypotheses; the Gpix/s figure is what discriminates.

## Outcome: built, measured, 2.4x

Both halves landed the same day.

| step | `dt_ms` | `gpu_total_ms` | fps |
| --- | --- | --- | --- |
| before | 158.74 | 155.51 | 6.3 |
| chunks bound as ByteAddressBuffer | 116.43 | 100.61 | 8.5 |
| three dynamic uniform buffers | **66.82** | **56.40** | **15.0** |

The two steps are different mechanisms and both were needed:

- **Binding the chunk** removed the 64-bit device address and with it `OpCapability Int64`. The read
  was still a *storage* buffer load - global memory - but the address became wave-uniform, so the
  driver could issue a scalar load. Worth 35%.
- **Making it a uniform buffer** put the read on Adreno's constant path proper. Worth a further
  1.75x. `g_VSC[256]`, `g_PSC[224]`, `g_SHC[22]`, indexed by register rather than byte offset, so
  the address is a literal.

Verified: all 141 modules declare `Uniform` storage class, none declares `StorageBuffer` or `Int64`;
stereo depth unchanged and still crossed; two pipeline failures, the known sun-occlusion ones.

**What made it tractable was that most of it already existed.** `g_ConstantChunks[8]` was declared,
eight descriptors were reserved ahead of the bindless texture array, and `TextureDescriptor()`
already applied the index shift. Only the push-constant payload and the `#define`s still pointed at
a device address.

**What had to be built:** `plume` had no dynamic uniform buffers - `CONSTANT_BUFFER_DYNAMIC` and
`setGraphicsDescriptorSetDynamic` are new. And the per-slot chunk lists became one 64 MiB buffer
with a fixed 32 MiB span per frame slot, because the descriptors must be written once: rewriting one
while a submitted frame may still be reading it is a hazard no validation layer reliably catches.

**A latent bug fell out of it.** The ImGui overlay declared its texture array at binding 0 while
binding the *shared* texture set, whose textures have sat behind a leading range ever since the
constant chunks were reserved. Its layout now mirrors the real one.

## The rewrite, and the good news

The replacement is already written. The `#else` branch is a complete, working `cbuffer`
declaration - `register(b0, space4)` for vertex, `b1` for pixel, `b2` for shared - with
packoffset slots and the alias-winner logic reblue already added. Making `__spirv__` take
it is a recompiler change plus a host binding change, not a design.

What has to be solved on the host side, from the earlier note and still true:

- The constants change every draw, which is why a push-constant device address was chosen -
  it needs no descriptor update. A UBO needs **dynamic offsets** (plume does not expose
  them) or a per-draw descriptor write.
- **There is no spare descriptor set.** Adreno reports `maxBoundDescriptorSets = 4` and
  sets 0-3 are taken, so the buffer has to become a *binding* on an existing set, with DXC
  register-shift flags to dodge `t0` and the variable-count array kept last.

Collapsing HLSL spaces 0/1/2 - which are one physical descriptor set bound three times -
frees a slot and is the same prerequisite as the dropped sun-occlusion set.

## The other X360 pattern the same census exposed

`[perf] targets: 64 rows, 264 draws/frame attributed of 459 counted, 190.4 Mpix/frame`

Sixteen distinct `1376x720x2L` render targets are live. Four are the scene, alternating at
0.25 binds/frame each; the other twelve take 0.25-0.50 draws/frame. Below them sit seven
`320x180`, four `172x90`, four `160x90`, six `86x45` and eight `80x45`, each touched about
a quarter of a frame. These are not distinct passes - they are **pooled aliases of a
handful of logical surfaces**, cycling because the guest requests a fresh surface per pass
instead of reusing one. At 7.9 MB per full-resolution two-layer target that is ~127 MB of
render targets for a game with a 4 MB framebuffer.

That is EDRAM semantics transplanted literally: on a Xenon you took a 10 MB tile, resolved
it to memory, and took another, and the allocation was free. On a tiler with real memory it
is bandwidth, allocation churn, and a barrier every time the pool hands out a different
image.

## Sources

- `research/20260829_0030_shader-constants-are-global-loads.md` - the original measurement
- Qualcomm, *Adreno GPU developer guide*, on uniform vs. global load paths
- `thirdparty/XenosRecomp/XenosRecomp/shader_recompiler.cpp:1291,1340-1345,1400-1402,1506`
