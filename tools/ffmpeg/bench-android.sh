#!/usr/bin/env bash
# Builds tests/video/dapple_video_bench.cpp for x86_64 Android, runs it on the emulator against a video already on the device.
#   EMU_PORT=5554 tools/ffmpeg/bench-android.sh /sdcard/DCIM/Camera/C0021.MP4 [startSec durSec maxEdge maxFps]
set -e
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; R="$(cd "$HERE/../.." && pwd)"
source "$R/tools/env.sh"
export MSYS_NO_PATHCONV=1
EMU_PORT="${EMU_PORT:-5554}"
adb() { command adb -s "emulator-$EMU_PORT" "$@"; }   # never a physical phone
TC="$R/tools/android-sdk/ndk/28.2.13676358/toolchains/llvm/prebuilt/windows-x86_64/bin"
I="$HERE/install/x86_64"; OUT="$HERE/build/test"; mkdir -p "$OUT"
"$TC/x86_64-linux-android29-clang++.cmd" -std=c++17 -O2 -I"$(cygpath -m "$I")/include" "$(cygpath -m "$R")/tests/video/dapple_video_bench.cpp" "$(cygpath -m "$R")/core/video/dapple_video.cpp" \
  -L"$(cygpath -m "$I")/lib" -lavformat -lavcodec -lswscale -lswresample -lavutil -llog -static-libstdc++ -Wl,-rpath,'$ORIGIN' -o "$(cygpath -m "$OUT")/dapple_video_bench"
D=/data/local/tmp/dvtest
adb shell "mkdir -p $D"
for f in "$OUT/dapple_video_bench" "$I"/lib/*.so; do adb push "$(cygpath -w "$f")" $D/ >/dev/null; done
adb shell "chmod +x $D/dapple_video_bench; cd $D && LD_LIBRARY_PATH=$D ./dapple_video_bench $*"
