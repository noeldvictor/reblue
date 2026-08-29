# Research: the levers, measured on the Quest at last

Date: 2026-08-29 10:00
Topic: `bench_quest.py all` on a Quest 2, and the question it was built to settle.

```
configuration                                        frame    fence     else    fps   draws
baseline                                           170.1ms  108.6ms   53.2ms    5.9    2834
render_scale=50, reflections=false                 129.0ms   68.7ms   52.0ms    7.8    2761
  + shadows=false                                  129.4ms   65.4ms   55.6ms    7.7    2777
  + stereo (sep 0.06, conv 0.03)                   164.3ms   91.5ms   63.9ms    6.1    2766
```

## The fill work transfers

**GPU fence 108.6ms -> 68.7ms, a 37% cut, and 5.9 -> 7.8 fps.** Comfortably outside the +/-30%
cross-restart band, so it is a result rather than noise, and it is the first time anything verified
on the desktop build has been confirmed on the device. The desktop loop was worth building.

`bd_shadows=false` is **not** a result: 65.4ms against 68.7ms is inside noise. The per-surface census
could never see the shadow pass because it hooks the colour target, so this flag went to the device
unverified and has now measured as approximately nothing. It should be described that way rather
than as a lever.

## The back-pressure hypothesis is dead, and that is the useful part

`research/20260829_0900_guest-call-census.md` raised the possibility that part of the ~62ms of
`elsewhere` was the guest thread **waiting** on the render thread rather than computing - which
would have meant the fill work shrank the CPU floor for free, and that culling was not worth
building. The test was to free the GPU and watch `elsewhere`.

**43ms of GPU time was freed and `elsewhere` moved 53.2ms to 52.0ms.** Nothing. The floor is real
computation.

So:

- **The CPU floor is genuine** and caps this port near **19 fps** on its own, independent of
  everything the GPU does.
- **Culling is worth building.** The census target stands: `bdSceneNodeDrawSingle`, 420 calls a
  frame in a village and 130 in a wasteland, first place in both scenes.
- The two halves of the frame are as separable as the earlier notes assumed. The factor-of-three
  anomaly in the old `bd_debug_max_draws` number remains unexplained, but it is not back-pressure.

## Stereo runs on the headset

The fourth row is the first time `bd_stereo` has been on a Quest. It did not crash, and it costs
what it should: **fence 65.4 -> 91.5ms** for a second view of a half-scale scene, and **`elsewhere`
55.6 -> 63.9ms** for the doubled draw recording, which is the ~8ms the design predicted. Frame
129 -> 164ms, 7.7 -> 6.1 fps.

Worth stating plainly: **stereo at half scale costs less than mono at full scale** - 91.5ms of fence
against the baseline's 108.6ms. Two eyes are genuinely cheaper than today's single eye, which is
what `bd_render_scale` was for.

**Not visually confirmed in the headset.** The frame cost says it drew twice; nobody has looked
through the lenses. That is the next thing, and it needs a person wearing it.

## Where this leaves the port

| | |
| --- | --- |
| best mono | **7.8 fps** (129ms: 68.7 GPU, 52.0 CPU) |
| best stereo | **6.1 fps** (164ms: 91.5 GPU, 63.9 CPU) |
| CPU floor alone | ~52ms, i.e. **19 fps** ceiling |
| target | 72 fps, i.e. 13.9ms |

Still an order of magnitude out, and the shape of the remaining work is now unambiguous: the GPU has
a proven lever with more room in it (render scale below 50, and foveation, which was dismissed on
the false "not fill-bound" reading and should be reassessed), and the CPU needs the node-submission
cost attacked directly, with culling as the first candidate.
