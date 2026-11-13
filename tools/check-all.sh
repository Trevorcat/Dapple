#!/usr/bin/env bash
# One-command verification. Usage: tools/check-all.sh [--quick] [--android] [--emu] [--keep-going(default)] [--fail-fast]
#   default : desktop core build (GPU on, SwiftShader) + ctest, flutter analyze, flutter test, JVM unit tests, host video tests
#   (+ an OpenCV-ON desktop build running merge_tests: SKIP line when tools/_dl/opencv is absent)
#   --quick   skip slow suites (gpu_parity_swiftshader, JVM unit tests, flutter test)
#   --android NDK arm64-v8a + x86_64 core builds and debug APK builds
#   --emu     on-emulator native video suite + dapple_golden selfcheck (free port; starts/stops its own emulator)
# Logs: build/check/logs/<step>.log ; summary: build/check/logs/summary.txt. Exit code non-zero if any step FAILs.
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -W)"; R="$(cd "$HERE/.." && pwd -W)"
source "$HERE/env.sh"
export MSYS_NO_PATHCONV=1
QUICK=0; ANDROID=0; EMU=0; FAILFAST=0
for a in "$@"; do case "$a" in
  --quick) QUICK=1;; --android) ANDROID=1;; --emu) EMU=1;; --fail-fast) FAILFAST=1;;
  -h|--help) sed -n 2,8p "$0"; exit 0;; *) echo "unknown flag $a"; exit 2;; esac; done
LOGS="$R/build/check/logs"; mkdir -p "$LOGS"; rm -f "$LOGS"/*.log "$LOGS/summary.txt"
RES=(); FAILED=0; NJ="${JOBS:-$(nproc)}"
BD="$R/build/check-desktop"
TC_BIN="$HERE/llvm-mingw-20261020-ucrt-x86_64/bin"
NDK="$HERE/android-sdk/ndk/28.2.13676358"
SS_ICD="$HERE/android-sdk/emulator/lib64/vulkan/vk_swiftshader_icd.json"

# step <name> <command...> : runs in a subshell, logs, records PASS/FAIL/SKIP (exit 77 = SKIP)
step() {
  local name="$1"; shift; local t0=$SECONDS rc
  printf '>> %-28s ' "$name"
  ( "$@" ) > "$LOGS/$name.log" 2>&1; rc=$?
  local dt=$((SECONDS - t0)) st=PASS
  [ $rc -eq 77 ] && st=SKIP; [ $rc -ne 0 ] && [ $rc -ne 77 ] && { st=FAIL; FAILED=1; }
  RES+=("$st|$name|${dt}s|$(tail -n 1 "$LOGS/$name.log" | cut -c1-90)")
  echo "$st (${dt}s)"
  [ $st = FAIL ] && [ $FAILFAST = 1 ] && { summary; exit 1; }
  return 0
}
skip() { RES+=("SKIP|$1|-|$2"); printf '>> %-28s SKIP (%s)\n' "$1" "$2"; }
summary() {
  { echo; echo "================ check-all summary ================"
    printf '%-5s %-28s %-8s %s\n' RESULT STEP TIME LAST-LOG-LINE
    for r in "${RES[@]}"; do IFS='|' read -r s n t l <<<"$r"; printf '%-5s %-28s %-8s %s\n' "$s" "$n" "$t" "$l"; done
    [ $FAILED = 0 ] && echo "OVERALL: PASS" || echo "OVERALL: FAIL (see $LOGS/<step>.log)"; } | tee "$LOGS/summary.txt"
}

desktop_cfg() {
  if [ -f "$BD/build.ninja" ]; then return 0; fi
  cmake -S "$R" -B "$BD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$TC_BIN/clang.exe" -DCMAKE_CXX_COMPILER="$TC_BIN/clang++.exe" \
    -DCMAKE_MAKE_PROGRAM="$HERE/ninja/ninja.exe" \
    -DDAPPLE_BUILD_CLI=ON -DDAPPLE_BUILD_TESTS=ON -DDAPPLE_WITH_OPENCV=OFF -DDAPPLE_GPU=ON \
    -DDAPPLE_GLSLC="$NDK/shader-tools/windows-x86_64/glslc.exe" -DDAPPLE_VULKAN_DL="$HERE/_dl/vulkan" \
    -DDAPPLE_SWIFTSHADER_ICD="$SS_ICD"
}
desktop_build() { cmake --build "$BD" -j "$NJ"; }
desktop_ctest() {
  local ex=(); [ $QUICK = 1 ] && ex=(-E gpu_parity_swiftshader)
  ( cd "$BD" && ctest --output-on-failure -j "${CTEST_JOBS:-4}" "${ex[@]}" )
}
# HDR merge / panorama / sequence detection need OpenCV (DAPPLE_HAVE_OPENCV); the main desktop build above has it OFF, so merge_tests' OpenCV blocks are
# compiled out there. This step builds a second tree with OpenCV ON (GPU off, tests only; sources in tools/_dl/opencv, built static, ~650 objects the first
# time) and runs merge_tests + the golden selfcheck. No OpenCV sources -> an explicit SKIP (exit 77), never a silent pass.
OCV_SRC="$HERE/_dl/opencv/opencv-4.12.0/CMakeLists.txt"
OCV_BD="$R/build/check-opencv"
desktop_opencv() {
  if [ ! -f "$OCV_SRC" ]; then echo "SKIP: OpenCV sources not found at $OCV_SRC: HDR merge / panorama / sequence tests NOT exercised"; return 77; fi
  if [ ! -f "$OCV_BD/build.ninja" ]; then
    cmake -S "$R" -B "$OCV_BD" -G Ninja -DCMAKE_BUILD_TYPE=Release       -DCMAKE_C_COMPILER="$TC_BIN/clang.exe" -DCMAKE_CXX_COMPILER="$TC_BIN/clang++.exe"       -DCMAKE_MAKE_PROGRAM="$HERE/ninja/ninja.exe"       -DDAPPLE_BUILD_CLI=ON -DDAPPLE_BUILD_TESTS=ON -DDAPPLE_WITH_OPENCV=ON -DDAPPLE_GPU=OFF || return 1
  fi
  cmake --build "$OCV_BD" -j "$NJ" || return 1
  ( cd "$OCV_BD" && ctest --output-on-failure -R 'merge_tests' ) || return 1
  ( cd "$R" && "$OCV_BD/bin/dapple_golden" selfcheck )
}
flutter_analyze() { cd "$R/app" && flutter analyze --no-pub; }
flutter_test() { cd "$R/app" && flutter test --no-pub; }
jvm_tests() { unset MSYS_NO_PATHCONV; cd "$(cygpath -u "$R/app/android")" && ./gradlew --no-daemon -q :app:testDebugUnitTest; }
video_host() { bash "$HERE/ffmpeg/test-host.sh"; }

step desktop-configure desktop_cfg
step desktop-build desktop_build
step ctest desktop_ctest
# off (old Renderer) vs full (engine) bit for bit on this machine, one-shot renders included; also inside ctest as engine.selfcheck, kept as its own line so a
# regression in it is visible in the summary. Same binary runs on the emulator / phones (see the --emu step and docs/engine/old-pipeline-removal.md).
golden_selfcheck() { ( cd "$R" && "$BD/bin/dapple_golden" selfcheck ); }
step golden-selfcheck golden_selfcheck
step desktop-opencv desktop_opencv
step flutter-analyze flutter_analyze
if [ $QUICK = 1 ]; then skip flutter-test "--quick"; skip jvm-unit-tests "--quick"; else
  step flutter-test flutter_test; step jvm-unit-tests jvm_tests; fi
step video-host video_host

if [ $ANDROID = 1 ]; then
  droid() { # abi
    local d="$R/build/check-droid-$1"
    [ -f "$d/build.ninja" ] || cmake -S "$R" -B "$d" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI=$1 -DANDROID_PLATFORM=android-29 \
      -DCMAKE_MAKE_PROGRAM="$HERE/ninja/ninja.exe" -DDAPPLE_BUILD_CLI=ON -DDAPPLE_BUILD_TESTS=ON -DDAPPLE_WITH_OPENCV=OFF -DDAPPLE_GPU=ON \
      -DDAPPLE_GLSLC="$NDK/shader-tools/windows-x86_64/glslc.exe" -DDAPPLE_VULKAN_DL="$HERE/_dl/vulkan" || return 1
    cmake --build "$d" -j "$NJ"
  }
  step android-core-arm64 droid arm64-v8a
  step android-core-x86_64 droid x86_64
  apk_arm() { cd "$R/app" && ANDROID_USER_HOME="$(cygpath -m "$ANDROID_USER_HOME")" flutter build apk --debug --no-pub --target-platform android-arm64; }
  apk_x64() { cd "$R/app" && ANDROID_USER_HOME="$(cygpath -m "$ANDROID_USER_HOME")" flutter build apk --debug --no-pub --target-platform android-x64 -Pdapple.x86=true; }
  step apk-debug-arm64 apk_arm
  step apk-debug-x64 apk_x64
fi

if [ $EMU = 1 ]; then
  emu_suite() {
    local busy; busy="$(command adb devices | awk 'NR>1 && $1 ~ /^emulator-/{print $1}')"
    local p; for p in 5556 5560 5564 5566 5568; do echo "$busy" | grep -qx "emulator-$p" || break; done
    export EMU_PORT=$p; echo "using free emulator port $p"
    "$HERE/emu.sh" start dapple_n5 || return 1
    bash "$HERE/ffmpeg/test-android.sh"; local rc=$?
    "$HERE/emu.sh" stop; return $rc
  }
  step emu-video emu_suite
  # golden selfcheck on the x86_64 emulator (avx2 = 0 kernels); one-liner by hand: EMU_PORT=<free port> tools/emu.sh start dapple_n5 &&
  #   adb -s emulator-<port> push build/check-droid-x86_64/bin/dapple_golden /data/local/tmp/ && adb -s emulator-<port> shell 'cd /data/local/tmp && chmod +x dapple_golden && ./dapple_golden selfcheck'
  emu_golden() {
    local busy; busy="$(command adb devices | awk 'NR>1 && $1 ~ /^emulator-/{print $1}')"
    local p; for p in 5556 5560 5564 5566 5568; do echo "$busy" | grep -qx "emulator-$p" || break; done
    export EMU_PORT=$p; local ser="emulator-$p"
    local g="$R/build/check-droid-x86_64/bin/dapple_golden"; [ -f "$g" ] || { echo "build the android x86_64 core first (--android)"; return 77; }
    "$HERE/emu.sh" start dapple_n5 || return 1
    local rc=1
    command adb -s "$ser" push "$g" /data/local/tmp/dapple_golden >/dev/null &&       command adb -s "$ser" shell 'cd /data/local/tmp && chmod +x dapple_golden && ./dapple_golden selfcheck; echo "exit=$?"' | tee /dev/stderr | grep -q 'exit=0' && rc=0
    "$HERE/emu.sh" stop; return $rc
  }
  step emu-golden-selfcheck emu_golden
fi

summary
exit $FAILED
