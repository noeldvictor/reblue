#!/usr/bin/env python3
"""Find out where the guest's CPU time actually goes, on the device.

A field scene costs about 180ms of CPU per frame against 1ms on the GPU
fence, so the thing standing between this port and playable VR is the
recompiled PowerPC - not shaders, not textures, not foveation. This says
which functions.

No instrumentation is needed. Every recompiled guest function is a real
symbol in libreblue.so - 27,080 of them - so simpleperf, which ships in the
NDK, can name them directly:

    python tools/profile_quest.py                # 10s, top 40 symbols
    python tools/profile_quest.py --seconds 20
    python tools/profile_quest.py --callgraph    # who calls the expensive ones

Two things have to be true, and both already are: the manifest sets
android:debuggable, and extractNativeLibs leaves the .so on disk where
simpleperf can read its symbol table.

Read the output against the names in config/functions.toml - a hot function
already named there is one that can be replaced wholesale with a host
REX_FUNC implementation, which is the cheapest large win available.
"""
import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SDK = os.environ.get(
    "ANDROID_HOME",
    os.path.expanduser("~/AppData/Local/Android/Sdk").replace("\\", "/"))
ADB = os.path.join(SDK, "platform-tools", "adb.exe")
NDK = os.environ.get(
    "ANDROID_NDK",
    os.path.join(SDK, "ndk", "30.0.15729638"))
SIMPLEPERF = os.path.join(NDK, "simpleperf", "bin", "android", "arm64",
                          "simpleperf")
PACKAGE = "com.reblue"
DEVICE_DIR = "/data/local/tmp"


def adb(*args, **kw):
    """adb, with MSYS path conversion off - it rewrites /data/local/tmp into
    C:/Program Files/Git/data/local/tmp and the failure is silent."""
    env = dict(os.environ, MSYS_NO_PATHCONV="1")
    return subprocess.run([ADB] + list(args), env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, **kw)


def device_pid():
    out = adb("shell", "pidof", PACKAGE).stdout.strip()
    return out.split()[0] if out else None


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seconds", type=int, default=10)
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--callgraph", action="store_true",
                    help="record call chains too; heavier, but says who is "
                         "calling the expensive function")
    args = ap.parse_args()

    if not os.path.exists(SIMPLEPERF):
        sys.exit("no simpleperf at %s - set ANDROID_NDK" % SIMPLEPERF)

    pid = device_pid()
    if not pid:
        sys.exit("%s is not running. Launch it, get into a field scene "
                 "(bd_xr_autoplay does this unattended), then profile." %
                 PACKAGE)
    print("profiling pid %s for %ds" % (pid, args.seconds))

    push = adb("push", SIMPLEPERF, DEVICE_DIR + "/simpleperf")
    if "1 file pushed" not in push.stdout:
        sys.exit("could not push simpleperf:\n" + push.stdout)
    adb("shell", "chmod", "755", DEVICE_DIR + "/simpleperf")

    data = DEVICE_DIR + "/perf.data"
    # cpu-clock rather than cycles: the Adreno/Kryo PMU is not always readable
    # from an unrooted shell, and wall-clock sampling is what is wanted here
    # anyway - a function blocked on a lock costs the frame just as much as one
    # burning cycles.
    record = ["shell", DEVICE_DIR + "/simpleperf", "record",
              "-e", "cpu-clock", "-f", "1000",
              "-p", pid, "--duration", str(args.seconds),
              "-o", data]
    if args.callgraph:
        record.insert(-2, "-g")
    result = adb(*record)
    if "Failed" in result.stdout or "error" in result.stdout.lower():
        print(result.stdout)
        sys.exit("simpleperf record failed - is the build debuggable?")

    print("\n=== where the time went ===")
    report = adb("shell", DEVICE_DIR + "/simpleperf", "report",
                 "-i", data, "--sort", "symbol", "-n")
    lines = [l for l in report.stdout.splitlines() if l.strip()]
    shown = 0
    for line in lines:
        # Percentage rows only; the header block is noise here.
        if line.lstrip()[:1].isdigit() or line.startswith("Overhead"):
            print(line)
            shown += 1
            if shown >= args.top:
                break
    if shown == 0:
        print(report.stdout)

    print("\nA hot symbol that appears in config/functions.toml can be "
          "replaced\nwholesale with a host REX_FUNC implementation - see "
          "'How the guest is\npatched' in CLAUDE.md.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
