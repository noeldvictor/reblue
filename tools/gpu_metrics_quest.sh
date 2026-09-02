#!/usr/bin/env bash
# GPU-side metrics for a field scene on the Quest, from the profiler that ships
# on the headset. The per-target census says WHERE the GPU time goes (the scene
# pass); this says WHY: % time shading vertices vs fragments, ALU and texture
# utilisation, fetch stalls, fragments and vertices per second, polygon area.
#
# Runs verify_quest.sh's normal launch, then samples ovrgpuprofiler for ~12 s
# once the field scene is up. Extra cvars pass through.
#
# Usage:  bash tools/gpu_metrics_quest.sh [extra_cvar=value,...]
set -u
ADB="${ADB:-C:/Users/leanerdesigner/AppData/Local/Android/Sdk/platform-tools/adb.exe}"
EXTRA="${1:-}"
AT="${AT:-138}"
METRICS="${METRICS:-2,6,7,10,16,17,18,19,21,22,24,25,31,32,33,38,40,44,48}"
OUT="out/device/gpu_metrics.txt"

run_adb() { MSYS_NO_PATHCONV=1 "$ADB" "$@"; }

# verify_quest does install, config, launch and the proximity broadcast. Run it
# in the background and sample the GPU while it waits out its settle.
SETTLE=175 bash tools/verify_quest.sh "$EXTRA" > out/device/gpu_metrics_run.txt 2>&1 &
VQ=$!
sleep "$AT"
echo "== ovrgpuprofiler at ${AT}s ==" | tee "$OUT"
run_adb shell "ovrgpuprofiler -m" > out/device/gpu_metric_names.txt 2>&1
run_adb shell "timeout 12 ovrgpuprofiler -r \"$METRICS\"" 2>&1 | tee -a "$OUT"
wait $VQ
echo "== run output in out/device/gpu_metrics_run.txt =="
grep -E "config|fps \|" out/device/gpu_metrics_run.txt | head -4
