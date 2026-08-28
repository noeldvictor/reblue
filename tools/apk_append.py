#!/usr/bin/env python3
"""Append the dex and native libraries to the aapt2-produced APK.

Git Bash ships no zip(1), and `jar` rewrites the whole archive. zipfile appends
to what aapt2 already built, which is all this step needs.
"""
import os
import sys
import zipfile

out = os.environ.get("OUT", "out/apk")
staging = os.path.join(out, "staging")
added = 0
with zipfile.ZipFile(os.path.join(out, "unsigned.apk"), "a", zipfile.ZIP_DEFLATED) as z:
    for root, _dirs, files in os.walk(staging):
        for name in sorted(files):
            full = os.path.join(root, name)
            arc = os.path.relpath(full, staging).replace(os.sep, "/")
            z.write(full, arc)
            added += 1
if added == 0:
    sys.exit("nothing staged to add; did javac or d8 fail?")
print("    added %d entries" % added)
