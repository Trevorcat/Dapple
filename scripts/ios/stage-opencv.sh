#!/usr/bin/env bash
# PC -> Mac: stages the pinned OpenCV 4.12.0 source tarball (tools/_dl/opencv, the same one the Android / desktop builds use) into the Mac checkout and
# extracts it; sha256 checked against cmake/OpenCV.cmake. Usage: stage-opencv.sh [host]
set -euo pipefail
cd "$(dirname "$0")/../.."
H="${1:-dapple-mac}"
T=tools/_dl/opencv/opencv-4.12.0.tar.gz
SHA=44c106d5bb47efec04e531fd93008b3fcd1d27138985c5baf4eafac0e1ec9e9d
[ "$(sha256sum "$T" | cut -d' ' -f1)" = "$SHA" ] || { echo "checksum mismatch: $T"; exit 1; }
ssh "$H" 'mkdir -p ~/dapple/src/tools/_dl/opencv'
cat "$T" | ssh "$H" 'cat > ~/dapple/src/tools/_dl/opencv/opencv-4.12.0.tar.gz'
ssh "$H" 'cd ~/dapple/src/tools/_dl/opencv && [ -f opencv-4.12.0/CMakeLists.txt ] || tar -xzf opencv-4.12.0.tar.gz; ls opencv-4.12.0 | head -3'
