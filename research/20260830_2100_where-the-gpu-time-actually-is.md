# Per-pass GPU timing, and it is the scene - not the post chain

2026-08-30. Quest 2, multiview, field scene.

## The instrument

The per-target census reported "Mpix/frame if each draw covered it once" - an upper bound that says
nothing about real coverage, and which this repo has over-read twice into opposite conclusions. It
now reports **measured GPU milliseconds per render target**.

The mechanism is small: the GPU timing journal already writes a timestamp whenever the segment
category changes, so each segment is now also tagged with the render target current at the time
(`NotePassTarget`, called where the framebuffer is bound), and the collect pass sums elapsed time
per target. Every segment is attributed, not only draw ones - a pass costs what it costs, including
the barriers and resolves belonging to it.

**Coverage: 53.91 ms attributed of ~56 ms of `gpu_total_ms`, 96%.** Good enough to reason from.

## The answer

| target size | count | ms/frame | share |
| --- | --- | --- | --- |
| **1376x720x2L** | 12 | **48.22** | **89%** |
| 344x180x2L | 10 | 2.14 | 4% |
| 688x360x2L | 4 | 1.52 | 3% |
| 344x193x2L | 2 | 1.48 | 3% |
| everything else | 12 | 0.55 | 1% |

And within the full-resolution two-layer group:

| target | draws/frame | binds/frame | ms/frame |
| --- | --- | --- | --- |
| BBCACAD0 | 60.7 | 0.50 | 11.15 |
| BBCB0790 | 60.6 | 0.50 | 11.37 |
| BBCB29D0 | 59.3 | 0.50 | 11.47 |
| BBCAFD10 | 59.1 | 0.50 | 11.22 |
| the other eight | <= 0.5 | <= 0.5 | 3.03 combined |

**The scene pass is 45.2 ms of 56 - 81% of all GPU time.** The post chain, all of it, is under 8 ms.

## This corrects a conclusion made hours earlier

The draw-queue work measured that only 166 of 562 draws are opaque and that two of three flushes per
frame contain none, and that was written up as "the frame is dominated by blended full-screen
passes, so the next lever is the post chain". **That was wrong**, and it was wrong because it
reasoned from draw counts instead of measuring time. The post chain has many passes and they are
cheap; the scene has few and they are not.

Same mistake shape as the two before it: a count is not a cost.

## What it points at

**The scene renders twice per frame.** Four alternates at 0.50 binds/frame each is 2.0 scene binds
per frame, ~60 draws and ~11.3 ms apiece. Finding out what the second one is - a reflection, a
shadow pass, a pre-pass - is the first question, because if it is avoidable in VR it is worth ~22 ms
of a 56 ms frame on its own.

**It is a two-layer target**, so under multiview every fragment of it is rasterised twice. That is
correct and required for stereo - but it means the scene pass is exactly where fixed foveated
rendering pays, and foveation acts on fragment cost in the periphery of precisely this pass.

**Neither lever is draw submission.** The deferred queue is correct and free and is the seam
instancing and GPU culling will attach to, but reordering 166 opaque draws cannot touch 45 ms of
full-resolution two-layer fragment work. Occlusion culling can, by removing draws from this pass;
foveation can, by making its fragments cheaper.


## Following it up: what the two scene binds are

A probe on every full-resolution two-layer framebuffer bind, skipping the title and load screens,
reports the guest's own render pass id. In a field frame **every one of them is `guest_pass=7`** -
so this is one logical guest pass rendered into alternating pool surfaces, not two different passes
that might be separately removable. The "renders twice per frame" reading is 2.0 binds/frame across
four pool alternates.

**A caution, because it nearly became the next wrong conclusion.** That probe also reported
`ds=no` on all fourteen binds it caught, which would mean the scene renders with no depth
attachment - no early-Z possible, and a tidy explanation for why front-to-back sorting did nothing.
It is an artefact: fourteen consecutive binds at one instant are not a sample of the frame, and the
aggregated census disagrees plainly -

```
target 0001BBCB0790 1376x720x2L depth: 52.50 draws/frame over 0.50 binds/frame, 11.50 ms/frame GPU
```

The full-resolution group mixes depth uses (the 3D geometry, ~11.5 ms each) with colour-only ones
(post and resolve, well under 1 ms). The expensive ones have depth. **A bounded log answers what
happened first, never what happens** - the same trap this repo recorded once already for a periodic
sampler.

So the scene pass genuinely is ~45 ms of a 56 ms frame, it is one guest pass, and it does have a
depth buffer. The levers remain foveation and occlusion culling; there is no free second pass to
delete.
