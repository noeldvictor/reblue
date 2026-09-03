"""ledger_diff.py <draw_ledger.txt> <frame> [frame_before]

Diffs the scene draws of one frame against the frame before it (default
frame - 1) from bd_draw_ledger's logs/draw_ledger.txt: the draws present
before and missing now, and the ones new now, each with the path it came by
(interp, replay, dropped) and its node (matrix, mesh, visual, view, list).
The capture log's "[capture] wrote ... (seq K, frame N)" line maps a
sequence capture to its frame.
"""
import collections
import sys


def load(path):
    by_frame = collections.defaultdict(list)
    with open(path) as fh:
        for line in fh:
            p = line.split()
            if len(p) < 9:
                continue
            frame = int(p[0])
            by_frame[frame].append(p)
    return by_frame


def keyed(rows):
    d = collections.defaultdict(list)
    for p in rows:
        d[(p[1], p[2], p[3], p[4], p[5])].append(p)
    return d


def main():
    path = sys.argv[1]
    frame = int(sys.argv[2])
    before = int(sys.argv[3]) if len(sys.argv) > 3 else frame - 1
    by_frame = load(path)
    a = keyed(by_frame.get(before, []))
    b = keyed(by_frame.get(frame, []))
    print("frame %d: %d draws; frame %d: %d draws" % (before, sum(len(v) for v in a.values()), frame, sum(len(v) for v in b.values())))
    for key in sorted(set(a) | set(b)):
        na, nb = len(a.get(key, [])), len(b.get(key, []))
        pa = sorted({p[6] for p in a.get(key, [])})
        pb = sorted({p[6] for p in b.get(key, [])})
        if na != nb or pa != pb:
            matrix, mesh, visual, view, lst = key
            print("  %s matrix %s mesh %s visual %s view %s list %s: before %d %s -> now %d %s"
                  % ("MISSING" if nb < na else "NEW    ", matrix, mesh, visual, view, lst, na, pa, nb, pb))


if __name__ == "__main__":
    main()
