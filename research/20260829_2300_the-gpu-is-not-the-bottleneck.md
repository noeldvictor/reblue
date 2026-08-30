# The GPU does 2ms of a 100ms frame. It is not the GPU.

2026-08-29. Written after a session that went looking for a GPU fill problem and found there is not
one. Five separate experiments, each of which could have shown a GPU cost, all came back negative.
Recorded so nobody spends another session on the same ground.

## The measurement that reframes it

VR off, so no compositor pacing in the way, `render_scale=100`, shadows and reflections off:

```
dt_ms 103.71   fence_ms 73.70   other_ms 34.08   gpu_total_ms 1.99   draws 570
```

We block 73.7ms on the GPU fence while the GPU timestamps say the command buffer executes in 2ms.
That looks like the instrumentation lying, and the first suspicion was the 512-entry timestamp pool
saturating - but `MarkDraw` writes one query per *category change*, not per draw, so it never fills,
and a reserved frame-end query (added this session, `gpu_timing.cpp`) confirmed the total spans the
whole command buffer. plume already scales results by `timestampPeriod`, so the units are right.

**The GPU genuinely does about 2ms of work.**

## Five negative results

| Experiment | Expectation if GPU-bound | Measured |
| --- | --- | --- |
| Scissor to 25% (`bd_debug_fill_scale`), draws and state identical | frame collapses | 116.5 -> 88.3ms, inside the cross-restart band |
| **All colour tile loads and all depth stores forced to `DONT_CARE`** | large win on a tiler | 102-117ms against 100-120ms. **Nothing.** |
| Fold full-area clears into `loadOp = CLEAR` (kept - it is correct) | fewer tile loads | engaged on 16% of 46,000 passes; frame unchanged |
| `XR_EXT_performance_settings` SUSTAINED_HIGH (accepted, `cpu=0 gpu=0`) | clocks rise | cpu4/cpu7 unchanged, frame unchanged |
| `XR_KHR_android_thread_settings` renderer-main registration (accepted) | better scheduling | frame unchanged |

The second row is the important one: it is the *upper bound* on every tile-traffic optimisation at
once, deliberately rendering incorrectly to remove the whole category. It bought nothing. Tile
load/store is not where the time goes on this workload, and the clear-folding work is kept because
it is correct practice, not because it was measured to help.

## The experiment that actually locates it

`bd_cull_distance=1` - almost the entire scene culled away, a handful of draws left:

```
frame 73.3ms = xrWait 60.3 + fence 0.1 + drain 0.4 + elsewhere 12.4
13.6 fps
```

**Drawing nearly nothing still costs 73ms.** Our own frame-loop work is 12.4ms. The remainder is
`xrWaitFrame` blocking, and the frame times land on exact 60Hz compositor tiers - 50.3ms (3x16.67),
102.3ms (6x), 117ms (7x). The compositor paces us several tiers below what our own work needs.

## What is actually consuming the machine

`top -H` on the running process, which had never been looked at:

```
SDLThread          82-100%     Cpus_allowed_list: 4-6
Main Thread (F8..)  ~70% x5    Cpus_allowed_list: 0-7
Audio Worker        51-75%     Cpus_allowed_list: 4-6
Draw Thread          ~10%      Cpus_allowed_list: 4-6
```

Around 500% of 800% total, 34% idle. The renderer (Draw Thread) is nearly idle. **SDLThread - the
guest main thread - is saturated**, and up to five guest worker threads at ~70% each are free to run
on cores 4-6, the same three big cores the runtime pins the render thread to. Those workers are
transient: they appear in some scenes and are gone in others, so a before/after across restarts can
straddle two completely different loads. Do not A/B across them.

This matches `20260829_2200_where-the-cpu-actually-is.md` and does not replace it: the cost is the
recompiled guest, and `bdSceneNodeDrawSingle` with its ~770,000 marshalled memory operations a frame
is still the target.

## The one thing that does not fit, and the hypothesis for it

Render scale halves the frame, reproducibly, back to back in one build:

| `bd_render_scale` | frame | fps | `elsewhere` (our CPU) |
| --- | --- | --- | --- |
| 35 | 50.5ms | 19.8 | 19.3ms |
| 100 | 102-120ms | 8.3-9.8 | 18.3-21.0ms |

Our CPU is flat. The GPU is 2ms either way. Fragments do not explain it (the scissor test) and tile
traffic does not explain it (the `DONT_CARE` test). What is left is **memory bandwidth contention**:
the guest simulation is memory-bound - that is what 770,000 marshalled accesses a frame means - and
the GPU touching eight times as much memory at `rs=100` starves it. That is a hypothesis, not a
result. It predicts that cutting guest memory traffic helps *more* than its instruction count
suggests, which is worth knowing before the rewrite starts.

## Fixed on the way, worth keeping regardless

- **`CpuPause()` on ARM64 was `std::this_thread::yield()`** - a `sched_yield` syscall. The analogue
  of `PAUSE` is the `YIELD` instruction. Spinning on a syscall turns a wait into a scheduler storm.
- **`Sleep_hook` busy-waited the last 1.5ms of every guest sleep**, and the whole millisecond of a
  `Sleep(1)`. Measured: 20,000 calls and 274 seconds of requested sleep in one run. On Android it
  now just sleeps. Neither change moved the frame, and both are still right.
- **The app was not profileable**, so `simpleperf` returned "Permission denied" whatever
  `perf_event_paranoid` said - which is why `tools/profile_quest.py` had never produced a profile.
  `profileable android:shell` is now in the manifest. Horizon OS still refuses shell perf on this
  device, so the tool remains blocked, but the manifest was a real missing piece.
- **`gpu_busy_percentage` reads 99% with the app force-stopped.** The counter is useless on this
  device; do not quote it.

## Sources

- Arm, tile-based rendering and render pass load/store: https://developer.arm.com/documentation/102479/latest/
- `XR_EXT_performance_settings`: https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html
- `XR_KHR_android_thread_settings`: https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html
