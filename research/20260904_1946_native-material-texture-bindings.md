# Native material image bindings and sampler recipes

2026-09-04. A bounded host-renderer checkpoint, not completion of the renderer
transition. Gameplay, generated sources, game assets and dependency gitlinks
are unchanged. Native bindings remain on by default.

## Boundary and implementation

Read the repository devloop, guest-source and vrsim skills, current transition
scope, native texture contract and prior GPU ownership evidence. Inspected
`config/hooks/render_list.toml`, `config/hooks/physical_buffers.toml`, node/list
dispatch, constant publication, sampler caching and native GPU lifetime.

Previously replay held guest texture pointers even for shared native images.
Explicit immutable material bindings now hold `NativeTextureBinding` handles
for primary/cube/volume-slice assets. Converted slots clear their guest pointer
and allocation address. Shared constants publish descriptors directly; draw
diagnostics and scene recording use native IDs and dimensions. Strong ownership
extends through recording; the existing native GPU cache controls fence release.

Stable fetch deltas import `RenderSamplerDesc` once. Normal replay skips fetch
reads/composition for those native sampler slots. Live host filtering/mip policy
is applied separately. Sampler cache identity now includes all 15 fields,
including mip bias, LOD bounds, comparison and visibility, with signed-zero
normalization. Creation failure returns the allocated slot and is not cached as
valid. Verification/recording still compose fetch words when explicitly needed.

Only explicit immutable bindings cross this boundary. Inherited slots, render
surfaces, aliases and incomplete companion imports remain live compatibility
inputs. Neither texture conversion nor native descriptors remove the retained
shader/register ABI, frame/pass producers or initial guest asset discovery.

Temporary node recipes now expire against texture replacement/eviction and
geometry generations at lookup. The associated visual/pass history is retained.
Unused recipes retire after 300 frames; new draw recipes have a 4096-entry cap.
Direct draws and their deferred list are one compound recipe for retirement.
A volatile direct part must still force interpretation, not masquerade as a
list-only node and omit geometry. These safeguards do not make replay a native
scene database or prove complete scene-transition safety.

## Correctness investigation

All controls were correctness-only, not a decision about whether to implement
the mandated native renderer. Run outputs stay ignored under `out/verification`.

| Log / isolated capture | Result and interpretation |
| --- | --- |
| 639 / `native_bindings_flat` | Initial implementation: 12 jumps above 6% in 119 pairs; inherited native images were incorrectly frozen |
| 641 / `native_bindings_coherent` | 80 jumps after removing one-sided native binding replacement during merge; not sufficient |
| 642 / `native_bindings_explicit` | 81 jumps, with missing geometry, after restricting conversion to explicit binds |
| 643 / `native_bindings_off` | 79 jumps with zero native image/sampler publications; direct descriptor binding was not the sole cause |
| 644 / `native_bindings_no_retire` | Temporary diagnostic disabling recipe refresh/retirement: zero jumps, complete village pixels |
| 645 / `native_bindings_retirement` | Paired draw/list retirement alone: 80 jumps; still incorrect |
| 646 / `native_bindings_epoch` | Recipe-scoped generation expiry, preserved producer history and volatile-direct classification: zero jumps |

The diagnostic early return and native-binding-off profile are removed. Final
code retains bounded retirement and native binding. Failed runs are not counted
as verification passes. Log 640 sampled the retained-state verifier every 31
nodes on an earlier iteration: 10,319/80,017 nodes differed, including geometry,
constants and inherited textures; fetch differences were zero. This is not a
clean verifier result and does not qualify the final revision.

## Final build, tests and flat pixels

Clang 22.1.8, existing `win-amd64-release`, Vulkan-only target `reblue`, OpenXR
enabled. Final incremental build linked `reblue_vk.exe`; codegen wrote zero
files, with no guest translation units rebuilt. Final focused tests passed:

- Texture CTest 3/3: assets, GPU lifetime, new native binding/sampler tests.
- Material CTest 1/1 and mesh CTest 1/1.
- Stereo checker Python unit tests 2/2 (not a runtime stereo verdict).

Binding tests use the production descriptor mapping, fenced cache and compound
pruning helpers. They exercise native ownership after importer release, exact
descriptor retirement after the matching fence, dimensional companions, all
sampler fields, age boundaries/wrap and volatile direct classification. They
do not execute Vulkan driver operations.

Log `reblue_646.log`: started 19:36:37 local, RTX 3060 / Vulkan 1.4.341, full
1673-archive / 119346-record mount. All five original profile settings audited:
autoplay and perf CSV on, capture after 60 seconds, minimum 600 draws, 120 frames.
The final 1920x1080 sequence completed at 19:37:43.188, frame 2961. Analysis:
119 adjacent pairs, zero jumps above 6%; zero cyan patch frames, median cyan
0.003%, maximum 0.01%. Inspected `first.png`: character, foliage, terrain,
buildings and shadows are present. This is a short idle village slice.

The 19:37:43.801 periodic report shows 842,896 replay dispatches with native
images, 2,277,935 native image-slot publications and 435,126 native sampler-slot
publications. Compatibility sampler publications were 23,712,970. These count
published/attempted slots, not shader-used samples or fully host-owned frames.
The GPU store last reported 615 uploads, 25 reuses, 10 retirements, 605 resident,
91,512,512 payload bytes, zero refusals/failures. Recipe count was 520 with 481
cumulative retirements at that report. These are snapshots, not final totals.

The app continued beyond the capture. At **19:40:53.915**, its log reports
constant-buffer slot 1 exceeding its **32 MiB** span at **33,637,888 bytes**,
with wrapping that can corrupt draw constants. This remains unresolved and
prevents a clean long-session claim. No device-loss or retire-race message was
found. The process was stopped at about 19:42; original profile is intact.

## Remaining gates

This revision has not yet received a desktop multiview capture. Earlier
multiview runs exhibited a 64-frame lighting defect and inconclusive depth in
the distant diorama; neither is cleared by the flat sequence. No Quest or Thor
run occurred. Full field/battle/cutscene/menu/transition/reload coverage,
constant-buffer lifetime/bounds, native dynamic inputs, asset-level scene
loading, shader ABI and complete host pass/frame ownership remain required.
