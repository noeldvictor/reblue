# bd_stereo on a Quest 2: GPU down 30%, and the frame is now CPU-bound

2026-08-31. Quest 2 (`1WMHH830AY1165`), 3439 field frames of 8137, 60Hz.

## The numbers

| | `bd_stereo` (this run) | `bd_stereo_multiview` (the standing baseline) |
| --- | --- | --- |
| `gpu_total_ms` | **39.67** | 56.40 |
| `other_ms` (CPU) | **66.07** | 27.03 |
| `dt_ms` | 66.88 | 66.82 |
| draws | 577 | 523 |
| us/draw | 114.51 | - |

**GPU time fell 30%, 56.40 -> 39.67ms**, which is what the pass-count analysis
predicted: multiview as implemented carried five extra full-resolution resolve
passes and gave two layers to every render target, 301 MB/frame of tile traffic
against side-by-side's 103.

**And the frame did not get faster.** `dt_ms` is unchanged at 66.88 - the same
4-slot pacing tier - because the bottleneck moved. `other_ms` is **66.07ms**
against a 66.88ms frame: on this path the port is **CPU-bound**, with the GPU
finishing 27ms early.

## What that costs and what it buys

Side-by-side submits every draw twice, once per eye, which is exactly the "it
costs a second submission" this repo has always noted - and here that submission
is 66ms of CPU against multiview's 27ms. So the two paths trade:

| | GPU | CPU |
| --- | --- | --- |
| `bd_stereo` | 39.67 | 66.07 |
| `bd_stereo_multiview` | 56.40 | 27.03 |

Neither is fast, and each is limited by the opposite end. This is **not** an
argument for choosing between them - it is the clearest possible statement of
why multiview is the right technique and why fixing it matters: **multiview's
CPU cost with the array heap's GPU cost is a frame with no obvious bottleneck at
all.** Multiview already submits once for both eyes; the array bindless heap
landed this session removes the five resolve passes that made its GPU side
expensive.

Arithmetically: multiview's 27ms CPU with something near side-by-side's 39.67ms
GPU would be a ~40ms frame - the 50ms tier, 20fps at 60Hz - against 15fps today.
That is a tier, from work already done, once multiview's remaining per-eye bug
is fixed.

## And it validates the CPU work the owner has been asking for

`other_ms 66.07` with `us/draw 114.51` says the recompiled guest and the draw
submission around it are now the limiting factor on the working path. Every
CPU-side item this project has deferred as "the GPU is the bottleneck" is back
on the table:

- the `bdSceneNodeDrawSingle` host seam (built this session, currently a
  pass-through)
- instancing - 2083 calls a frame take only 1270 distinct first arguments
- indirect draws off the deferred queue, which is shipped and correct
- and the vector-register codegen work in the SDK, never attempted

Note the earlier negative result stands and is not contradicted: replacing
`bdSetSamplerState` with host code measured *slower*. The win is not translating
guest functions one-for-one, it is submitting less.

## Method note: `args.txt` did not apply, the profile TOML did

`tools/verify_quest.sh` pushes `args.txt` and reports `args.txt lines matched:
0`. Some settings took effect (a perf CSV was written, and `bd_perf_csv`
defaults false) while others did not - `bd_vr_enabled` stayed off, so the XR pad
never installed, so `bd_xr_autoplay` never pressed anything, so the run sat at
the title screen at 127 draws for 170 seconds and produced no field frames at
all.

**The profile TOML works and says so.** `profiles/default/reblue.toml` in the
app's external files directory is read by the same `bd::AuditProfileConfig` the
desktop uses, and prints:

```
[config] all 9 settings in reblue.toml took effect
OpenXR: instance up, per-eye 1440x1584
OpenXR: 13 input actions attached
```

Use it on device. It is the only config path that tells you whether it worked.
