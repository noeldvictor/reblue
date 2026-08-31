# Replacing a hot guest function with host code made it slower

2026-08-31, desktop, two within-run A/Bs.

## What was tried

`bdSetSamplerState` (0x82287060, 0x78 bytes) is called **11 times per scene
node** - about 23,000 times a frame - and is the single most-called named guest
function in the frame after the node draw itself. Read out of the recompiled
body, it is:

```
index  = sampler * 20 + state / 4
cached = *(u32 *)(0x82DBE330 + index * 4)
if (cached == value) return;                 <- the common case
... indirect call to a per-state setter, update the cache, set a dirty flag
```

The premise this port has carried from the start is that recompiled guest code
is wasteful, so doing the comparison host-side - no guest stack frame pushed and
popped through `REX_STORE_U32`, no PPCRegister/XER/CR temporaries, one load
instead of a recompiled prologue - should be cheaper. Only the early-out was
reimplemented; anything that changes state still falls through to
`__imp__bdSetSamplerState`, so behaviour cannot drift.

## The result: the fast path is 100% of calls, and it is slower

```
[node] bdSetSamplerState: 2000001 of 2000000 calls took the host fast path (100.0%)
```

So the guest early-outs essentially always, exactly as this repo already
believed. And:

| A/B (within one run, ~4,800 frames per arm) | us/draw |
| --- | --- |
| first attempt, with two atomic counters in the hot path | **+1.4%** |
| counters removed entirely | **+3.1%** |

**Host code lost both times, and removing the instrumentation made it worse,
not better.** The counters were a real cost - two atomic read-modify-writes at
23,000 calls a frame - but they were not the explanation.

What is left is that the wrapper's own overhead (a cvar read per call, and a
bounds-checked `bd::mem::at` instead of a raw indexed load) costs about what the
recompiled prologue costs, and the compiler has already optimised the guest body
down to very little: a shift, a multiply, a load, a compare, a return.

## Why this matters more than the code does

It is a direct test of the assumption behind the whole rewrite programme -
*"the recompilation is inefficient, host C++ will be faster"* - on the best
available candidate: a tiny, extremely hot, trivially replaceable function whose
common path is a single comparison. **It came out negative.**

That does not mean the rewrite is pointless. It means the win has to come from
*doing less work*, not from doing the same work in a different language:
batching draws, instancing, indirect submission, not marshalling a transform
through big-endian guest memory at all. Replacing a guest function one-for-one
with a host function that does the same thing is not where the time is.

It also sharpens what the seam in `gpu/hooks/scene_node.cpp` is for. That seam
is worth having - it links, it runs, the frame is unchanged - but as somewhere
for *instancing and indirect draws* to attach, not as a way to make the same
per-node work cheaper.

## Kept, off by default

`bd_host_sampler_state` defaults **false**. Verified correct when enabled
(95.5% non-black, mean RGB 59/54/43, unchanged) so it is a usable probe, but it
is not a default and should not become one without a number that says otherwise.
