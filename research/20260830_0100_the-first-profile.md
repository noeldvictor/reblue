# The first profile, and what it changes

2026-08-30. The project has never had a profile. `tools/profile_quest.py` has never produced one,
because Horizon OS refuses shell perf on a Quest 2 whatever `perf_event_paranoid` says and whether
or not the app is `profileable`. Every performance decision so far has come from static instruction
counts, call censuses and frame timers - and several of them were wrong.

There is one now, from `bd_sample_profiler`, taken on the **desktop** build: a 170s run, no APK, no
install, no headset, and the guest code being profiled is the same code that runs on the Quest.

## What it says

Field scene, 18,252 samples, 98.5% resolved, attributed to the outermost frame of each inline stack:

```
__imp__sub_82287788                7.5%     __imp__bdSceneNodeDrawSingle   6.4%
bd::gpu::FindPhysicalBufferByStruct 3.4%    __imp__bdSceneNodeCullTraverse 3.3%
__imp__sub_8272BE80                2.6%     __imp__sub_82281D28            2.5%
InsetQuadUVs                       2.4%     __imp__bdMatrixSet             2.3%
bd::gpu::UploadSharedConstants     2.1%     __imp__sub_821621C8            2.0%
```

**Nothing dominates.** That is the finding, because the plan assumed something did.

## Two corrections

**`bdSceneNodeDrawSingle` is 6.4%, not 23x everything else.** That figure came from
`bd_guest_census`, which counts *calls*. 2084 calls a frame of a cheap function is not the same
claim as 2084 calls of an expensive one, and the census cannot tell the difference. Rewriting it as
a host `REX_FUNC` should be expected to buy single digits, not a frame. `20260829_2200_where-the-
cpu-actually-is.md` should be read with that in mind.

**Attribute to the outermost inline frame, not the innermost.** Read the first way, the profile put
`std::_Atomic_integral::fetch_add` at 3.9% and `simde_mm_shuffle_epi8` at 3.0% - true, useless, and
actively misleading, because both are leaves inlined into everything. The same samples attributed to
the outermost frame name `NoteDrawPhases` and `sub_82287788` instead, which are things you can act
on. `tools/symbolize_profile.py` does the latter.

## The two things it found that were ours

**`Sleep_hook`, 15.9% of all samples** - the hottest thing in the process, more than twice the
hottest guest function. It slept short by a 1.5ms guard band and then busy-waited out the remainder
for precision nothing needs; Sleep is a floor, not a deadline. Removing the spin took `other_ms`
from **8.49ms to 7.79ms** at an identical draw count and unchanged `logic_tps`, and it left the
profile entirely. `bd_sleep_spin` restores it.

**`NoteDrawPhases`, 3.4%** - per-draw phase timing, four `steady_clock` reads and three atomics on
every one of ~1200 draws. Its own comment estimated "~100us a frame"; it was out by a factor of 25.
Now behind `bd_draw_phase_timing`, default off. The frame-level delta on a desktop is inside noise
(7.79 -> 7.82ms at 1241 -> 1274 draws, about 2% per draw), which is expected: this machine is not
CPU-limited. It is work removed, and the Quest is where that matters.

Together those two were **19.3% of the process spent measuring and waiting.**

## The next target, with the evidence

`sub_82287788` is now the hottest thing at 7.5% and **it is not named in `config/functions.toml`**.
It is 1119 lines, reached from `bdSceneNodeCullTraverse` and `sub_82282608` - the same caller as
`bdSceneNodeDrawSingle` - so it runs per scene node. Its opcode mix:

```
lvlx 20   lvrx 20   vor 20   stvx 16   vmrghw 14   vspltw 12   vmaddfp 12
lfs 30    stfs 17   fcmpu 15
```

`lvlx` + `lvrx` + `vor` is the PowerPC idiom for **one unaligned 16-byte vector load**, and there
are twenty of them. Here is what one `lvlx` costs after recompilation:

```cpp
simde_mm_store_si128((simde__m128i*)ctx.v0.u8,
  simde_mm_shuffle_epi8(
    simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(temp.u32 & ~0xF)),
    simde_mm_load_si128((simde__m128i*)&VectorMaskL[(temp.u32 & 0xF) * 16])));
```

Two loads - one of them a 256-byte mask table indexed by the low four address bits - a shuffle, and
a store. The idiom does that **twice, plus a `vor`**, to load sixteen unaligned bytes. On x86 that
is one `movdqu`; on ARM64 it is one `ldr q`. So a single guest instruction pair that modern hardware
does in one instruction currently costs four loads, two shuffles, two stores and an OR.

Worse, `v0` and `v12` here are **argument registers**, and only v14-v127 are localised
(`non_volatile_as_local`), so these genuinely round-trip through `ctx` memory rather than staying in
registers.

**The change is to pattern-match `lvlx`/`lvrx`/`vor` in the forked SDK's codegen and emit one
unaligned load plus the byte-swap.** It is not specific to this function - it is every unaligned
vector load in the guest, which is what makes `simde_mm_shuffle_epi8` visible in the profile at all.
That is the next piece of work, and it is exactly the "remove the X360 pattern, use the ARM64 one"
shape.

Naming `sub_82287788`, `sub_8272BE80` and `sub_82281D28` in `config/functions.toml` should come
first, so the profile stops printing addresses.

## Acted on: the visibility test was being computed and thrown away

`sub_82287788` is the per-node **visibility test**, and the call site says so:

```
bdSceneCullBiasHook(f1, r3)    <- ours, before the call
sub_82287788(ctx, base)        <- 7.4% of all samples
bdSceneCullDistanceHook(r3)    <- ours, after; forces r3 = 0 to cull
cmpwi r3,0 / beq loc_822825E0  <- 0 means skip this node
```

It takes a node pointer and a scaled radius, opens by returning 1 if a global
"culling off" flag is set, and returns visible/not. Our distance cull ran *after*
it and zeroed the result - deliberately, so that no control flow was redirected.
Safe, and it meant the decision cost nothing to make and everything to act on:
the distance cull rejects about **95% of nodes**, so we computed full visibility
for 95% of the scene and discarded it.

The hook now returns `bool` and the table carries
`jump_address_on_true = 0x822825E0`. That is not a new path - the guest already
branches there from two earlier early-outs in the same traversal, and nothing
after the label reads the call's result.

**Measured, same binary, cvar toggle, back to back:**

| `bd_cull_early` | `other_ms` | draws | ms/1000 draws | `sub_82287788` |
| --- | --- | --- | --- | --- |
| false | 5.12 | 513 | 9.98 | 7.4% |
| true | **4.20** | 505 | **8.32** | **absent from the top 30** |

**-18% CPU.** Draw counts agree within 1.6%, which is the correctness signal:
the same nodes are culled, the decision is just made before the expensive test
instead of after it. A captured frame was looked at - a field scene with the
character, foliage, cliffs and shadows all present and nothing popping.

`bd_cull_early` reverts to the old ordering without a rebuild.

## Fixed, and the first trustworthy GPU attribution this project has had

With the readback repaired (below), the per-category split is valid for the first time. Desktop,
field frames only, 6,605 of them:

```
dt_ms          16.67     (vsync)
other_ms        5.42     CPU
gpu_total_ms    5.83     GPU  - not the ~2.0 that was quoted all session
  gpu_draw_ms   4.54     78%
  gpu_resolve   1.12     19%   <- EDRAM emulation
  gpu_inter     0.16      3%
fence_ms        0.01
draws            516
```

**19% of GPU time is the resolve path** - the emulation of the Xbox 360 copying render targets out of
EDRAM into textures, which a modern GPU has no reason to do at all. That is squarely the "remove the
X360 pattern" target and it is now measurable, which it was not this morning: the column that would
have shown it was stale.

### And within the resolve, the cost is seeding

Per field frame:

```
rs_eager        3.0     copies actually issued
rs_lazy        18.0     deferred
rs_deadelide   17.0     eliminated as dead        <- this already works
rs_materialize  1.0
rs_seed        14.0     fresh targets seeded      <- what is left
bar_resolve    36.0
```

**Dead-resolve elimination is already doing its job** - 17 of 21 resolves never execute. The
remaining GPU cost in this category is not the resolves at all, it is `rs_seed`: **14 full-surface
copies a frame** from `SeedFreshColorTarget`, which copies prior content into a freshly acquired
surface so that a pass reading untouched pixels sees what a Xenon's EDRAM tile would have held.

That is the X360 artifact in its purest form - a copy that exists only to reproduce the persistence
of a tile buffer that is not there.

**Measured, with the within-run A/B, ~4,800 frames an arm:**

| `bd_seed_targets` | `gpu_total_ms` | `gpu_resolve_ms` | `other_ms` | `rs_seed` |
| --- | --- | --- | --- | --- |
| off | 5.46 | **0.50** | 5.99 | 0 |
| on | 5.88 | **1.14** | 6.10 | 14 |

**Seeding costs 0.42ms of GPU a frame - about 7% of it - and more than half of the entire resolve
category.** The CPU side does not move, which is what it should do. This is the first GPU-side A/B
this project has been able to run at all: it needs both the timestamp fix and the within-run
harness, and neither existed twelve hours ago.

`bd_seed_targets` is a **measurement handle, not a feature**. Off, the frame is wrong wherever a
pass genuinely relied on inherited content - that is precisely what seeding is for. The real change
is to skip the copy only where the pass then overwrites the surface completely, which is a copy for
nothing, and the A/B above says what the ceiling on that is worth: 0.42ms here, and proportionally
more on a Quest 2 whose GPU is several times weaker.

It also reframes the GPU's share. 5.83ms of a 16.67ms frame is a third, against a CPU side of
5.42ms. The GPU is still not *the* bottleneck on a desktop, but it is no longer a rounding error,
and on a Quest 2 - where the GPU is several times weaker and the CPU only somewhat - that 19% is
worth real milliseconds.

## The bug: the query readback failed thousands of times a run

Found by capturing `stderr`, which every run in this session had been sending to `/dev/null`:

```
6913  vkGetQueryPoolResults failed with error code 0x1.     # VK_NOT_READY
```

`VulkanQueryPool::queryResults` returns early when that happens and **leaves the previous contents
of `results` in place**, so `CollectGPUTimings` then computes a frame time from whatever the last
successful readback held. `gpu_total_ms`, `gpu_draw_ms` and `gpu_resolve_ms` are therefore stale far
more often than not.

That undermines the number this file opens with. The GPU being nearly idle is still the right
conclusion - it rests on `fence_ms`, which is a CPU-side wall-clock wait and owes nothing to the
query pool, and on the scissor and `DONT_CARE` experiments - but **the specific "~2ms" figure should
not be quoted**, and the fix is to make a failed readback mark the slot invalid rather than silently
reuse the last one.

The wider lesson is the one worth carrying: **plume reports every failure to `stderr`, and this
session discarded `stderr` on every single run.** It was also printing `multiview feature ENABLED`
and `multiview maxViewCount=32` the whole time, which would have closed two multiview hypotheses
immediately.

## Retracted: the host bdSinCos "win" was drift, and so is desktop A/B at this scale

This section previously reported host `bdSinCos` as worth a third to a half of the main thread,
measured over field frames across two back-to-back pairs run in both orders (5.78 -> 4.10 and
6.84 -> 3.75). The default was changed on it. **It does not replicate.**

A third pair, OFF / ON / OFF, minutes later on the same binary:

```
OFF   5.12ms
ON    5.18ms     +1.1%  - no difference
OFF   8.62ms     +68%   - and this is OFF again
```

**Two OFF runs differ by 68% from each other with no configuration change**, and the ON run sits
between them. The earlier direction was the drift happening to line up twice.

The default is back to off, with a comment saying not to re-enable it on the strength of another
pair of runs.

### What this costs, and what actually measures

Three claims made today rest on paired desktop runs and all of them are now suspect at anything
under about 50%:

- The `-18%` cull redirect. **Re-run through the within-run A/B: it is real, and it is -5.6%.**
  Arm 0 (off) 10.91us/draw over 4,866 frames, arm 1 (on) 10.30 over 4,776. The mechanism was never
  in doubt - it stops computing a visibility test whose result is discarded for 95% of nodes - but
  the magnitude was overstated threefold by the two-run method, the same way sincos was overstated
  into existence.
- `bd_host_matrix_copy` at "+9.1%, wrong direction". Inside the drift. It says nothing.
- `bd_host_sincos`, retracted above.

Field-frame filtering was a real improvement - it removed the menu contamination that made the tail
window meaningless - and it is **not sufficient**. Draw count holds steady at ~520 while the work
behind those draws varies with what the camera happens to be looking at, and the drift over an hour
is larger than any of the effects being chased.

**That method now exists.** `bd_ab_flag` names a boolean cvar, `bd_ab_period` says how many frames
to hold each arm, every frame is labelled in the perf CSV, and `tools/perf_summary.py` reports the
two populations. Run against the retracted claim: **arm 0 (off) 10.23us/draw over 4,857 frames, arm
1 (on) 10.52 over 4,772 - +2.9%.** One run, interleaved, and it agrees with the retraction rather
than with either of the two pairs that produced the false result.

**The only method that can settle a sub-50% change here is alternating the two paths within a single
run** - flip the cvar every N frames and compare the two populations from the same run, the same
scene and the same thermal state. CLAUDE.md has said this since before today; today is the
demonstration of why. Anything measured as two whole runs, however carefully ordered, is a guess
dressed as a number.

## The measurement floor, which decides what is worth trying next

Cross-run variance on the desktop is around +/-20%: the same binary measured 4.20, 5.93 and 6.69ms
`other_ms` depending on where autoplay had wandered. So **a change worth less than about 2% cannot
be resolved by comparing two runs**, however carefully they are set up.

That is the reason to stop picking single-digit functions off the profile one at a time. What can
still be measured:

- **Effects above ~5%**, like the cull redirect (-18%) - two runs settle those.
- **A function's own share within one profile**, which is far more stable than the frame time
  because it is normalised by the run. `bdMatrixCopyAligned` going 1.2% -> 0.9% is real even though
  the frame times around it are not comparable.
- **Anything toggled within a single run** - a cvar flipped per frame, two code paths alternating.
  CLAUDE.md already says this and it is the only way to see a 1% change honestly.

**`bdMatrixCopyAligned` is exactly a 64-byte byte copy** - proven, not assumed: the verification
path ran the original and compared destination against source, `identical=true` on every sample
across several threads and address pairs. The guest does it with four rounds of the
lvlx/lvrx/vor unaligned idiom and a store through a full byte-reverse mask, so the reversal on the
way in is undone on the way out. `bd_host_matrix_copy` replaces it with `memcpy` and is **off by
default**: correct, and worth 0.3% by profile share, which is under the floor above. It is left in
because the ARM64 cost of that idiom is relatively higher and it should be re-measured there.

## Closed on the way

**Memoising `FindPhysicalBufferByStruct` does not help.** It is 3.2-3.4% of samples, a mutex
acquire and a hash lookup reached a couple of times per draw, so a single-entry memo guarded by a
generation counter looked free. Built and measured: **6.15us/draw against 6.28 and 6.14 without
it**, and the function stayed at 3.2%. The struct VAs do not repeat consecutively, so the cache
never hits - the cost is the hash lookup itself, not the lock. Reverted rather than carried.

**Desktop runs vary by scene as much as device runs do.** One run of the same binary came back at
12.38ms `other_ms` with `InsetQuadUVs` at 7.4% instead of 2.4% - autoplay had wandered into a
HUD-heavy stretch. It had not crashed and nothing was wrong with it. Compare **microseconds per
draw** rather than `other_ms`, check the sample count, and look at whether the profile's *shape*
matches before believing a delta. The +/-30% warning in CLAUDE.md is not Quest-specific.

## How to take one

```sh
# desktop - the fast loop, 170s, no device
#   reblue.toml: bd_sample_profiler = true
python tools/symbolize_profile.py out/build/win-amd64-release/logs/guest_profile.txt \
       --so out/build/win-amd64-release/reblue_vk.exe

# device
adb pull /sdcard/Android/data/com.reblue/files/logs/guest_profile.txt
python tools/symbolize_profile.py guest_profile.txt
```

The desktop reads the PDB through `llvm-symbolizer`, since a PE keeps no symbol table; Android reads
the unstripped `libreblue.so` through `llvm-nm`.

## Sources

- PowerPC `lvlx`/`lvrx` unaligned vector idiom: https://www.ibm.com/docs/en/aix/7.3
- SIMDe shuffle emulation: https://github.com/simd-everywhere/simde
