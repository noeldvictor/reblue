#!/usr/bin/env python3
"""Build the game/ tree re:Blue expects, from disc images.

The desktop installer copies files listed in res/embed/installer/manifest.txt
out of the three discs. There is no installer on Android - it wants GTK dialogs
and it is disabled in that build - so the same job is done here and the result
pushed to the device.

    python tools/extract_game_data.py out/discs/disc1.iso -o out/game
    python tools/extract_game_data.py out/discs/*.iso -o out/game --skip-media

--skip-media leaves out movie/ and snd_stream*, which are most of the bytes and
none of the boot path. Useful for getting to a first frame without moving 15 GB
onto a headset; the game will want them back before it plays through.

--all ignores the manifest and takes every file on the disc instead. Prefer it.
The manifest is the desktop installer's list, and it is missing 1107 files that
disc 1 alone carries - every locale-specific pack and sound bank among them,
including pack/packmem_us.ipk. Without that one the game boots, renders, takes
input, and then dies the moment a new game starts:

    [disc] file-load fatal, failed file: 'D:\database\camp\ene_dic_us.u16'

Nothing before that point needs those records, so a manifest-only install looks
completely healthy right up until it isn't.

Reuses the XDVDFS reader from extract_xex.py, so this works on a local image or
straight off an adb-connected device.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_xex import Reader, find_base, walk, SECTOR  # noqa: E402

MANIFEST = os.path.join("res", "embed", "installer", "manifest.txt")

# Prefixes that --skip-media drops: prerendered video and streamed audio.
MEDIA_PREFIXES = ("movie/", "map/movie/", "snd_stream")


def load_manifest(path):
    with open(path, encoding="utf-8") as fh:
        return [line.strip().replace("\\", "/") for line in fh if line.strip()]


def index_disc(reader, base, sector, size, prefix, out):
    """Walk one directory table, recursing into subdirectories.

    Keyed by lowercased path because the manifest's casing does not always match
    the disc's, but the original casing is carried along - --all writes files the
    manifest never names, so it has nothing else to take a destination name from.
    """
    table = reader.read(base + sector, max(1, (size + SECTOR - 1) // SECTOR))
    for name, entry_sector, entry_size, attr in walk(table):
        if not name:
            continue
        path = prefix + name
        if attr & 0x10:
            # A zero-size directory table has no entries to read.
            if entry_size:
                index_disc(reader, base, entry_sector, entry_size, path + "/", out)
        else:
            out.setdefault(path.lower(), (entry_sector, entry_size, path))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("images", nargs="+", help="disc images, in disc order")
    ap.add_argument("-o", "--output", default="out/game")
    ap.add_argument("--manifest", default=MANIFEST)
    ap.add_argument("--skip-media", action="store_true",
                    help="omit movie/ and snd_stream*, most of the bytes")
    ap.add_argument("--all", action="store_true",
                    help="take every file on the disc instead of the manifest's "
                         "list. The manifest misses every locale-specific pack, "
                         "which the game needs the moment a new game starts.")
    ap.add_argument("--adb-serial")
    ap.add_argument("--adb", default="adb")
    args = ap.parse_args()

    wanted = [] if args.all else load_manifest(args.manifest)
    if args.skip_media and not args.all:
        before = len(wanted)
        wanted = [p for p in wanted if not p.lower().startswith(MEDIA_PREFIXES)]
        print("manifest: %d files, %d after --skip-media" % (before, len(wanted)))
    else:
        print("manifest: %d files" % len(wanted))

    # Later discs win only where an earlier one lacked the file, matching the
    # installer's "first disc that has it" behaviour closely enough: identical
    # files are duplicated across discs.
    catalogue = {}
    readers = []
    for image in args.images:
        reader = Reader(image, args.adb_serial, args.adb)
        readers.append(reader)
        _fmt, base = find_base(reader)
        root_sector, root_size = struct.unpack_from(
            "<II", reader.read(base + 32), 20)
        found = {}
        index_disc(reader, base, root_sector, root_size, "", found)
        print("%s: %d files on disc" % (os.path.basename(image), len(found)))
        for key, value in found.items():
            catalogue.setdefault(key, (reader, base) + value)

    if args.all:
        # Disc order still decides which copy wins, because catalogue was built
        # with setdefault above.
        wanted = sorted(entry[4] for entry in catalogue.values())
        if args.skip_media:
            before = len(wanted)
            wanted = [p for p in wanted
                      if not p.lower().startswith(MEDIA_PREFIXES)]
            print("disc: %d files, %d after --skip-media" % (before, len(wanted)))
        else:
            print("disc: %d files" % len(wanted))

    written = missing = 0
    total_bytes = 0
    for path in wanted:
        hit = catalogue.get(path.lower())
        if not hit:
            missing += 1
            continue
        reader, base, sector, size, _disc_path = hit
        dest = os.path.join(args.output, path.replace("/", os.sep))
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        if os.path.exists(dest) and os.path.getsize(dest) == size:
            continue  # already extracted; makes reruns cheap
        data = reader.read(base + sector, max(1, (size + SECTOR - 1) // SECTOR))[:size]
        with open(dest, "wb") as fh:
            fh.write(data)
        written += 1
        total_bytes += size

    # The installer drops this once a copy completes; the runtime uses it to
    # tell a finished install from an interrupted one.
    marker = os.path.join(args.output, "reblue_install.marker")
    with open(marker, "w", encoding="utf-8") as fh:
        fh.write("extracted by tools/extract_game_data.py\n")

    for reader in readers:
        reader.close()

    print("wrote %d files (%.1f MB), %d manifest entries not on these discs"
          % (written, total_bytes / 1e6, missing))
    return 0


if __name__ == "__main__":
    sys.exit(main())
