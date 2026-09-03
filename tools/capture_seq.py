"""capture_seq.py <capture dir> [threshold%]

Consecutive-frame diffs over a bd_capture_frames sequence: for each pair of
neighbouring captures (frame_<epoch>_<seq>.raw, by seq), the share of pixels
that differ by more than 8 of 255. Camera motion changes a few percent a
frame; a draw that appears or vanishes for a few frames is a jump well above
that, and the pair that jumps is written as a side-by-side PNG beside the
raws (frame A | frame B | diff x8) for looking at.
"""
import os
import re
import struct
import sys

import numpy as np
from PIL import Image


def load(path):
    with open(path, "rb") as fh:
        header = fh.readline().decode().split()
        tag, w, h, order = header[0], int(header[1]), int(header[2]), header[3]
        bpp = 8 if tag == "RGBA16F" else 4
        data = fh.read(w * h * bpp)
    if bpp == 8:
        a = np.frombuffer(data, dtype=np.float16).reshape(h, w, 4).astype(np.float32)
        a = np.clip(a * 255.0 + 0.5, 0, 255).astype(np.uint8)
    else:
        a = np.frombuffer(data, dtype=np.uint8).reshape(h, w, 4).copy()
        if order == "bgra":
            a = a[:, :, [2, 1, 0, 3]]
    return a[:, :, :3]


def main():
    d = sys.argv[1]
    threshold = float(sys.argv[2]) if len(sys.argv) > 2 else 6.0
    files = []
    for name in os.listdir(d):
        m = re.match(r"frame_(\d+)_(\d+)\.raw$", name)
        if m:
            files.append((int(m.group(1)), int(m.group(2)), name))
    files.sort()
    if len(files) < 2:
        print("need at least two sequence captures in", d)
        return
    prev = load(os.path.join(d, files[0][2]))
    jumps = 0
    for i in range(1, len(files)):
        cur = load(os.path.join(d, files[i][2]))
        if cur.shape != prev.shape:
            print(files[i][2], "shape changed")
            prev = cur
            continue
        diff = np.abs(cur.astype(np.int16) - prev.astype(np.int16)).max(axis=2)
        pct = 100.0 * (diff > 8).mean()
        flag = ""
        if pct > threshold:
            flag = "  <-- jump"
            jumps += 1
            dv = np.clip(diff.astype(np.int32) * 8, 0, 255).astype(np.uint8)
            out = os.path.join(d, "jump_%03d.png" % files[i][1])
            Image.fromarray(np.concatenate([prev, cur, np.stack([dv] * 3, axis=2)], axis=1)).save(out)
            flag += " -> " + out
        print("%3d -> %3d  %5.2f%% differ%s" % (files[i - 1][1], files[i][1], pct, flag))
        prev = cur
    print("%d pairs, %d jumps over %.1f%%" % (len(files) - 1, jumps, threshold))


if __name__ == "__main__":
    main()
