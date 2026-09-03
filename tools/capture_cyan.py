"""capture_cyan.py <capture dir> [min area %]

Counts, per capture of a bd_capture_frames sequence, the pixels that are the
artefact colour of the host-issued draw's stale texture slot (a saturated
cyan: blue and green high, red low), and lists the frames whose cyan area
exceeds the threshold. Camera motion does not make cyan; the artefact does.
"""
import os
import re
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_seq import load  # noqa: E402


def main():
    d = sys.argv[1]
    min_area = float(sys.argv[2]) if len(sys.argv) > 2 else 0.3
    files = []
    for name in os.listdir(d):
        m = re.match(r"frame_(\d+)_(\d+)\.raw$", name)
        if m:
            files.append((int(m.group(1)), int(m.group(2)), name))
    files.sort()
    hits = 0
    areas = []
    for epoch, seq, name in files:
        a = load(os.path.join(d, name)).astype(np.int16)
        r, g, b = a[:, :, 0], a[:, :, 1], a[:, :, 2]
        cyan = (b > 170) & (g > 140) & (r < 110) & (b - r > 90)
        area = 100.0 * cyan.mean()
        areas.append(area)
        if area > min_area:
            hits += 1
            if 2.0 < area < 60.0:
                print("frame %3d: cyan %.2f%%" % (seq, area))
    if areas:
        patches = sum(1 for a in areas if max(min_area, 2.0) < a < 60.0)
        whole = sum(1 for a in areas if a >= 60.0)
        print("%d frames, %d with cyan over %.2f%%: %d patch frames (2-60%%), "
              "%d whole-frame (the zenith sky reads the same); median %.3f%%, max %.2f%%"
              % (len(files), hits, min_area, patches, whole, float(np.median(areas)), max(areas)))


if __name__ == "__main__":
    main()
