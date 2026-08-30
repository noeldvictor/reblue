#!/usr/bin/env bash
# One command that answers "what does the port actually do on the headset".
#
# Installs the staged APK, runs autoplay into a field scene, and pulls back
# everything worth reading in one go: the frame breakdown, the per-frame CSV,
# the sampling profile, a composited capture, and the per-thread CPU split.
#
# It exists because the expensive part of a device measurement was never the
# 170 seconds - it was remembering all the traps, each of which has cost hours
# here at least once:
#
#   - Git Bash rewrites any argument that looks like a Unix path, so every adb
#     invocation needs MSYS_NO_PATHCONV=1 and Windows-style local paths.
#   - A failed adb push reports success from the surrounding && chain, so every
#     push result is printed rather than discarded.
#   - The proximity sensor suspends the app the moment nobody is wearing the
#     headset, which looks exactly like a startup hang.
#   - Cvars with kRequiresRestart need a force-stop, not a relaunch.
#   - An APK missing libopenxr_loader.so installs cleanly and then dies in
#     dlopen before main(), writing no log at all - so the newest log on the
#     device is the *previous* run's, reporting the old build's numbers under
#     the new build's name.
#
# Usage:  bash tools/verify_quest.sh [extra_cvar=value,...]

set -u

ADB="${ADB:-C:/Users/leanerdesigner/AppData/Local/Android/Sdk/platform-tools/adb.exe}"
PKG="${PKG:-com.reblue}"
APK="${APK:-out/apk/reblue.apk}"
SETTLE="${SETTLE:-170}"
EXTRA="${1:-}"

run_adb() { MSYS_NO_PATHCONV=1 "$ADB" ${SERIAL:+-s "$SERIAL"} "$@"; }

# Prefer an actual headset. More than one Android device is often attached here
# - an AYN Thor is the other target and answers adb just as readily - and
# installing a 68MB VR build onto the wrong one wastes a cycle and leaves a
# confusing log behind. SERIAL= overrides.
if [ -z "${SERIAL:-}" ]; then
  for cand in $(MSYS_NO_PATHCONV=1 "$ADB" devices | awk '$2 == "device" {print $1}'); do
    model="$(MSYS_NO_PATHCONV=1 "$ADB" -s "$cand" shell getprop ro.product.model 2>/dev/null | tr -d '\r')"
    case "$model" in
      *Quest*|*quest*) SERIAL="$cand"; break ;;
    esac
  done
fi

# Say what was passed over, always. This filter silently hid the only ARM64
# hardware attached for an entire session while the work reported itself as
# "desktop only" - an AYN Thor is a stated target of this fork and answers adb
# exactly like a headset does. A tool that filters should name what it skipped.
if [ -n "${SERIAL:-}" ]; then
  for cand in $(MSYS_NO_PATHCONV=1 "$ADB" devices | awk '$2 == "device" {print $1}'); do
    [ "$cand" = "$SERIAL" ] && continue
    echo "note: also attached, not used: $cand ($(MSYS_NO_PATHCONV=1 "$ADB" -s "$cand" shell getprop ro.product.model 2>/dev/null | tr -d '
'))" >&2
  done
fi

if [ -z "${SERIAL:-}" ]; then
  echo "no Quest attached. Devices adb can see:" >&2
  MSYS_NO_PATHCONV=1 "$ADB" devices -l | sed 1d >&2
  echo "Plug the headset in and accept the USB debugging prompt, or set SERIAL=." >&2
  exit 1
fi
echo "device: $SERIAL ($(MSYS_NO_PATHCONV=1 "$ADB" -s "$SERIAL" shell getprop ro.product.model | tr -d '\r'))"

if [ ! -f "$APK" ]; then
  echo "no $APK - run tools/build_apk.sh first" >&2
  exit 1
fi

echo "== install =="
run_adb install -r "$APK" 2>&1 | tail -1

# Everything the measurement needs, plus whatever the caller added.
CVARS="bd_vr_enabled=true,bd_xr_autoplay=true,bd_msaa=0,bd_xr_refresh_rate=60"
CVARS="$CVARS,bd_render_scale=100,bd_reflections=false,bd_shadows=false"
CVARS="$CVARS,bd_cull_distance=350,bd_stereo=true,bd_perf_csv=true"
CVARS="$CVARS,bd_sample_profiler=true,bd_capture_after_s=150"
[ -n "$EXTRA" ] && CVARS="$CVARS,$EXTRA"

ARGS_LOCAL="$(cygpath -w "$(mktemp -d)")/args.txt"
: > "$ARGS_LOCAL"
echo "$CVARS" | tr ',' '\n' | while IFS='=' read -r k v; do
  [ -n "$k" ] && printf -- "--%s\n%s\n" "$k" "$v" >> "$ARGS_LOCAL"
done
echo "== cvars =="; sed 'N;s/\n/ /' "$ARGS_LOCAL" | sed 's/^--/  /'

# Printed, never discarded: a silent push failure is indistinguishable from a
# setting that does nothing.
run_adb push "$ARGS_LOCAL" "/storage/emulated/0/Android/data/$PKG/files/args.txt" 2>&1 | tail -1

FILES_PRE="/sdcard/Android/data/$PKG/files"
run_adb shell "am force-stop $PKG"
run_adb shell "am broadcast -a com.oculus.vrpowermanager.prox_close" > /dev/null
# Delete every artefact this script is about to pull, BEFORE the run.
#
# Without this the script happily pulls a capture and a CSV from a previous
# session and there is nothing in its output to say so. That is not
# hypothetical: a run that crashed 33 seconds in was reported as a full
# 170-second result, complete with a stereo verdict and a per-draw cost, all of
# it read off files written by an earlier build on an earlier day. The freshness
# checks after the run exist for the same reason.
run_adb shell "rm -rf $FILES_PRE/logs/capture $FILES_PRE/logs/perf $FILES_PRE/logs/guest_profile.txt"

RUN_STARTED_AT="$(run_adb shell "date +%s" | tr -d '\r')"
run_adb shell "am start -n $PKG/.ReblueActivity" > /dev/null

echo "== running ${SETTLE}s (field scene lands around 130s) =="
elapsed=0
while [ "$elapsed" -lt "$SETTLE" ]; do
  sleep 10
  elapsed=$((elapsed + 10))
  # Re-sent throughout: the sensor re-arms, and one broadcast at launch is not
  # enough for an unattended run.
  run_adb shell "am broadcast -a com.oculus.vrpowermanager.prox_close" > /dev/null 2>&1
  printf "  %ds\r" "$elapsed"
done
echo

FILES="$FILES_PRE"
LOG="$(run_adb shell "ls -t $FILES/logs/*.log | head -1" | tr -d '\r')"

echo
echo "== did the build actually start, and did it survive? =="
run_adb shell "grep -c 'args.txt added' '$LOG'" | tr -d '\r' | sed 's/^/  args.txt lines matched: /'
run_adb logcat -d 2>/dev/null | grep -i "dlopen.*reblue" | tail -2

# Did it crash? A crash mid-run leaves every artefact below short or absent, and
# the numbers then describe a fraction of a run - or, if the pre-clean above is
# ever removed, a previous one entirely.
CRASH="$(run_adb shell "grep -cE 'ACCESS_VIOLATION|Fatal: reblue Crashed|SIGSEGV' '$LOG'" | tr -d '\r')"
if [ "${CRASH:-0}" != "0" ]; then
  echo "  *** CRASHED - $CRASH fatal line(s) in the log. Numbers below are a"
  echo "  *** partial run at best. Crash tail:"
  run_adb shell "grep -E 'ACCESS_VIOLATION|Fatal: reblue Crashed' '$LOG' | tail -2" | tr -d '\r' | sed 's/^/      /'
fi

# Is it even still alive? verify does not force-stop at the end, so a live pid
# is the normal case and a dead one means it exited or crashed.
if [ -z "$(run_adb shell "pidof $PKG" | tr -d '\r')" ]; then
  echo "  *** process is NOT running at the end of the settle period"
fi

echo
echo "== frame breakdown =="
run_adb shell "grep 'fps |' '$LOG' | tail -3" | sed 's/.*\[xr\]/  /'

echo
echo "== performance hints and thread policy =="
run_adb shell "grep -E 'performance level|registered thread|core clusters|thread policy' '$LOG' | head -4" | sed 's/.*\[bd\]/ /'

echo
echo "== per-thread CPU =="
PID="$(run_adb shell "pidof $PKG" | tr -d '\r')"
[ -n "$PID" ] && run_adb shell "top -H -b -n 1 -p $PID | sed -n '5,12p'" | tr -d '\r'

OUT="out/device"
mkdir -p "$OUT"
# Clear the local side too, so a failed pull leaves nothing to misread as a
# result. The remote side was cleared before the run; this closes the other end.
rm -f "$OUT/perf.csv" "$OUT/guest_profile.txt" "$OUT/capture.raw"
echo
echo "== pulling artefacts to $OUT =="
CSV="$(run_adb shell "ls -t $FILES/logs/perf/*.csv 2>/dev/null | head -1" | tr -d '\r')"
[ -z "$CSV" ] && echo "  *** no perf CSV was written this run"
# The WHOLE file, not its tail. Pulling `tail -400` here would hand
# perf_summary exactly the window that made every measurement in this project
# wrong until 2026-08-30: a run does not end in a steady state, and its last
# stretch is a menu at ~20 draws a frame. Field filtering and the within-run A/B
# both need the full run to select from.
[ -n "$CSV" ] && run_adb shell "cat '$CSV'" > "$OUT/perf.csv" &&   echo "  perf.csv ($(wc -l < "$OUT/perf.csv" | tr -d ' ') rows)"
run_adb shell "cat $FILES/logs/guest_profile.txt 2>/dev/null" > "$OUT/guest_profile.txt" 2>/dev/null
[ -s "$OUT/guest_profile.txt" ] && echo "  guest_profile.txt"
CAP="$(run_adb shell "ls -t $FILES/logs/capture/*.raw 2>/dev/null | head -1" | tr -d '\r')"
if [ -n "$CAP" ]; then
  run_adb pull "$CAP" "$OUT/capture.raw" > /dev/null 2>&1 && echo "  capture.raw"
else
  echo "  *** no capture was written this run - do NOT run stereo_check on a"
  echo "  *** stale out/device/capture.raw and call it a result"
fi

echo
echo "next:"
echo "  python tools/perf_summary.py $OUT/perf.csv                 # field frames, not the tail"
echo
echo "  To compare a change on the device, do not run it twice - this workload"
echo "  drifts far more than that can survive. Pass an A/B instead:"
echo "     bash tools/verify_quest.sh bd_ab_flag=bd_cull_early,bd_ab_period=240"
echo "  and perf_summary will report the two arms from the one run."
echo
echo "  python tools/symbolize_profile.py $OUT/guest_profile.txt   # what the guest spends its time on"
echo "  python tools/stereo_check.py --raw $OUT/capture.raw        # does stereo still have depth"
echo "  # and look at the capture. A frame-time number is not a substitute."
