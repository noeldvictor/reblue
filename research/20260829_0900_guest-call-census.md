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
