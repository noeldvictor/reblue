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

run_adb shell "am force-stop $PKG"
run_adb shell "am broadcast -a com.oculus.vrpowermanager.prox_close" > /dev/null
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

FILES="/sdcard/Android/data/$PKG/files"
LOG="$(run_adb shell "ls -t $FILES/logs/*.log | head -1" | tr -d '\r')"

echo
echo "== did the build actually start? =="
run_adb shell "grep -c 'args.txt added' '$LOG'" | tr -d '\r' | sed 's/^/  args.txt lines matched: /'
run_adb logcat -d 2>/dev/null | grep -i "dlopen.*reblue" | tail -2

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
echo
echo "== pulling artefacts to $OUT =="
CSV="$(run_adb shell "ls -t $FILES/logs/perf/*.csv 2>/dev/null | head -1" | tr -d '\r')"
[ -n "$CSV" ] && run_adb shell "tail -400 '$CSV'" > "$OUT/perf.csv" && echo "  perf.csv"
run_adb shell "cat $FILES/logs/guest_profile.txt 2>/dev/null" > "$OUT/guest_profile.txt" 2>/dev/null
[ -s "$OUT/guest_profile.txt" ] && echo "  guest_profile.txt"
CAP="$(run_adb shell "ls -t $FILES/logs/capture/*.raw 2>/dev/null | head -1" | tr -d '\r')"
if [ -n "$CAP" ]; then
  run_adb pull "$CAP" "$OUT/capture.raw" > /dev/null 2>&1 && echo "  capture.raw"
fi

echo
echo "next:"
echo "  python tools/symbolize_profile.py $OUT/guest_profile.txt   # what the guest spends its time on"
echo "  python tools/stereo_check.py --raw $OUT/capture.raw        # does stereo still have depth"
echo "  # and look at the capture. A frame-time number is not a substitute."
