# Fusing lvlx/lvrx/vor: two attempts, both reverted, and why

2026-08-30. The largest X360-pattern removal still available in the guest. Two implementations were
written in the forked SDK, both compiled, both ran, and **both are reverted** - the first fired on
1.2% of the idiom and the second on none of it. Each failed for a concrete reason that was invisible
until the guest was measured rather than reasoned about. Written up so the next attempt starts from
the measurements instead of the idea.

## What the pattern costs

PowerPC loads a 16-byte vector from a possibly-unaligned address with three instructions:

```
lvlx vA, rX, rY     # left part
lvrx vB, rX, rY     # right part
vor  vC, vA, vB     # combine
```

`bdMatrixCopyAligned` uses it four times to copy 64 bytes, and `sub_82287788` - which was the
hottest function in the process before the cull redirect - uses it **twenty times per scene node**.
After recompilation each `lvlx` is:

```cpp
simde_mm_store_si128((simde__m128i*)ctx.v0.u8,
  simde_mm_shuffle_epi8(
    simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(temp.u32 & ~0xF)),
    simde_mm_load_si128((simde__m128i*)&VectorMaskL[(temp.u32 & 0xF) * 16])));
```

Two loads - one of them a 256-byte mask table indexed by the low four address bits - a shuffle, and
a store. The triple therefore costs **four loads, two shuffles, two stores and an OR** to move
sixteen bytes that x86 does in one `movdqu` and ARM64 in one `ldr q`.

Worse, the intermediate registers are usually v0-v13, the *argument* registers, and only v14-v127
are localised (`non_volatile_as_local`), so they genuinely round-trip through `ctx` memory.

## The semantics are established, not guessed

`bdMatrixCopyAligned` is exactly a 64-byte byte copy - proven by running the recompiled original and
comparing destination against source, `identical=true` on every sample. Since it is built entirely
out of this idiom, the net effect of `lvlx`/`lvrx`/`vor` at an address is confirmed: **load sixteen
bytes, byte-reversed into the register**, which is the same convention `lvx` uses.

## The mechanism exists

`BuilderContext` already reaches its neighbours - `mmio_check_d_form` does
`base + 4 < fn.end() && *(data + 1) == kEieioEncoding`, so `data` is the instruction stream and
`base` the current address. And `function_graph.cpp:529` shows the decoder is reusable:

```cpp
ppc_insn next;
Disassemble(data + 1, 4, base + 4, next);
```

So no dispatcher change and no skip mechanism is needed. Each of the three builders re-detects the
same pattern from its own position and they agree:

- **`build_lvlx`** - if `data+1` is LVRX/LVRX128 with the same operands[1] and operands[2], and
  `data+2` is VOR/VOR128 combining exactly this lvlx's destination with that lvrx's, emit one
  unaligned load into the *vor's* destination register and nothing else.
- **`build_lvrx`** - if `data-1` is the matching LVLX and `data+1` the matching VOR, emit nothing.
- **`build_vor`** - if `data-1` and `data-2` are the matching pair, emit nothing.

## Implemented, and it does not fire. Measure adjacency before building this.

The three builders were written, they compile, and the flag
(`fuse_unaligned_vector_loads`, off by default) reaches codegen. Running it changed **nothing** -
219 files unchanged - and the reason is the assumption underneath the whole design:

```
lvlx instructions in the guest            2358
adjacent lvlx / lvrx / vor triples          28     (1.2%)

distance from an lvlx to its matching lvrx
   +1 : 166      +2 : 243      +3 : 74      +4 : 114      +5 : 41 ...
```

**The Xenon compiler scheduled the two halves apart.** In `bdMatrixCopyAligned` the sequence reads
`lvlx v8` / `li r10,32` / `li r9,32` / `addi r8,r3,48` / `lvlx v0` / `li r11,48` / `lvrx v13` - four
loads in flight at once, interleaved with address arithmetic, which is exactly what a scheduler for
an in-order core with a deep load pipeline should do. An adjacent-triple matcher catches 1.2% of the
idiom and is therefore worth approximately nothing.

Making it real needs **block-local dataflow**: from an `lvlx`, scan forward inside the block for the
`lvrx` sharing both address registers and the `vor` combining the two destinations, and prove that
nothing in between writes the address registers or reads either half. The disassembler gives
operands but not their roles, so "is this a write to vN" has to be inferred per opcode - and
inferring it wrong is a silent miscompile in every unaligned vector load in the game. That is a
real analysis pass, not a peephole.

The scaffold is kept because the detection, the block-containment check and the flag plumbing are
the parts that were fiddly, and they are correct. What is missing is the scan.

## The forward scan was tried too, and it cannot be done this way

The obvious repair for the adjacency problem is to scan forward inside the block for the matching
`lvrx` and `vor`, refusing whenever an intervening instruction might disturb the registers involved.
Doing that soundly needs to know which operand slots an instruction *writes*, and the disassembler
hands back `uint32_t operands[8]` with no roles - so the conservative version was tried instead: bail
if a register number appears anywhere in an intervening instruction.

**It never fires once.** Instrumented over the whole guest: 2358 `lvlx` sites, 0 matching `lvrx`
found. The reason is specific and worth writing down, because it is invisible until measured:

- In `lvlx v8,0,r4` the base register field is **`r0`** - the encoding's "no base register, treat as
  zero". So `rA == 0`.
- `operands[8]` is **zero-filled** for slots an instruction does not use.
- So "does this instruction mention register 0" is true of very nearly every instruction, and the
  scan bails on the first one every time.

Distinguishing "slot 2 holds register r0" from "slot 5 is unused" requires the per-opcode operand
descriptors, which is the same role information the sound version needed in the first place. The
conservative shortcut does not exist.

**Both attempts are reverted.** The adjacent matcher fired on 28 of 2358 sites and the forward scan
on none, so neither earns its place in the tree; carrying a codegen path that never executes is
worse than not having one. What survives is this note.

## What a real attempt needs

1. Operand role information - binutils-style `powerpc_opcode` carries operand descriptor indices, so
   the data is there; it needs plumbing through `ppc_insn` or a lookup beside it.
2. A block-local liveness scan built on that, not a "mentions" heuristic.
3. Validation on ARM64 before it is enabled anywhere, per below.

Worth doing when someone has a device in front of them, and not before: the payoff is real (2358
sites, each currently four loads, two shuffles, two stores and an OR) but every failure mode is a
silent miscompile.

## Why it stays off

**The three instructions must be in one basic block**, and this is checked: `fn.blocks()` gives the
ranges and the detector requires all three inside one of them. A block is straight-line by
construction, so nothing can branch into the middle - without that check a branch targeting the
`lvrx` or the `vor` would find its register unwritten, which miscompiles silently.

The intermediate registers are deliberately left unwritten even in the good case. That is sound for
the idiom as a compiler emits it and is not guaranteed by the encoding, which is the second reason
the block check is not optional.

Neither of those is what stops it. It stays off because it is worth 1.2% of the idiom until the
forward scan is written, and because a codegen change of this reach has to be proven on ARM64.

**And desktop validation is not sufficient evidence for a codegen change.** `non_argument_as_local`
built cleanly, cut `ctx.` accesses by 36%, ran on x86, and killed the game 0.2s after the VFS mounts
on ARM64. This change touches every unaligned vector load in the guest. Shipping it on the strength
of a desktop run would be repeating that exactly.

## How to validate it when the device is back

Unusually good, because a direct test already exists:

1. `bd_verify_guest_math=true` runs `bdMatrixCopyAligned`'s original and compares destination
   against source byte for byte. That function is built *entirely* out of this idiom, so a wrong
   fusion shows up as `identical=false` on the first frame rather than as corruption three scenes
   later.
2. `tools/stereo_check.py --raw` on a capture - the camera maths is vector-heavy.
3. A capture, looked at.
4. `bash tools/verify_quest.sh` for the frame breakdown and an on-device profile, which is what says
   whether it was worth doing at all.

Do 1 before 4. A miscompile that only shows as a frame-rate change is the worst outcome available.

## Sources

- PowerPC `lvlx`/`lvrx` unaligned vector idiom: https://www.ibm.com/docs/en/aix/7.3
- SIMDe `_mm_shuffle_epi8` emulation on NEON: https://github.com/simd-everywhere/simde
