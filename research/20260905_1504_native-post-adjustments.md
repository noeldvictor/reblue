# Native fisheye and colour inversion

2026-09-05, Windows Vulkan desktop, EDT. Base `4e79e82` plus local changes.
Verified component checkpoint; the complete frame and full desktop game-coverage gate
remain required before Quest work.

## Source and conversion

The guest-source skill guided complete reads of `sub_8221AF58` (initialization),
`sub_8221B1D8` (outer schedule), wrappers `sub_8221E700` / `sub_8221E758`,
`sub_822187D8` (fisheye parameter), `sub_82218840` (fisheye submission),
`sub_82218DB0` (reverse initialization), `sub_82218E88` (reverse parameter),
and `sub_822191E0` (reverse submission). The existing translated shader bodies
and literal lanes identify the optical curve and inversion equation. Owned XEX
strings confirm fisheye at root+6472, reverse at +9500, and NTSC at +10172.
The decoded XEX existed in memory only. No game asset was changed or copied.

The native schedule reads authored enable/strength gates and buffered scalar
properties, not shader constants. Fisheye then inversion becomes one layered
pass with explicit parameters. The preceding composite and optical sprites
render directly into private native scratch only when an adjustment is active;
the new pass writes the final attachment. No seed copy, engine intermediate
handle, per-effect texture setter, depth binding or emulated resolve executes
for these two filters. Native composite/sprite destinations now use a native
attachment record instead of requiring an engine resource wrapper.

The shared C++/HLSL radial curve retains the original positive/negative equations,
with a defined center and output height/width instead of a fixed 720/1280 ratio.
Inversion preserves alpha and uses color + (pivot - 2*color)*strength. These are
authored effects, not headset lens correction. VR comfort is not qualified.
The NTSC tail, intervening filter, packed effects, dual-mask mode, authored
properties and final output/getter/UI adapters remain tracked boundaries.

The existing original-schedule diagnostic can compare both parameter publications
and exact producer counts. `bd_native_post_adjustment_preview=1/2` supplies
labelled positive/negative fisheye plus full inversion in native memory only;
it cannot run with the original-parameter comparator. It does not change engine
properties or establish authored effect activation.

## Storage preflight and verification plan

At 15:04, actual available space: 53,656,072,192 bytes (49.97 GiB).
Budget 1 GiB incremental build/link scratch and 4 GiB bounded flat/VR captures;
expected reserve about 45 GiB. Reuse the configured desktop build, shader tools,
native assets and simulator. Devloop/vrsim skills keep verification on desktop.

Retain the last folded lens-flare flat/VR sets as baseline, new normal sequences
as current qualification, and failed/fixed optical preview evidence until the
regression no longer needs it. Earlier pre-fold normal sets are superseded
historical controls, eligible for lossless compression. Isolate runs with hard
links, export only inspected images, and restore the owner's profile after runs.
Unresolved late-scene evidence remains protected. No deletion is planned.

## Build and normal flat check

The Vulkan-only `reblue` target linked at 15:05:03: 47,454,720-byte
`reblue_vk.exe`, embedded base `4e79e8295` with local changes. Codegen was up
to date; no guest translation unit rebuilt. All 26 CTests pass, including
independent shader-literal transcription checks across both signs and three
aspect ratios, inversion endpoints and activation policy. All 24 source
guards pass (11 post, 10 scene, 3 reflection). Emitted SPIR-V confirms push
offsets 24/28/32/36, output-size ratio, both optical branches, layer selection
from ViewIndex, inversion and unchanged sampled alpha. Shared sampler slot 0
was checked as linear-clamp in `device_pipelines.cpp`.

Normal flat log 771, PID 25044, 15:06:01.193-15:08:22.131. Original five
settings audited; full 1673 archives / 119346 names mounted. Native adjustment
preview and comparison off. The 120 hard links in `native_adjust_flat` span
`frame_1788635223_0.raw` to `frame_1788635226_119.raw`, frames 2833-2952,
1920x1080, 8,294,420 bytes each. 0/119 changes over 6%, max 3.29%; no cyan
patches, median 0.011%, max 0.02%. Full-size first/last PNGs inspected: Shu,
his cast silhouette, moving windmill/shadows, foliage, rocks and distant blur.
Last counters: 6341 native, 860 original (858 packed effects/two inputs), zero
tail/state calls; 241 flare frames/3615 sprites. Fisheye/reverse were inactive,
so this control is not active-adjustment image or parameter qualification.

## Negative optical preview

Log 772, PID 25944, 15:08:27.376-15:10:35.625. All six settings audited,
full install mounted. Original flat profile except count 32 and native adjustment
preview 2: fisheye -0.75, reverse strength/pivot 1/1. No engine property writes.
The 32 hard links in `native_adjust_negative_flat` span
`frame_1788635369_0.raw` to `frame_1788635370_31.raw`, frames 2844-2875,
1920x1080, 8,294,420 bytes each. 0/31 changes over 6%, max 3.87%; cyan median
0.150%, max 0.15%, no patch/whole-frame hits. Full-size first/last PNGs inspected:
visible radial magnification/curved edges and colour inversion, while the
character, moving windmill shadows and background remain coherently rendered.
This is synthetic GPU coverage, not a real game event or authored comparison.
Last counters: 5730 native fisheye/reverse frames, 871 original (869 packed
effects/two inputs), zero tail/state calls. Private role-3 scratch was reused
for the observed 1920x1080 and 1280x720 output sizes. Available space before
the following VR launch: 52,373,495,808 bytes (48.78 GiB).

## Positive optical preview in both eyes

Log 773, PID 26292, 15:10:40.545-15:12:46.460. All 17 settings audited and
full install mounted. Native adjustment preview 1: fisheye +0.75, reverse 1/1.
Desktop OpenXR used the checked absolute local simulator manifest, process-local
1440x1584/height-zero runtime settings, XR scale 1.0 and diorama camera mode 2.
Native sun/shadow passes and layered multiview on; legacy stereo, scene-array
capture and mirror off. Capture delay 60, minimum 450, count 32.

The 32 hard links in `native_adjust_positive_vr` span
`frame_1788635502_0.raw` to `frame_1788635504_31.raw`, frames 8374-8405,
1440x3168, 18,247,700 bytes each. 0/31 changes over 6%, max 0.53%; cyan zero.
All four first/last left/right full-size PNGs inspected: smooth radial warping
and inversion fill each eye, with retained foreground/background and moving
windmill/shadow details. Distant blur remains. This synthetic optical stress
case does not qualify normal stereo geometry, VR comfort or authored activation.
Last counters: 5617 native fisheye/reverse frames, 5784 original (5782 packed
effects/two inputs), zero tail/state calls. No flare was visible in this run.

## Normal final-eye control and handoff

Log 774, PID 24052, 15:12:52.876-15:15:41.775. Same 15:05:03 binary; all
16 settings audited, full install mounted, preview/comparison off, otherwise
the same full-size VR configuration with count 120. The complete sequence
finished before the process stopped; the longer capture-write interval is not
a GPU timing result. The 120 hard links in `native_adjust_vr` span
`frame_1788635635_0.raw` to `frame_1788635686_119.raw`, frames 8470-8589,
15:13:55.345-15:14:46.173, 1440x3168, 18,247,700 bytes each.
0/119 changes over 6%, max 0.37%; cyan zero. First/last stereo bands at
44/52/62/72/82/90/95% are -1/-2/-3/-5/-6/-8/-9 pixels: far -1, near -9,
spread 8, correctly crossed depth. All four first/last full-size eye PNGs were
inspected: full native-eye coverage, foreground ground/stairs, distant scenery
and moving windmill geometry/shadows remain; distant blur remains. Shu's cast
shadow is not qualified in this framing. Last counters: 5583 native, 5818
original (5816 packed effects/two inputs), no flare/adjustment activation or
tail/state calls. These short controls do not supersede the late-scene failure.

Logs 771-774 contain none of the checked error/critical/device-loss/VK_ERROR/
exception/assertion markers. Original five-setting profile restored exactly;
all four agent-started app runs stopped, and analysis completed. All builds
and captures used desktop Vulkan, never Quest or Thor. The diagnostic hooks
are implemented but this checkpoint has **no authored active-adjustment
parameter comparisons**: field controls were inactive, previews were synthetic.
Authored gate/payload agreement and real event/transition coverage remain to
be exercised; standalone math and synthetic pixels do not prove those cases.

Final measured available space: 49,553,776,640 bytes (46.15 GiB), within the
preflight budget; net volume usage increased 4,102,295,552 bytes (3.82 GiB)
from the recorded preflight. No deletion, asset copy/cook or build-tree duplication.
Current/baseline normal flat/VR, these two previews, and the failed/fixed lens
previews total about 7.82 GiB of unique raw payload (eight active sets), not
double-counting hard links. New normal captures remain current until the next
qualified post checkpoint; the preceding folded normal sets then become
historical. Effect previews remain regression references until superseded by
equivalent coverage; historical raw sequences are eligible for verified lossless
compression. Separate unresolved late-scene evidence stays protected.

Pre-commit review after the separate storage-guidance commit `ad37d1a` reran
all 26 CTests and 24 source guards successfully. Capture counts, final counters,
checked log markers, the restored profile and the stopped app were rechecked.
No renderer source changed after the verified build and no new capture or
full build was started. Free space rechecked at 49,549,484,032 bytes (46.15 GiB).

Next ownership work: NTSC scanline/noise, intervening/packed filters and dual
masks; native authored light/visibility and image/scene associations; remaining
post/output/UI and full-frame producers. Full game, both-eye event coverage,
late-scene correctness, animation/material/scene ownership and the eventual
Quest gate remain open. This checkpoint removes two more rendering producers
and their console intermediate-resource path, not the entire guest renderer.
