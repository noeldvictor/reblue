# Reflection validation lock ordering

2026-09-05, Windows desktop. Follow-up to
`20260905_0144_native-reflection-selection.md`, code checkpoint `f60aef5`.

## Observed failure and source

The normal late run, `reblue_701.log`, PID 21460, ran 02:21:31-02:33:46 EDT.
All five settings were audited: autoplay/perf on, capture delay 270 seconds,
minimum 30 draws and 120 frames. No replay comparison override was enabled.
It stopped advancing at frame 10087 during loading, before any capture files.
The process remained alive and the audio timer continued logging Sleep calls.
Its last reflection report had 440992 checks, zero mismatches, five unsupported
draws, no lookup/null refusals and 1246229 composed bindings, all native.
Those counters do not qualify a stalled frame or the later scene.

A non-invasive CDB thread snapshot at approximately 02:31 EDT was saved to
`out/verification/reflection_701_stacks.txt`. The debugger detached normally.
The relevant stacks were:

- Draw thread 23600 (`5C30`): `DispatchDraw -> HostDrawCapture ->
  ResolveReflectionBinding -> ResolveGuestTexture`, waiting for the mirror mutex.
- File01 thread 21180 (`52BC`): `GetOrCreateNativeMirror -> Build2DMirror ->
  BuildNativeMipTexture -> BuildNativeTexture -> AcquireNativeTextureGpu`,
  waiting for `VideoState::mutex`.
- Other file threads waited for the mirror mutex; the main game thread waited
  in `bdEnqueueRead` while loading a visual/model.

The source closes the lock cycle: `src/gpu/hooks/draw.cpp` takes the video mutex
before calling capture; `native_texture_mirror.cpp` holds its registry mutex
through mirror creation; `native_texture_gpu.cpp` takes the video mutex for
upload. The new capture-time registry lookup inverted that existing order.
This was a confirmed deadlock, not an observation timeout or a GPU-error log.
Only the owned renderer process was stopped after the evidence was collected.

## Correction

Capture now snapshots the selected logical address and actual native binding
while draw state is protected. It does not look up the registry. Node commit,
after the draw hook releases the video mutex and before taking the template
store lock, resolves that captured address and performs the source comparison.
Failure still refuses the complete node before template publication. It does
not disable checks, guess a previous image or turn lock contention into a
successful binding. Pending checks are cleared at commit and new-node reset.

The table selection is fixed at draw time, not recalculated at node commit.
Native handles preserve the actual image through comparison. Dynamic wrappers
remain the existing short-lived compatibility/lifetime boundary; this change
does not replace that boundary or the complete native scene association work.
Replay preflight still resolves current bindings outside the video lock.

## Initial verification

- The host-only renderer built and linked successfully; codegen wrote/deleted
  nothing and no guest objects rebuilt. Version-dependent host objects also
  rebuilt after CMake refreshed the revision stamp.
- Extended material CTest: 1/1 passed (0.04 seconds), including a table-row change
  after selection that must not mutate the captured address.
- Existing texture/upload/state/verification/lighting CTests: 13/13 passed
  (0.62 seconds).
- `python tools/reflection_lock_order_test.py`: two checks passed. These are
  explicitly source-boundary regression guards, not a runtime concurrency proof.

The first two sandboxed build jobs made no compiler progress and were stopped;
the separately authorized builds above completed. A fresh normal late-scene run
of the correction is in progress. Late-scene and VR pixels are not yet qualified
by this checkpoint, and the full desktop host-renderer goal remains open.
