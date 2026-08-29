---
name: guest-source
description: Read Blue Dragon's original code. The whole XEX is already translated to C++ in generated/, with the PowerPC interleaved as comments - use this to find guest addresses, struct offsets, and what the original binary does, before reaching for any disassembler.
---

# Reading the guest

**Do not install a decompiler. The binary is already decompiled, in this repo.**

re:Blue is a static recompilation: `rexglue codegen` translates the entire XEX into C++ ahead of
time. `generated/` is 223 files and about 110 MB containing **18,777 functions** - the whole
executable, not a sample. It is a build product and gitignored, so it exists only after a codegen
run; `cmake --build … --target reblue_codegen` takes about 7 seconds and is deterministic.

This is strictly better than a decompiler for this codebase, for three reasons:

- **It is exact.** Not a reconstruction that might be wrong - it is the translation the game
  actually runs.
- **It is named.** Every address in `config/functions.toml` becomes
  `DEFINE_REX_FUNC(bdPlayerFieldMovementUpdate)`, so 1608 functions read in the project's own
  vocabulary. The rest get generated names.
- **It carries the original PowerPC as comments**, line by line, next to the C++ that implements it.
  You get the assembly and its meaning together.

## Finding things

```sh
# A function, by its name from config/functions.toml
grep -rln "DEFINE_REX_FUNC(bdPlayerFieldMovementUpdate)" generated/

# By guest address - it appears in the comments, at branches into it
grep -rn "0x82207858" generated/

# What exists at all, when you do not know the name
grep -rho "DEFINE_REX_FUNC([A-Za-z0-9_]*)" generated/*.cpp | sort -u | grep -i camera
```

Then read it. The body is a register machine, which sounds unreadable and is not:

```c
DEFINE_REX_FUNC(bdPlayerFieldMovementUpdate) {
	// mr r31,r3
	r31.u64 = ctx.r3.u64;        // r3 is the first argument - the player object
	// lwz r11,7224(r31)
	r11.u64 = REX_LOAD_U32(r31.u32 + 7224);   // and 7224 is a field in it
```

## What it is good at

**Struct offsets, which is usually the actual question.** `lwz r11,7224(r31)` says field 7224 of
whatever `r3` pointed at. Read a few call sites and the layout falls out. This is how to answer
things like "where does the player keep its world position", which `config/functions.toml` cannot
tell you because it only has names and sizes.

**Register-to-argument mapping.** `ctx.r3` is the first argument, `r4` the second, and so on -
exactly the register names a `midasm_hook` lists in `config/hooks/*.toml`. A hook body and the
generated source talk about the same registers, so they read against each other directly.

**Control flow**, since the branches are in the comments with their targets, which is what
`jump_address_on_true` in a hook needs.

## What to be careful about

- **Guest memory is big-endian, the host is not.** `REX_LOAD_U32` and `REX_STORE_U32` handle it
  inside the generated code, but a host hook reading the same field has to swap it itself -
  `__builtin_bswap32`. Every guest struct read in `src/` does.
- **A float field is `lfs`, not `lwz`**, and floats in a struct are usually a run at 4-byte
  intervals: a vec3 is 12 bytes, a 4x4 matrix 64.
- **Confirm an offset at two call sites**, or check it is written where you would expect it to be
  written. One reader can be a coincidence.
- **`generated/` is a build product.** Never edit it, and never commit it. If it rebuilds
  unexpectedly a codegen input changed - find out which.

## Look here first, even before generated/

`config/hooks/*.toml` carries guest addresses **with a comment describing the surrounding code**,
written by whoever worked it out. Those comments are the accumulated map of this binary and are
often the whole answer. Read them first, and add to them whenever you learn something - that is how
the map grows.

## The open question this was set up for

`bd::xr::Camera::SubmitCharacter()` is never called. `CharacterAnchor` - the party leader's
position, eye height and facing - has no source, so `ThirdPerson` and `FirstPerson` fall back to the
game's own camera position and the character-anchored VR modes do not do what their names say.

The lead is `bdPlayerFieldMovementUpdate` (`0x82207858`) in `generated/reblue_recomp.30.cpp`, where
`r3` is the player object. What is needed is the offset of its world position and facing.
