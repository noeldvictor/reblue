#!/usr/bin/env python3
"""Count Xenos shader containers in a file, the way XenosRecomp finds them.

XenosRecomp scans raw bytes for a ShaderContainer whose big-endian flags match
0x102A1100, then sanity-checks the sizes. Reproducing that check here means a
candidate file can be tested in a second instead of by running a build.
"""
import struct
import sys


def count(path):
    data = open(path, "rb").read()
    hits, i = 0, 0
    while True:
        i = data.find(b"\x10\x2a\x11", i)
        if i < 0 or i + 36 > len(data):
            break
        flags, virtual, physical, _c, _ct, _dt, _so, f1c, f20 = struct.unpack_from(">9I", data, i)
        size = virtual + physical
        # Same three conditions XenosRecomp applies.
        if (flags & 0xFFFFFF00) == 0x102A1100 and size <= len(data) - i and f1c == 0 and f20 == 0:
            hits += 1
            i += max(size, 4)
        else:
            i += 1
    return len(data), hits


if __name__ == "__main__":
    for path in sys.argv[1:]:
        size, hits = count(path)
        print("%-40s %10d bytes  containers: %d" % (path, size, hits))
