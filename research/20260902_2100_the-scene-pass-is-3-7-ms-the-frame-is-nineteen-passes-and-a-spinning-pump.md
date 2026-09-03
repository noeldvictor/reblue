# The scene pass is 3.7 ms; the frame is nineteen passes, and a spinning pump

2026-09-02, 21:00. Quest 2, `verify_quest.sh` defaults (side-by-side, shadows and
reflections off, cull 350, MSAA off, 60 Hz), builds `reblue_bones.apk` and
`reblue_fencefix.apk`; one render-stage trace; the first sampling profile since
the capture hang.

## The capture hang, first

Every Quest run today stopped at `[capture] wrote`: the log went quiet, the
profile never dumped, the verify's last 20 s and the desktop screenshots after
120 s were a frozen frame. The desktop reproduced it; `cdb -pv -p <pid> -c "~*k"`
on the hung process put the render thread in `AdvanceAndWaitReused ->
waitForCommandFence`. The capture path waits the slot's fence in Present (plume
resets a fence it waits) and then still marked the slot submitted, so the ring
waited the dead fence when it came back round. Fixed in `2d3f8af`; the slot is
marked not submitted after the capture wait. The desktop log now runs on after
the capture.

## The render-stage trace, bones build

`tools/gpu_drawtrace_quest.sh`, window 0, one frame (surfaces 0-18):

| pass | size | mode | ms |
| --- | --- | --- | --- |
| shadow map, shadows off | 64x64 d32 x2 | Direct | 1.17 (0.31 preempt) |
| reflection, reflections off | 128x72 c32 d32 | SwBinning | 1.30 (0.46 preempt) |
| **scene** | 1376x720 c64 d32 | Direct | **3.67** |
| ? | 1376x720 | HwDirect | 0.08 |
| scene copy | 1376x720 c64 | Direct | 0.56 |
| dof downsample chain | 688x360, 344x180, 172x90, 86x45 x2 | Direct | 1.37 |
| bright mask + blurs | 344x180 x3 | Direct | 2.53 (1.06 preempt) |
| composite and after | 1376x720 x4 | Direct | 3.98 (1.07 preempt) |
| present | 1466x768 | Direct | 0.54 |

About 16.3 ms in all, of which 3.0 ms is preemption by the compositor at pass
boundaries, and the scene pass is 22%. Yesterday's mono trace had the scene pass
at 19.5 ms for 535 draws (36 us a draw); today it is 3.67 ms for ~470 draws
(under 8 us a draw). The per-draw ABI cut of the afternoon (four sets,
content-keyed constants, so the VS/PS constant windows stop moving between
draws) is the only change on that path. The scene is lighter than yesterday's
(261 node draws against 535), so the ratio is approximate; the order of
magnitude is not.

**The frame is the pass count now.** Fifteen passes after the scene, each with a
fixed cost around 0.3-1 ms and a preemption slot. Fewer passes is the lever:
scene straight into a texture the composite reads, one composite that writes
the eye-resolution swapchain image with gamma folded in (stage 4/7), and the
shadow and reflection stubs skipped when off.

## The profile

`bd_sample_profiler`, 16 s before the capture at 150 s, symbolised:

- **"SDLThread" (tid 17207) is SDL's event pump, not the guest main loop**:
  `clock_gettime` 24%, mutex lock/unlock 25%, atomics 20%, `SDL_PeepEvents`,
  joystick detection, `Android_WaitLifecycleEvent`. SDL3's `SDL_WaitEvent` on
  Android loops `PumpEvents` + `Android_PumpEvents` whose lifecycle wait returns
  at once while the activity runs: a hard spin, 100% of a big core. The other two
  SDL threads sit in a futex. `threading.cpp` had pinned all three to the big
  cluster as "guest main".
- **The guest's "Draw Thread" (tid 17304) is the frame**: the scene walk, the
  node draws, the constant uploads, Present. 36% in `libreblue.so`, 20% in a
  futex wait, and **20% in `std::recursive_mutex` lock/unlock plus 4.6% in
  `BaseHeap::QueryProtect`** - the SDK heap lock, taken by `bd::mem::try_translate`
  on every checked guest read the host scene code makes.
- The five "Main Thread" workers are the PSO precache compiler threads, idle in
  this window; the XThreads are guest workers, idle.

Fixes (`9e45bc2`, SDK fork `a7f9db4`): `try_translate` keeps a per-thread cache
of pages found readable this frame; the thread policy pins the Draw Thread to the
big cluster and the SDL threads to the little cores; the SDK's Android message
loop polls and sleeps 4 ms instead of spinning.

## Numbers before those fixes (bones build, fence fix)

```
7724 of 8580 frames field; other_ms 18.2, gpu_total_ms 16.9, draws 474, dt 19.1
[node] host-issued 175 of 308 node draws a frame (0 never)
```

CPU and GPU are both just over the 16.7 ms slot. The next run measures the CPU
fixes; the pass count is the GPU work.

## Sources

- `out/device/gpu_drawtrace.txt`, `python tools/gpu_trace_summary.py`
- `out/device/guest_profile.txt`, `python tools/symbolize_profile.py`
- `out/rexglue-src/thirdparty/sdl3/src/events/SDL_events.c` (`SDL_WaitEventTimeoutNS`, the Android branch)
- `out/rexglue-src/src/system/xmemory.cpp` (`BaseHeap::QueryProtect`)
- `/tmp/hang_cdb.txt` (the hung process's stacks)
