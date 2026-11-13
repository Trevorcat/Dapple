#!/usr/bin/env bash
# Host tests that need no FFmpeg: the Ultra HDR-aware Motion Photo container (core/src/motion_photo.cpp).
set -e
R="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CXX="$R/tools/llvm-mingw-20261020-ucrt-x86_64/bin/clang++.exe"
mkdir -p "$R/tools/ffmpeg/build/test"
"$CXX" -static -std=c++23 -O1 -I"$(cygpath -m "$R")/core/include" "$(cygpath -m "$R")/tests/video/motion_ultrahdr_test.cpp" "$(cygpath -m "$R")/core/src/motion_photo.cpp" \
  -o "$(cygpath -m "$R")/tools/ffmpeg/build/test/motion_ultrahdr_test.exe"
"$R/tools/ffmpeg/build/test/motion_ultrahdr_test.exe" "$(cygpath -m "$R")/tests/data/video"
