#!/usr/bin/env python3
"""Summarise a bd_perf_csv run, using only the frames worth measuring.

Taking the tail of the CSV is wrong, and it was how every measurement in this
project was read until 2026-08-30. A run does not end in a steady state: the
character walks into a transition about 35 seconds after setting off, something
opens, and the last stretch of the file is a menu at ~20 draws a frame. Reading
the last 300 rows therefore samples whatever the run happened to be sitting in.

Selecting by *content* instead - frames whose draw count says "field scene" -
gives about 9,600 frames per run rather than 300, at a consistent draw count,
and cuts the spread across runs from roughly 20% to 8%.

    python tools/perf_summary.py logs/perf/perf-*.csv
    python tools/perf_summary.py a.csv b.csv        # compare two configurations

The `us/draw` column is what to compare when the draw counts differ, since
culling changes how many draws a frame has without changing how expensive the
frame's work is.
"""

import argparse
import csv
import statistics as st
import sys

# A field scene runs 400-600 draws; a menu is around 20 and a battle sits in
# between. 300 separates them with room to spare, and the count is printed so a
# scene that does not fit the assumption is visible rather than silent.
FIELD_DRAW_FLOOR = 300

FIELDS = ["dt_ms", "fence_ms", "other_ms", "gpu_total_ms", "draws", "logic_tps"]


def load(path):
    rows = []
    with open(path, newline="") as fh:
        reader = csv.reader(fh)
        header = next(reader, None)
        if not header:
            return [], {}
        index = {name: i for i, name in enumerate(header)}
        for row in reader:
            if len(row) < len(header) or not row[0].isdigit():
                continue
            rows.append(row)
    return rows, index


def summarise(path, floor):
    rows, index = load(path)
    if not rows:
        print("%s: no data rows" % path)
        return None
    if "draws" not in index:
        print("%s: no draws column - not a bd_perf_csv?" % path)
        return None

    d = index["draws"]
    field = [r for r in rows if float(r[d]) >= floor]
    if not field:
        print("%s: no frames at or above %d draws - the run never reached a "
              "field scene, or the floor needs lowering" % (path, floor))
        return None

    def med(name):
        i = index.get(name)
        if i is None:
            return float("nan")
        return st.median([float(r[i]) for r in field])

    out = {name: med(name) for name in FIELDS}
    out["frames"] = len(field)
    out["total"] = len(rows)
    out["us_per_draw"] = 1000.0 * out["other_ms"] / max(out["draws"], 1.0)
    print("%s" % path)
    print("  %d of %d frames are field scenes (>= %d draws)"
          % (out["frames"], out["total"], floor))
    print("  other_ms %6.2f   us/draw %6.2f   draws %5.0f   dt_ms %6.2f   "
          "gpu_total_ms %5.2f"
          % (out["other_ms"], out["us_per_draw"], out["draws"], out["dt_ms"],
             out["gpu_total_ms"]))
    return out


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", nargs="+")
    ap.add_argument("--floor", type=int, default=FIELD_DRAW_FLOOR,
                    help="draw count separating a field scene from a menu "
                         "(default: %(default)s)")
    args = ap.parse_args()

    results = [(p, summarise(p, args.floor)) for p in args.csv]
    results = [(p, r) for p, r in results if r]

    if len(results) >= 2:
        base = results[0][1]
        print("\nagainst %s:" % results[0][0])
        for path, r in results[1:]:
            d_other = 100.0 * (r["other_ms"] - base["other_ms"]) / base["other_ms"]
            d_draw = (100.0 * (r["us_per_draw"] - base["us_per_draw"])
                      / base["us_per_draw"])
            print("  %-40s other_ms %+6.1f%%   us/draw %+6.1f%%"
                  % (path, d_other, d_draw))
        print("\nCross-run spread on an unchanged binary is around 8%, so treat "
              "anything smaller than that as no result.")


if __name__ == "__main__":
    main()
