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
