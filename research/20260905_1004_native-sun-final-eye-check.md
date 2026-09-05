# Native sun camera: final-eye check remains unqualified

2026-09-05, Windows Vulkan desktop, EDT. Follows pushed character-caster fix
`9cd34a9` and `20260905_0956_native-sun-character-visibility.md`.
`bd_native_sun_camera` remains **off by default**. This records verification,
not another rendering change or a complete native camera/shadow conversion.

## Configuration and run

The vrsim skill directed the desktop OpenXR check. PID 16020 ran hidden from
09:59:32 to 10:00:53.149, log `reblue_746.log`. It used the same 09:42:38 /
47391744-byte executable as the fixed-camera flat run: its embedded version
still reports base `3e5c756` plus local modifications, now committed as
`9cd34a9`. There was no rebuild merely to update that metadata.

All sixteen profile settings audited successfully: autoplay and perf CSV on;
capture delay 60 seconds, minimum 450 draws, 120 frames; native sun camera and
native shadow passes on; VR on; legacy stereo off; multiview and layered
textures on; scene-array capture and mirror off; camera mode 2; diorama height
0; XR render scale 1.0. Shadow-fit diagnostics/original cull comparisons are off.
The full install mounted 1673 archives / 119346 record names.

Process-local `XR_RUNTIME_JSON` named the absolute
`out/xrsim-build/reblue_xrsim.json`, whose runtime path is also absolute and
names the verified 31232-byte DLL. `XRSIM_WIDTH=1440`,
`XRSIM_HEIGHT_PX=1584`, `XRSIM_HEIGHT=0`. OpenXR created a session and reported
1440x1584 per eye. Logged eye positions differ from the game camera, confirming
an active override. No machine-wide runtime setting, Quest or Thor was used.

## Actual pixels and sequence

All 120 final-eye captures were isolated after stopping the verified renderer
in `out/verification/native_sun_character_fix_vr`. Files are
`frame_1788616834_0.raw` through `frame_1788616843_119.raw`, frames
12852-12971, 10:00:34.470-10:00:43.126. Each is 1440x3168: two stacked
1440x1584 final eyes, not the scene-array diagnostic capture.

`capture_seq.py` flags **10/119 pairs above 6%**, all from 84-85 through 93-94;
the largest is 31.13% at 87-88. Cyan analysis finds no patches or whole-cyan
frames, with zero measured median/maximum cyan. The first preview, both
full-resolution last eyes and jump images 85/88/91/94 were actually inspected.
They show rocky scenery/orange sky with heavy blur and black bars; a large
dark/blue foreground object sweeps through both eyes during the flagged
cluster. The view is open again afterward. This is not by itself proof of
corrupted geometry or a shadow regression, and it is not a stable-sequence pass.
The cause/expectedness of that foreground passage has not been isolated.

Both `stereo_check.py --raw ... --stacked` calls return **INCONCLUSIVE**
(exit 2). The first has only the 52% band matched, at -1 pixel; the last has
no qualifying bands. Black bars, uniform sky and blurred distant content
cannot establish stereo depth. The first exit 2 deliberately stopped the
chained analysis command; the last-eye test was then run separately. Both
stereo analyzer regression tests pass; that does not upgrade the live verdict.

The 4096-square shadow attachment and scene attachments have two layers.
Scene content remains **1440x808**, despite full-size final eye layers. Shu's
cast silhouette cannot be qualified in this distant framing. These pixels
do not justify enabling the experimental camera or claiming VR correctness.

## Counters and limits

Last sampled totals: 13201 native sun fits/snapshots, zero inactive/refused;
13201 shadow begins / 13200 ends, 13200 explicit outputs, 10617 empty clears
and **103019 matching attachment checks**, with no compatibility/lifecycle
fallback or null output. Original camera snapshot/light-fit and cull-comparison
calls are zero. Native culling tests 2322764 objects, admits 2322577, and skips
2580 obsolete character light-eye cutoffs. These sampled totals include loading
and are not balanced shutdown counts or full-frame guest-removal proof.

Main-scene ownership has 103018 matching checks and no compatibility/refusal;
native views report 29120 productions, no compatibility/refusal, matrix import
or cache bootstrap. No error, critical, assertion, fatal, device-lost, VK_ERROR
or exhaustion marker was found in the checked log patterns. Zero error markers
and ownership mismatches do not qualify the images.

All capture analysis happened after the renderer stopped. The original five
profile settings were restored exactly. No asset/cache, dependency or shader
was changed, and no headset performance improvement is claimed. A useful next
camera gate needs correctly framed, readable near/far geometry and character
shadows in both eyes, followed by broader desktop mode coverage. Later
scenery/text failures and the complete host-renderer acceptance gate remain.

## Default-off control on the hooked binary

The restored original profile was also run on this same 09:42:38 binary, so
the published default path is checked after adding the instruction adapter,
not merely on the older pre-hook control. PID 23172 ran hidden
10:03:45-10:06:00.368, log `reblue_747.log`. All five settings audited:
autoplay/perf on, delay 60, minimum 600 draws and 120 captures. Native sun,
VR and diagnostics remain off. The full archive/name mount succeeded.

The 120 1920x1080 frames are isolated in
`out/verification/native_sun_character_fix_default`,
`frame_1788617088_0.raw` through `frame_1788617091_119.raw`, captured
10:04:48-10:04:51. Analysis finds 0/119 jumps over 6%, no cyan patches,
median cyan 0.011%, maximum 0.02%. Actual first/last full-resolution images
show Shu and his cast silhouette, with the moving windmill shadow overlapping
in the first. This is a short default-path control, not full-game qualification.

Last sampled totals: 6901 shadow begins / 6900 ends, 6900 explicit outputs,
865 empty clears and 42266 matching ownership checks, with no lifecycle
fallback, refusal or null output. Engine snapshots/light fits are 6901 each.
Native sun sphere tests, comparisons and the new character-cutoff bypass all
remain zero. No error/critical/assertion/fatal/device-lost/VK_ERROR/exhaustion
marker was found. The process was stopped by verified path/PID/start time
before capture analysis. No renderer/replay process remains running and the
original five-setting profile remains restored.
