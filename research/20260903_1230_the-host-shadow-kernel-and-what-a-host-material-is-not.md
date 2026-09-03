# The host shadow kernel, and what a host material is not

2026-09-03, 12:00-12:30, desktop only (owner decision of 10:00: no Quest runs until the host
owns the frame). Stage 4, materials: the first host shaders substituted for the guest's scene
pixel shaders, and the finding that changed the plan written at 10:05.

## The mechanism, proven on an identical image

`BloomMaskClampBlob` in `gpu/shaders/guest_shaders.cpp` already swapped two post shaders by
guest hash at link time. A host copy of the recompiled `bd_normal_ps` (the dump's body with
the common header `#include`d instead of inlined, built by `reblue_host_shader(... ps_6_1
-D REBLUE_RECOMP)`) went in as `bd_normal_lit.hlsl` under hash `FB83DD3F5E67CEB7`, behind
`bd_host_materials` (default on, read at link time). `[material] host shader substituted for
guest ps FB83DD3F5E67CEB7` in the log says it took.

Verification, and its limit. Three desktop autoplay runs captured at 60 s (`bd_capture_after_s
= 60`, `bd_capture_min_draws = 300`), the same camera in all three:

| pair | pixels differing (>8 of 255) |
| --- | --- |
| host copy on vs guest shader | 10.8% |
| second host-copy run vs guest shader | 12.1% |
| host copy on vs host copy on (two runs) | **16.6%** |

The on/off difference is under the on/on run-to-run noise, so the copy is indistinguishable
from the guest shader. The diff image says what the noise is: the wind-driven bushes, the
clouds, and the fence's shadow band - **the whole shadow band moves between any two runs**
(an animated light or a map fit that follows animated casters). So a cross-run capture can
verify a material's look but never a shadow's placement to the pixel.

## The kernel

The recompiled shadow block (dump lines 900-1000) is six `tfetch2D` depth fetches plus six
`shadowCmp2D` compares, and `shadowCmp2D` in `shader_common.h` is four `Load`s and a manual
bilinear: **thirty texture operations per shadowed fragment**. The taps sit at literal offsets
of +-1.3/1024 of the map, multiplied by `g_ShadowPcfScale`, which the host computes
(`constant_buffers.cpp`, `RecomputeShadowPcfScale`) to hold the penumbra constant in world
space as the coverage box widens, floored at one texel of the real map.

The host kernel keeps the recompiled projection, biases and edge rule and replaces the taps:
the shadow map is `D32_SFLOAT_S8_UINT`, so `GatherRed` on the array heap returns the four
texels of a bilinear PCF tap in one fetch; four gathers at the corners of a quad half the
guest kernel's width (`0.65 * c252.z`, where `c252.z` is `(1/1024) * g_ShadowPcfScale`) give
sixteen compared texels for four fetches, with the same world-space penumbra. Decoded from the
microcode, for the record: uv = `(0.5 + 0.5 * r6.x / r6.w, 0.5 - 0.5 * r6.y / r6.w)` (TEXCOORD6
is the map projection, v flipped as D3D does), reference depth = `r6.z / r6.w - (r5.z / r5.w)
* eps.x - 0.4 * (1 - NdotL) * eps.x` (a depth-proportional and a slope-scaled bias from
`g_vShadowEpsilon`), taps `> ref` are lit, the six averaged, and a superellipse edge test that
lights anything at the map's edge (the host uses the rectangle). `r7.y` leaves the block as the
lit fraction and the recompiled diffuse block consumes it with `g_vShadowSubColor`.

Crops of the same ground region: one gather gave shadows attached to their casters with a
hard edge; four gathers give a penumbra comparable to the guest's, whose own edge is dithered
(the 2*frac sub-texel terms in its tap positions).

## What a host material is not: "the four paths and nothing else" is withdrawn

The 10:05 plan said the host lit material would be the four census paths (colour texture,
normal map, shadow, fog+diffuse) and nothing else. The first rewrite did that - environment
map, detail textures (`g_bTexture1/2`) and debug branches removed - and the capture showed a
building's door region rendering wrong: **a scene outside the census takes the detail-texture
path**. The census measures one scene. Two facts settle the design:

- Every one of those paths is a **uniform** boolean branch. An untaken uniform branch costs
  nothing on the GPU; removing it saves no fetch and no ALU, it only removes correctness
  somewhere else.
- The fetch cost that the census's "fragment x fetch bound" pointed at is **inside the taken
  paths**: the shadow kernel. Lights 1-3, specular and fog are ALU under uniform branches, and
  the ALUs sit at 22% on the Quest.

So the host material is the recompiled body plus the host's kernel, the branches kept. The
cel slot is the existing `SPEC_CONSTANT_CEL` band at export.

## The family

A scan of the dump for `shadowCmp2D` calls: eleven shaders carry a six-compare kernel. The
shadows-on census of run 265 (this scene, 1920x1080):

| shader | fragments a frame | kernel |
| --- | --- | --- |
| `bd_normal_ps` | 48.4% | six compares - **host, `bd_normal_lit`** |
| `bd_normal_ps_nolight` | 20.5% | none |
| `bd_shadowmap_ps` | 19.2% | none (the 4096 map's own pass) |
| `bd_normal_ps_wind` | 10.5% | byte-identical block - **host, `bd_normal_wind_lit`** |
| `bd_normal_ps_ref`, `bd_toon_ps`, `bd_water_ps`, `bd_toon_ps_ref` | 1.5% together | toon: six compares, different registers |

`bd_normal_ps_barrier`, `_dir`, `_spot`, `bd_toon_ps*`, `bd_caustics_ps` differ from the
plain block in register allocation and constant slots (`c250.y` vs `c251.y` for 0.5, and so
on); `tools/make_host_material.py` transplants only byte-identical blocks and refuses the rest. They are under
1% of the field's fragments; when a scene puts them on screen, each is a per-shader decode of
the same five lines.

## Commits

`766308f` the verbatim host copy and the substitution; `3aaf3ed` the four-gather kernel;
`386bb83` the wind variant.
