"""Summarise an ovrgpuprofiler render-stage trace: surfaces ranked by GPU time,
their rendering mode and bin count, and the stage breakdown.

    python tools/gpu_trace_summary.py out/device/gpu_drawtrace.txt

The mode column is the whole point. `Direct` with one bin the size of the
surface is system-memory rendering - the tiler switched off for that pass -
and on 2026-09-02 every surface of a field frame read that way, the scene pass
at 24.5 ms. A pass that bins reads `HwBinning` with tens of bins and
Load/Store stages for colour and depth.
"""
import collections
import re
import sys

LINE = re.compile(
    r"Surface (\d+)\s+\| (\d+\s*x\s*\d+)\s+\| color (\d+)\s*bit, depth (\d+)\s*bit.*?"
    r"Mode: (\d) \((\w+)\)\s*\| (\d+)\s+\S+\s+bins.*?\|\s+([\d.]+) ms \|\s+(\d+)\s+stages : (.*)")


def parse(path):
    windows = []
    rows = []
    for line in open(path, errors="ignore"):
        if line.startswith("== render stage trace"):
            if rows:
                windows.append(rows)
            rows = []
            continue
        m = LINE.search(line)
        if m:
            rows.append({
                "id": int(m.group(1)),
                "size": m.group(2).replace(" ", ""),
                "color": int(m.group(3)),
                "depth": int(m.group(4)),
                "mode": m.group(6),
                "bins": int(m.group(7)),
                "ms": float(m.group(8)),
                "stages": m.group(10).strip(),
            })
    if rows:
        windows.append(rows)
    return windows


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "out/device/gpu_drawtrace.txt"
    top = int(sys.argv[2]) if len(sys.argv) > 2 else 8
    for i, rows in enumerate(parse(path)):
        modes = collections.Counter(r["mode"] for r in rows)
        total = sum(r["ms"] for r in rows)
        print("window %d: %d surfaces, %.1f ms, modes %s" % (i, len(rows), total, dict(modes)))
        scene = [r for r in rows if r["depth"] > 0 and r["size"].startswith(("1376", "1280", "1920"))]
        if not scene:
            print("  (no full-size colour+depth pass in this window - not a field scene)")
        for r in sorted(rows, key=lambda r: -r["ms"])[:top]:
            print("  %6.2f ms  %-10s c%-2d d%-2d %-9s bins=%-3d %s" % (
                r["ms"], r["size"], r["color"], r["depth"], r["mode"], r["bins"], r["stages"][:110]))


if __name__ == "__main__":
    main()
