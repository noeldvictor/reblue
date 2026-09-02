#!/usr/bin/env bash
# Per-draw GPU timing for a field scene on the Quest, from the profiler that
# ships on the headset. The realtime metrics say the frame is 99% fragment
# work at ~6.6 fragments per pixel; this says which draws those fragments
# belong to, so the material or pass that emits them can be named.
#
# Detailed profiling mode has to be enabled before the app starts and turned
# off afterwards - it changes what every later frame time means.
#
# Usage:  bash tools/gpu_drawtrace_quest.sh [extra_cvar=value,...]
set -u
ADB="${ADB:-C:/Users/leanerdesigner/AppData/Local/Android/Sdk/platform-tools/adb.exe}"
PKG="${PKG:-com.reblue}"
EXTRA="${1:-}"
AT="${AT:-140}"
LEN="${LEN:-0.2}"
OUT="out/device/gpu_drawtrace.txt"

run_adb() { MSYS_NO_PATHCONV=1 "$ADB" "$@"; }

run_adb shell "ovrgpuprofiler -e $PKG" 2>&1 | tail -1
run_adb shell "ovrgpuprofiler -i" 2>&1 | tail -1
SETTLE=175 bash tools/verify_quest.sh "$EXTRA" > out/device/gpu_drawtrace_run.txt 2>&1 &
VQ=$!
sleep "$AT"
# Two samples, twelve seconds apart, because autoplay lands in a transition
# often enough that one window is a coin toss: a trace of 120 small surfaces
# and no scene pass is a loading screen, not a result.
echo "== render stage trace at ${AT}s ==" | tee "$OUT"
run_adb shell "ovrgpuprofiler -t $LEN ${VERBOSE:+-v}" 2>&1 | tee -a "$OUT" | tail -3
sleep 12
echo "== render stage trace at $((AT + 12))s ==" | tee -a "$OUT"
run_adb shell "ovrgpuprofiler -t $LEN ${VERBOSE:+-v}" 2>&1 | tee -a "$OUT" | tail -3
wait $VQ
run_adb shell "ovrgpuprofiler -d" 2>&1 | tail -1
echo "== $OUT: $(wc -l < "$OUT") lines =="
grep -E "config|fps \|" out/device/gpu_drawtrace_run.txt | head -3
