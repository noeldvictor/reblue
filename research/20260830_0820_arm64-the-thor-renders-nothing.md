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

## The fix, specified but not written

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
