#!/usr/bin/env bash
# Dapple emulator helper. Usage: tools/emu.sh <cmd> [args]
#   start <avd> | stop | shot <name> | install <apk> | push-samples | tap x y | text "str" | key <KEYCODE> | ui | wait | adb ...
#   env EMU_GPU=host uses the host GPU (perf runs: swiftshader raster costs 100-300 ms per frame and skews UI timings)
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$HERE/env.sh"
ROOT="$(cd "$HERE/.." && pwd)"
OUT="$ROOT/build/emu"; mkdir -p "$OUT"
SAMPLES_SRC="${SAMPLES_SRC:-/d/图片}"
export MSYS_NO_PATHCONV=1
# SAFETY: physical phones may be attached to this PC and are NOT ours. Every adb call
# goes through this wrapper, which pins -s emulator-<port> and refuses anything else.
export ANDROID_AVD_HOME="$HERE/android-avd"
EMU_PORT="${EMU_PORT:-5554}"; EMU_SERIAL="emulator-$EMU_PORT"; export ANDROID_SERIAL="$EMU_SERIAL"
adb() {
  case "$EMU_SERIAL" in emulator-[0-9]*) ;; *) echo "refusing: bad serial $EMU_SERIAL" >&2; return 99;; esac
  command adb -s "$EMU_SERIAL" "$@"
}
require_emulator() {
  command adb devices | awk 'NR>1 && $2=="device"{print $1}' | grep -qx "$EMU_SERIAL"     || { echo "no running emulator ($EMU_SERIAL) - run: tools/emu.sh start <avd>" >&2; exit 2; }
}

emu_start() {
  local avd="${1:-dapple_n5}" t0=$SECONDS
  # MSYS_NO_PATHCONV disables env-var path translation, so hand the .exe Windows-style paths.
  ANDROID_AVD_HOME="$(cygpath -m "$ANDROID_AVD_HOME")" ANDROID_SDK_ROOT="$(cygpath -m "$ANDROID_SDK_ROOT")"   ANDROID_HOME="$(cygpath -m "$ANDROID_HOME")" ANDROID_USER_HOME="$(cygpath -m "$ANDROID_USER_HOME")"   nohup "$ANDROID_HOME/emulator/emulator.exe" -avd "$avd" -no-window -no-audio -no-boot-anim \
    -port $EMU_PORT -no-snapshot -gpu ${EMU_GPU:-swiftshader_indirect} > "$OUT/emulator-$avd.log" 2>&1 &
  until adb get-state >/dev/null 2>&1; do sleep 2; done
  until [ "$(adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = "1" ]; do
    (( SECONDS - t0 > ${BOOT_TIMEOUT:-600} )) && { echo "boot timeout"; return 1; }
    sleep 3
  done
  adb shell settings put global window_animation_scale 0
  adb shell settings put global transition_animation_scale 0
  adb shell settings put global animator_duration_scale 0
  sleep 5; adb shell input keyevent KEYCODE_WAKEUP
  echo "booted $avd in $((SECONDS - t0))s"
}
emu_stop() { adb emu kill >/dev/null 2>&1; sleep 5; echo stopped; }
emu_shot() { local f="$OUT/${1:-shot}.png"; adb exec-out screencap -p > "$f" && echo "$f"; }
emu_install() { adb install -r -g "$(cygpath -w "$1")"; }
emu_push_samples() {
  adb shell mkdir -p /sdcard/DCIM/Camera
  local n=0 f
  for f in "$SAMPLES_SRC"/*/*.ARW "$SAMPLES_SRC"/*.ARW "$SAMPLES_SRC"/*/*.arw; do
    [ -f "$f" ] || continue
    adb push "$(cygpath -w "$f")" "/sdcard/DCIM/Camera/$(basename "$f")" >/dev/null && n=$((n+1))
    adb shell am broadcast -a android.intent.action.MEDIA_SCANNER_SCAN_FILE -d "file:///sdcard/DCIM/Camera/$(basename "$f")" >/dev/null
    [ $n -ge 6 ] && break
  done
  echo "pushed $n files"
}
emu_tap() { adb shell input tap "$1" "$2"; }
emu_text() { adb shell input text "$(printf '%s' "$*" | sed 's/ /%s/g')"; }
emu_key() { adb shell input keyevent "$1"; }
emu_ui() { adb shell uiautomator dump /sdcard/ui.xml >/dev/null && adb exec-out cat /sdcard/ui.xml > "$OUT/ui.xml" && echo "$OUT/ui.xml"; }

cmd="$1"; shift
case "$cmd" in start|"") ;; *) require_emulator;; esac
case "$cmd" in
  start) emu_start "$@";; stop) emu_stop;; shot) emu_shot "$@";; install) emu_install "$@";;
  push-samples) emu_push_samples;; tap) emu_tap "$@";; text) emu_text "$@";; key) emu_key "$@";;
  ui) emu_ui;; wait) adb wait-for-device;; adb) for x in "$@"; do case "$x" in -s|-d|-e|-H|-P|-L) echo "refusing adb flag $x" >&2; exit 2;; esac; done; adb "$@";;
  *) sed -n 2,3p "$0"; exit 1;;
esac
