#!/usr/bin/env python3
"""Resolve a sampled guest profile into function names.

`bd_sample_profiler` writes module-relative PCs into
`logs/guest_profile.txt` on the device, because Horizon OS refuses shell
perf on a Quest 2 and simpleperf therefore cannot attach - which is why
`tools/profile_quest.py` has never produced a profile there. Nothing is
symbolised on device; this maps those offsets against the unstripped
`libreblue.so` from the build tree, which carries all 18,777 recompiled
function names.

  adb pull /sdcard/Android/data/com.reblue/files/logs/guest_profile.txt
  python tools/symbolize_profile.py guest_profile.txt

The recompiled guest is ordinary C++ in `generated/`, so a name here is a
name you can grep for - see the `guest-source` skill. Names that come back
as `sub_ADDR` are guest functions `config/functions.toml` has not named
yet; the address is the original PowerPC one.
"""

import argparse
import bisect
import collections
import os
import re
import subprocess
import sys

DEFAULT_SO = os.path.join(
    "out", "build", "android-arm64-release", "libreblue.so")


def find_nm():
    """The NDK ships llvm-nm; plain nm on a Windows host will not read an
    aarch64 ELF."""
    ndk = os.environ.get("ANDROID_NDK_HOME") or os.environ.get("ANDROID_NDK")
    candidates = []
    if ndk:
        candidates.append(os.path.join(
            ndk, "toolchains", "llvm", "prebuilt", "windows-x86_64", "bin",
            "llvm-nm.exe"))
    local = os.path.expanduser("~/AppData/Local/Android/Sdk/ndk")
    if os.path.isdir(local):
        for ver in sorted(os.listdir(local), reverse=True):
            candidates.append(os.path.join(
                local, ver, "toolchains", "llvm", "prebuilt", "windows-x86_64",
                "bin", "llvm-nm.exe"))
    candidates += ["llvm-nm", "nm"]
    for c in candidates:
        try:
            subprocess.run([c, "--version"], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, check=True)
            return c
        except (OSError, subprocess.CalledProcessError):
            continue
    sys.exit("no llvm-nm found - set ANDROID_NDK_HOME")


SYM_RE = re.compile(r"^([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+[tTwW]\s+(.+?)\s*$")


def load_symbols(so_path, nm):
    """Address-sorted (start, size, name), text symbols only."""
    out = subprocess.run([nm, "--defined-only", "-S", "--no-sort",
                          "--demangle", so_path],
                         capture_output=True, text=True, errors="ignore")
    syms = []
    for line in out.stdout.splitlines():
        m = SYM_RE.match(line)
        if m:
            syms.append((int(m.group(1), 16), int(m.group(2), 16), m.group(3)))
    if not syms:
        sys.exit("no text symbols in %s - is it the stripped copy from the "
                 "APK rather than the build tree?" % so_path)
    syms.sort()
    return syms


def resolve(syms, starts, off):
    i = bisect.bisect_right(starts, off) - 1
    if i < 0:
        return None
    start, size, name = syms[i]
    # A zero size means nm could not tell; accept it as the best guess rather
    # than dropping the sample.
    if size and off >= start + size:
        return None
    return name


def find_symbolizer():
    for c in ["C:/Program Files/LLVM/bin/llvm-symbolizer.exe", "llvm-symbolizer"]:
        try:
            subprocess.run([c, "--version"], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, check=True)
            return c
        except (OSError, subprocess.CalledProcessError):
            continue
    return None


def pe_image_base(path):
    """A Windows build keeps its names in a PDB, not the image, so nm finds
    nothing there. Offsets recorded on device are RVAs; llvm-symbolizer wants
    virtual addresses, which is the preferred base plus the RVA."""
    import struct
    with open(path, "rb") as f:
        f.seek(0x3c)
        pe = struct.unpack("<I", f.read(4))[0]
        f.seek(pe)
        if f.read(4) != b"PE\0\0":
            return None
        f.seek(pe + 24)
        magic = struct.unpack("<H", f.read(2))[0]
        if magic != 0x20b:
            return None
        f.seek(pe + 24 + 24)
        return struct.unpack("<Q", f.read(8))[0]


def symbolize_pe(image, offsets):
    """offsets -> {offset: name} via llvm-symbolizer, one batch."""
    sym = find_symbolizer()
    if not sym:
        sys.exit("llvm-symbolizer not found; needed for a Windows build")
    base = pe_image_base(image)
    if base is None:
        sys.exit("%s is not a 64-bit PE" % image)
    stdin = "\n".join("0x%x" % (base + o) for o in offsets)
    out = subprocess.run([sym, "--obj=" + image, "--functions=short",
                          "--demangle", "--output-style=LLVM"],
                         input=stdin, capture_output=True, text=True,
                         errors="ignore")
    names = {}
    lines = out.stdout.splitlines()
    i = 0
    for off in offsets:
        # A record is one or more (name, file:line) pairs then a blank line;
        # the pairs are an inline stack, innermost first. Attribute to the
        # OUTERMOST one - the function that actually got called - or a flat
        # profile blames std::atomic::fetch_add instead of the code doing the
        # counting, which is how the first read of this profile went wrong.
        frames = []
        while i < len(lines):
            ln = lines[i]
            i += 1
            if ln.strip() == "":
                break
            if not ln.strip():
                continue
            # names and file:line alternate; a file:line has a trailing :N:N
            if not re.search(r":\d+:\d+$", ln.strip()):
                frames.append(ln.strip())
        names[off] = frames[-1] if frames else "<unresolved>"
    return names


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("profile", help="guest_profile.txt pulled from the device")
    ap.add_argument("--so", default=DEFAULT_SO,
                    help="unstripped libreblue.so (default: %(default)s)")
    ap.add_argument("--top", type=int, default=40)
    args = ap.parse_args()

    if not os.path.exists(args.so):
        sys.exit("%s not found - build the android-arm64-release preset first"
                 % args.so)

    # The module map the profiler now writes alongside the samples.
    #
    # Samples are raw PCs, because an offset from libreblue.so cannot name a PC
    # in any other library once ASLR has moved them independently - and on
    # Android 90.8% of samples were in another library. Each "# MAP" line is a
    # /proc/self/maps row for an executable file-backed mapping.
    modules = []  # (lo, hi, file_offset, path)
    entries = []
    with open(args.profile, "r", errors="ignore") as fh:
        for line in fh:
            if line.startswith("# MAP "):
                row = line[6:].split()
                if len(row) >= 6 and "-" in row[0]:
                    lo, hi = (int(x, 16) for x in row[0].split("-", 1))
                    modules.append((lo, hi, int(row[2], 16), row[-1]))
                continue
            if line.startswith("#") or not line.strip():
                continue
            parts = line.split()
            if len(parts) != 2:
                continue
            entries.append((int(parts[0], 16), int(parts[1])))

    def module_for(pc):
        for lo, hi, foff, path in modules:
            if lo <= pc < hi:
                return path, pc - lo + foff
        return None, None

    if modules:
        # Attribute by module first, so the share landing outside libreblue is
        # visible even where we have no symbols for it.
        per_module = collections.Counter()
        for pc, count in entries:
            path, _ = module_for(pc)
            per_module[os.path.basename(path) if path else "<unmapped>"] += count
        grand = sum(per_module.values()) or 1
        print("samples by module")
        for name, n in per_module.most_common(12):
            print("  %-34s %8d %6.1f%%" % (name, n, 100.0 * n / grand))
        print()
        # Only libreblue offsets can be symbolised against --so.
        ours = os.path.basename(args.so)
        rebased = []
        for pc, count in entries:
            path, off = module_for(pc)
            if path and os.path.basename(path) == ours:
                rebased.append((off, count))
        if rebased:
            entries = rebased

    by_fn = collections.Counter()
    total = 0
    unresolved = 0

    if args.so.lower().endswith(".exe"):
        names = symbolize_pe(args.so, [e[0] for e in entries])
        for off, count in entries:
            total += count
            name = names.get(off, "<unresolved>")
            if name == "<unresolved>":
                unresolved += count
            by_fn[name] += count
    else:
        syms = load_symbols(args.so, find_nm())
        starts = [s[0] for s in syms]
        for off, count in entries:
            total += count
            name = resolve(syms, starts, off)
            if name is None:
                unresolved += count
                by_fn["<unresolved>"] += count
            else:
                by_fn[name] += count

    if not total:
        sys.exit("no samples in %s" % args.profile)

    print("%d samples, %.1f%% resolved\n" % (
        total, 100.0 * (total - unresolved) / total))
    print("%-52s %8s %7s" % ("function", "samples", "share"))
    for name, n in by_fn.most_common(args.top):
        print("%-52s %8d %6.1f%%" % (name[:52], n, 100.0 * n / total))

    print("\nA name here is a real function in generated/ - grep for "
          "DEFINE_REX_FUNC(<name>) to read its recompiled body.")


if __name__ == "__main__":
    main()
