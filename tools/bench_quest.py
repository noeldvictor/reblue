#!/usr/bin/env python3
"""Run a matrix of cvar configurations on device and print a comparison table.

Every knob worth sweeping reaches the game through args.txt beside the game
data, so a whole matrix costs no build and no reinstall - the expensive part of
a measurement is the ~2 minutes of launch and settle, not the compile. This
exists because running one configuration by hand is a dozen adb invocations
with three traps in them, and because a single-variable comparison is the only
kind that has ever survived on this project.

  python tools/bench_quest.py fill        # the fill-rate sweep
  python tools/bench_quest.py levers      # render scale, shadows, reflections
  python tools/bench_quest.py --config "bd_render_scale=50,bd_shadows=false"

Traps this encodes, all of which have cost hours:

  * MSYS_NO_PATHCONV must be set for device paths and NOT for local ones. Every
    push is checked rather than assumed - silent adb push failures invalidated
    three experiments in one session, each of which read as a clean negative.
  * The headset suspends the app when nobody is wearing it, which looks exactly
    like a startup hang. The prox_close broadcast is sent before and after.
  * Cross-restart precision is about +/-30%, so a difference smaller than that
    is not a result. --repeat prints the spread so that is visible.
  * Removing draws also removes their fragments. Any "cap the draws" experiment
    moves two variables at once; bd_debug_fill_scale moves only fragments.
"""

import argparse
import re
import subprocess
import sys
import time

ADB = r"C:/Users/leanerdesigner/AppData/Local/Android/Sdk/platform-tools/adb.exe"
PKG = "com.reblue"
ACTIVITY = PKG + "/.ReblueActivity"
ARGS_ON_DEVICE = "/storage/emulated/0/Android/data/" + PKG + "/files/args.txt"
LOG_GLOB = "/sdcard/Android/data/" + PKG + "/files/logs/*.log"

# Always on: VR plus autoplay, so a field scene is reached with an empty
# headset, and MSAA off so it is never silently a second variable.
BASE = {"bd_vr_enabled": "true", "bd_xr_autoplay": "true", "bd_msaa": "0",
        # The tier the port actually targets. Without this it is paced against
        # 72Hz it will never hit and loses a whole tier for nothing.
        "bd_xr_refresh_rate": "60"}

PRESETS = {
    # Proven fill-bound: fence 141ms -> 17ms -> 0.1ms while the draw count rises.
    "fill": [
        {"bd_debug_fill_scale": "100"},
        {"bd_debug_fill_scale": "50"},
        {"bd_debug_fill_scale": "25"},
    ],
    # The three real levers, one variable at a time, then combined.
    "levers": [
        {},
        {"bd_render_scale": "50"},
        {"bd_reflections": "false"},
        {"bd_shadows": "false"},
        {"bd_render_scale": "50", "bd_reflections": "false",
         "bd_shadows": "false"},
    ],
    # Stereo, and the cost of getting it back into a pacing tier. Stereo has
    # real depth as of 2026-08-29; use tools/stereo_check.py to confirm that,
    # not this, which only measures speed.
    #
    # Mono at these settings is 34.6ms / 28.9fps. Stereo alone falls to 50.0ms
    # because the compositor drops a tier - and note the CPU barely moves
    # (19.0 -> 20.2ms), so the cost is GPU fill that `fence` does not show.
    "stereo": [
        {},
        {"bd_stereo": "true"},
        {"bd_stereo": "true", "bd_render_scale": "20",
         "bd_cull_distance": "250", "bd_shadows": "false"},
        {"bd_stereo": "true", "bd_render_scale": "15",
         "bd_cull_distance": "200", "bd_shadows": "false"},
    ],
    # Everything verified on desktop, together. The one to run first.
    "all": [
        {},
        {"bd_render_scale": "50", "bd_reflections": "false"},
        {"bd_render_scale": "50", "bd_reflections": "false",
         "bd_shadows": "false"},
        {"bd_render_scale": "50", "bd_reflections": "false",
         "bd_shadows": "false", "bd_stereo": "true",
         "bd_stereo_separation": "0.06", "bd_stereo_convergence": "0.03"},
    ],
    # Is any of this VR's fault? It was not, the last time it was asked.
    "vr": [
        {},
        {"bd_vr_enabled": "false"},
    ],
}


def sh(args, timeout=120):
    return subprocess.run(args, capture_output=True, text=True, timeout=timeout)


def adb(serial, *args, **kw):
    return sh([ADB, "-s", serial] + list(args), timeout=kw.get("timeout", 120))


def pick_device(explicit):
    out = sh([ADB, "devices"]).stdout
    devices = [l.split()[0] for l in out.splitlines()[1:]
               if l.strip() and l.split()[-1] == "device"]
    if explicit:
        if explicit not in devices:
            sys.exit("device %s not attached; attached: %s" % (explicit, devices))
        return explicit
    if not devices:
        sys.exit("no device attached. Wake the headset and check the cable.")
    if len(devices) > 1:
        sys.exit("several devices attached, pass --serial: %s" % devices)
    return devices[0]


def push_args(serial, cvars, local_path):
    body = "".join("--%s\n%s\n" % (k, v) for k, v in cvars.items())
    with open(local_path, "w", newline="\n") as fh:
        fh.write(body)
    r = sh([ADB, "-s", serial, "push", local_path, ARGS_ON_DEVICE])
    if r.returncode != 0 or "error" in (r.stderr or "").lower():
        sys.exit("adb push failed, the measurement would be meaningless:\n"
                 + (r.stderr or ""))


def wake(serial):
    adb(serial, "shell", "am", "broadcast", "-a",
        "com.oculus.vrpowermanager.prox_close")


FRAME_RE = re.compile(r"frame ([\d.]+)ms =.*?fence ([\d.]+).*?elsewhere ([\d.]+)")
PERF_RE = re.compile(r"\[perf\] (\d+) draws/frame, (\d+) verts/frame")
TARGET_RE = re.compile(
    r"\[perf\]\s+target (?:[0-9A-F]{12} )?(\d+)x(\d+): (\d+) draws/frame"
    r"(?: over (\d+) binds)?")


def newest_log(serial):
    return adb(serial, "shell",
               "ls -t %s 2>/dev/null | head -1" % LOG_GLOB).stdout.strip()


def stop_and_wait(serial, timeout=25):
    """force-stop, then wait until the process is actually gone.

    `am force-stop` returns immediately and does not reliably kill a VR app the
    Oculus runtime is holding. When it does not, `am start` answers "intent has
    been delivered to currently running top-most instance", the app keeps
    running with its *previous* args and keeps writing to the same log - so
    every configuration in a sweep measures one unchanged live process and
    produces identical numbers that read as "this setting does nothing".

    That is exactly what happened to two rounds of culling measurements.
    """
    adb(serial, "shell", "am", "force-stop", PKG)
    deadline = time.time() + timeout
    while time.time() < deadline:
        if not adb(serial, "shell", "pidof", PKG).stdout.strip():
            return True
        time.sleep(1)
        adb(serial, "shell", "am", "force-stop", PKG)
    return False


def run_one(serial, cvars, settle, local_args, show_targets):
    push_args(serial, cvars, local_args)
    # Remembered before launch: the app writes a fresh numbered log per run, so
    # if the newest afterwards is still this one the run never started and
    # reading it would report the previous configuration's numbers.
    before = newest_log(serial)
    if not stop_and_wait(serial):
        print("    the app would not stop - its args are stale, skipping")
        return None
    wake(serial)
    adb(serial, "shell", "am", "start", "-n", ACTIVITY)
    time.sleep(20)
    wake(serial)            # again: it suspends again while nobody wears it
    time.sleep(settle)

    logf = newest_log(serial)
    if not logf:
        return None
    if logf == before:
        print("    STALE: no new log after launch - not reporting the previous "
              "run's numbers")
        return None
    tail = adb(serial, "shell", "tail -n 400 '%s'" % logf).stdout

    frames = FRAME_RE.findall(tail)[-5:]
    perfs = PERF_RE.findall(tail)[-1:]
    if not frames:
        return None
    n = len(frames)
    return {
        "frame": sum(float(f[0]) for f in frames) / n,
        "fence": sum(float(f[1]) for f in frames) / n,
        "elsewhere": sum(float(f[2]) for f in frames) / n,
        "spread": max(float(f[0]) for f in frames)
        - min(float(f[0]) for f in frames),
        "draws": int(perfs[0][0]) if perfs else 0,
        "verts": int(perfs[0][1]) if perfs else 0,
        "targets": TARGET_RE.findall(tail)[-12:] if show_targets else [],
    }


def label(cvars):
    diff = dict((k, v) for k, v in cvars.items() if BASE.get(k) != v)
    return ", ".join("%s=%s" % kv for kv in sorted(diff.items())) or "baseline"


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("preset", nargs="?", choices=sorted(PRESETS),
                    default="levers")
    ap.add_argument("--config", action="append", default=[],
                    help="k=v,k=v - one extra configuration, repeatable")
    ap.add_argument("--serial")
    ap.add_argument("--settle", type=int, default=95,
                    help="seconds after launch before reading (default 95)")
    ap.add_argument("--repeat", type=int, default=1,
                    help="runs per configuration; restart spread is ~30%%")
    ap.add_argument("--targets", action="store_true",
                    help="also print the per-surface draw census")
    ap.add_argument("--tmp", default="bench_args.txt")
    args = ap.parse_args()

    serial = pick_device(args.serial)
    configs = [] if args.config else list(PRESETS[args.preset])
    for c in args.config:
        configs.append(dict(p.split("=", 1) for p in c.split(",")))

    minutes = (args.settle + 25) * args.repeat * len(configs) / 60.0
    print("device %s, %d configuration(s), about %.0f min total\n"
          % (serial, len(configs), minutes))

    rows = []
    for cvars in configs:
        merged = dict(BASE)
        merged.update(cvars)
        name = label(merged)
        for i in range(args.repeat):
            suffix = " [%d/%d]" % (i + 1, args.repeat) if args.repeat > 1 else ""
            print("  running %s%s ..." % (name, suffix), flush=True)
            r = run_one(serial, merged, args.settle, args.tmp, args.targets)
            if r is None:
                print("    no frame lines - did it crash? check the log")
                continue
            r["name"] = name
            rows.append(r)
            for t in r["targets"]:
                w, h, d, binds = t
                print("      target %sx%s: %s draws%s"
                      % (w, h, d, (" over %s binds" % binds) if binds else ""))

    if not rows:
        sys.exit("nothing measured")

    print("\n%-42s %9s %8s %8s %6s %7s" %
          ("configuration", "frame", "fence", "else", "fps", "draws"))
    print("-" * 86)
    base = rows[0]["frame"]
    for r in rows:
        delta = "" if r is rows[0] or not base else "  (%.2fx)" % (base / r["frame"])
        print("%-42s %7.1fms %6.1fms %6.1fms %6.1f %7d%s"
              % (r["name"], r["frame"], r["fence"], r["elsewhere"],
                 1000.0 / r["frame"], r["draws"], delta))

    if len(rows) > 1 and all(abs(r["frame"] - rows[0]["frame"]) < 0.05
                             for r in rows):
        print("WARNING: every configuration produced an identical frame "
              "time. That is not a measurement - suspect stale logs or "
              "settings that never reached the device.")
    print("\nA difference under ~30% is inside cross-restart noise, not a result.")
    print("'else' is the CPU floor: it does not move when the GPU is freed.")


if __name__ == "__main__":
    main()
