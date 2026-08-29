# Research: is the recompiled guest actually using ARM64 well?

Date: 2026-08-28 22:00
Topic: what the generated ARM64 looks like for Xenon's vector unit, measured out of the shipped
`libreblue.so`.

The question was whether the port takes advantage of ARM64 and the Quest 2's hardware. Answered by
disassembling the 139 MB `libreblue.so` and counting, with no device involved.

Headline: **NEON is being used, and it is being used badly** - not because the wrong instructions
are chosen, but because the vector register file lives in memory.

---

## 1. Yes, NEON is mandatory and yes, it is being used

Unlike ARMv7 where it was optional, **NEON (Advanced SIMD) is a required part of ARMv8-A**, so every
AArch64 device - the Quest 2's Snapdragon XR2 included - has it. No feature detection needed.

Xenon's VMX/AltiVec unit is translated to **SSE intrinsics through SIMDe**, which maps them to NEON
on AArch64. A `vmaddfp` comes out as:

```c
// vmaddfp v12,v6,v12,v7
simde_mm_store_ps(ctx.v12.f32,
    simde_mm_add_ps(simde_mm_mul_ps(simde_mm_load_ps(ctx.v6.f32),
                                    simde_mm_load_ps(ctx.v12.f32)),
                    simde_mm_load_ps(ctx.v7.f32)));
```

That is real vector code, and there is a lot of VMX in this game: 1865 `vor`, 1562 `vmulfp128`,
1345 `vmsum4fp128`, 1333 `vspltw`, 1117 `vmrghw`, 900 `vperm`, 719 `vmaddfp`, and so on.

## 2. But look at the shape of it

Every vector operation **loads its operands out of `ctx.vN` and stores the result straight back**.
The vector registers are memory, not registers. Counting mnemonics across the whole library:

| | instructions |
| --- | --- |
| Vector register memory traffic (`ldr`/`str`/`stur`/`stp`/`ldp`/`ldur` on q registers) | **~129,800** |
| Endian swizzling (`tbl` 9595, `rev32` 2703, `rev64` 2577, `rev16` 1330, `ext` 2400) | **~18,600** |
| Actual float SIMD arithmetic (`fmul` 5646, `fadd` 1184, `fsub` 1259, `fmla` 248, `fdiv` 273) | **~8,600** |

**About fifteen memory operations for every useful arithmetic operation.** And byte-swizzling - the
big-endian conversion on every vector load and store, which is what `_mm_shuffle_epi8` becomes, all
12,311 uses of it - costs **more than twice** what the arithmetic does.

Some of that memory traffic is inherent: the original PowerPC code did `lvx`/`stvx` too. What is not
inherent is the round trip *between consecutive vector instructions*. When `vmaddfp` writes `v12`
and the next instruction reads `v12`, that value should stay in a NEON register. It does not; it
goes to memory and comes back.

## 3. Why the general-purpose registers do not have this problem

Because there is a flag for them, and it was switched on:

```toml
non_volatile_as_local = true
non_argument_as_local = true    # added this session, -36% ctx accesses
```

Those make the guest's GPRs C locals, which the compiler then keeps in ARM64 registers. **There is
no equivalent flag for the vector registers.** The SDK exposes exactly eight codegen flags -
`skip_lr`, `ctr_as_local`, `xer_as_local`, `reserved_as_local`, `cr_as_local`,
`non_argument_as_local`, `non_volatile_as_local`, `skip_msr` - and not one of them touches VMX.

So the same optimisation that just removed 743,696 context accesses for integer registers has never
been applied to the vector unit, in a game whose maths is largely vector.

## 4. What to do about it

This is the deep ARM64 rework the fork has been authorised for, and it is a codegen change in the
forked SDK rather than anything in `src/`:

1. **Vector registers as locals.** Emit `simde__m128 v12;` as a function local and let the compiler
   allocate it, spilling to `ctx.vN` only at call boundaries and on entry/exit - exactly what
   `non_argument_as_local` does for GPRs. This is the large one.
2. **Hoist the endian conversion.** A value loaded, operated on, and stored back is byte-swapped on
   the way in and the way out for no reason. If a vector register can stay in host byte order for
   the length of a function, most of those 9,595 `tbl` instructions disappear. Harder than (1), and
   should follow it.
3. **`fmla`.** Only 248 fused multiply-adds against 5,646 `fmul` and 1,184 `fadd`, despite 719
   `vmaddfp` in the source, which is exactly a multiply-add. SIMDe's `_mm_add_ps(_mm_mul_ps(a,b),c)`
   is not being contracted into `fmla`. Worth checking whether `-ffp-contract=fast` is safe here -
   the guest's own results are already not bit-exact with a Xenon.

## 5. What was ruled out, so it is not chased again

- **LSE atomics.** The build targets baseline `armv8-a` with no `-march`/`-mcpu`, so single-
  instruction atomics are not available - but there are only about 11 LSE and 24 exclusive-loop
  instructions in 5.8 million. Atomics are nowhere near hot. `-mcpu=cortex-a77` may still help
  scheduling, but not for this reason.
- **fp16 and dotprod**: zero uses. Recompiled PowerPC would not produce them naturally, and nothing
  here would benefit.
- **`-O3` is already on**, and `REX_PHYS_HOST_OFFSET` compiles to nothing on Android.

## 6. Caveat on the numbers

These counts are over the whole `libreblue.so`, which includes SDL, plume and the rest, so a
fraction of the `ldr q` traffic is ordinary `memcpy` in other libraries rather than guest vector
state. The guest is the overwhelming majority of a 139 MB binary, so the ratio is directionally
right, but it is a static instruction count and **not a profile**. It says where the instructions
are, not where the time goes. `tools/profile_quest.py` still has to run before anything here is
called the bottleneck.
