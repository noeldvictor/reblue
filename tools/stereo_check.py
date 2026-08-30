#!/usr/bin/env python3
"""Capture a VR frame from the device and report whether stereo has depth.

Answers, in one command and with nobody in the headset, the question that took
a whole session to answer by hand: does the image reaching the compositor
actually carry depth, and is it the right way round?

  python tools/stereo_check.py                       # default stereo config
  python tools/stereo_check.py --config "bd_render_scale=20,bd_cull_distance=250"
  python tools/stereo_check.py --separation 0.03     # sweep the eye offset
  python tools/stereo_check.py --mono                # capture without stereo

How it decides, and why these are the right tests:

  * A *flat* disparity means the eye offset is being applied as something
    proportional to clip.z. clip.z and clip.w agree to a fraction of a percent
    past a few metres, so that divides out to a constant sideways slide of the
    whole image - it looks like stereo in a still frame and has no depth at
    all. This is what shipped for three sessions, measuring +59px at the sky
    against +57px on the near ground.

  * The *sign* matters as much as the magnitude. With the convergence plane at
    infinity every point must have crossed disparity - further left in the
    right eye - by more the nearer it is. Backwards renders the scene
    pseudoscopic: near geometry reads as far and the world turns inside out.
    It fuses badly and it is invisible in a symmetric test pose, which is the
    trap that has cost this port three separate bugs.

So the verdict is on near-minus-far, not on the absolute numbers. A constant
offset across all bands is the two half-width viewports not being pixel-aligned
with each other; it is a convergence error, not a depth error, and it is
reported separately rather than failing the check.

Needs Pillow on the host. The device needs nothing: bd_capture_after_s writes
the composited frame and the whole thing is pulled back as raw RGBA.
"""

import argparse
import os
import subprocess
import sys
import time

ADB = os.environ.get(
    "ADB", r"C:/Users/leanerdesigner/AppData/Local/Android/Sdk/platform-tools/adb.exe")
PKG = "com.reblue"
ACTIVITY = PKG + "/.ReblueActivity"
FILES = "/storage/emulated/0/Android/data/" + PKG + "/files"

# Reaches a field scene at ~130s and autoplay starts walking at 150s, so this
# lands on a standing character in open terrain - the same scene every run,
# which is what makes two captures comparable at all.
DEFAULT_CAPTURE_AT = 143

BASE = {
    "bd_vr_enabled": "true",
    "bd_xr_autoplay": "true",
    "bd_msaa": "0",
    "bd_xr_refresh_rate": "60",
    "bd_render_scale": "25",
    "bd_reflections": "false",
    "bd_cull_distance": "350",
}

STEREO = {
    "bd_stereo": "true",
    "bd_stereo_separation": "0.02",
    "bd_stereo_convergence": "0.0",
}


def sh(args, timeout=180):
    return subprocess.run(args, capture_output=True, text=True, timeout=timeout)


def adb(serial, *args, **kw):
    return sh([ADB, "-s", serial] + list(args), timeout=kw.get("timeout", 180))


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


def wake(serial):
    # The headset suspends the app the moment nobody is wearing it, which looks
    # exactly like a startup hang. Sent repeatedly through the whole wait.
    adb(serial, "shell", "am", "broadcast", "-a",
        "com.oculus.vrpowermanager.prox_close")


def stop_and_wait(serial, timeout=25):
    """force-stop, then wait until the process is really gone.

    `am force-stop` returns immediately and does not reliably kill a VR app the
    Oculus runtime is holding. When it does not, the app keeps running with its
    *previous* args and every configuration measures one unchanged process.
    """
    adb(serial, "shell", "am", "force-stop", PKG)
    deadline = time.time() + timeout
    while time.time() < deadline:
        if not adb(serial, "shell", "pidof", PKG).stdout.strip():
            return True
        time.sleep(1)
        adb(serial, "shell", "am", "force-stop", PKG)
    return False


def push_args(serial, cvars, local_path):
    body = "".join("--%s\n%s\n" % (k, v) for k, v in cvars.items())
    with open(local_path, "w", newline="\n") as fh:
        fh.write(body)
    r = sh([ADB, "-s", serial, "push", local_path, FILES + "/args.txt"])
    if r.returncode != 0 or "error" in (r.stderr or "").lower():
        sys.exit("adb push failed, the run would measure the previous config:\n"
                 + (r.stderr or ""))


def capture(serial, cvars, at_seconds, out_raw):
    adb(serial, "shell", "rm -f %s/logs/capture/*.raw" % FILES)
    if not stop_and_wait(serial):
        sys.exit("the app would not stop; its args are stale")
    wake(serial)
    adb(serial, "shell", "am", "start", "-n", ACTIVITY)

    waited = 0
    while waited < at_seconds + 25:
        time.sleep(10)
        waited += 10
        wake(serial)

    got = adb(serial, "shell",
              "ls -t %s/logs/capture/*.raw 2>/dev/null | head -1" % FILES)
    remote = got.stdout.strip()
    if not remote:
        sys.exit("no capture written. Did it reach a field scene? Check the log.")
    r = sh([ADB, "-s", serial, "pull", remote, out_raw])
    if r.returncode != 0:
        sys.exit("pull failed:\n" + (r.stderr or ""))
    return remote


def load(path):
    from PIL import Image
    with open(path, "rb") as fh:
        header = fh.readline().decode().split()
        w, h, order = int(header[1]), int(header[2]), header[3]
        data = fh.read(w * h * 4)
    img = Image.frombytes("RGBA", (w, h), data)
    if order == "bgra":
        b, g, r, a = img.split()
        img = Image.merge("RGBA", (r, g, b, a))
    return img, w, h


def disparity(img, w, h, bands, search):
    """Per-band horizontal shift that best aligns the right eye onto the left.

    A narrow central patch rather than the full width: a wide band spans mixed
    depths, and the global SAD minimum then jumps between features instead of
    tracking one.
    """
    from PIL import ImageChops
    grey = img.convert("L")
    ew = w // 2
    left = grey.crop((0, 0, ew, h))
    right = grey.crop((ew, 0, w, h))
    patch = min(520, ew - 40)
    x0 = (ew - patch) // 2
    out = []
    for frac in bands:
        top = int(h * frac)
        bottom = min(top + 90, h)
        lb = left.crop((x0, top, x0 + patch, bottom))
        best, best_sad = 0, None
        for s in range(-search, search + 1):
            rb = right.crop((x0 + s, top, x0 + patch + s, bottom))
            sad = sum(ImageChops.difference(lb, rb).getdata())
            if best_sad is None or sad < best_sad:
                best, best_sad = s, sad
        out.append((frac, best))
    return out


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--serial")
    ap.add_argument("--config", default="", help="k=v,k=v extra cvars")
    ap.add_argument("--separation", type=float)
    ap.add_argument("--convergence", type=float)
    ap.add_argument("--mono", action="store_true",
                    help="capture with stereo off, as a control")
    ap.add_argument("--at", type=int, default=DEFAULT_CAPTURE_AT,
                    help="seconds after launch to capture (default %d)"
                         % DEFAULT_CAPTURE_AT)
    ap.add_argument("--search", type=int, default=90,
                    help="max |disparity| searched, px (default 90)")
    ap.add_argument("--out", default="stereo_capture",
                    help="basename for the .raw and .png written here")
    ap.add_argument("--keep-png", action="store_true", default=True)
    ap.add_argument("--raw", default="",
                    help="analyse an existing capture instead of driving a "
                         "device - the desktop loop writes one to "
                         "out/build/<preset>/logs/capture/")
    args = ap.parse_args()

    # A capture already on disk needs no device at all, which is what makes this
    # runnable in the desktop loop rather than only against a headset.
    if args.raw:
        img, w, h = load(args.raw)
        if args.keep_png:
            png = args.out + ".png"
            img.convert("RGB").resize((w // 3, h // 3)).save(png)
            print("wrote %s - look at it, do not just read the numbers\n" % png)
        if args.mono:
            print("mono control; no disparity to report")
            return
        report(img, w, h, args.search)
        return

    serial = pick_device(args.serial)

    cvars = dict(BASE)
    if not args.mono:
        cvars.update(STEREO)
    if args.separation is not None:
        cvars["bd_stereo_separation"] = repr(args.separation)
    if args.convergence is not None:
        cvars["bd_stereo_convergence"] = repr(args.convergence)
    for pair in filter(None, args.config.split(",")):
        k, v = pair.split("=", 1)
        cvars[k.strip()] = v.strip()
    cvars["bd_capture_after_s"] = str(args.at)

    print("device %s, capturing at t=%ds (about %d min)\n"
          % (serial, args.at, (args.at + 40) // 60 + 1))
    for k in sorted(cvars):
        print("    %s = %s" % (k, cvars[k]))
    print()

    push_args(serial, cvars, args.out + "_args.txt")
    remote = capture(serial, cvars, args.at, args.out + ".raw")
    print("pulled %s\n" % remote)

    img, w, h = load(args.out + ".raw")
    if args.keep_png:
        img.convert("RGB").resize((w // 3, h // 3)).save(args.out + ".png")
        print("wrote %s.png - look at it, do not just read the numbers\n"
              % args.out)

    if args.mono:
        print("mono control captured; no disparity to report")
        return

    report(img, w, h, args.search)


def report(img, w, h, search):
    bands = [0.32, 0.44, 0.52, 0.62, 0.72, 0.82, 0.90, 0.95]
    rows = disparity(img, w, h, bands, search)
    print("band (y%)   disparity(px)      [top = distant, bottom = near]")
    for frac, shift in rows:
        print("   %4.0f%%        %+5d" % (frac * 100, shift))

    far = rows[0][1]
    near = rows[-1][1]
    spread = max(s for _, s in rows) - min(s for _, s in rows)
    delta = near - far
    print("\nfar %+d, near %+d  ->  near - far = %+d px, spread %d px"
          % (far, near, delta, spread))

    if spread < 6:
        print("\nFLAT. The eye offset is not producing depth - the classic cause "
              "is\napplying it as separation * clip.z, which divides out to a "
              "constant\nsideways slide. It must be a constant added to clip.x.")
        sys.exit(1)
    if delta > 0:
        print("\nINVERTED (uncrossed). Near geometry is separating the wrong "
              "way, so the\nscene renders pseudoscopic - the world inside out. "
              "Flip the per-eye sign:\nthe LEFT eye takes the positive "
              "constant.")
        sys.exit(1)
    print("\nOK: crossed disparity, near separating more than far. Stereo has "
          "depth\nand is the right way round.")
    if abs(far) > 12:
        print("Note: far sits at %+d rather than near 0. That is a uniform "
              "offset between\nthe two half-width viewports - a convergence "
              "error, not a depth error." % far)


if __name__ == "__main__":
    main()
