# The bdPlayerField* family never runs, and that is why two features stalled

2026-08-30. Two separate pieces of work have now hooked a named `bdPlayerField*` function, verified
the hook compiles and registers, and watched it never fire. Recorded so the third attempt does not
spend the same afternoon.

## What was tried

**Tourist mode's encounter suppression.** `bdPlayerFieldCheckEncounter` (`0x82209678`) is the obvious
seam and the call site says exactly how to use it:

```
bl     bdPlayerFieldCheckEncounter
cmplwi r3,0
stw    r3,7652(r31)        # remembered on the player
beq    <no encounter>
```

Zero means "nothing happened", so forcing `r3 = 0` sends the guest down a path it already takes -
the same minimal intervention the distance cull uses, no control flow invented. A `REX_HOOK_RAW`
was written, with an **unconditional** entry counter, because "the hook does nothing" and "the guest
never calls it" are indistinguishable from the outside.

Over a 200 second desktop run including the whole autoplay walking phase: **zero entries.** The
guest never calls it.

**The VR character anchor**, earlier, hit the same wall on `bdPlayerFieldMovementUpdate`
(`0x82207858`) - see `20260829_1420_autoplay-walks-and-the-anchor-hook-is-dead.md`. That was read at
the time as one awkward function. It is not: it is a family.

## The callers do not rescue it

```
bdPlayerFieldCheckEncounter
  <- sub_82209FE8   <- bdPlayerFieldUpdateMain
  <- sub_8220C298   <- bdPlayerFieldUpdateMain
  <- sub_8220A690   <- sub_8220CD18, sub_8220D388
```

The chain reaches `bdPlayerFieldUpdateMain`, which is itself named - so this is not an unnamed
indirect-dispatch problem where the callgraph simply cannot see the edge. Either
`bdPlayerFieldUpdateMain` does not run in the configuration the game actually boots into, or the
branch that reaches these leaves is never taken.

Blue Dragon plausibly has more than one player controller - field, vehicle, cutscene, and the
tutorial areas the autoplay path walks through - and the one named `bdPlayerField*` may simply not be
the one driving. That is a guess. What is measured is that the entry point is never reached.

## What this costs and what to do instead

It blocks the two things that most want a player-side seam:

- **Encounter suppression**, which is wanted for VR sightseeing *and* as a measurement tool: autoplay
  walks, walking rolls encounters, and 26% of frames after the walk begins came in under 100 draws
  in the run above, against a field scene's 500-600. A benchmark that wanders into a battle is not
  measuring the thing it claims to.
- **The character anchor** for the head-anchored camera modes, which currently derives the character
  position from the follow camera instead - approximate, and the reason
  `bd_vr_anchor_distance` needs tuning at all.

## Where the live code is

Following that advice immediately corrected part of it. **A profiler ranks by cost, not by
liveness** - a per-frame player update for one character is cheap and will never appear in a top-120
list however alive it is. Searching the profile for "player" or "field" found nothing, and that is
not evidence.

Searching by *address range* is evidence. From a 20,817-sample profile taken across the walking
phase, the distinct guest functions sampled in the neighbourhood are:

```
0x8220xxxx   nothing at all
0x8221xxxx   sub_82211EB8 82212EC0 82215050 822150F0 82215AC0 82217108
             8221B1D8 8221CD08 8221CE78 8221D548 8221DB00 8221DBE0
             8221DCA0 8221E758 8221E8C0 8221E9B8
```

**The entire `0x8220xxxx` block is absent**, and that is where `bdPlayerFieldCheckEncounter`
(`0x82209678`), `bdPlayerFieldMovementUpdate` (`0x82207858`) and every caller of the encounter check
live. The block immediately above it is busy. So the player/field code that runs is a different
implementation sitting around `0x8221xxxx`, not the named one.

`sub_82215050` is the most interesting of them: `tools/callgraph.py callers` finds **no direct
caller**, meaning it is reached through a vtable or jump table - which is the shape of a task or
controller entry point, and exactly what a live player controller would look like in an engine built
on a task list.

## How to finish this

1. Start from `sub_82215050` and its neighbours, not from anything named `bdPlayerField*`.
2. Confirm liveness with an **unconditional entry counter** before building on it. That is one
   counter and one run, and it is what turns a candidate into a fact.
3. Name what survives in `config/functions.toml`, so the next profile prints something readable.
4. Then hook: the encounter seam wants a result forced to zero, and the anchor wants a position read.

The general rule this is the second instance of: **a hook on a function nobody has watched fire is a
guess.**

Cost of checking is one unconditional counter and one run.

## Sources

- `research/20260829_1420_autoplay-walks-and-the-anchor-hook-is-dead.md` - the first instance
- `config/functions.toml` - where the `bdPlayerField*` addresses are named
