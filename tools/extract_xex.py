#!/usr/bin/env python3
"""Pull default.xex out of an Xbox 360 disc image without copying the disc.

codegen needs assets/default.xex, which is about 8 MB inside a 7.8 GB ISO.
Copying the whole disc to get it is a waste when the filesystem tells you
exactly which sectors to read - this walks XDVDFS and reads only those.

Works against a local file or straight off an adb-connected device, which
matters when the discs live on a handheld rather than a PC. Extraction over USB
takes under a second either way.

    python tools/extract_xex.py "D:/Blue Dragon (Disc 1).iso"
    python tools/extract_xex.py --adb-serial c3ca0370 "/storage/XXXX/Blue Dragon (Disc 1).iso"

Only reads. Never writes to the device, and never touches the disc image.
"""
import argparse
import os
import struct
import subprocess
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"

# Where the game filesystem starts, per disc format. An XGD2 disc is exactly
# 7,835,492,352 bytes; the others are listed so an unusual dump still has a
# chance of being recognised instead of silently failing.
BASES = {
    "xgd2": 0x0FD90000,
    "xgd3": 0x02080000,
    "xgd1": 0x18300000,
    "xiso": 0x00010000,
}


class Reader:
    """Reads sectors, locally or through adb. adb is the interesting case: the
    disc stays on the device and only the sectors we ask for cross the wire."""

    def __init__(self, path, adb_serial=None, adb=None):
        self.path = path
        self.adb_serial = adb_serial
        self.adb = adb or "adb"
        self._fh = None
        if adb_serial is None:
            self._fh = open(path, "rb")

    def read(self, sector, count=1):
        if self._fh is not None:
            self._fh.seek(sector * SECTOR)
            return self._fh.read(count * SECTOR)
        cmd = [self.adb, "-s", self.adb_serial, "exec-out",
               "dd if='%s' bs=%d skip=%d count=%d 2>/dev/null"
               % (self.path, SECTOR, sector, count)]
        out = subprocess.run(cmd, stdout=subprocess.PIPE, check=True).stdout
        if len(out) != count * SECTOR:
            sys.exit("short read: wanted %d bytes, got %d" % (count * SECTOR, len(out)))
        return out

    def close(self):
        if self._fh is not None:
            self._fh.close()


def find_base(reader):
    """The volume descriptor sits 32 sectors past the filesystem base."""
    for name, base in BASES.items():
        sectors = base // SECTOR
        if reader.read(sectors + 32)[:len(MAGIC)] == MAGIC:
            return name, sectors
    sys.exit("no XDVDFS volume descriptor found; is this an Xbox 360 disc image?")


def walk(table):
    """XDVDFS directory tables are binary trees of variable-length entries.
    Walking iteratively rather than recursively keeps a corrupt table from
    blowing the stack, and the visited set keeps a cyclic one from hanging."""
    seen, todo, out = set(), [0], []
    while todo:
        off = todo.pop()
        if off in seen or off + 14 > len(table):
            continue
        seen.add(off)
        left, right, sector, size, attr, nlen = struct.unpack_from("<HHIIBB", table, off)
        if nlen == 0 or off + 14 + nlen > len(table):
            continue
        name = table[off + 14:off + 14 + nlen].decode("latin1")
        out.append((name, sector, size, attr))
        # 0xFFFF is the empty-subtree sentinel; offsets are in 4-byte units.
        for child in (left, right):
            if child and child != 0xFFFF:
                todo.append(child * 4)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", help="path to the disc image, on this machine or on the device")
    ap.add_argument("-o", "--output", default="assets/default.xex")
    ap.add_argument("--adb-serial", help="read through this adb device instead of locally")
    ap.add_argument("--adb", default="adb", help="path to the adb executable")
    ap.add_argument("--name", default="default.xex", help="file to extract from the disc root")
    ap.add_argument("--list", action="store_true", help="list the disc root and exit")
    args = ap.parse_args()

    reader = Reader(args.image, args.adb_serial, args.adb)
    try:
        fmt, base = find_base(reader)
        root_sector, root_size = struct.unpack_from("<II", reader.read(base + 32), len(MAGIC))
        print("%s image, filesystem base sector %d, root table at %d (%d bytes)"
              % (fmt.upper(), base, root_sector, root_size))

        table = reader.read(base + root_sector, max(1, (root_size + SECTOR - 1) // SECTOR))
        entries = walk(table)

        if args.list:
            for name, sector, size, attr in sorted(entries):
                print("  %-4s %-34s sector=%-9d size=%d"
                      % ("DIR" if attr & 0x10 else "FILE", name, sector, size))
            return 0

        match = [e for e in entries if e[0].lower() == args.name.lower()]
        if not match:
            sys.exit("%s is not in the disc root; --list to see what is"
                     % args.name)
        name, sector, size, attr = match[0]
        if attr & 0x10:
            sys.exit("%s is a directory" % name)

        os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
        data = reader.read(base + sector, (size + SECTOR - 1) // SECTOR)[:size]
        if data[:4] != b"XEX2":
            sys.exit("extracted %s but it does not start with XEX2; wrong disc?" % name)
        with open(args.output, "wb") as fh:
            fh.write(data)
        print("wrote %s (%d bytes, XEX2)" % (args.output, size))
    finally:
        reader.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
