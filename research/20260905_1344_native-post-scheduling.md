# Native DoF/bloom scheduling and explicit post output

2026-09-05, Windows Vulkan desktop, EDT. Base `67eba28` plus this checkpoint.
This converts the supported DoF/bloom schedule, not every post effect or a
fully host-owned frame. The complete desktop and later Quest gates remain.

## Exact source and native boundary

The complete PPC-comment body of `sub_8221B1D8` (0x8221B1D8,
`generated/reblue_recomp.4.cpp`) owns phase-3 post dispatch. It calls DoF,
maintains two cached pairs of quarter-size bloom textures, generates bright
and weighted blur passes, fills the ms_tex input array/weights and submits
the combined draw. It then dispatches additional filters. Nearby
`sub_822150F0`, `sub_82215AC0` and `sub_82216228` are blur-kernel producers,
not the outer bloom owner; replacing them alone would retain its old schedule.

`sub_8221AF58` confirms the bright-pass object at owner+84, ms_tex at +2084,
DoF at +3440, and the later filter at +8660. The complete bright-pass
initializer `sub_82214CF8` and parameter body `sub_82214DD0` establish scalar
payloads +652/+664; `sub_82179FE0` and `sub_8217A040` establish buffered bool
and scalar getters. The same owner+8+bank byte gates bloom preparation and
composition; owner+24+bank enables DoF. Mode is owner+12648+4*bank.
The wrappers `sub_8221E700`/`sub_8221E758`, weighted input append
`sub_822166E8`, count publication `sub_82216740`, and complete binding/draw/
resolve/reset body `sub_82216780` were inspected.

Constants were checked in the owned XEX's decoded in-memory PE image, without
writing another image: scene weight 4.0 at 0x8205E43C; mask RGB/alpha 1/0 at
0x820551AC/0x82055230; trailing-effect epsilon 0.0001 at 0x8208EB5C.
Shader path strings in the image helped locate the exact initializers; no
decompiler, generated-source edit, shader change or dependency change was used.

`native_post_bridge.cpp` now replaces the supported whole schedule. Typed
`BloomParameters` carry threshold/intensity and scene/mask weights, and the
existing native DoF property production is shared without executing its
engine filter wrapper or publishing registers. `HostPostRender` takes explicit
scene/depth/output images, builds the native atlas and issues the folded
composite directly. It does not wait for a shader-hash draw intercept, execute
the bloom texture caches or blur loops, or fill/bind an ms_tex entry array.

An explicit persistent `PostColor` role owns the output. Queued draws flush
before barriers; the complete native image is published through the existing
explicit-source output adapter. No post EDRAM allocation, seed or inferred
resolve is used in this supported scope. The adapter still publishes the
downstream post/UI chain and uses the shared resource/lifetime representation.
Framebuffer caches are invalidated after native commands. Failures after GPU
work begins are not replayed through the original schedule.

The existing folded bloom appearance is retained, not re-benchmarked against
the old kernel. One trailing +8660 filter still executes per tested field
frame, with its original ordering/state. Other enabled strength-gated trailing
filters remain counted calls. Intervening +2712, packed effect combinations,
mode-1 dual-mask composition, alternate settings and unavailable image inputs
retain a counted original scope. These are unfinished conversion boundaries.

## Build and diagnostic correction

The first build linked at 13:25:44, 47420416 bytes. Its refusal-only diagnostic
(PID 25660, 13:30:31-13:32:04.073, log 756) exposed the enabled +8660 trailing
effect. Log 757 (PID 24216, 13:32:56-13:34:42.541) identified its enable flag
32. No native verification was claimed for these refusal-only checks.

After preserving the trailing calls, the 13:34:44 build is 47421440 bytes.
Diagnostic PID 7532 ran 13:35:18-13:36:50.346, log 758, with capture delay
600 and `bd_native_post_verify=true`; all six profile settings audited and
the full 1673 archives / 119346 names mounted. The diagnostic runs the
original schedule and compares imported values at the combined draw against
the independently produced native bloom values. Last sample: 3642 checks,
zero mismatches; 859 startup calls used an unsupported effect combination.
No capture was scheduled within this diagnostic's duration. This proves
matching flat parameters for those calls, not native execution or all modes.

The final source/comment build linked at 13:36:59, also 47421440 bytes,
embedded base `67eba282b` with local modifications. Codegen remained up to
date; no guest translation unit rebuilt. All 26 CTests pass, along with ten
scene, three reflection and six post source guards. Post unit coverage now
includes enabled/off/two-mask weight construction. Mode-1 GPU coverage is
still not qualified. Source guards are not independent GPU verification.

## Normal flat verification

PID 21136, 13:37:27-13:39:16.450, log 759, final 13:36:59 binary. Original
five-setting profile: autoplay/perf on, delay 60, minimum 600, 120 frames.
All settings audited; full install mounted. Native post/DoF defaults on;
diagnostics, VR and native sun off.

120 exact hard-linked frames in `out/verification/native_post_flat`:
`frame_1788629910_0.raw` through `frame_1788629913_119.raw`, frames 2841-2960,
13:38:30.308-13:38:33.834. Each 1920x1080 / 8294420 bytes. Sequence analysis:
0/119 changes over 6%, maximum 2.16%; no cyan patches, median 0.012%, max 0.02%.
Actual full-resolution first/last images show Shu's cast silhouette, foliage,
rocks and moving windmill shadows, with the existing distant DoF appearance.

Last sample: 4543 native post schedules, 858 original (856 effect-combination
refusals and two image/preflight refusals), 4543 retained trailing filter calls,
13629 state-308 calls. No later field increase in original-scope calls. The
exact two failed image/preflight conditions remain untraced. No checked
error/critical/assertion/fatal/device-lost/VK_ERROR/exhaustion markers appeared.

## Normal final-eye verification

PID 2884, 13:40:24-13:42:54.118, log 760, same final binary. All sixteen
settings audited: autoplay/perf on; delay 60/minimum 450/120 frames; native
sun and shadow passes on; VR on, legacy stereo off; multiview/layered images
on; scene-array capture and mirror off; camera mode 2, diorama height 0,
XR scale 1.0. Full install mounted. Native post/DoF on, comparisons off.
The checked 31232-byte xrsim DLL and absolute manifest were process-local;
runtime width 1440, height 1584, simulated eye height 0. No global XR change.

120 final stacked frames in `out/verification/native_post_vr`:
`frame_1788630087_0.raw` through `frame_1788630101_119.raw`, frames 7867-7986,
13:41:27.373-13:41:41.549. Each 1440x3168 / 18247700 bytes. Scene/final
eyes use 1440x1584. Sequence: 0/119 changes over 6%, maximum 0.34%; no cyan.
Both first/last stereo checks are correctly crossed: bands 44/52/62/72/82/
90/95% give -1/-2/-3/-5/-6/-8/-9 px, near-far -8, spread 8. Actual first/last
left/right images were inspected: native full-eye coverage, village stairs,
foreground ground, distant rocks and changing windmill geometry/shadows.
Distant blur remains; Shu's shadow is not qualified in this framing.

Last sample: 5847 native schedules, 5554 original (5552 effect-combination
refusals and two image/preflight refusals), 5847 trailing filter calls and
17541 state-308 calls. Scene ownership checks 85423, zero wrong/fallback.
No checked error markers. These short field windows do not supersede the
earlier late-scene failure or establish battle/cutscene/menu/reload coverage.

## Storage and handoff

The guest-source skill guided exact ownership tracing; devloop/vrsim kept
builds and verification on desktop. Original profile restored exactly and
all owned renderer processes stopped. No Quest/Thor test or headset timing
claim. Historical-capture NTFS compression ran concurrently, so these runs
are correctness evidence, not timing measurements.

The owner additionally requested cleanup and durable space discipline.
`AGENTS.md` policy was separately committed/pushed as `67eba28`. Before the
bulk archive job, an exact 120-frame older VR set was compressed with every
SHA-256 unchanged, recovering about 1 GiB. The separate September-4 capture
compression job is still running at this note's creation; its completion and
actual storage accounting must be checked separately. No capture was deleted.
