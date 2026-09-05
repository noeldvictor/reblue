# Host deferred allocation and ordering, 2026-09-04

## Ownership change

Deferred arena planning and depth ordering now execute in host C++, not the
recompiled allocation/sort functions. The SDK-independent `deferred_work.h`
operates on bounded byte offsets and `(depth, payload index)` records, with no
guest addresses, shader registers or GPU dependency. The standalone core and
tests were committed and pushed as `af20811` before runtime integration.

`deferred_list.cpp` is an explicitly temporary entry bridge. The remaining
guest draw loop still consumes its pool, pointer array and big-endian entry
images. This is not yet a native deferred draw producer, scene asset format or
host frame scheduler. Depth keys are still imported from those entries; replay
still retains their captured depth and most other fields. Replacement of that
data producer, visual/pass setup and the whole list consumer remains required.

The host sorter orders greater depth first, with deterministic submission order
for equal depths. The former quicksort could arbitrarily swap equal keys. This
tie-order policy change is intentional; it is not claimed bit-identical to the
old sort. NaN/infinite keys refuse the operation before publication and leave
submission order intact. `bd_native_deferred_order` defaults on; off exists only
for correctness comparisons, not as a performance-based alternative.

Allocation validates both byte and item capacity, aligned offsets, pointer-array
cursor consistency and every touched page before publishing. A captured node's
entire deferred batch is planned before copying; it cannot append only half a
batch. Mixed direct/deferred nodes check the batch before issuing direct draws.
The single-entry allocator hook also runs host code for interpreted producers.
The bridge still uses the engine's storage; these counters do not prove native
scene storage or fully host-owned frames.

## Source evidence and relocation correction

Read the exact translated C++/PPC comments and `config/hooks/render_list.toml`.
No generated source or hook TOML was edited.

- `generated/reblue_recomp.13.cpp`, `bdInitInputSystem`: initializes global
  list `0x82DBA8F8`, a 4194304-byte pool and 20560-byte pointer array (5140
  entries). The pool and pointer-array capacities are separate constraints.
- `generated/reblue_recomp.25.cpp`, `sub_8227DB50`: advances pool cursor,
  appends the entry pointer, records the last allocation, and increments count.
  Its old check covered pool bytes, not pointer-array capacity. The host hook
  replaces this function; runtime callers no longer need its guest stack frame.
- `generated/reblue_recomp.10.cpp`, `sub_8227F290`: sorts by descending float
  depth at entry +276. The host hook imports each key once and publishes only
  after sorting succeeds, with no recursive guest memory round trips.
- `generated/reblue_recomp.84.cpp`, `sub_8227F360`: calls the sorter conditionally,
  switches visuals, executes material callbacks, issues draws and resets the
  list. All but sorting remains a tracked consumer dependency.
- `generated/reblue_recomp.40.cpp`, `bdSceneNodeDrawSingle`, list build starting
  at `0x82280A68`: allocates `(204 + bones) * 4` bytes; world is at +16, palette
  +268, bone count +289, bone indices +800. It stores `entry + 388` into +264,
  the material-state pointer inside the callback record starting at +240.
- The previous host replay copied the entire old image but refreshed only
  world and palette. Thus +264 still pointed into the original pooled slot.
  The new `deferred_entry_bridge.h` relocates that self-reference to the new
  destination. This is an observed source-level defect, not yet a claim that
  it caused every recorded visual failure.
- `generated/reblue_recomp.41.cpp`, `sub_8227EFC8`: computes the depth key from
  object bounds, world and view transforms, or a fixed key for special cases.
  Replacing that producer and refreshing deferred depth is still outstanding.

## Verification

Reused the configured desktop Vulkan-only `reblue` target, OpenXR/PCH on,
Clang 22.1.8. Incremental builds completed without guest translation units
rebuilding; codegen reported zero writes. The relocation helper was extracted
after the first flat run, with unchanged successful-write behavior, then rebuilt.

The five standalone texture/lifetime/binding/upload/deferred tests pass.
Deferred tests run assertions even in Release and cover 5140 shuffled items,
back-to-front order, deterministic equal-depth order, unique payload retention,
nonfinite-key transactional rejection, exact-fit batches, item/byte exhaustion,
invalid alignment, arithmetic overflow, unchanged output on failure, and empty
work. Entry tests cover all preserved bytes, fresh world/palette, +264 rebasing,
invalid destination ranges and truncated/inconsistent bone payloads.

Material tests (1/1), mesh tests (1/1) and stereo-check utility tests (2/2) also
pass. These utility tests do not establish depth in the runtime VR capture.

### Flat sequence, log 657

Launched at 20:50:31 EDT with the original five profile settings: autoplay and
perf CSV on, 60-second capture delay, minimum 600 draws, 120 frames. All five
were audited as applied. Sequence 119 completed at 20:51:37, frame 2962.
Isolated output: `out/verification/host_deferred_flat`, 1920x1080.

`capture_seq.py`: 0/119 pairs above 6%. `capture_cyan.py`: no cyan patches,
median 0.012%, maximum 0.02%. Actual first/last frames were inspected: character,
terrain, vegetation and shadows intact. At the last periodic report: 1865307
entries allocated in 1248860 host batches, 6900 lists / 369351 items ordered,
zero refusals. These counts include repeated scene work, not unique assets.
The exact renderer process was stopped and confirmed exited at 20:53:03.

### Multiview sequence, log 658

Launched at 20:54:08 EDT. All 13 settings applied: autoplay/perf CSV, capture
after 60 seconds with minimum 450 draws and 120 frames; VR and multiview on,
legacy stereo off, layered textures on, scene-array capture off, mirror off,
camera mode 2 and diorama height 0. Process-local simulator settings used
1440x1584 recommendations and height 0. Instance, session, 936x1030x2 swapchain
and the actively composed camera pose were confirmed in the log.

Sequence 119 completed at 20:55:15, frame 21037. Isolated output:
`out/verification/host_deferred_vr`, final stacked 936x2060 eyes. 10/119 pairs
exceed 6%, at destination frames 40, 41, 43, 44, 46 and 104, 105, 107, 108, 110.
That retains the 64-frame cadence; native sorting and self-pointer relocation
do not solve it. No cyan patches. Inspected both eyes of frames 0/16/17:
distant blurred ground and horizontal banding remain visible.
Frames 40/41 were also inspected across a detected jump: both eyes change
between the more legible ground plane and the banded/blurred result.

Stereo check returned exit 2, INCONCLUSIVE: only the 44% image band had a
bounded match (-1 pixel); fewer than two textured depth bands were usable.
There is no stereo-depth or headset qualification. Last periodic report:
5823951 entries allocated in 4957209 host batches, 6900 lists / 3042606 items
ordered, zero refusals. No error/critical, Vulkan-error, overflow or
retirement-race matches in either short-run log. The exact renderer process
was stopped and confirmed exited at 20:56:21.

After this run, the final build/test pass additionally tightened planner
alignment validation and parenthesized the destination offset subtraction
before pointer addition. This avoids an out-of-range intermediate pointer;
successful entry bytes are unchanged. The later run below uses that build.

### Later-scene verification

The final binary launched at 20:57:12, log 659, with autoplay/perf CSV and
270-second capture delay, minimum 30 draws, 120 frames; all five settings took
effect. The recorder held at the threshold while the transition had only 20
draws. Sequence 119 completed at 21:02:42, frame 14799. Isolated output:
`out/verification/host_deferred_late`, 120 frames at 1920x1080.

79/119 pairs exceed 6%; no cyan patches (median 0%, maximum 0.01%). Actual
frames 0/44/45/119 were inspected: nearly empty dark frames, damaged text,
block-like black/red silhouettes and badly incomplete scenery persist. This
is the same class of failure as the pre-change late baseline (77/119 jumps),
not a successful later-scene qualification. The difference of two flagged
pairs is not a measured improvement/regression attribution across differently
timed scene captures. The stale self-reference was real, but fixing it did
not fix the whole visual failure.

The last periodic report recorded 3487233 allocated entries, 2513539 host
batches, 13213 ordered lists / 765452 items, zero allocation/sort refusals.
Host uploads peaked at 37748736 reserved bytes / 26362624 in one slot, with
zero failures; shader constants peaked at 4460384 of 33554432 bytes. No
error/critical, Vulkan-error, overflow or retirement-race matches were present.
The failing pixels remain authoritative despite those clean counters.

All three renderer processes have been stopped with exact-path validation
and process-exit confirmation; the final one exited at 21:03:24. The original
five-setting profile is restored (60 seconds, minimum 600 draws, 120 frames,
autoplay/perf CSV enabled). No generated source, game data, saves, derived
assets, captures, binaries or profiles are committed. Implementation was
pushed as `8cc274f`; this final evidence is a separate documentation checkpoint.

The complete host renderer, representative scene coverage, both-eye depth and
animated-effects verification remain incomplete. No Quest run or performance
claim was made.

## Next producer boundary

`sub_8227EFC8` receives entry, world matrix, mesh and a fixed-depth selector in
r3/r4/r5/r6. For the ordinary path it multiplies world and the current view
matrix, transforms the mesh sphere centre at +20/+24/+28, adds radius +32 to
view-space Z and negates it. The view matrix is built from the global base
`(lis -32034, addi -19936) + 65536 - 10816`. Its vector dot products are emitted
as `simde_mm_dp_ps`; preserve their arithmetic ordering when comparing a host
implementation. Special/fixed-depth cases must be represented explicitly, not
inferred from an old numeric key. Host object bounds and live transforms should
produce depth for both initial submission and subsequent replay; sorting a
captured key is not that conversion. This remains unimplemented here.
