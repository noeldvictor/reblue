# The AYN Thor renders nothing, and validation named it in one line

2026-08-30. First ARM64 run of this port's current code. The device was attached for the whole
session while I repeated "nothing is measured on ARM64" - it is an **AYN Thor**, not a Quest, and
the fork's own goal list says "an AYN Thor first, then Quest 2 natively". I was filtering for the
wrong device.

## What happens

The app installs, starts, loads the XEX, mounts the VFS, runs autoplay and writes a capture. It
renders **nothing**. The capture is a single uniform colour - `(102, 204, 255)` in every pixel, the
clear colour - and the log is 21,615 lines of one message:

```
CreateHostGraphicsPipeline(pipeline) failed: backend pipeline null
```

| | Thor (SD 8 Gen 2, Adreno 740, Android 13) |
| --- | --- |
| `dt_ms` | 370 (2.7 fps) |
| `gpu_total_ms` | 1.94 - idle, because nothing draws |
| `other_ms` | 365 |
| draws/frame | 216-249 |
| pipeline failures | 21,615 in ~250 frames, ~86 a frame |

**Do not read the 365ms as guest CPU.** ~86 failed `vkCreateGraphicsPipelines` calls a frame is its
own cost, and it is being retried for ever because nothing caches a failure.

**Not multiview.** Re-run with `bd_stereo_multiview=false`: 22,180 failures. Nothing from
2026-08-30's stereo work is involved.

## What it is

`tools/validate_quest.sh` with `SERIAL=` pointed at the Thor. One run, one line:

```
VUID-VkGraphicsPipelineCreateInfo-Input-08733
vkCreateGraphicsPipelines(): pCreateInfos[0].pVertexInputState
  ->pVertexAttributeDescriptions[3].format (VK_FORMAT_R16G16B16A16_UINT) at Location 7
  does not match [VK_SHADER_STAGE_VERTEX_BIT] [Input variable, Location 7]
  type of (vec4 of float32).
```

Same at Location 9. The vertex buffer declares an **integer-class** format against a shader input
declared **float4**. The spec requires the numeric types to match. The Adreno 650 driver on Quest 2
tolerates it; the Adreno 740's does not.

**The codebase already knows this rule, and one site breaks it.** `gpu/format.cpp:285`:

> SHORT4 (non-normalized) binds as SNORM, not SINT: the recompiled BD shaders declare every SHORT4
> input as float4, and an integer class IA format against a float class shader input is a D3D12
> contract violation.

`ConvertDeclType(kShort4)` duly returns `R16G16B16A16_SNORM`. Then
`gpu/vertex_declaration.cpp:199-208` overrides it back to `*_UINT` for `kTexCoord`, and sets
`sintTexcoords` so the shader sign-extends - reintroducing exactly the violation that comment warns
about. The override is guarded `if (!g_mvk)`, and the MoltenVK comment beside it describes the
correct behaviour: *"Metal already converts raw signed 16-bit to float, so MoltenVK needs no
shader-side recovery."*

## The fix, written and measured

**SSCALED was the obvious answer and Adreno does not have it.** Float-class and unnormalised is
exactly what a SHORT texcoord wants, so the first attempt added `R16G16_SSCALED` /
`R16G16B16A16_SSCALED` to plume and bound those. The Thor traded one violation for another:

```
VUID-VkVertexInputAttributeDescription-format-00623
```

- the format is not allowed as a vertex buffer format on that device at all. Reverted; recorded so
nobody spends the same hour.

**SNORM plus a multiply is what works.** `ConvertDeclType(kShort4)` already returns
`R16G16B16A16_SNORM`; the override to `*_UINT` is removed, and `sintTexcoord` in
`XenosRecomp/shader_common.h` changes from sign-extending raw bits to `value * 32767.0f`. Exact for
every value the driver can deliver - `32767 * (v / 32767)` round-trips inside a float32 mantissa -
and only -32768 is lost, clamping to -32767.

Measured:

| | before | after |
| --- | --- | --- |
| pipeline failures (70s run) | 21,615 | **5,449** |
| pipeline-creation validation errors | 10+ per run | **none** |
| Thor renders | no | **still no** |
| desktop stereo verdict | far -4, near -26, OK | **far -4, near -26, OK** |
| desktop image | rope, wood grain, rivets, foliage | **unchanged** |

So the violation is gone and **the Thor still draws nothing**. There is at least one more cause,
and it is not one the validation layers flag - the remaining failures come back clean, with only
`AdrenoVK-0: Shader compilation failed for shaderType: 0` beside them. That is the next thread.

## A revert on a false signal, which is worth recording

Between those two states the desktop capture came back **entirely black** and I reverted the whole
change, believing I had broken the working path. I had not. Those runs captured the **swapchain**
(`bd_mv_capture_resolved=false`), and the flat present is black under `bd_stereo_multiview`
regardless of this change - a separate, pre-existing gap, since the multiview path composites
through the resolve and the mirror is not wired for it. Every earlier good capture had read the
array.

**Two captures of the same frame are not the same measurement.** The rule that would have caught it:
when a result changes, check that the *instrument* is the one that produced the baseline, before
concluding the code changed. Re-applied and verified through `bd_mv_capture_array`, the path every
earlier reading used, the image and the stereo verdict are byte-comparable to the baseline.

## The original diagnosis, for the record

The tension is real: `SNORM` is spec-legal but divides by 32767, and a texcoord wants the raw
integer. The format that is both float-class and unnormalised is **`SSCALED`** -
`VK_FORMAT_R16G16_SSCALED` and `VK_FORMAT_R16G16B16A16_SSCALED` hand the shader the signed integer
value as a float, which is what the shader's sign-extension path reconstructs by hand today.

**plume has no `SSCALED` in `RenderFormat`.** So the change is:

1. `noeldvictor/plume`: add `R16G16_SSCALED` / `R16G16B16A16_SSCALED` to the enum and the Vulkan
   `toVk` mapping. D3D12 has no SSCALED, so that backend keeps whatever it does now.
2. `gpu/vertex_declaration.cpp`: on Vulkan use the SSCALED formats and **stop setting
   `sintTexcoords`** - the driver has already done the conversion, exactly as on Metal.
3. Verify on the Thor that pipelines create, and on the desktop that textures are unchanged, because
   step 2 changes what the shader receives.

Not done here: it crosses two repositories and changes what every texcoord-reading shader is handed,
and landing that half-tested at the end of a long session is how a working path gets broken.

## What this cost, and the rule

A device was attached all session. `tools/verify_quest.sh` selects on `ro.product.model` matching
`*Quest*` - written deliberately, because installing a 68MB VR build on the wrong device wastes a
cycle - and that filter also silently hid the only ARM64 hardware available. The script is right to
prefer a headset and wrong to be silent about what it skipped.

**A tool that filters should say what it filtered out.** `verify_quest.sh` prints the device list
only when it finds nothing; it should print the ones it passed over too.


## The second cause, and it is the keystone

`shaderInt64=0` on the Adreno 740, with `bufferDeviceAddress=1`. Printed from plume at device
creation, because an Adreno driver that refuses such a shader says only
`Shader compilation failed for shaderType: 0` and the validation layers have nothing to flag.

Every recompiled shader reads guest constants as

```hlsl
#define cN vk::RawBufferLoad<float4>(g_PushConstants.VertexShaderConstants + off, 0x10)
```

a **64-bit** device address, so every one of them declares `OpCapability Int64`. The device cannot
compile that. **That is why the Thor renders nothing**, and it is the whole of what is left after
the vertex-format fix.

**CLAUDE.md already knew.** From the Quest validation run, recorded and never acted on: *"`Int64` is
declared by every shader while `shaderInt64` is not enabled, which makes the renderer's hottest path
formally undefined on this device."* The Adreno 650 compiles it anyway. The 740 does not. It was
filed as a curiosity and it is actually the blocker.

**And it is the same change as the port's headline performance problem.** `research/20260829_0030_shader-constants-are-global-loads.md`
argues for binding the constant blocks as a **uniform buffer** instead of pushing a device address,
because a buffer-device-address load is ordinary global memory per invocation where a UBO read goes
through the constant path - measured there as ~225ns per vertex. That change also removes the
`uint64_t`, and with it the `Int64` capability.

So one piece of work closes both:

- the ARM64 blocker, on any driver without `shaderInt64` - which is most mobile hardware, and
  undefined behaviour on the rest;
- the largest known GPU-side cost in the port.

The scoping in that note still stands and is the hard part: constants change every draw, which is
exactly why a push-constant address was chosen; a UBO needs dynamic offsets (plume does not expose
them) or a per-draw descriptor write; and Adreno allows four descriptor sets with 0-3 already taken,
so the UBO has to become a *binding* on the shared texture set.

**Do not treat that note's conclusion as optional any more.** It was written as a performance
optimisation with a real cost/benefit argument against it. It is now also the only route to a
rendering frame on an Adreno 740.


## Confirmed in the artefact, and the one obstacle that actually blocks the fix

Not inferred from the driver message. Decoding every `OpCapability` in the dumped SPIR-V:

```
spv files: 141   declaring Int64: 141   declaring PhysicalStorageBufferAddresses: 141
```

All 141, against a device reporting `shaderInt64=0`.

**The fix does not need a UBO with dynamic offsets.** The cheaper shape is a `ByteAddressBuffer`
bound once and indexed by a **32-bit** offset from the push constant, replacing the three
`uint64_t` addresses:

```hlsl
struct PushConstants { uint VertexShaderConstants, PixelShaderConstants, SharedConstants; };
#define g_SintTexcoords g_ConstantHeap.Load(g_PushConstants.SharedConstants + 324)
```

No 64-bit arithmetic, so no `Int64`; no per-draw descriptor write, because the buffer is bound once;
and one extra *binding* on an existing set rather than a fifth set, which is what Adreno's limit of
four demands.

**The obstacle is the allocator, and this is the part that was not written down anywhere.**
`gpu/constant_buffers.cpp` hands out allocations from **chunks** -
`a.ref = RenderBufferReference(chunk.buffer.get(), off)` with `a.gpuAddress = chunk.gpuBase + off` -
so there is no single buffer to bind and index into. A device address does not care which chunk it
came from; a bound descriptor does. So the change starts there:

1. Make the constant heap **one buffer** (suballocated), or bind per chunk and accept a descriptor
   write whenever the chunk rolls over - measure which, since chunk rollover frequency is unknown.
2. Push three `uint` offsets instead of three `uint64_t` addresses.
3. `shader_common.h` and `shader_recompiler.cpp:~1345`: every `vk::RawBufferLoad<T>(addr + off)`
   becomes a `ByteAddressBuffer` load at `base + off`.
4. Rebuild the shader cache and confirm `Int64` is gone from all 141 modules with the
   OpCapability decoder above - that check is three lines and it is the one that proves it landed.

Steps 1-3 touch the hottest path in the renderer and break desktop, Quest and Thor together if they
are wrong. **Not started here**, deliberately: this session already produced one revert made on a
false signal, and this is not the change to make tired.
