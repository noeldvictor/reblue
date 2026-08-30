#!/usr/bin/env bash
# Run reblue on the Quest under Vulkan validation layers and print what they say.
#
# This exists because inference has repeatedly lost to validation on this
# project. CLAUDE.md records the layers settling a multiview question in one run
# after three sessions of reasoning, and surfacing an unrelated live bug
# (`Int64` declared by every shader while `shaderInt64` is not enabled) in the
# same run. The open multiview bug - the resolve draw writes nothing into its
# companion, with ten causes eliminated by measurement and none of them right -
# is exactly the shape a layer names in one line.
#
# Khronos publishes Android binaries and no Windows ones, so this is the route
# that exists. Fetch once:
#
#   curl -sL -o out/vvl/android.zip \
#     https://github.com/KhronosGroup/Vulkan-ValidationLayers/releases/latest/download/android-binaries-<ver>.zip
#   # extract arm64-v8a/libVkLayer_khronos_validation.so
#
# The layers are slow enough to change what a frame time means, so this turns
# them off again on the way out. Never leave them enabled and then measure.
#
# Usage:  bash tools/validate_quest.sh [extra_cvar=value,...]

set -u

ADB="${ADB:-C:/Users/leanerdesigner/AppData/Local/Android/Sdk/platform-tools/adb.exe}"
PKG="${PKG:-com.reblue}"
SETTLE="${SETTLE:-160}"
EXTRA="${1:-bd_stereo_multiview=true}"

LAYER="${LAYER:-$(find out/vvl -name 'libVkLayer_khronos_validation.so' 2>/dev/null | head -1)}"
if [ -z "$LAYER" ] || [ ! -f "$LAYER" ]; then
  echo "no validation layer found under out/vvl - see the header for the fetch" >&2
  exit 1
fi
echo "layer: $LAYER"

run_adb() { MSYS_NO_PATHCONV=1 "$ADB" ${SERIAL:+-s "$SERIAL"} "$@"; }

if [ -z "${SERIAL:-}" ]; then
  for cand in $(MSYS_NO_PATHCONV=1 "$ADB" devices | awk '$2 == "device" {print $1}'); do
    model="$(MSYS_NO_PATHCONV=1 "$ADB" -s "$cand" shell getprop ro.product.model 2>/dev/null | tr -d '\r')"
    case "$model" in *Quest*|*quest*) SERIAL="$cand"; break ;; esac
  done
fi
if [ -z "${SERIAL:-}" ]; then
  echo "no Quest attached. Devices adb can see:" >&2
  MSYS_NO_PATHCONV=1 "$ADB" devices -l | sed 1d >&2
  exit 1
fi
echo "device: $SERIAL"

# The layer only loads from the app's own lib directory, which is what EXTRA_LIBS
# in build_apk.sh is for.
echo "== packaging with the layer =="
EXTRA_LIBS="$LAYER" bash tools/build_apk.sh 2>&1 | grep -iE "signature verified|error" | tail -2
run_adb install -r out/apk/reblue.apk 2>&1 | tail -1

echo "== enabling layers for $PKG =="
run_adb shell "settings put global enable_gpu_debug_layers 1"
run_adb shell "settings put global gpu_debug_app $PKG"
run_adb shell "settings put global gpu_debug_layers VK_LAYER_KHRONOS_validation"
run_adb shell "settings put global gpu_debug_layer_app $PKG"

CVARS="bd_vr_enabled=true,bd_xr_autoplay=true,bd_msaa=0,bd_stereo=true,$EXTRA"
TMP="$(cygpath -w "$(mktemp -d)")/args.txt"
: > "$TMP"
echo "$CVARS" | tr ',' '\n' | while IFS='=' read -r k v; do
  [ -n "$k" ] && printf -- "--%s\n%s\n" "$k" "$v" >> "$TMP"
done
run_adb push "$TMP" "/storage/emulated/0/Android/data/$PKG/files/args.txt" 2>&1 | tail -1

run_adb logcat -c
run_adb shell "am force-stop $PKG"
run_adb shell "am broadcast -a com.oculus.vrpowermanager.prox_close" > /dev/null
run_adb shell "am start -n $PKG/.ReblueActivity" > /dev/null

echo "== running ${SETTLE}s under validation (slow; do not read frame times) =="
e=0
while [ "$e" -lt "$SETTLE" ]; do
  sleep 10; e=$((e + 10))
  run_adb shell "am broadcast -a com.oculus.vrpowermanager.prox_close" > /dev/null 2>&1
  printf "  %ds\r" "$e"
done
echo

echo
echo "== validation output, most frequent first =="
run_adb logcat -d 2>/dev/null \
  | grep -iE "VALIDATION|VUID-" \
  | sed 's/.*VUID/VUID/' \
  | cut -c1-160 \
  | sort | uniq -c | sort -rn | head -25

echo
echo "== turning the layers back off =="
# Left on, they change what every subsequent frame time means.
run_adb shell "settings put global enable_gpu_debug_layers 0"
run_adb shell "settings delete global gpu_debug_app" > /dev/null 2>&1
echo "done - rebuild without EXTRA_LIBS before measuring anything."
