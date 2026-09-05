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
The log revision is `eab360a52` plus local changes. Codegen reported the module
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

Normal comparison-off and final-eye runs are not yet qualified at this
initial producer checkpoint. No performance improvement is claimed.

## Remaining work

Whole scene-begin scheduling, fog/plane producers, native scene/light and
material associations, remaining parameter writers and replacement of the
shader-register ABI remain. No asset formats or gameplay were changed.
The previously observed late rock-wall/text failure and blurred/letterboxed,
depth-inconclusive VR remain open until separately qualified. No Quest or
other headset run is authorized before the full desktop gate.
