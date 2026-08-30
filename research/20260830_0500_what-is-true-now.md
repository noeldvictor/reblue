# What is true now, after a day of correcting what was not

2026-08-30. One session produced about twenty commits, six corrections and four reverted ideas,
spread across four notes. This is the consolidated state, so the next attempt does not have to
reconstruct it from the log. Where this disagrees with an older note, this is later.

## The headline

**Nothing in this project could be measured reliably until today, and several standing conclusions
were wrong as a result.** The work that matters from this session is the instrumentation; the
performance wins are real but small, and two of the three I first reported did not survive being
measured properly.

## Corrected: things this repo believed that are false

| Was believed | Actually |
| --- | --- |
| The frame is fill-bound; the GPU half is the problem | The GPU does ~5.8ms of a 16.7ms desktop frame. Cutting fragments to a sixteenth barely moves it, and forcing every tile load and depth store to `DONT_CARE` moves nothing at all |
| `gpu_total_ms` reads ~2ms | That was **stale**. `vkGetQueryPoolResults` failed 6,913 times a run and plume reused the previous frame's results. Fixed; the real figure is 5.83ms |
| `bdSceneNodeDrawSingle` is 23x everything else | It is ~5% of samples. The census counts *calls*, and cheap calls made often are not the same claim |
| Random encounters end autoplay runs early | `bdBattleEncounterBegin` is called **zero** times in a walking run. The low-draw frames are menus |
| Take the last few hundred CSV rows | The tail is a menu the run gets stuck in. Select field frames by draw count instead |
| `bd_render_scale=25` is the best config | It reads as "blurry gibberish" through the lenses. Readability first |

## Corrected: things I claimed today that were also wrong

Recorded because the pattern matters more than the individual errors.

- **`bd_host_sincos` is worth a third of the frame.** No: two back-to-back reversed pairs said so and
  a third pair minutes later read 5.12 / 5.18 / 8.62ms with the *same* configuration. It was drift.
  Within-run A/B says **+2.9%, slower**. Default off.
- **The cull redirect is -18%.** Real, but **-5.6%** when measured within a run.
- **Field filtering gives a 0.4% noise floor.** No - that was two adjacent runs agreeing. The same
  binary has measured 68% apart. Only within-run A/B settles anything under ~50%.
- **Multiview: the scene is not rendered into a layered surface.** Wrong; it binds layered colour and
  depth. That conclusion came from a log capped at ten entries which something else had spent - the
  *third* wrong multiview conclusion from a capped counter in one investigation.

**The rule that came out of it: a bounded log answers "what happened first", never "what happens".**
Count into a total and print the total, or filter to the case you care about before you cap.

## What is measured and true

- **The GPU is not the bottleneck on desktop** - but it is a third of the frame, not a rounding
  error. `gpu_draw 4.54ms (78%)`, `gpu_resolve 1.12ms (19%)`, `gpu_inter 0.16ms`.
- **19% of GPU time is the EDRAM resolve category**, and within it the cost is *seeding*:
  `SeedFreshColorTarget` does 14 full-surface copies a frame to reproduce the persistence of a tile
  buffer that does not exist. Measured at **0.42ms of GPU a frame** by within-run A/B.
  Dead-resolve elimination already works - 17 of 21 resolves never execute.
- **~19% of CPU was overhead, not game**: `Sleep_hook` busy-waiting a 1.5ms guard band (15.9% of all
  samples) and `NoteDrawPhases` per-draw timing (3.4%). Both removed or gated.
- **The cull redirect is worth -5.6%** and is on by default.
- **Stereo has depth** and is correctly signed: `far +4, near -7, near - far = -11px`.

## The tools, which are the actual output

| | |
| --- | --- |
| `bd_sample_profiler` + `tools/symbolize_profile.py` | A sampling profiler that works **on a Quest**, where `simpleperf` cannot attach at all. Also works on desktop. Symbolises offline against the unstripped binary |
| `bd_ab_flag` / `bd_ab_period` + `tools/perf_summary.py` | Within-run A/B. Flips a cvar every N frames, labels each frame, compares two populations from one run. **The only thing that resolves a sub-50% change here** |
| `tools/perf_summary.py` | Field-frame selection - ~9,600 frames a run instead of 300, and not the menu the tail window was sampling |
| `tools/stereo_check.py --raw` | Stereo depth verdict from a capture on disk, no device |
| `bd_mv_capture_array` | Photographs a multiview array's layers instead of inferring their contents |
| `tools/verify_quest.sh` | The whole device measurement in one command, with every adb trap encoded |

## What to do next, in order

1. **Put the Quest 2 on USB and run `bash tools/verify_quest.sh`.** Nothing in this session is
   measured on ARM64, and the relative costs differ - CLAUDE.md already records
   `bdSceneNodeDrawSingle` measuring 23x on device against 1.9x on desktop. Every number above is a
   desktop number.
2. **Multiview.** The array is empty - both layers, max pixel value zero - while pipelines,
   framebuffers, view masks, the device feature, the resolve and the SRVs are all verified correct.
   Nine hypotheses checked, nine correct.

   The obvious tool is **Vulkan validation layers**, which this project has already used to settle a
   multiview question in one run after three sessions of inference. Two obstacles, both checked:
   they are not installed on this machine, and the Khronos
   `Vulkan-ValidationLayers` releases publish **Android binaries only** - `android-binaries-*.zip`,
   nothing for Windows. Desktop layers come inside the Vulkan SDK installer, which is a deliberate
   decision to make rather than a download. On Android the existing route works:
   `EXTRA_LIBS=/path/to/libVkLayer_khronos_validation.so bash tools/build_apk.sh`, which is a reason
   to do this on the Quest rather than the desktop.

   Without them, the discriminating test is a **host draw into the layered scene target** with a
   multiview pipeline - the resolve already proves host draws into a *single-layer* target work. If
   a known-good host draw into the layered one also produces nothing, the fault is the multiview
   render path itself; if it appears, the fault is in the guest pipelines or state feeding it. That
   separates the two halves without any layer.
3. **The guest CPU**, which is still the largest share and still barely understood. Start from an
   on-device profile, not a desktop one.
4. **Seeding**, worth 0.42ms of GPU. The provable case - a pending clear that overwrites the surface
   - never fires, so this needs to know whether the coming draws cover the target.

## The one habit worth keeping

Everything found today came from making something visible that was not: a profiler where none could
attach, field frames instead of a tail, a photograph of an array instead of a deduction, and -
last and most embarrassing - `stderr`, which every run in this session sent to `/dev/null` while
plume printed the query-pool failure 6,913 times a run and `multiview feature ENABLED` right
alongside it.

**Read the output you already have before adding more.**
