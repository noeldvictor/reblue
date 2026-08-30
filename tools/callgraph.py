#!/usr/bin/env python3
"""Call graph over the recompiled guest, for planning a rewrite.

`generated/` is the whole XEX translated to C++ - 18,777 functions, every
address from config/functions.toml applied as a real name. That makes the call
graph extractable with a regex rather than a decompiler, and having it is what
makes rewriting a rendering path tractable: before replacing a function you need
to know who calls it, what it calls, and how much of the frame hangs off it.

  python tools/callgraph.py callers bdSceneNodeDrawSingle
  python tools/callgraph.py callees bdSceneNodeDrawSingle
  python tools/callgraph.py tree    bdSceneNodeCullTraverse --depth 3
  python tools/callgraph.py subtree bdSceneNodeDrawSingle    # everything reachable
  python tools/callgraph.py hot                              # most-called functions

The index is cached in out/callgraph.json, because parsing 110 MB of C++ takes
about half a minute and the sources only change when codegen re-runs.

Reading the graph, not guessing at it, is the point. The renderer rewrite has to
replace whole paths - a function that batches instead of submitting per node
changes the contract for everything above it - and "what else touches this"
is not answerable by eye across 223 files.
"""

import argparse
import json
import os
import re
import sys
import time

GEN = "generated"
CACHE = os.path.join("out", "callgraph.json")

# DEFINE_REX_FUNC(name) { ... } opens a function; the recompiler emits one per
# guest function, named where config/functions.toml names it and sub_ADDR
# otherwise.
DEF_RE = re.compile(r"^DEFINE_REX_FUNC\((\w+)\)")
# A call is `name(ctx, base);` at statement level. __imp__ prefixed calls are
# the host taking over a replaced function, and are counted as the same edge.
CALL_RE = re.compile(r"\b(?:__imp__)?((?:sub_[0-9A-Fa-f]+)|(?:[A-Za-z_]\w*))\s*\(\s*ctx\s*,")


def build_index():
    graph = {}
    files = sorted(f for f in os.listdir(GEN) if f.endswith(".cpp"))
    if not files:
        sys.exit("no generated sources - run the reblue_codegen target first")
    for name in files:
        path = os.path.join(GEN, name)
        current = None
        with open(path, "r", encoding="utf-8", errors="ignore") as fh:
            for line in fh:
                m = DEF_RE.match(line)
                if m:
                    current = m.group(1)
                    graph.setdefault(current, {"callees": [], "file": name})
                    continue
                if current is None:
                    continue
                c = CALL_RE.search(line)
                if c:
                    callee = c.group(1)
                    # The function's own recompiled body calls itself through
                    # __imp__ when a host hook has replaced it; that is not a
                    # real edge and would make every hooked function recursive.
                    if callee != current:
                        graph[current]["callees"].append(callee)
    # De-duplicate but keep counts: a function called five times in one body is
    # five sites to change, and that matters when planning a rewrite.
    for fn in graph.values():
        counted = {}
        for c in fn["callees"]:
            counted[c] = counted.get(c, 0) + 1
        fn["callees"] = counted
    return graph


def load(refresh=False):
    if not refresh and os.path.exists(CACHE):
        newest = max(
            (os.path.getmtime(os.path.join(GEN, f))
             for f in os.listdir(GEN) if f.endswith(".cpp")),
            default=0)
        if os.path.getmtime(CACHE) >= newest:
            with open(CACHE, "r", encoding="utf-8") as fh:
                return json.load(fh)
    t0 = time.time()
    graph = build_index()
    os.makedirs(os.path.dirname(CACHE), exist_ok=True)
    with open(CACHE, "w", encoding="utf-8") as fh:
        json.dump(graph, fh)
    print("indexed %d functions in %.1fs -> %s"
          % (len(graph), time.time() - t0, CACHE), file=sys.stderr)
    return graph


def callers_of(graph, target):
    out = []
    for name, fn in graph.items():
        if target in fn["callees"]:
            out.append((name, fn["callees"][target], fn["file"]))
    return sorted(out, key=lambda r: -r[1])


def tree(graph, root, depth, seen=None, indent=0):
    if seen is None:
        seen = set()
    if root in seen or depth < 0:
        return
    seen.add(root)
    fn = graph.get(root)
    if not fn:
        print("%s%s  (no body - host implemented or an import)"
              % ("  " * indent, root))
        return
    print("%s%s  [%s]" % ("  " * indent, root, fn["file"]))
    if depth == 0:
        return
    for callee, n in sorted(fn["callees"].items(), key=lambda kv: -kv[1]):
        suffix = "" if n == 1 else "  x%d" % n
        if callee in seen:
            print("%s%s%s  (above)" % ("  " * (indent + 1), callee, suffix))
            continue
        tree(graph, callee, depth - 1, seen, indent + 1)


def subtree(graph, root):
    seen, stack = set(), [root]
    while stack:
        cur = stack.pop()
        if cur in seen:
            continue
        seen.add(cur)
        fn = graph.get(cur)
        if fn:
            stack.extend(fn["callees"])
    seen.discard(root)
    return sorted(seen)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode",
                    choices=["callers", "callees", "tree", "subtree", "hot"])
    ap.add_argument("function", nargs="?")
    ap.add_argument("--depth", type=int, default=2)
    ap.add_argument("--refresh", action="store_true",
                    help="reindex even if the cache looks current")
    args = ap.parse_args()

    graph = load(args.refresh)

    if args.mode == "hot":
        counts = {}
        for fn in graph.values():
            for callee, n in fn["callees"].items():
                counts[callee] = counts.get(callee, 0) + n
        print("%-44s %s" % ("function", "call sites"))
        for name, n in sorted(counts.items(), key=lambda kv: -kv[1])[:30]:
            print("%-44s %d" % (name, n))
        return

    if not args.function:
        sys.exit("that mode needs a function name")

    if args.mode == "callers":
        rows = callers_of(graph, args.function)
        if not rows:
            print("nothing calls %s directly.\n"
                  "Either it is an entry point, or it is reached indirectly "
                  "through a vtable or jump table - the recompiler emits those "
                  "as an address lookup, which this tool cannot see."
                  % args.function)
            return
        print("%-44s %-6s %s" % ("caller", "sites", "file"))
        for name, n, f in rows:
            print("%-44s %-6d %s" % (name, n, f))
        print("\n%d caller(s). Each is a place the contract changes if %s is "
              "replaced." % (len(rows), args.function))
    elif args.mode == "callees":
        fn = graph.get(args.function)
        if not fn:
            sys.exit("%s has no recompiled body - host implemented, or a name "
                     "that is not in the graph" % args.function)
        print("%s  [%s]" % (args.function, fn["file"]))
        for callee, n in sorted(fn["callees"].items(), key=lambda kv: -kv[1]):
            print("  %-42s x%d" % (callee, n))
    elif args.mode == "tree":
        tree(graph, args.function, args.depth)
    elif args.mode == "subtree":
        names = subtree(graph, args.function)
        print("%d function(s) reachable from %s" % (len(names), args.function))
        for n in names[:80]:
            print("  " + n)
        if len(names) > 80:
            print("  ... and %d more" % (len(names) - 80))


if __name__ == "__main__":
    main()
