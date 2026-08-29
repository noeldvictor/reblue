# Research: which guest functions are actually called, per frame

Date: 2026-08-29 09:00
Topic: aiming the CPU work with call counts instead of instruction counts, on the desktop build.

The CPU floor is ~62ms of a Quest frame, of which roughly 46ms is recompiled guest code, and it caps
the port near 14 fps with a completely free GPU. Both ready-made levers against it are gone -
`REBLUE_RELAXED_GUEST_MEMORY` hangs the game, and `bd_effect_distance` turned out to gate particles.
What is left is hand-writing host implementations of hot functions with `REX_FUNC`, which needs to
be aimed at the right ones.

The optimisation plan picks its candidates by **counting SIMD instructions in a disassembly**. Every
static count trusted in this port has been wrong, so this counts calls instead.

## Method

Entry-only midasm hooks on six named candidates, incrementing an atomic each, reported per frame
alongside the existing `[perf]` line. Entry hooks need no exit address, which would otherwise have
to be found one function at a time. Multiplying by each function's size from `config/functions.toml`
gives bytes of guest code executed per frame - a far better proxy for time than size alone.

`bd_guest_census`, default off, no device required.

## Result, in a field scene

```
per frame, over 150 frames:
  bdAnimBoneEvaluate           63 calls   353304 bytes of guest code
  bdAnimationUpdate            56 calls   101920 bytes
  bdMatrixInverse4x4            9 calls     5112 bytes
  bdMatrixTransformVector       0 calls        0
  bdAnimCurveSample3            0 calls        0
  bdMatrix4x4Copy               0 calls        0
```

**`bdAnimBoneEvaluate` is confirmed**, and by a wide margin - 3.5x the guest code of the next
candidate, from a 5,608-byte recursive bone walk entered 63 times a frame. The plan's static reading
of it (780 SIMD ops, recursive, per character per frame) was right.

**Three of the six are never called at all.** `bdMatrixTransformVector`, `bdAnimCurveSample3` and
`bdMatrix4x4Copy` are named, plausible, small, maths-heavy, and completely cold in a field scene. A
static count lists them as work worth doing; an hour spent hand-writing any of them would have
bought exactly nothing.

## Caveats worth keeping

- **One scene, one party member.** A battle has five characters and enemies, so
  `bdAnimBoneEvaluate` should scale up sharply and the ranking may shift. Run the census in a battle
  before committing to a rewrite.
- **Bytes of code is not time.** It ignores loops inside a function and cache behaviour. It is a
  ranking, not a budget.
- **Cold here is not cold everywhere.** The three zero-call functions may be hot in menus, battle or
  cutscenes. What this rules out is "hot in a field scene", which is the scene the VR port cares
  most about.

## What to do with it

`bdAnimBoneEvaluate` (`0x822834E8`) is the one function worth hand-writing, and it is the natural
first `REX_FUNC` target. Before that, run the census in a battle to see whether the ranking holds,
and extend the table rather than trusting this list to be complete - six functions were chosen from
a plan, not from a profile, and the real top consumer may not be among them.

The honest next step is a sampling profile on device (`tools/profile_quest.py`, still never run),
which would name functions this table never thought to include. The census is what can be had
without a headset, and it has already eliminated half its own candidates.

---

## Widened, and the answer changed

The caveat above - "six functions chosen from a plan, not from a profile, and the real top consumer
may not be among them" - was the important sentence. Eight more hooks, chosen as the largest named
functions that plausibly run per frame rather than at load time:

```
per frame, over 150 frames:
  bdSceneNodeDrawSingle       420 calls   3250800 bytes of guest code
  bdAnimBoneEvaluate           63 calls    353304 bytes
  bdAnimationUpdate            56 calls    101920 bytes
  bdRenderViewSubmit            1 call       6572 bytes
  bdFieldInteractionSearch      1 call       5908 bytes
  bdFieldHUDUpdate              1 call       5076 bytes
  bdFrameSubmitAndDebugHUD      1 call       3696 bytes
  ScriptManTaskUpdate           0 calls          0
  bdScriptExecute               0 calls          0
  bdEffectEmitterUpdate         0 calls          0
```

**`bdSceneNodeDrawSingle` is 9.2x `bdAnimBoneEvaluate`** and dwarfs everything else combined. The
previous conclusion - that `bdAnimBoneEvaluate` was the function worth hand-writing - was wrong, and
so is the optimisation plan's, which reached the same place from a static SIMD count. Hand-writing
it would have been days of careful work against roughly a tenth of the cost.

`ScriptManTaskUpdate` is the largest named function in the game at 14,068 bytes and is **never
called** in a field scene.

### This joins up with the fill finding

420 calls against ~840 draws is one per two draws: this *is* the guest side of draw submission. So
the frame has one shape on each side of the fence -

- **GPU: fragments.** Draws are free; `bd_render_scale` and `bd_reflections` are the levers, both
  verified.
- **CPU: draws.** Node submission dominates; the lever is submitting fewer nodes.

And it explains a measurement from the very first session that was never accounted for: capping
draws to 500 took the frame's CPU from **60.7ms to 29.5ms** as well as emptying the GPU. That was
read at the time as evidence the frame was draw-bound on the *GPU*, which the scissor test later
disproved. It was really evidence about the CPU, and it was sitting there the whole time.

### So the CPU lever is culling

Which makes `src/xr/xr_cull.cpp` - **written, 49 checks passing, and never connected to anything** -
the most valuable dead code in the tree. `bdCameraViewFrustumTest` (`0x82135030`) is named and is
the seam. Distance culling and LOD sit next to it.

Before building any of that: **run this census in a battle.** A field scene has one party member,
and the ranking has already changed once by looking somewhere new.

## Correction: bd_debug_max_draws does not test the census inference

The previous section concluded "the CPU pays for draws" and pointed at culling. The experiment run
to check it says something narrower, and the difference matters.

`bd_debug_max_draws` caps submission inside `DispatchDraw`, which is **host** code. The guest still
walks every scene node, so `bdSceneNodeDrawSingle` still runs its 420 times whatever the cap is.
Capping therefore removes renderer cost only, and cannot say anything about the guest-side node
submission the census measured.

Desktop, last 400 frames:

| draws submitted | CPU (`other_ms`) | change |
| --- | --- | --- |
| 829 (uncapped) | 4.20ms | — |
| 400 | 3.70ms | -0.50ms |
| 150 | 3.13ms | -1.07ms |

Linear, at **~1.3us per draw of renderer cost**, which agrees with the `flushState` 0.8ms and
`bindFB` 0.2ms already reported. So of the desktop's 4.20ms of CPU, about **1.1ms is the renderer
and about 3.1ms is guest simulation**. Nothing here touches the guest.

### Which makes the Quest number strange, in a useful way

On the Quest, `bd_debug_max_draws=500` took `elsewhere` from **60.7ms to 29.5ms** - 31ms across
about 2400 skipped draws. Scaling the measured 1.3us per draw by a core roughly three times slower
predicts about 4us per draw, so 2400 draws should have cost near **10ms of renderer time, not
31ms**. The gap is a factor of three and it is not explained by anything measured here.

The explanation that fits is **back-pressure**: part of what the Quest frame counts as `elsewhere` is
the guest thread waiting on the render thread, not computing. Fewer submitted draws means less GPU
work, which means a shorter wait, which shows up as "CPU" time that was never CPU work.

If that holds, then some of the ~62ms floor is not a floor at all, and **the fill work already
verified would shrink it as a side effect** - the two halves of the frame are not as separable as
this note has been assuming.

### How to settle it, in one run

It needs the device, and it is already covered by the `levers` preset:
`bd_render_scale=50` makes the GPU much faster while leaving every draw and every guest node walk
exactly where it was. If `elsewhere` falls, the wait was real and the CPU floor is partly a
symptom. If `elsewhere` does not move, the floor is genuine computation and culling is worth
building.

**Do not build the culling change before running that.** The inference that pointed at culling came
from a census that measures calls, checked against an experiment that measures something else.

## A second scene, which is what the caveat asked for

`bd_devmode` boots the game's own debug menu, and autoplay's START-then-A lands in a different field
area - a sparse wasteland with the HUD live, 690 draws a frame at 393 vertices each, against the
village's 829 draws at 249. Sparser and heavier per draw, which is a genuinely different shape.

| function | village | wasteland | change |
| --- | --- | --- | --- |
| `bdSceneNodeDrawSingle` | 3,250,800 bytes | **1,006,200** | 420 calls -> 130 |
| `bdAnimationUpdate` | 101,920 | **535,080** | 56 calls -> **294** |
| `bdAnimBoneEvaluate` | 353,304 | 342,088 | 63 -> 61, steady |

**`bdSceneNodeDrawSingle` leads in both**, which is what matters - the target holds. But the margin
collapses from 9.2x to 1.9x, and `bdAnimationUpdate` moves by a factor of five between two field
scenes of the same game.

So the earlier caveat was not hypothetical: **the ranking below first place is scene-dependent and
should not be trusted from one sample.** Anything built on "second place" needs measuring in the
scene it is meant to help. First place has now survived two very different scenes, which is the
first thing in this investigation to do so.

`bd_devmode` is the cheap way to get a second sample, and is worth using before any conclusion drawn
from a single autoplay run. It still is not a battle - five characters and enemies would be the real
test of `bdAnimBoneEvaluate`, which has been suspiciously steady at ~61 calls across both scenes and
is presumably per-character.

## On the device, in VR, the answer is far starker

Everything above was measured on the desktop build. Run on a Quest, in the VR scene, at the best
known configuration (`render_scale=25, reflections=false, cull_bias=0.6`):

```
per frame, over 150 frames:
  bdSceneNodeDrawSingle          2084 calls   16130160 bytes of guest code
  bdAnimBoneEvaluate              126 calls     706608
  bdAnimationUpdate               112 calls     203840
  bdMatrixInverse4x4               14 calls       7952
  ScriptManTaskUpdate               1 call       14068
  bdFieldInteractionSearch          3 calls      17724
  everything else                 0-1 calls     negligible
```

**`bdSceneNodeDrawSingle` is 23x the next consumer**, where the desktop showed 1.9x. 2084 calls a
frame against 2891 draws - close to one node per draw - and 16 megabytes of guest code executed per
frame in that one function.

The desktop census got the *winner* right and badly understated the *margin*. Two field scenes on a
PC gave 420 and 130 calls; the real VR scene gives 2084. Anyone tuning against the desktop numbers
would have concluded animation was worth attacking too. It is not: at 706KB against 16.1MB it is
noise.

### The frame, fully attributed at last

The renderer's own phases in the same run: `mutex 0.4ms, bindFB 2.0ms, flushState 10.5ms`.

| | of ~56ms of CPU |
| --- | --- |
| draw recording (ours) | **~13ms** |
| guest code | **~43ms**, overwhelmingly `bdSceneNodeDrawSingle` |

So the CPU floor is not diffuse and it is not the recompiler in general. It is one guest function,
walking two thousand scene nodes a frame, and it is where every remaining frame of headroom is.

### Why cull_bias only bought 11%

It removed 110 nodes of 3041. A radius bias culls the *small* ones, and the cost is per node rather
than per pixel, so culling small nodes and large nodes is worth the same. The axis has to be
something that removes *many* nodes - distance, or LOD, or whatever makes a JRPG field need two
thousand of them in the first place.

**Open question worth answering before writing any code**: why are there 2084 nodes? At one draw
each with 144 vertices, this is a scene built from very many very small pieces. If they are
individually placed props and foliage, distance culling should remove most of the far ones. If they
are the same handful of meshes instanced across a terrain grid, the fix is batching, not culling.

## Props, not instances - so distance culling is the right axis

The open question above, answered on device by counting how many distinct values
`bdSceneNodeDrawSingle`'s first two arguments take in a frame:

```
bdSceneNodeDrawSingle   2083 calls    distinct r3 = 1270    distinct r4 = 497
```

**1270 distinct first arguments out of 2083 calls.** If the scene were a handful of meshes instanced
across a terrain grid, that number would be small and the fix would be batching. It is not: this is
on the order of a thousand individually placed objects, each drawn once or twice.

The second argument takes 497 distinct values, about four calls each, which is the amount of sharing
you would expect from a few hundred distinct meshes reused across those objects - enough to matter
for state changes, not enough to make batching the answer.

So the lever is **removing objects**, and the axis is distance rather than size. `bd_cull_bias`
culls by bounding radius, which is why it only reached 110 of 3041 draws: it takes the small ones,
and the cost is per object regardless of size.

**A thousand objects in a field scene is the actual problem.** At 144 vertices each they are small
props - rocks, plants, fence posts, the clutter a 2007 JRPG scatters to make a field look inhabited -
and on a Xenon with a hardware command processor they were nearly free. Each one now costs a walk
through 7,740 bytes of recompiled PowerPC.

## The distance cull works; a second call path bypasses it

`bd_cull_distance` changed no draw counts, which read like a broken hook. It is not:

```
[cullhook] fired, limit=700 decision=true r3now=0
```

The hook runs, the limit reaches it, the distance test rejects the node, and `r3` is zeroed so the
guest takes its own not-visible branch. The mechanism is correct.

**The draws come from somewhere else.** `bdSceneNodeDrawSingle` has *two* callers -
`bdSceneNodeCullTraverse` (`0x82282490`), which is hooked, and `sub_82282608`, which is not. Culling
one path cannot reduce a draw count the other path is producing, and at 2084 calls a frame against
roughly 1270 distinct objects, the second path is evidently carrying most of it.

That was visible from the very first grep - two call sites in
`generated/reblue_recomp.38.cpp` and `generated/reblue_recomp.51.cpp` - and it was read past,
because the first caller had a name that matched what was being looked for and the second did not.

**`sub_82282608` is the next thing to read.** It sits immediately after `bdSceneNodeCullTraverse`
(`0x82282490` + `0x178` = `0x82282608`), so it is the very next function in the binary, which
usually means a sibling: a second traversal, probably for a different node class - transparent
objects, or a second pass over the same tree.

The general lesson is the one this file keeps recording in different forms: a diagnostic that says
"no effect" is not evidence the mechanism is broken. Here the mechanism was provably working and
the population it acted on was the wrong one.

## The second path's addresses were wrong, and hung the guest

Hooking `sub_82282608` used addresses counted backwards from `loc_82282780`, assuming four bytes an
instruction from the emitted comment listing:

```
bl 0x82392EC8   -> 0x8228275C     (guessed)
cmpwi r3,0      -> 0x82282760     (guessed)
```

**Both are wrong.** With those hooks in, the app launches, displays, stays resident and never
writes a log line at all - it hangs before logging init rather than crashing, so there is no signal
in logcat either. Removing them restored it immediately.

Counting instructions backwards from a label works only if every line in the comment listing is one
instruction and none were folded or reordered by the recompiler. The first path's addresses were
right because they were checked against the `loc_` labels on *both* sides of the block; this one was
counted from one side only.

**The way to get these right is to read the address off the generated code, not to derive it.** The
recompiler emits `// bl 0x...` comments and `loc_XXXXXXXX:` labels, and any hook address should be
confirmed against a label that brackets it, or taken from `config/functions.toml` where the function
is already named.

So the second path is still unhooked and the distance cull still only covers one of the two callers
of `bdSceneNodeDrawSingle`. Its real effect remains unmeasured.
