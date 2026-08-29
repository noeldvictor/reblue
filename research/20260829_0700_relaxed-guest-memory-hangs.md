# Research: REBLUE_RELAXED_GUEST_MEMORY hangs the game

Date: 2026-08-29 07:00
Topic: measuring the last unverified CPU lever, on the desktop build, in about five minutes.

`REBLUE_RELAXED_GUEST_MEMORY` drops `volatile` from the eight guest RAM accessor macros in
`generated/reblue_pch.h`. Every guest load and store in the game goes through them, and `volatile`
forbids the host compiler from eliminating a redundant reload, forwarding a store to a load, or
keeping anything in a register across statements - on every variable access in the whole game. The
option's own note measured 11 instructions against 23 on a representative recompiled sequence.

It has sat in the tree as the largest single unclaimed CPU win, and as Phase C of the optimisation
plan. **It hangs Blue Dragon.**

## Two bugs, in order

**It could never have been built.** The relax step was pointed at
`${REBLUE_GEN_DIR}/reblue_pch.h` - the *binary* directory - while codegen emits the guest sources
and that header into the *source* tree's `generated/`. Turning the option on failed with
`FileNotFoundError` and took the build down with it. So the flag has been unbuildable for as long
as it has existed, which is why nobody ever measured it. Fixed.

**With that fixed, it hangs.** Applied, all 54 guest translation units rebuilt, `volatile` confirmed
gone from `REX_LOAD_*`/`REX_STORE_*` and confirmed still present on the `REX_MM_*` MMIO macros:

```
baseline log   121,814 bytes, reaches a field scene, 840 draws/frame
relaxed  log     5,995 bytes, stops after "guest debug config applied (devmode=false)"
```

No crash, no fatal, no assert - the process sits there until it is killed. That is a hang, and it is
precisely the failure the option's own comment predicted:

> `volatile` presumably stops the compiler hoisting a load out of a guest spin-loop, and a guest
> thread polling a flag in shared memory would hang. Nothing has established that Blue Dragon has no
> such loop.

Something has now established that Blue Dragon *does* have such a loop, and it is reached during
startup.

## Where this leaves the CPU floor

The Quest frame carries ~62ms of CPU that caps the port near 14 fps with a completely free GPU. This
option was the one landed, ready-to-try lever against it, and it is gone. What remains:

- Host implementations of hot guest functions (`REX_FUNC`), which is per-function and safe, but
  needs a profile to aim - `tools/profile_quest.py` still has never been run.
- A narrower version of this: relaxing loads only in leaf functions with no loops, or excluding a
  known set of poll sites, rather than every access in the game. Much more work, much less win.
- `FlushRenderStateLocked`, measured at 12ms of the Quest's frame, which is renderer CPU and not
  guest CPU, and is the one piece of the floor that is fully under our control.

## The pattern, now three deep

Every large "free" win in the recompiler family has broken the guest:

| lever | static win | reality |
| --- | --- | --- |
| `non_argument_as_local` | -36% context accesses | miscompiles; fatal during file load |
| `REBLUE_RELAXED_GUEST_MEMORY` | -18% instructions, -42% loads | **hangs during startup** |
| hoisting shader constant loads | -28% loads per fragment | inert; DXC already CSEs them |

An instruction count says where the instructions are. It says nothing about whether the program
still works, and in this codebase the answer has been "no" every single time. **Measure the running
game, and measure it before believing the count.**

The good news is the loop: this took about five minutes on the desktop build, needed no headset, no
adb and no APK, and the answer was unambiguous. The same question on-device would have been a
codegen rebuild, a 62 MB reinstall, and a hang that looks exactly like the headset having gone to
sleep.
