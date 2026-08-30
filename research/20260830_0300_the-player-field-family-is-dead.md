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

The next attempt should **find the seam that does run before writing any hook**, and the tooling for
that already exists rather than needing invention:

1. `bd_sample_profiler` with the character walking. A profile names what is actually executing on
   the guest thread; if a player controller is running, it is in there.
2. `bd_guest_census` counts calls into named guest functions, so a candidate can be confirmed live
   before anything depends on it.
3. `tools/callgraph.py callers <fn>` to walk up from whatever the profile names.

The general rule this is the second instance of: **a hook on a function nobody has watched fire is a
guess.** Cost of checking is one unconditional counter and one run.

## Sources

- `research/20260829_1420_autoplay-walks-and-the-anchor-hook-is-dead.md` - the first instance
- `config/functions.toml` - where the `bdPlayerField*` addresses are named
