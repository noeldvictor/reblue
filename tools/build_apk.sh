#!/usr/bin/env bash
# Package the android-arm64 build into an installable APK.
#
# Deliberately not Gradle. Gradle wants a network fetch of a wrapper and a
# plugin on first run, which is exactly the sort of wait this project is trying
# to avoid; the build-tools binaries it would drive are already on disk. This
# does the same six steps directly and takes a couple of seconds.
#
#   cmake --build out/build/android-arm64-release   # produces the .so files
#   tools/build_apk.sh
#   adb install -r out/apk/reblue.apk
#
# In: ANDROID_HOME (or the default below), and a completed android-arm64 build.
set -euo pipefail

SDK="${ANDROID_HOME:-$LOCALAPPDATA/Android/Sdk}"
SDK="${SDK//\\//}"
BUILD_TOOLS="${BUILD_TOOLS:-$SDK/build-tools/36.0.0}"
PLATFORM_JAR="${PLATFORM_JAR:-$SDK/platforms/android-33/android.jar}"
NATIVE_DIR="${NATIVE_DIR:-out/build/android-arm64-release}"
SDL_JAVA="${SDL_JAVA:-out/rexglue-src/thirdparty/sdl3/android-project/app/src/main/java}"
OUT="${OUT:-out/apk}"
OUT_BUILD_LOG="${TMPDIR:-/tmp}/reblue_apk_build.log"

# Build first, and refuse to package if it fails.
#
# Without this the script happily packages whatever libreblue.so was left on
# disk by the last *successful* build. Not hypothetical: a compile error in
# plume produced "ninja: build stopped", this script then signed and installed
# the previous binary, and its results were read as the new build's - the same
# shape as the missing-loader trap below, and as the args.txt push failures that
# made three perf experiments measure a stale file.
#
# cmake --build on an up-to-date tree is a no-op costing under a second, so this
# is free in the common case. SKIP_BUILD=1 to package a binary built elsewhere.
if [ -z "${SKIP_BUILD:-}" ] && [ -f "$NATIVE_DIR/build.ninja" ]; then
  echo "==> building $NATIVE_DIR"
  if ! cmake --build "$NATIVE_DIR" --target reblue > "$OUT_BUILD_LOG" 2>&1; then
    tail -5 "$OUT_BUILD_LOG" >&2
    echo "build failed - refusing to package a stale libreblue.so" >&2
    exit 1
  fi
fi

for f in "$BUILD_TOOLS/aapt2.exe" "$PLATFORM_JAR" "$NATIVE_DIR/libreblue.so" \
         "$NATIVE_DIR/librexruntime.so" "$SDL_JAVA/org/libsdl/app/SDLActivity.java"; do
  [ -e "$f" ] || { echo "missing: $f" >&2; exit 1; }
done

rm -rf "$OUT"
mkdir -p "$OUT/res" "$OUT/classes" "$OUT/staging/lib/arm64-v8a"

echo "==> compiling resources"
"$BUILD_TOOLS/aapt2.exe" compile --dir android/res -o "$OUT/res.zip"

echo "==> linking resources and manifest"
"$BUILD_TOOLS/aapt2.exe" link \
  -o "$OUT/base.apk" \
  -I "$PLATFORM_JAR" \
  --manifest android/AndroidManifest.xml \
  --java "$OUT/gen" \
  "$OUT/res.zip"

echo "==> compiling java"
mkdir -p "$OUT/gen"
# SDL's activity plus ours plus the R class aapt2 just generated.
find "$SDL_JAVA" android/java "$OUT/gen" -name '*.java' > "$OUT/sources.txt"
javac -nowarn -source 17 -target 17 -encoding UTF-8 \
  -classpath "$PLATFORM_JAR" \
  -d "$OUT/classes" "@$OUT/sources.txt"

echo "==> dexing"
find "$OUT/classes" -name '*.class' > "$OUT/classes.txt"
"$BUILD_TOOLS/d8.bat" --min-api 29 --lib "$PLATFORM_JAR" \
  --output "$OUT/staging" "@$OUT/classes.txt" > /dev/null

echo "==> staging native libraries"
# extractNativeLibs is on in the manifest, so these need not be page-aligned or
# stored uncompressed; the installer unpacks them.
cp "$NATIVE_DIR/libreblue.so" "$NATIVE_DIR/librexruntime.so" "$OUT/staging/lib/arm64-v8a/"
# The OpenXR loader. libreblue.so links against it by soname, so if the build
# needs it and it is not in the APK the app dies in dlopen before main() -
# "library libopenxr_loader.so not found", logged by the Java harness to
# logcat, with no reblue log file written at all. That looks exactly like a
# launch hang, and the previous version of this block made it easy to hit: the
# path came only from $OPENXR_LOADER, so forgetting to export it silently
# produced a broken APK that installed and reported success.
#
# So: find it without being told, and refuse to package a binary that needs it
# and has not got it.
if [ -z "${OPENXR_LOADER:-}" ]; then
  for cand in "out/xr-loader-android/src/loader/libopenxr_loader.so"; do
    [ -f "$cand" ] && OPENXR_LOADER="$cand" && break
  done
fi
if [ -n "${OPENXR_LOADER:-}" ] && [ -f "$OPENXR_LOADER" ]; then
  cp "$OPENXR_LOADER" "$OUT/staging/lib/arm64-v8a/"
  echo "    staged $(basename "$OPENXR_LOADER")"
fi
# Anything else that has to ship in the APK's lib dir - a Vulkan validation
# layer, a replacement Turnip driver. Space-separated paths in $EXTRA_LIBS.
# Android only loads layers from the app's own lib directory, so a layer that
# is not packaged here cannot be enabled at all, whatever the settings say.
for extra in ${EXTRA_LIBS:-}; do
  if [ -f "$extra" ]; then
    cp "$extra" "$OUT/staging/lib/arm64-v8a/"
    echo "    staged $(basename "$extra")"
  else
    echo "EXTRA_LIBS: no such file: $extra" >&2
    exit 1
  fi
done

# DT_NEEDED is the authority on whether this build actually wants it.
if grep -aq "libopenxr_loader.so" "$NATIVE_DIR/libreblue.so"    && [ ! -f "$OUT/staging/lib/arm64-v8a/libopenxr_loader.so" ]; then
  echo "libreblue.so links against libopenxr_loader.so but it is not staged." >&2
  echo "The APK would install cleanly and then fail in dlopen before main()." >&2
  echo "Build the loader, or point OPENXR_LOADER at it." >&2
  exit 1
fi

echo "==> assembling"
cp "$OUT/base.apk" "$OUT/unsigned.apk"
OUT="$OUT" python tools/apk_append.py

echo "==> aligning"
"$BUILD_TOOLS/zipalign.exe" -f -p 4 "$OUT/unsigned.apk" "$OUT/aligned.apk"

echo "==> signing"
# A throwaway debug key. Regenerated if absent so a fresh clone just works;
# never use this for anything that leaves the machine.
# Outside $OUT, which is wiped each run - otherwise every build is signed with
# a fresh key and adb refuses the upgrade with INSTALL_FAILED_UPDATE_INCOMPATIBLE.
KEYSTORE="${KEYSTORE:-out/reblue-debug.keystore}"
if [ ! -f "$KEYSTORE" ]; then
  keytool -genkeypair -keystore "$KEYSTORE" -alias reblue -storepass android \
    -keypass android -keyalg RSA -keysize 2048 -validity 10000 \
    -dname "CN=reblue debug" >/dev/null 2>&1
fi
"$BUILD_TOOLS/apksigner.bat" sign --ks "$KEYSTORE" --ks-pass pass:android \
  --key-pass pass:android --min-sdk-version 29 \
  --out "$OUT/reblue.apk" "$OUT/aligned.apk"

"$BUILD_TOOLS/apksigner.bat" verify --min-sdk-version 29 "$OUT/reblue.apk" \
  && echo "==> signature verified"

ls -lh "$OUT/reblue.apk"
