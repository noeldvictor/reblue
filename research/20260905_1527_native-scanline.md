# Native scanline filter

2026-09-05, Windows Vulkan desktop, EDT. Base `4d11a1e` plus local changes.
Native producer/pass checkpoint with bounded desktop verification; full renderer
completion and the complete desktop game-coverage gate remain required before Quest.

## Source and ownership

Guest-source guidance: read the existing render-tweaks hook before the complete
translated initializer `sub_82218F30`, producer `sub_82219008`, submission
`sub_822191E0`, buffered-property accessor `sub_8217A040`, random function
`sub_826BF400`, and the outer schedule's optical/NTSC/packed tail. The existing
`bd_pe_ps_ntsc.hlsl` body defines four taps, signed powers 235/159/33/87, and
one-pixel plus diagonal wave offsets. Its `FLT_MIN` macro is negative max float,
not the standard C++ smallest positive value. No decompiler was needed.

The owned XEX was decoded in memory only to verify constants: 1 at 0x820551AC,
0 at 0x82055230, 65536 at 0x8208EE54, and 1/65536 at 0x8203A8A4. The random
producer actually returns 0..32767. Consequently the roll rate is approximately
min(1, 2/max(interval,1)), with phase below .5; the old hook comment's
1/interval claim is not accurate. The native distribution preserves the code,
but uses a deterministic render-frame hash instead of gameplay RNG/TLS. It is
deliberately not identical to the original random sequence. Both eyes share it.

The native plan imports only authored enable/strength/interval values. Actual
image dimensions drive the shader. A private native input is shared with the
optical adjustments; a second private image is needed only when both stages
are active. Native composite/flare -> optional optical pass -> scanline ->
explicit output publication. No seed copy, engine temporary target, per-effect
texture setter, guest producer, old state-308 call or emulated resolve remains
on this supported path. Other original post scopes remain explicitly counted.

`bd_ntsc_filter` still controls noise (default off); disabling noise leaves the
four-tap blur when the authored effect is active. The default-off
`bd_native_scanline_preview` supplies native-only strength 1/interval 4 for
GPU coverage. It cannot run with the original-parameter comparator. That
comparator checks authored strength and exact producer count only; it does
not claim to compare different random sequences or imported console dimensions.

## Storage preflight

Actual free space before implementation/build: 49,549,029,376 bytes (46.15 GiB).
Budget 1 GiB incremental build/link scratch plus 4 GiB bounded verification;
expected reserve about 41 GiB. Reuse the configured desktop target, native
assets and simulator. No asset conversion, downloads or deletion planned.

Retain the preceding normal optical-adjustment flat/VR sequences as baseline,
and new sequences as current qualification. Previous lens normal controls are
now historical, eligible for verified lossless compression. Keep optical and
failed/fixed lens previews until equivalent regression coverage supersedes
them; unresolved late-scene evidence stays protected. This lifecycle keeps
active unique raw payload around 10 GiB without reducing verification counts.
New run isolation uses hard links; export only inspected images and small
reports. Restore temporary profile overrides and stop completed app jobs.

## Verification

The first restricted CMake/Ninja launches stayed live without compiler children
or further output. After checking their exact process trees, only the two
agent-started Ninja children were stopped; both build sessions terminated with
exit 1. A parent identity guard declined a stale parent stop. No tree was wiped.
The same bounded builds with Windows permissions then completed successfully.

Vulkan-only/OpenXR-on/PCH-on `reblue` linked at 15:35:28, 47,465,472 bytes,
embedded `4d11a1e1c` with local changes, Clang 22.1.8. Codegen was up to date;
no guest translation unit rebuilt. All 26 CTests pass. The new source guard
initially matched `rand()` in a historical comment, not a call; after excluding
line comments all 26 source guards pass (13 post, 10 scene, 3 reflection).
CPU cases cover activation, noise off, thresholds and nonfinite intervals,
10,000 deterministic frame phases and 18,432 signed-power offset comparisons
against an independent shader-literal/log2/exp2 transcription (2e-7 tolerance).
Emitted SPIR-V confirms image dimensions, shared push offsets 24/28/32/36,
ViewIndex, signed powers and four same-layer samples, including filtered alpha.

### Normal flat control

Log 775, PID 27416, 15:36:44.691-15:38:53.086. Original five settings audited,
1673 archives / 119346 record names mounted. Same 15:35:28 binary, all previews
and comparisons off. 120 hard links in `out/verification/native_scanline_flat`,
`frame_1788637067_0.raw` through `frame_1788637070_119.raw`, frames 2831-2950,
1920x1080, 8,294,420 bytes each. Capture 15:37:47.094-15:37:50.436.
0/119 changes over 6%, maximum 2.68%; cyan median .011%, maximum .02%, no hits.
First/last full images inspected: Shu and his shadow, moving windmill/ground
shadows, foliage and rocks remain; the existing distant blur remains.
Last counters: 5744 native, 857 original (855 packed scopes/two inputs),
241 flare frames/3615 sprites, no fisheye/reverse/scanline activation or
parameter comparisons. This inactive control does not qualify scanline pixels.

### Noise-disabled flat preview

Log 776, PID 25760, 15:38:59.005-15:40:46.225. All six settings audited, full
install mounted. Original flat settings except count 32 and native scanline
preview on; noise and optical adjustment previews off. 32 hard links in
`out/verification/native_scanline_preview_flat`, `frame_1788637201_0.raw`
through `frame_1788637202_31.raw`, frames 2838-2869, 1920x1080,
8,294,420 bytes each. Capture 15:40:01.292-15:40:02.142. 0/31 changes over 6%,
maximum 3.18%; cyan median .010%, maximum .01%, no hits. Full first/last images
inspected: coherent scene and characters with the intended extra four-tap
softening, moving windmill/shadows retained. Last counters: 4536 native scanline
frames, zero noisy frames, 865 original (863 packed scopes/two inputs),
241 flare frames/3615 sprites. No authored parameter comparisons. Preview is
synthetic, not a real effect event. Free space before VR: 48,284,950,528 bytes
(44.97 GiB). Original noise-off policy remains unchanged.

### Animated combined-effects VR preview

Log 777, PID 25764, 15:40:52.186-15:42:39.707. All 19 settings audited, full
install mounted. Scanline preview on, `bd_ntsc_filter=true`, optical preview 1
(fisheye +.75, full inversion), count 32/minimum 450/delay 60. Native sun and
shadow passes, VR camera mode 2/diorama height 0, multiview/layered textures and
XR scale 1.0 enabled. Legacy stereo, scene-array capture and mirror off.
Process-local simulator manifest has an absolute DLL path; runtime size
1440x1584 with height zero. No global OpenXR changes or device run.

32 hard links in `out/verification/native_scanline_preview_vr`,
`frame_1788637314_0.raw` through `frame_1788637316_31.raw`, frames 8363-8394,
1440x3168, 18,247,700 bytes each. Capture 15:41:54.517-15:41:56.331.
1/31 changes over 6%, maximum 6.33% (0->1); cyan zero. First/last full-size
images in both eyes inspected, plus frame 1 in both eyes and the jump montage
overview. Changing narrow horizontal wave bands over the coherent distorted/
inverted scene are consistent with the enabled animation. This is synthetic
effect coverage, **not** a normal-frame stability pass, authored event or VR
comfort qualification. Original full-size montage hit an image-tool transport
error; its pixels loaded successfully through Pillow, and the smaller overview
plus full-size individual eye images were inspected instead.

Native scratch roles 3 and 4 each allocated once for observed 1440x1584 and
1280x720 sizes, then reused. Last counters: 4735 native scanline/fisheye/reverse
frames, 2359 with nonzero scanline phase; 5766 original (5764 packed/two inputs).
No flare or authored strength comparisons occurred. The optical-before-scanline
ordering and both-image route have GPU coverage; exact random-sequence parity
is deliberately not claimed.

### Normal final-eye control

Log 778, PID 22012, 15:42:44.519-15:44:46.076. Same 15:35:28 executable, all
16 settings audited, full install mounted. Same VR setup, now count 120 and
all previews/comparisons/noise off. 120 hard links in
`out/verification/native_scanline_vr`, `frame_1788637426_0.raw` through
`frame_1788637434_119.raw`, frames 8332-8451, 1440x3168/18,247,700 bytes each.
Capture 15:43:46.851-15:43:54.680. 0/119 changes over 6%, maximum .52%; cyan
zero. First and last stereo checks both exit 0: bands 44/52/62/72/82/90/95%
are -1/-2/-3/-5/-6/-8/-9 pixels, far -1/near -9/spread 8, correctly crossed.
All four first/last full-size eye images inspected: full-height views, near
ground/stairs, distant scenery and moving windmill/shadows remain. Existing
distant blur remains, and this framing does not qualify Shu's cast shadow.
Last counters: 5021 native, 5780 original (5778 packed/two inputs), no optical,
scanline or flare activation and no authored parameter comparisons.

### Handoff and retained evidence

Logs 775-778 contain none of the checked error/critical/device-loss/VK_ERROR/
exception/assertion markers. All four app runs stopped by exact PID/path/start
identity after complete captures; analysis finished. The original five-setting
profile was restored and read back. No Quest/Thor run or timing claim.

Final actual free space: 45,457,014,784 bytes (42.34 GiB). Net volume usage
increased 4,092,014,592 bytes (3.81 GiB), within the preflight budget. No outputs
deleted, assets converted/copied, build tree duplicated or guest objects rebuilt.
Active raw evidence is 9,236,657,280 unique bytes (8.60 GiB): preceding optical
flat/VR baseline, current normal flat/VR, optical and scanline previews, and
failed/fixed lens previews (ten sets). Source capture hard links are not counted
again. Baselines become historical after the next qualified checkpoint; previews
remain until equivalent coverage supersedes them. Historical lens normal sets
are eligible for verified lossless compression. Unresolved late-scene evidence
is separately protected. Only representative eye images and one jump montage
were exported, not every frame as a PNG.

Authored scanline activation/strength comparison, actual effect events and
transition/reload coverage remain unqualified. Other packed/intervening filters,
dual masks, light/visibility and image/scene/property/getter adapters, UI,
animation/material/scene ownership and complete frame scheduling remain. These
short controls do not supersede the earlier late-scene failure or satisfy the
full desktop game/both-eye gate. The native scope has removed its last trailing
guest filter wrapper and state setters, not the game's whole renderer.
