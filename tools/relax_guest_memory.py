#!/usr/bin/env python3
"""Drop `volatile` from the guest RAM access macros in the generated PCH.

Every guest load and store in the game goes through eight macros in
generated/reblue_pch.h, and all eight cast through `volatile`. That forbids the
host compiler from eliminating a redundant reload, forwarding a store to a
load, or keeping anything in a register across statements - on every variable
access in the entire game.

Measured on a representative recompiled sequence, compiled for
aarch64-linux-android29 at -O2: 11 instructions without `volatile`, 23 with.
A 1:1 PowerPC translation undoes the original compiler's register allocation,
so the redundant loads this would remove are everywhere.

    python tools/relax_guest_memory.py generated/reblue_pch.h

**This is not obviously safe.** `volatile` is presumably there so the compiler
cannot hoist a load out of a guest spin-loop - a guest thread polling a flag in
shared memory would hang. Nothing here proves Blue Dragon has no such loop, so
this is an experiment to be measured on a device, not a default. It is wired to
REBLUE_RELAXED_GUEST_MEMORY, which is OFF.

The MMIO macros are deliberately left alone. Those really are device registers
and really do need `volatile`; the point of this is that ordinary guest RAM is
not a device register.

The edit is idempotent, so a rebuild that re-runs codegen and then re-applies
this is fine.
"""
import io
import re
import sys

# The eight ordinary RAM accessors. Named explicitly rather than matched by
# shape, so REX_MM_* and anything added later are untouched by construction.
MACROS = ("REX_LOAD_U8", "REX_LOAD_U16", "REX_LOAD_U32", "REX_LOAD_U64",
          "REX_STORE_U8", "REX_STORE_U16", "REX_STORE_U32", "REX_STORE_U64")


def main(argv):
    if len(argv) != 2:
        sys.exit("usage: relax_guest_memory.py <path to reblue_pch.h>")
    path = argv[1]

    with io.open(path, encoding="utf-8", newline="") as fh:
        raw = fh.read()
    crlf = "\r\n" in raw
    text = raw.replace("\r\n", "\n")

    changed = 0
    out = []
    for line in text.split("\n"):
        if line.startswith("#define ") and any(
                line.startswith("#define " + m + "(") for m in MACROS):
            relaxed = re.sub(r"\(volatile (u(?:8|16|32|64))\*\)",
                             r"(\1*)", line)
            if relaxed != line:
                changed += 1
            out.append(relaxed)
        else:
            out.append(line)

    if changed == 0:
        print("guest memory macros already relaxed (or not found)")
        return 0

    text = "\n".join(out)
    if crlf:
        text = text.replace("\n", "\r\n")
    with io.open(path, "w", encoding="utf-8", newline="") as fh:
        fh.write(text)
    print("relaxed %d guest memory macros in %s" % (changed, path))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
