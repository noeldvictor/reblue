# Host pass matrices and float-parameter publication

Date: 2026-09-05, desktop EDT. This is a producer-execution checkpoint, not
complete host parameter, pass or frame ownership.

## Source and implementation

Read the render-tweak hook map and exact translated bodies (generated code was
not edited):

- `sub_821764F8` (`generated/reblue_recomp.45.cpp`): enabled source objects
  multiply matrices at +68 and +132 and publish through the descriptor at
  +364. The primary object additionally publishes enable and reciprocal-size
  scalars. Scene begin, shadow and reflection callers use this helper.
- `sub_82179868` (`reblue_recomp.61.cpp`): enabled fixed-global input multiplies
  matrices at +16 and +80 and publishes through +156. It does not take its
  source object from incoming r3.
- `sub_8217A630` (`reblue_recomp.99.cpp`): snapshot and transpose a matrix into
  an engine parameter block. Owner +12 supplies the block; descriptor +12 is
  its vector index, not a shader-register index.
- `bdShaderConstantFlush` (`reblue_recomp.38.cpp`): flags select VS then PS;
  start/end delimit vectors. The source is loaded once, but stage control is
  reread after VS. Empty ranges retain the original dirty-mask behavior.
- `D3DDevice_SetVertexShaderConstantFN` (`reblue_recomp.9.cpp`) and
  `D3DDevice_SetPixelShaderConstantFN` (`reblue_recomp.5.cpp`): grouped
  four-vector loads/stores, then individual tail vectors, then dirty-mask OR.

`host_parameter_bridge.cpp` replaces those six bodies for supported inputs.
The shared SDK-independent `MultiplyRenderMatrices` retains the existing
pairwise, non-contracted arithmetic convention. Projection arithmetic uses
the original flush mode; primary scalar division uses the original float /
double rounding sequence. Matrix transpose and constant copies preserve bits.
Return r3 preserves the original zero/sign extension, including inactive calls.

`shader_parameter_import.h` isolates temporary range/mask and sequential-copy
contracts. Full-range memcpy/memmove would be wrong for overlapping inputs:
later groups must observe earlier stores. A two-stage flush likewise lets PS
observe earlier VS writes. Copy publication retains dirty/upload and replay
write/source notifications exactly once per stage.

Preflight refuses invalid ranges, device misalignment, overflowing matrix
indices and unusual stack/control aliases before effects. Source/destination
data overlap is supported where the original snapshot/group semantics are
preserved. Compatibility and refusals remain counted.

`bd_host_parameters=true` is the default. With
`bd_host_parameters_verify=true`, host values are predicted, the original is
executed exactly once, and publications are compared without native writes
hiding differences. Nested replacements bypass themselves during this
reference execution. Float setters and flushes compare the entire device and
r3, including untouched bytes; projection results compare their 16 values with
the existing 1e-5 relative/absolute tolerance (both NaNs accepted), scalar bits
exactly and r3 exactly. Direct transpose is bit-exact. Inactive checks compare
r3 only; ABI stack scratch and caller-clobbered registers are not compared.

No native parameter registry or per-draw matrix-value identity guessing was
added. `NoteConstantsSource` remains a diagnostic, not a provenance tracker.
Engine parameter blocks/device register storage remain adapters; inline
constant writers and draw-time shader-register import still exist.

## Build and standalone checks

Built the existing Vulkan-only `reblue` target with four jobs. Final executable:
`out/build/win-amd64-release/reblue_vk.exe`, 47,282,688 bytes, 05:13:33 EDT.
The log revision is `eab360a52` plus local changes; final code was committed
and pushed as `8d4b389`. Codegen reported the module
up to date; no guest translation unit was rebuilt. No dependency changed.

All 18 texture/upload/state/parameter CTests and the material CTest pass,
as do the three reflection/source-boundary tests. Parameter tests cover:

- 66,564 bounded range/mask pairs, all 32,896 nonempty supported ranges and
  10,000 full-width wrapped mask inputs against an independent PPC oracle;
- 38,640 guarded overlapping/unaligned copies, group/tail ordering, zero
  length, 256 vectors, exceptional float payload bits and VS-to-PS sequencing;
- existing transform tests plus explicit multiplication order checks.

## Desktop verification

The first exploratory comparison (PID 7608, 05:11:27-05:13:31,
`reblue_714.log`) used the earlier 05:10:22 binary before the additional
stack/control-alias guards. It reached over 2.2 million matching checks and
zero refusals. Its captures are not the final guarded-build qualification.

Final guarded-build comparison: PID 24656, 05:13:54-05:15:30,
`reblue_715.log`. All seven profile settings were audited: the preserved
autoplay/perf/capture settings (60 seconds, 600 minimum draws, 120 frames),
plus host parameters and comparison both enabled. Mounted 1673 archives /
119346 names; RTX 3060, Vulkan 1.4.341, MSAA 4. No error/critical/VK_ERROR or
upload-exhaustion messages.

Last counter: 1,891,328 matching checks, zero wrong/compatibility/refused;
8,173 primary and 4,521 secondary enabled projections, 40,689 inactive;
926,997 flushes, 1,333,715 VS and 504,230 PS publications, 19,382,370 vectors.
These counts include direct calls and flush stages; nested reference calls
are not counted twice. Direct standalone matrix-writer calls were zero;
their transposition was exercised inside the parent projection comparisons.

Isolated capture: `out/verification/host_parameters_compare`, 120 1920x1080
frames 2846-2965, `frame_1788599696_0.raw` through
`frame_1788599700_119.raw`, 05:14:56.634-05:15:00.112. Sequence analysis:
0/119 jumps over 6%; no cyan threshold/patch frames, median 0.011%, max 0.02%.
First/last actual image previews show Shu in the village with animated
waterwheel shadows, without a broad missing band or cyan patch. `--mono`
only decoded flat captures; it is not a replay-disabled control.

### Normal comparison-off desktop

PID 25300, 05:17:20-05:19:24, `reblue_716.log`, same final binary. All seven
profile settings audited, with comparison changed to false. Full archives
mounted and no error/critical/VK_ERROR/upload-exhaustion messages.

Last normal totals: primary 10,583, secondary 5,722, inactive 51,498;
1,718,798 flushes, 2,020,436 VS and 1,389,265 PS publications, 33,342,305
vectors; zero compatibility/refused. Checked stays zero as expected.

`out/verification/host_parameters_flat` contains 120 1920x1080 frames
2845-2964, `frame_1788599902_0.raw` through `frame_1788599906_119.raw`,
05:18:22.619-05:18:26.392. Analysis: 0/119 jumps over 6%, zero cyan threshold
or patch frames, median 0.012%, max 0.02%. First/last actual previews show
Shu in the village and changing waterwheel shadows without a broad missing
band or cyan patch. This short view does not requalify the known late-scene
rock-wall/text failure.

### Desktop final-eye setup

PID 5052 started 05:20:18, same binary, desktop xrsim manifest with verified
absolute runtime DLL path (31,232 bytes). Process-only environment requests
1440x1584 per eye and head height 0. Profile uses the normal seven entries,
minimum draws 450, plus:

```toml
bd_vr_enabled = true
bd_stereo = false
bd_stereo_multiview = true
bd_mv_layered_textures = true
bd_mv_capture_array = false
bd_xr_mirror = false
bd_vr_camera_mode = 2
bd_vr_diorama_height = 0
bd_xr_render_scale = 1.0
```

### Final-eye result and handoff

PID 5052 stopped at 05:22:10, log `reblue_717.log`. All 16 settings audited;
full archives mounted, runtime/final eyes 1440x1584, layered direct
presentation confirmed, but scene content remains 1440x808 with MSAA 4.
No error/critical/VK_ERROR/upload-exhaustion messages.

`out/verification/host_parameters_vr_fullsize`: 120 final stacked 1440x3168
frames 12067-12186, `frame_1788600080_0.raw` through
`frame_1788600090_119.raw`, 05:21:20.842-05:21:30.861. All 119 pairs are below
6%, no cyan threshold/patch frames, median/max cyan 0%. Both eyes of actual
first/last previews were inspected: no broad missing bands or cyan patches,
but blur and large black bars remain. Both stereo checks are **inconclusive**
(exit 2), with only 44%/52% bands usable, disparities -1/-2 pixels and spread
1 pixel. This does not establish correct depth, framing or VR completion.

Last normal totals: primary 19,387, secondary 14,316, inactive 128,844;
5,465,104 flushes, 3,561,785 VS and 6,402,004 PS publications, 78,540,159
vectors. Compatibility/refused/checked/wrong all zero. Normal runtime confirms
supported producer execution without calling the original bodies, not removal
of engine storage or other rendering calls.

All renderers and analyzers completed; original five profile settings restored.
No device deployment, gameplay/save edits, asset changes, dependency changes or
performance improvement is claimed. These are desktop correctness runs, not
headset measurements. Full scene and final-eye qualification remain incomplete.

## Remaining work

Whole scene-begin scheduling, fog/plane producers, native scene/light and
material associations, remaining parameter writers and replacement of the
shader-register ABI remain. No asset formats or gameplay were changed.
The previously observed late rock-wall/text failure and blurred/letterboxed,
depth-inconclusive VR remain open until separately qualified. No Quest or
other headset run is authorized before the full desktop gate.
