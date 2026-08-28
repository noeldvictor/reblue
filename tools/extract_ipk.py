#!/usr/bin/env python3
"""Unpack Blue Dragon's IPK1 archives.

The game keeps its data in `pack/*.ipk`, and every entry inside is zlib
compressed. That is why scanning the archives for Xenos shader containers finds
nothing: the containers are there, just not in the clear.

    python tools/extract_ipk.py out/gamedata/battle.ipk -o out/unpacked
    python tools/extract_ipk.py out/gamedata/*.ipk -o out/unpacked --only-shaders

Format, worked out from battle.ipk:

    0x00  'IPK1'
    0x04  u32  header size (0x80)
    0x08  u32  entry count
    0x0C  u32  total archive size
    0x10  entry records, 96 bytes each:
            0x00  char[64]  name, NUL padded, backslash separated
            0x40  u32       flags (1 on every entry seen)
            0x44  u32       compressed size
            0x48  u32       offset, 256-aligned
            0x4C  u32       decompressed size
            0x50  u32       constant, identical across entries in an archive
            0x54  12 bytes  padding

All little-endian, which is worth noting on a big-endian console - the archive
format is not the game's runtime data layout.
"""
import argparse
import os
import struct
import sys
import zlib

MAGIC = b"IPK1"
RECORD = 96
NAME_LEN = 64

# A Xenos shader container, as XenosRecomp identifies it. Big-endian here,
# unlike the archive around it.
SHADER_MAGIC = 0x102A1100


def entries(data):
    if data[:4] != MAGIC:
        raise ValueError("not an IPK1 archive")
    _magic, _header, count, _total = struct.unpack_from("<4sIII", data, 0)
    for i in range(count):
        off = 16 + i * RECORD
        if off + RECORD > len(data):
            break
        raw_name = data[off:off + NAME_LEN].split(b"\x00", 1)[0]
        flags, clen, data_off, dlen, _const = struct.unpack_from("<5I", data, off + NAME_LEN)
        yield raw_name.decode("latin1").replace("\\", "/"), flags, clen, data_off, dlen


def unpack(data, clen, off, dlen):
    blob = data[off:off + clen]
    if clen == dlen:
        return blob  # stored, not compressed
    out = zlib.decompress(blob)
    if len(out) != dlen:
        raise ValueError("expected %d bytes, got %d" % (dlen, len(out)))
    return out


def has_shader(blob):
    """The same check XenosRecomp applies, so a hit here means a hit there."""
    i = 0
    while True:
        i = blob.find(b"\x10\x2a\x11", i)
        if i < 0 or i + 36 > len(blob):
            return False
        flags, virtual, physical, _c, _ct, _dt, _so, f1c, f20 = struct.unpack_from(">9I", blob, i)
        if ((flags & 0xFFFFFF00) == SHADER_MAGIC and virtual + physical <= len(blob) - i
                and f1c == 0 and f20 == 0):
            return True
        i += 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("archives", nargs="+")
    ap.add_argument("-o", "--output", default="out/unpacked")
    ap.add_argument("--only-shaders", action="store_true",
                    help="write only entries containing a Xenos shader container, "
                         "which is what XenosRecomp wants fed to it")
    ap.add_argument("--list", action="store_true", help="list entries without writing")
    args = ap.parse_args()

    total_written = total_entries = total_shaders = 0
    for path in args.archives:
        data = open(path, "rb").read()
        try:
            records = list(entries(data))
        except ValueError as exc:
            print("%s: %s" % (path, exc), file=sys.stderr)
            continue

        written = shaders = 0
        for name, _flags, clen, off, dlen in records:
            if not name:
                continue
            total_entries += 1
            if args.list:
                print("  %-52s %8d -> %8d" % (name, clen, dlen))
                continue
            try:
                blob = unpack(data, clen, off, dlen)
            except Exception as exc:
                print("  %s: %s" % (name, exc), file=sys.stderr)
                continue

            if args.only_shaders:
                if not has_shader(blob):
                    continue
                shaders += 1

            dest = os.path.join(args.output, os.path.basename(path), name)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, "wb") as fh:
                fh.write(blob)
            written += 1

        total_written += written
        total_shaders += shaders
        print("%-40s %5d entries, wrote %d%s"
              % (os.path.basename(path), len(records), written,
                 " (%d with shaders)" % shaders if args.only_shaders else ""))

    print("total: %d entries, %d written%s"
          % (total_entries, total_written,
             ", %d containing shaders" % total_shaders if args.only_shaders else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
