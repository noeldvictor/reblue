# Host-owned targets, and the cyan skirt is the instance-record mask

2026-09-03, 19:00. Desktop only (owner: no Quest runs until the frame is host-owned).

## Stage 4: the host owns the shadow map, the scene pair and the reflection pair

`gpu/host_targets.{h,cpp}` (`bd_host_targets`, default on). The guest's `CreateSurface` is
classified from its arguments alone: a square depth surface is the sun shadow map (4096 on the
desktop, the 64x64 stub with shadows off); the multisample-requested pair is the scene; the small
8-bit colour + non-square depth pair is the water reflection view. Each class has one persistent
host surface, created on first use and recreated when the requested size changes (the cutscene
switch 1920x1080 -> 1280x720 -> back was seen and handled). The guest's `Release` of the handle
frees nothing: it is released in the Release hook itself, not through the fenced destroy queue,
because that queue drains one or two frames later and the guest's next `CreateSurface` came
first, so the target was handed out only every other frame (the pooled fallback logged
"requested while the guest still holds it"). A host target keeps its own clear until its pass
binds (`HostTargetApplyClears`, right after `setFramebuffer`), so the guest's clear never touches
plume's framebuffer state; its resolve links are dropped at the clear instead of copied
(`HostTargetDropLinks`), and a link still present at the bind means the guest draws into the
target again after resolving it, where the old materialise copy still applies. A host target is
never an alias root, never seeded, and never a resolve fallback: an undrawn host target resolves
as a link to the previous frame's image, which is what the guest's frame-start depth grab is
after.

Verified on the desktop (`PLUME_FB_TRACE`, perf CSV medians of field frames, a 30-frame capture
sequence all real scenes):

| | pool (before) | host targets |
| --- | --- | --- |
| passes a frame | 18 | 17 |
| held clears flushed as a zero-draw pass | 1 | 0 |
| plume held clears a frame | 5 | 1 |
| `rs_seed` | 1 | 0 |
| `fb_binds` | 10 | 9 |
| `barrier_calls` | 37 | 34 |
| `rs_eager` (the two desktop MSAA resolves) | 2 | 2 |
| `rs_materialize` (the tail's two 2D-overlay blits) | 2 | 2 |

The zero-draw clear pass was an ordering bug that predates the targets and is fixed for both
arms: `TransitionResolveSources` flipped the still-bound framebuffer's target to a read layout
(a stale texture slot named it) before the queued draws against that framebuffer went out, and
plume ran the target's held clear as a pass of its own ahead of them. It now skips the bound
targets on the first pass and runs again after the queue flush, at the pass boundary.

The two materialise copies left are the tail's blits for the 2D overlay (composite | blit | 2D
| blit | 2D | present), stage 2's input-attachment item; the two eager copies are the desktop's
MSAA resolves.

## The cyan skirt: not the game's streaming

The 17:10 verdict ("the game's own streaming") was wrong. The within-run A/B that the ledger
diff never had: `bd_ab_flag = "bd_host_draw"`, period 20, over a 240-frame capture sequence at
the village rock: **38 of 120 frames with the patch on the replay arm, 2 of 120 on the
interpreter arm, both of those on arm boundaries.** The host walk A/B (same shape) showed the
patch on both arms; the list build A/B likewise. The patch alternates between frames because the
replay and the interpreter alternate per node ("one node per visual per frame is interpreted"),
which the draw ledger showed as the same 775 draws with paths swapped, not missing draws.

### The replay verifier (`bd_host_draw_verify`)

Built after the owner asked for one instrument instead of an A/B per guess. A node the replay
would issue is composed by the replay exactly as it would be dispatched and kept; the
interpreter then runs the node anyway, and every interpreted sub-draw is diffed at capture
against the replay's composition: vertex and pixel blocks per register, fetch constants, texture
slots (set and inherited-with-a-configured-sampler), pipeline state, geometry, bool constants.
`[verify]` lines name the mismatches; a histogram per register prints every 300 frames.

First report, 1.4 million draws: world rows, geometry, pipeline state and fetch constants never
differ. What did:

- **VS c1 and PS c1, the eye position: the camera in the interpreter, zero in the replay, for
  the reflection view's render-list draws (441 draws).** The render-list loop writes the camera
  block c0-c4 on the first entry of a visual in a pass and not on the next (the delta merge's
  comment already said so), so a replayed entry composed it from the live block. Fixed: the pass
  camera (VS c0-c1, PS c0-c1) is recorded per render view from each frame's interpreted draws
  and applied to every replayed draw; a replay before the pass has one this frame interprets
  (`why_pass`, one a frame). Taking VS c2-c4 from the pass too was wrong (they are the node's:
  3,352 draws in the next report) and was narrowed to c0-c1.
- **PS c9, a screen-size constant, 480x270 in a template captured before a resolution change
  against 320x180 now (492 draws).** Fixed: a stable delta register the visual's interpreted node
  wrote differently this frame invalidates the template (`why_drift`, ~30 a frame).
- **Bool constants, VS bit 30 and PS bit 5 (503 draws):** the template's copy is from the capture
  frame. Taking the visual's fresh bools instead was wrong (118,737 draws: they are the node's).
  Open; small.
- VS c36-c38 (the fitted shadow matrix, 6-25 draws: the fit's camera lags a frame at a cut),
  c57 (foliage, 9), c2/c3 at 0.001 (animated UV offsets).

With those in, the scene and reflection views compose identically and **the patch is still
there**, which is the verifier's own limit: it compares what the replay composes, not what the
draw queue does with it afterwards.

### Pixel history, and what the queue does

`tools/rdc_pixel_history.py` (RenderDoc's python, `RDC_XY=x,y;...`) lists every event that
touched a pixel of the scene target and the presented image. On a clean frame the ground at the
patch's pixels is written by an indirect, instanced draw (`vkCmdDrawIndexedIndirect`), then a
blended decal; a patch frame is one where that draw leaves the clear colour. RenderDoc's own
presence hides the patch (two ten-frame captures at the same second, none with it), so the
capture never caught a patch frame; `bd_renderdoc_frames` captures N consecutive frames for the
next time.

The replay and interpreter alternation changes which draws are consecutive, so the instancing
groups differ from frame to frame. **`bd_record_mask = false` gives 0 of 60 frames with the
patch** (the same second, the rock scene, against 21-49 of 60 with it on in the runs before and
after). The mask's producer (`CommitInstanceRecords`, memcmp against the group's first staged
block), the window it binds (`UploadVertexBlockFromStaged`, byte-identical on a hash hit and read
back from the ring after a fresh upload), the shader's decode (`BD_VSC`, `mask[reg >> 7][(reg >>
5) & 3]`, bit `reg & 31`, record stride 4128) and the instance index (`SV_InstanceID` as
`InstanceIndex`, assigned before any pull or constant read) all check out. The split modes
(`bd_record_mask_mode` 2: window rebound, masks all ones; 3: masks, window not rebound) both read
clean in their first runs, and their repeat runs landed in the intro cutscene and say nothing.
**The mask is off by default until this is named**; every record then carries its whole block,
which on the Quest was 28 ms against 19.5 with every scene draw on records (2026-09-02), so it
has to come back.

### Instruments this left behind

- `bd_host_draw_verify` and its `[verify]` lines (above).
- `bd_draw_ledger` now fingerprints every draw (pixel block, vertex block without c32-c39,
  texture slots + pipeline, pixel shader hash), and the capture log names each sequence frame.
- `tools/rdc_pixel_history.py`; `bd_renderdoc_frames`.
- `bd_record_mask_mode`; `[records]` warnings for a window that does not hold its block and a
  record past the staging list (neither fired).
- The cyan detector must be read against the lower part of the frame and a look at the image:
  the intro cutscene and any zenith view read as "cyan" over the whole frame, and today's
  autoplay landed there in three runs out of the last four.

Sources: `src/gpu/host_targets.cpp`, `src/gpu/draw_framebuffer.cpp`, `src/gpu/scene/host_draw.cpp`
(`VerifyAgainstReplay`, `PassRegs`), `src/gpu/constant_buffers.cpp` (`CommitInstanceRecords`),
`src/gpu/draw_queue.cpp`, `tools/rdc_pixel_history.py`; logs of 2026-09-03 17:00-19:00 in
`out/build/win-amd64-release/logs/`.

## Addendum (20:30): the mask is innocent; the skirt is fixed

The mask-off run's clean sequence was one run's luck. With the mask on and the same scene,
runs alternated between clean and 15-76 patch frames of 120, and the patch's on/off pattern
followed the template refresh cadence (`bd_host_draw_refresh` 16), i.e. which entries were
interpreted that frame. The last asymmetry was outside the composition the verifier checks: the
guest's own state machine around a replayed entry.

`sub_8227F360`'s loop (generated/reblue_recomp.84.cpp, `loc_8227F520`) keeps the current visual
in `r23`: `cmplw cr6, r23, entry+272`; equal skips to the draw setup, different calls
`sub_8221DCA0` (end the previous visual) and sets `r23 = entry+272` before the visual's own
setup (constant block, render states, `SetVertexShaderConstantB`/`SetPixelShaderConstantB`).
The host's midasm hook at `0x8227F524` jumps to the loop tail for a replayed entry, so `r23`
kept the previous visual and the next interpreted entry of the replayed visual either skipped a
switch it needed or performed one the host state did not match. Fix: the hook carries `r23` and
an entry replays only when `r23 == entry.visual`; the first entry of every run of a visual is
the guest's, and every switch is the guest's own.

Two more replay corrections, both named by the verifier: the replay's bool constants come from
the live device (the pass and visual bits the guest toggles; the template's copy was the capture
frame's, 503 draws wrong) with the node's foliage bit applied; and the host state is restored
after a replay as before. Writing the replay's composition back into the guest's device block
was tried and reverted: it produced a *persistent* flat patch (the guest inherits bools it
believes it set), which is what showed the bools were the lever.

Result, `bd_capture_frames = 120` at the rock, lower-40% cyan metric: 0, 0, 0 patch frames in
three runs (one run had 49 consecutive uniform sky-blue frames, the zenith sweep, also seen
before the fix). Before: 53, 31, 15, 76 of 120 in the runs of the previous hour.

Corrections to the section above: "the cyan skirt is the instance-record mask" is withdrawn;
`bd_record_mask` is on again. The record-mask split modes and the `[records]` group dump stay
as diagnostics; the group dump showed groups whose members' blocks are identical (the same mesh
at the same transform, two to twelve times), which is a separate question for the queue.

## Addendum (23:30): three artefacts, two fixed, the patch still open

The evening's sequence runs separated what the eye read as one flicker:

1. **Whole-frame flashes** of the clear colour, 6-58 consecutive frames, with every draw of a
   normal frame in the ledger: `CopySurfaceToTextureLocked` recorded the resolve copy without
   flushing the deferred draw queue, so a frame whose scene draws were all still queued at the
   guest's Resolve copied the cleared target. Fixed (a `DrawQueueFlushAt` before the copy);
   zero uniform frames in 960 since.
2. **A one-frame change every sixteen frames** (`bd_host_draw_refresh`), which the user saw as
   flicker at the rock's edges and the top of the screen: the ledger's shadow-view diff around a
   refresh frame showed one visual's render-list entry present only in the refresh frame and
   every later entry one slot earlier. The DrawSingle hook built a replayed node's list entries
   only when its draw replay had been refused, so a node with both direct draws and a list part
   lost the list part on every replayed frame (the ground light at the rock). Fixed: both parts
   replay together (`HostDrawHasDrawTemplate`, `HostListBuildStatus`) or the whole node
   interprets and both are captured; neighbour jumps over 5 went from 7-12 per 240 frames to 2.
3. **The flat cyan polygon at the rock's base** is still open. What is established by
   within-run A/Bs: it needs the host replay (38 of 120 against 2); it appears on both arms of
   `bd_draw_indirect` (44 against 75); the record-mask and list-draw A/Bs happened to land in
   runs without it. What was believed and is withdrawn: the single-run "mode" verdicts
   (indirect, mask, mask high) - the artefact is present in about half the runs and, when
   present, in about half the frames, so a single 240-frame run reading zero means nothing.
   The composition verifier sees no difference, the ledger sees the same draws with paths
   swapped, and RenderDoc never captures a frame with it (three ten-frame captures). The next
   instrument is a draw-id render so a plain capture names the draw that paints it.

`bd_record_mask_high` (default off) keeps registers c64 and up in the record, which one run
read clean and which costs nothing measured on the desktop; it goes back on with the Quest
measurement if the patch is named elsewhere.
