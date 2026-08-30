#!/usr/bin/env python3
"""Summarise a bd_perf_csv run, using only the frames worth measuring.

Taking the tail of the CSV is wrong, and it was how every measurement in this
project was read until 2026-08-30. A run does not end in a steady state: the
character walks into a transition about 35 seconds after setting off, something
opens, and the last stretch of the file is a menu at ~20 draws a frame. Reading
the last 300 rows therefore samples whatever the run happened to be sitting in.

Selecting by *content* instead - frames whose draw count says "field scene" -
gives about 9,600 frames per run rather than 300, at a consistent draw count,
and removes the menu contamination entirely.

It does NOT make two runs comparable. The same binary in the same configuration
measured 5.12ms and 8.62ms other_ms minutes apart - 68% - so any cross-run
delta under about 50% is drift. Use the within-run A/B instead: set bd_ab_flag
to a boolean cvar and bd_ab_period to a frame count, and this script reports the
two arms separately from one run.

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


def ab_report(path, floor):
    """Compare the two arms of a within-run A/B from one file.

    This is the only comparison on this workload that means anything below
    about 50%: both populations come from the same run, the same scene and the
    same thermal state, interleaved, so whatever drifts drifts through both.
    """
    rows, index = load(path)
    if not rows or "ab_arm" not in index or "draws" not in index:
        return False
    a_i, d_i = index["ab_arm"], index["draws"]
    o_i = index.get("other_ms")
    if o_i is None:
        return False

    arms = {}
    for r in rows:
        arm = int(float(r[a_i]))
        if arm > 1 or float(r[d_i]) < floor:
            continue
        arms.setdefault(arm, []).append(
            1000.0 * float(r[o_i]) / max(float(r[d_i]), 1.0))
    if len(arms) < 2 or any(len(v) < 30 for v in arms.values()):
        return False

    print("\n%s - within-run A/B" % path)
    for arm in sorted(arms):
        v = arms[arm]
        print("  arm %d (%s): %5d frames   us/draw %6.2f"
              % (arm, "false" if arm == 0 else "true", len(v), st.median(v)))
    a0, a1 = st.median(arms[0]), st.median(arms[1])
    print("  arm 1 vs arm 0: %+.1f%%" % (100.0 * (a1 - a0) / a0))
    print("  Both arms come from one run, so this number is comparable in a way "
          "two whole runs are not.")
    return True


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", nargs="+")
    ap.add_argument("--floor", type=int, default=FIELD_DRAW_FLOOR,
                    help="draw count separating a field scene from a menu "
                         "(default: %(default)s)")
    args = ap.parse_args()

    for p in args.csv:
        ab_report(p, args.floor)

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
