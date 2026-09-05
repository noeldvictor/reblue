# Native post colour grading

2026-09-05, Windows Vulkan desktop, EDT. Base `c5e8dd6` plus local changes.
Native producer/pass checkpoint; this is not full renderer or desktop
game-coverage qualification. Documentation base advanced to `3cdbd40` while
the tested renderer source and executable remained unchanged.

## Source and storage preflight

Guest-source/devloop guidance used. Complete original outer post schedule
`sub_8221B1D8`, packed producer `sub_82219960`, submission `sub_82219758`,
packed initializer `sub_82219560`, colour initializer `sub_82219240`, grain
initializer `sub_82218920`, owner initializer `sub_8221AF58`, buffered RGB
getter `sub_82188840` and flag getter `bdGetDoubleBufferPtr` inspected.
`config/hooks/render_tweaks.toml` read before investigating original execution.
`bd_pe_ps_packed.hlsl` confirms that the old additional composite texture-list
bindings are unused by the pixel shader. It samples completed colour and
grain, then applies discoloration and colour correction, preserving alpha.

The packed producer's positive strength threshold is .01, not the other
filters' absolute .0001 threshold. Constant 0x82054278 and 1/65536 at
0x8203A8A4 checked in the owned XEX's in-memory decoded PE only; no image,
asset, dependency or build-tree copy. Flags come from the shared post owner,
not necessarily the producer argument. Authored properties remain adapters.
Native grain uses existing cooked image mirrors and native frame hashing,
shared between eyes. Its phase range and six-image grouping remain, but the
random sequence and rotation phase deliberately no longer use gameplay RNG
or the engine's per-object counter. No exact animation-sequence parity claim.

Free before implementation: 45,452,517,376 bytes (42.33 GiB). Before build:
45,451,526,144 bytes. Peak budget 1 GiB incremental build/link scratch plus
4 GiB bounded captures and analysis exports; reserve roughly 37 GiB. Reuse
configured target and standalone test trees. Previous optical normal captures
become historical; scanline normal flat/VR become baseline, new normal runs
current qualification. Keep optical/scanline and failed/fixed lens previews
as regression evidence; unresolved late-scene evidence stays protected.
Projected active raw payload with new normal flat/VR plus two 32-frame
previews is about 9.4 GiB. Hard-link run isolation; only representative PNGs.
Historical sequences are eligible for verified lossless compression, not
silently deleted. Restore profile and stop exact agent-started app processes.

## Resumed storage audit and bounded cleanup

The owner interrupted for a stricter storage policy, pushed as `3cdbd40`.
The exact agent-started VR preview PID 21232 was stopped at 16:21:18.623;
the original five settings were restored. Its 32-frame capture had already
completed and is reused, not rerun. Source changes remain the same tested
local renderer changes; the documentation commit does not justify a rebuild.

Free on resumption: 42,748,563,456 bytes (39.81 GiB). A scoped raw inventory
of `out/verification` and the installed app's `logs/capture` found 29,401
distinct NTFS file identities, 276,330,454,700 logical bytes and
245,514,026,024 allocated bytes (GetCompressedFileSizeW). Earlier "active"
totals excluded historical sequences and were not total disk usage. Some
older isolation directories are physical copies, not hard links. The first
inventory attempt used Windows DirEntry's zero inode and was rejected;
direct os.stat file identities, checked against fsutil, give the above totals.

Before allocating another sequence, remove only `.raw` files from these
superseded normal sets and their checked same-name source capture entries:
`native_adjust_flat`, `native_adjust_vr`, `native_lens_flare_folded_flat`,
`native_lens_flare_folded_vr`, `native_post_flat`, `native_post_vr`,
`native_dof_flat`, `native_dof_vr`, `scoped_camera_flat`, `scoped_camera_vr`.
Each has its first/last representative PNGs; dated research and logs remain.
The later normal scanline flat/VR baseline supersedes these short field
controls, not the protected late-scene failures. This is 1,024 sequence
frames (13,589,565,440 logical bytes before accounting for extra copies),
not a build-tree, game-data or directory deletion. The exact historical raw
sequences cannot be recovered after deletion; new captures are reproducible
tests, not byte-identical replacements. Validate all targets, references and
absence of app producers before deletion; measure actual free-space change.

The remaining historical raw archive still exceeds the new 10 GiB target.
Retain a temporary qualification/regression exception while its mixed older
failure/control pairs are reviewed; do not claim that relabeling them meets
the limit. This is an explicit outstanding cleanup item, not a new unlimited
archive allowance. No other capture set is deleted in this bounded cleanup.
The one remaining normal final-eye qualification is capped at 120 frames /
2,189,724,000 raw bytes, with at most 100 MiB of inspection exports and logs.
Launch only after cleanup and a fresh reserve check; stop on capture
completion, preserve its baseline until superseded, and restore the profile.

Cleanup completed: 2,048 exact file paths removed (the 1,024 sequence frames
and their source hard links), no directories or other files deleted. Free
space rose from 42,715,787,264 to 55,252,303,872 bytes: 12,536,516,608 bytes
(11.68 GiB) actually recovered. All target references were checked before
deletion, all ten directories retained their representative PNGs, no app was
running, and protected previews/baseline/failure captures were untouched.
The normal VR job's 2.14 GiB bound leaves roughly 49.3 GiB reserve.

## Native implementation and tests

`GradeParameters` imports the authored discolor/grain strengths and colour
gain, bias, target, gamma, saturation and blend. The shared post-owner flags
use the original positive .01 strength threshold. The native pixel pass
samples completed colour in the current eye and, when enabled, one of six
existing cooked grain images in layer zero through an explicit linear/wrap
sampler. It applies grain, discolor and correction in the original order,
preserving sampled alpha. The five float4/uint4 constant blocks are an
explicit 80-byte native layout, not PS-register reads or guest publications.

The supported path no longer executes the packed producer/submission,
per-object rotating counter/gameplay RNG, obsolete composite texture-list
binding loop, intermediate engine target or emulated resolve. Only the
intervening flag-16 filter and dual-mask mode 1 remain effect-combination
refusals. Authored-property and image/output adapters remain. Three optional
stages (optical, scanline, grade) ping-pong through at most two private native
images: composite -> A -> B -> A -> final for all three. No full-image seed
copy is introduced. Preflight failures precede GPU work; later failures throw
instead of rerunning a partially executed frame through the original path.

The initial compile exposed a nonexistent Plume POINT enumerator; the
corrected build completed at 16:05:19.728. Exact original grain/default
sampler source then established linear filtering, not nearest. The final
incremental build linked at 16:12:08.728, 47,481,344 bytes, embedded
`c5e8dd6bd` with local modifications, Clang 22.1.8. It rebuilt only the
changed host source and linked the existing Vulkan-only/OpenXR/PCH target.
Codegen reported no changed output; no guest translation unit rebuilt.
No build was repeated for the later AGENTS/documentation-only commit.

All 27 CTests pass (26 native texture/state/camera/post, one material), as
do 28 source guards (15 post, ten scene, three reflection lock-order).
The reflection guard was initially invoked with a nonexistent filename;
the discovered `tools/reflection_lock_order_test.py` then passed. The new
CPU test compares 16,448 RGB inputs / 49,344 channels with an independent
literal/register-swizzle log2/exp2 transcription, including black^0,
negative/HDR inputs, four gamma/strength choices and extrapolation (2e-5
tolerance). It checks activation boundaries, six frame/index extremes and
all eight pass-enable combinations for ordering and absence of feedback.
These are CPU/source checks, not substitutes for GPU verification.

The complete emitted SPIR-V assembly was inspected: ViewIndex and bindless
images/samplers, push offsets 24/28/32/36, constant offsets 0/16/32/48/64,
same-eye colour sampling, layer-zero noise, conditional grading and unchanged
alpha. The original packed HLSL's literal operations were also inspected.

## Runtime evidence

All logs 779-786 audit every applied profile setting and mount the full
1673 archives / 119346 record names. All contain zero checked
error/critical/device-loss/VK_ERROR/exception/assertion markers. The original
five settings are autoplay/perf true, delay 60, minimum 600, count 120.
Previews and comparisons are off unless explicitly stated below.

### Original-parameter diagnostic

Log 779, PID 22492, 16:05:56.764-16:07:09.995. Six settings: original five
except delay 600, plus `bd_native_post_verify=true`; no captures. Last sample
has 3,301 matching original schedules/bloom publications, 3,615 flare sprite
checks, 860 scanline-strength checks and 860 grading parameter/activation
checks, zero wrong/refusals. The comparator checks all five grading blocks,
three flags and exact producer counts. It deliberately does not compare the
new random sequence with gameplay RNG. No optical adjustment activated.
This is authored-parameter evidence, not native image or broad-event coverage.

### Normal flat field

Log 780, PID 20748, 16:07:15.046-16:09:13.503, original five settings.
120 hard links in `out/verification/native_grade_flat`,
`frame_1788638897_0.raw` through `frame_1788638900_119.raw`, frames 2837-2956,
1920x1080 / 8,294,420 bytes each; capture 16:08:17.337-16:08:20.676.
0/119 changes over 6%, maximum 2.77%; cyan median .011%, maximum .021%,
no hits. Full first/last images inspected: Shu's cast silhouette, foliage,
rocks and moving windmill/shadows remain; existing distant blur remains.
Last counters: 5,698 native post scopes, three original/input refusals,
861 authored grading/scanline frames, no grain, 241 flare frames/3,615 sprites.
No effect-combination refusal remains in this run.

This uses the 16:05 binary before the sampler-only correction. Grain was
off, so the changed sampler branch was not executed. Reuse this evidence
with that explicit limitation instead of duplicating another 0.93 GiB run.
Corrected grain and normal VR are exercised by the final binary below.

### Native startup and original-path control

Log 781, PID 8956, 16:09:19.219-16:10:28.577. Five settings with delay 5,
minimum 0, count 32. `native_grade_startup` retains 32 1920x1080 frames,
`frame_1788638966_0.raw` to `frame_1788638967_31.raw`, frames 303-334,
16:09:26.680-16:09:27.361. 0/31 changes, maximum 0%, no cyan. Last sample
has 2,998 native/three input refusals and 857 authored grade/scanline frames.

Log 783, PID 23916, 16:12:14.105-16:13:29.676, final binary. Same startup
settings plus original-parameter verification (six settings).
`native_grade_startup_original` retains `frame_1788639141_0.raw` to
`frame_1788639142_31.raw`, frames 303-334, 16:12:21.479-16:12:22.133.
0/31 changes, maximum 0%, no cyan. Last sample: 3,301 matching original
schedules, 850 grade/scanline checks, 3,615 flare checks, no wrong/refusals.

Both runs' full first/last images were inspected: white background, Start
prompt and copyright text. First and last native/control RGB images are
pixel-identical (maximum channel difference zero). This preserves the
existing result; it does **not** qualify title artwork. The control executes
the original post schedule/producers with existing host DoF/bloom intercepts,
not an untouched console renderer. Neither startup capture activates grain.

### Corrected flat grain/grade preview

Log 782, PID 21732, 16:10:34.086-16:12:01.455, used the superseded nearest
sampler. Its 32 source captures (`frame_1788639096_0.raw` to
`frame_1788639097_31.raw`) are not qualified evidence. No asset copies were
made. The source correction above supersedes that preview; its raw payload
is a small remaining cleanup candidate, not an active baseline.

Log 784, PID 2428, 16:13:35.635-16:16:04.058, final linear/wrap sampler.
Six settings: original five except count 32, plus `bd_native_grade_preview=2`.
Native-only preset: discolor .6, grain .15, gain (1.1,.95,.8), bias (.02,.01,0),
target (.1,.15,.2), gamma 1.1, saturation .7, blend .1; grain group 0-2.
32 hard links in `native_grade_preview_flat`, `frame_1788639277_0.raw` to
`frame_1788639278_31.raw`, frames 2836-2867, 1920x1080 / 8,294,420 bytes,
16:14:38.000-16:14:38.839. 0/31 changes over 6%, maximum 3.143% (6->7),
no cyan. Full first/last and worst-pair images inspected: coherent warm/grain
appearance, recognizable Shu/scenery and moving cast/windmill shadows.
Last counters: 7,798 native/grain frames, three input refusals, 856 authored
grading scopes, 241 flare frames/3,615 sprites. This is a synthetic preview,
not authored grain-event qualification or exact animation parity.

### Combined final-eye preview

Log 785, PID 21232, 16:16:10.219-16:21:18.623, final binary. All 19 settings:
autoplay/perf true, delay 60/minimum 450/count 32; grade preview 3 (alternate
grain 3-5), optical preview 2 (fisheye -.75/full inversion), scanline preview
true, native sun/shadow passes true, VR true, legacy stereo false, multiview
and layered textures true, scene-array capture/mirror false, camera mode 2,
diorama height 0, XR scale 1.0. Scanline noise retains its off default.
The existing absolute simulator manifest/DLL, width 1440, height 1584 and
eye height 0 were process-local, with no global runtime change or device use.

32 existing frames hard-linked into `native_grade_preview_vr`,
`frame_1788639432_0.raw` to `frame_1788639434_31.raw`, frames 7712-7743,
1440x3168 / 18,247,700 bytes; capture 16:17:12.624-16:17:14.381.
0/31 changes over 6%, maximum .262% (6->7), no cyan. Both eyes' full-size
first/last images inspected: coherent distorted/inverted, graded/grainy
village view. This exercises the three-stage/two-scratch route and alternate
grain group, not normal colours, authored event coverage or VR comfort.

The run remained live past its capture during the owner's interruption.
Last sample: 28,523 native/77 original image-input refusals, 5,127 authored
grading scopes, all native frames with synthetic optical/scanline/grain.
Those later input refusals are **not** a clean full-run result or attributed
to the grade change; no late-window images were captured. Preserve the log
for tracing, and do not use the short preview to supersede late-scene defects.

### Normal final-eye field

Log 786, PID 27556, 16:28:57.895-16:30:41.685, final 16:12:08 binary.
Same VR configuration, all three preview keys removed and count 120
(16 audited settings). No build, dependency or asset copies. 120 hard links
in `native_grade_vr`, `frame_1788640200_0.raw` to
`frame_1788640208_119.raw`, frames 7852-7971, 1440x3168 / 18,247,700 bytes;
capture 16:30:00.294-16:30:08.359. Standard sequence/cyan tools: 0/119 changes
over 6%, maximum .37%, no cyan. Both first/last stereo checks exit 0:
44/52/62/72/82/90/95% bands -1/-2/-3/-5/-6/-8/-9 pixels, near-far -8,
spread 8, correctly crossed. All four full-size first/last eye images
inspected: full-height ground/stairs, village/rocks, moving windmill and
shadows remain; existing distant blur and unqualified character-shadow
visibility in this framing remain. Last counters: 9,598 native, three
original/input refusals, 5,289 authored grading/scanline scopes, no grain
or optical activation. Startup packed effects no longer force thousands of
original schedules as in the prior normal VR checkpoint.

## Remaining scope and handoff

The three normal startup/transition image-preflight refusals need exact
reason tracing. Flag-16/intervening filtering, dual masks, authored grain and
other event coverage, light/visibility, scene/camera/property/image/getter/UI
adapters, native animation/material/scene ownership and complete host frame
scheduling remain. Short flat/VR controls do not supersede known late-scene
failures or qualify battles, cutscenes, menus, transitions and reloads.
No Quest/Thor run, headset timing, comfort or full-renderer completion claim.

Guest-source guided exact producer/initializer and shader tracing; devloop
kept incremental builds on the existing desktop target; vrsim supplied
process-local final-eye verification. All app and analysis runs terminated,
and the original five-setting profile was restored and read back.

Final measured free space: 53,041,643,520 bytes (49.40 GiB). Across this
grading checkpoint, net volume usage **decreased** 7,589,126,144 bytes
(7.07 GiB) relative to the initial 45,452,517,376-byte reserve; on resumption
alone free space increased 10,293,080,064 bytes. These are volume changes,
not sums of hard-linked directory sizes. No assets were converted/copied,
dependencies downloaded, build trees duplicated or guest objects rebuilt.
The seven new capture sets total 400 frames / 4,830,666,560 raw bytes
(4.50 GiB). Added startup controls and the superseded sampler preview
exceeded the initial 4 GiB capture-only estimate; the resumed cleanup and
final-run budget explicitly account for them. Future retries must share a
cumulative bound before launch under the updated storage policy.

Final scoped archive inventory: 28,497 unique raw file identities,
264,930,613,260 logical bytes and 235,170,393,832 allocated bytes. This
remains far above the 10 GiB retention target; the documented temporary
historical qualification/regression exception is still outstanding. Next
storage work should hash-verify and deduplicate older physical copies, then
review superseded normal sets, retaining complete unresolved failure/control
pairs until their investigation no longer requires them. The scanline
baseline, current normal grading flat/VR, startup controls and optical/
scanline/grading/failed-and-fixed-flare previews remain for this checkpoint;
they must be reviewed again before the next large capture allocation.
