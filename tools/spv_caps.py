#!/usr/bin/env python3
"""Report the SPIR-V capabilities declared across a directory of .spv modules.

This exists because a driver that refuses a shader says almost nothing. The
Adreno 740 answers `Shader compilation failed for shaderType: 0` and the
validation layers have nothing to flag, because declaring a capability the
device does not support is not a spec violation on its own - it just cannot be
compiled. The only way to see it is to decode the modules.

    python tools/spv_caps.py out/build/android-arm64-release/hlsl_dump

The check that matters for this port:

    Int64                            141 / 141
    PhysicalStorageBufferAddresses   141 / 141

against a device reporting `shaderInt64=0` - which is why nothing renders on an
Adreno 740. Every recompiled shader reads guest constants through
`vk::RawBufferLoad` at a 64-bit device address, so every one needs Int64. See
research/20260830_0820_arm64-the-thor-renders-nothing.md.

Run it after any change to how constants reach the shader: `Int64  0 / N` is
what landing that fix looks like, and nothing else proves it.
"""

import argparse
import glob
import os
import struct
import sys
from collections import Counter

SPIRV_MAGIC = 0x07230203
OP_CAPABILITY = 17

# Only the ones worth naming here; anything else prints as its number.
NAMES = {
    0: "Matrix", 1: "Shader", 11: "Int64", 22: "Int16",
    50: "SampledBuffer",
    # MultiView is 4439 (SPV_KHR_multiview), not 32. Confirmed against this
    # tree: it appears in exactly 55 of 141 modules, which is the 55 vertex
    # shaders that carry SV_ViewID.
    4439: "MultiView",
    4427: "ShaderNonUniform",
    5301: "GroupNonUniform",
    5302: "RuntimeDescriptorArray",
    5306: "GroupNonUniformArithmetic",
    5345: "StorageBuffer16BitAccess",
    5347: "PhysicalStorageBufferAddresses",
}


def capabilities(path):
    """Every OpCapability in a module, or None if it is not SPIR-V."""
    with open(path, "rb") as fh:
        blob = fh.read()
    if len(blob) < 20:
        return None
    if struct.unpack("<I", blob[:4])[0] != SPIRV_MAGIC:
        return None
    words = struct.unpack("<%dI" % (len(blob) // 4), blob[:len(blob) // 4 * 4])
    caps, i = set(), 5  # 5-word header
    while i < len(words):
        word = words[i]
        opcode, length = word & 0xFFFF, word >> 16
        if length == 0:
            break
        # Capabilities are declared in one block at the top; stop at the first
        # instruction that cannot appear in it, so this does not walk a whole
        # multi-megabyte module for nothing.
        if opcode == OP_CAPABILITY and i + 1 < len(words):
            caps.add(words[i + 1])
        elif opcode not in (OP_CAPABILITY, 10, 5, 3):  # Extension, Name, Source
            break
        i += length
    return caps


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dir", help="directory of .spv files (hlsl_dump)")
    ap.add_argument("--require-absent", action="append", default=[],
                    help="capability name that must appear in NO module; "
                         "exits non-zero if it does. Repeatable.")
    args = ap.parse_args()

    files = sorted(glob.glob(os.path.join(args.dir, "*.spv")))
    if not files:
        sys.exit("no .spv in %s - build the reblue_shader_hlsl_dump target"
                 % args.dir)

    counts, total = Counter(), 0
    for path in files:
        caps = capabilities(path)
        if caps is None:
            continue
        total += 1
        for c in caps:
            counts[c] += 1

    print("%d SPIR-V modules in %s\n" % (total, args.dir))
    for cap, n in sorted(counts.items(), key=lambda kv: (-kv[1], kv[0])):
        print("  %-32s %4d / %d" % (NAMES.get(cap, "capability %d" % cap),
                                    n, total))

    bad = 0
    for want in args.require_absent:
        hits = [c for c, n in counts.items() if NAMES.get(c) == want and n]
        if hits:
            print("\nFAIL: %s is still declared by %d of %d modules"
                  % (want, counts[hits[0]], total))
            bad = 1
        else:
            print("\nOK: %s is declared by no module" % want)
    return bad


if __name__ == "__main__":
    sys.exit(main())
