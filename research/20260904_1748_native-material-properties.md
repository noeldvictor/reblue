# Native material properties from model commands

2026-09-04, desktop Vulkan. This is a bounded material-data migration, not a
fully host-owned frame or a Quest qualification. Gameplay remains recompiled;
all rendering and native asset conversion remain the owner goal.

## Source and correction

Read `config/hooks/render_list.toml` and the translated
`bdSceneNodeDrawSingle` at 0x8227FEE8 in `generated/reblue_recomp.40.cpp`, including
the material-command branches. Unlike the earlier staging-global prototype,
the model's command stream supplies the fixed diffuse modulator and specular
colour directly. Copying the shared staging struct at replay time was not
equivalent: it could contain a sibling's material. The disabled
`bd_material_from_visual` prototype and setting have been removed.

The phase-0 recipe, in host-endian input words:

| Command | Asset property / framing |
| --- | --- |
| `0x0100..01ff` | Zero low byte enables diffuse RGB modulation; nonzero selects object colour unchanged |
| `0x9000..90ff` + word | Diffuse RGB8: command low byte, operand high byte, operand low byte |
| `0x9300..93ff` + word | Specular RGB8 with the same packing |
| `0x0400..04ff` | Shininess; changing to zero also clears specular RGB. Repeated power commands are skipped, including after intervening RGB writes |
| `0x9400..94ff` + two words | Reflection RGBA8: first low byte, second high byte, second low byte, first high byte |
| `0x1000/2000/3000` + two words | Strip index count is first operand + 2; second operand is first index |
| `0x4000` family + word / `0x5000` family | Vertex record/stream and index record used to identify the draw range |

Supporting paths: `sub_82198138` in `reblue_recomp.62.cpp` sets the colour gate;
`bdSceneNodeDrawIndexed` in `.65.cpp` confirms strip count + 2 and base vertex 0;
`sub_82173960` in `.9.cpp` copies float4 material constants. The diffuse formula
is at `loc_822807F4`, token writes at `loc_82281634`, specular at
`loc_822815A8`, reflection at `loc_822814BC`. These generated files were read,
not edited or committed.

## Implementation and limits

`native_material_data.*` is a dependency-free decoder/composer with named,
address-free properties, not a captured shader register file. The decoder
recognizes operand framing, including bone indices and colour bytes that equal
the terminator value. Unknown opcodes, truncated operands, missing terminators
and streams above 65536 words fail transactionally. Omitted properties remain
unknown rather than receiving guessed defaults.

The temporary `native_material.*` adapter reads big-endian asset words at the
guest boundary. A bounded-entry discovery cache is invalidated by physical
buffer generation. Matching requires range, index record and vertex record;
ambiguous repeated geometry with different materials is refused. The result
is held by the host subdraw. The cache is not a persistent material format or
a finished streaming/eviction policy.

Only direct-tree phase-0, non-technique-11 draws are supported. Technique 11
can select a different object colour, and phase 1 rewrites material commands.
Dynamic object RGBA still comes from visual + 3404; visual + 3044 controls
whether shininess is written. Alpha is not multiplied by diffuse RGB. Unknown
fields and unsupported recipes retain existing compatibility handling.

Replay composes known properties without sibling/staging state and skips the
old fresh-value guards only for those exact fields. It still writes through
the existing pixel-constant ABI (registers 3, 4, 5) at the final compatibility
boundary. Draw templates, generated shaders, textures, pass producers and
list-entry material constants are not thereby replaced. A persistent native
material format, stable asset identities, texture/lighting-model definitions
(including optional cel shading), and complete frame ownership remain work.

## Verification

Reused `out/build/win-amd64-release`, target `reblue`, D3D12 off, OpenXR on,
Clang 22.1.8. The final incremental build linked `reblue_vk.exe`; no guest
translation units rebuilt. Native material CTest 1/1, native mesh CTest 1/1,
and Python stereo tests 2/2 passed. Material tests cover packing, alpha,
colour gates, zero/repeated shininess, incomplete values, nonfinite object
colour, operand boundaries, unsupported commands and every truncated prefix.

`bd_native_materials_verify` compares decoded fields against actual interpreted
draw constants, separately from replay. This is a source check, not proof of
subsequent state inheritance or complete host frames.

All image A/B runs below are correctness-only, not performance comparisons.
Native meshes remained enabled. Captures are isolated under
`out/verification/`, not committed game data.

| Run / isolated directory | Evidence |
| --- | --- |
| `reblue_628.log`, source verification with native composition off | Diffuse 0/465064 and specular 0/450916 wrong/checked; reflection not exercised at that report |
| `reblue_629.log`, `native_material_flat` | Native materials toggled every frame; 120 final 1920x1080 frames, 119 pairs, zero jumps above 6%, zero cyan patch frames. Inspected village/character/foliage/shadows image. Last source report: diffuse 0/323861, specular 0/310913; replay fields composed 191459/191335/0 |
| `reblue_630.log`, `native_material_vr` | Native materials toggled every frame; 120 final stacked 936x2060 frames, zero cyan patches, 8 jumps at 37/38/41/43 and 101/102/105/107. Source checks diffuse 0/578027, specular 0/562679; replay fields composed 1689480/1689356/0 |
| `reblue_631.log`, `native_material_control` | Native materials off throughout, same multiview view; 120 stacked frames, zero cyan patches, 10 jumps at 16/17/18/20/22 and 80/81/82/84/86. Inspected `jump_022.png`: same oblique lighting strip in both eyes and blur changes. Last source report diffuse 0/4232073, specular 0/4189641, reflection 0/212610; no native replay compositions |
| `reblue_632.log`, `native_material_final` | Final rebuilt binary, materials on by default, no A/B overrides: 120 final 1920x1080 frames, 119 pairs, zero jumps above 6%, zero cyan patch frames. Inspected the village image; no new visible material artifact in this slice. Last source report diffuse 0/336516, specular 0/317568; replay fields composed 564727/564475/0 |

The two multiview runs reproduce the existing defect exactly 64 frames apart,
including with the new material path disabled. This does not identify the
retained-state field responsible, prove all differences identical, or clear
the overall VR path. The distant, blurred/letterboxed view was **INCONCLUSIVE**
for stereo depth (checker exit 2); both eye images were inspected, but useful
near/far framing is still needed. No Quest/Thor run or headset performance
claim was made.

Multiview settings: `bd_vr_enabled=true`, `bd_stereo=false`,
`bd_stereo_multiview=true`, `bd_mv_layered_textures=true`,
`bd_mv_capture_array=false`, `bd_xr_mirror=false`, `bd_vr_camera_mode=2`,
`bd_vr_diorama_height=0`. Simulator: `XRSIM_WIDTH=1440`,
`XRSIM_HEIGHT_PX=1584`, `XRSIM_HEIGHT=0`, absolute workspace runtime manifest.
Actual final capture dimensions above are not a claim of native target
resolution. Capture settings: after 60 s, 120 frames, minimum draws 450 for
multiview and 600 for flat. A/B used `bd_ab_flag="bd_native_materials"` and
`bd_ab_period=1`; the control removed both and set materials false.

`bd_native_materials` is now enabled by default for supported fields;
verification remains opt-in. Test processes were stopped and the original
five-line desktop profile was restored after verification. The next boundary is native material/texture
asset ownership and replacement of the remaining draw/pass producers, with
the 64-frame defect and full desktop qualification still explicitly open.
