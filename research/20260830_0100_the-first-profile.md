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

## Correction: host bdSinCos is a 30-45% win, and was dismissed by bad measurement

Recorded below as "correct but not proven faster". That was wrong, and the reason is the reason
everything else below it needs re-reading: it was measured on **the tail of the perf CSV**, which is
a menu the run gets stuck in and where almost nothing calls sin.

Re-measured over field frames only (`tools/perf_summary.py`), two pairs run in both orders, ~9,000
field frames each, identical draw counts:

| | host sincos ON | OFF | draws |
| --- | --- | --- | --- |
| pair 1 | 4.10ms | 5.78ms | 521 / 519 |
| pair 2 | 3.75ms | 6.84ms | 523 / 523 |

**Between a third and a half of the main thread's time.** The guest version is 222 recompiled
instructions that read their polynomial constants back through marshalled guest loads; the host
version is two libm calls. `bd_host_sincos` is now on by default, with the stereo check and a
captured frame confirming the world is still oriented correctly.

**Everything else dismissed on this page was measured the same wrong way** and is worth re-running
against the field-frame metric before staying closed - the matrix copy at "0.3%", the buffer memo,
and the tile-traffic upper bound especially.

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
