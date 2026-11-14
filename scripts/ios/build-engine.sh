#!/usr/bin/env bash
# Build the engine (libdapple) for Apple targets ON THE MAC (run via scripts/ios/mac.sh or directly there).
#   build-engine.sh <macos|ios-sim|ios-device> [gpu=on|off]
# Output: ~/dapple/build/<target>/bin/ (macos: libdapple.dylib + tests; ios-*: dapple.framework)
set -euo pipefail
source "$HOME/dapple/src/scripts/ios/env.sh"
TARGET="${1:-ios-sim}"; GPU="${2:-on}"
SRC="$HOME/dapple/src"; B="$HOME/dapple/build/$TARGET"
GLSLC="$HOME/dapple/tools/vulkansdk/macOS/bin/glslc"
ARGS=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DDAPPLE_GLSLC="$GLSLC" -DCMAKE_PROJECT_INCLUDE="$SRC/cmake/apple_framework.cmake")
[ "$GPU" = on ] && ARGS+=(-DDAPPLE_GPU=ON) || ARGS+=(-DDAPPLE_GPU=OFF)
case "$TARGET" in
  macos)      ARGS+=(-DCMAKE_OSX_ARCHITECTURES=arm64 -DDAPPLE_BUILD_TESTS=ON -DDAPPLE_BUILD_CLI=ON);;
  ios-sim)    ARGS+=(-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0
                     -DDAPPLE_BUILD_TESTS=OFF -DDAPPLE_BUILD_CLI=OFF);;
  ios-device) ARGS+=(-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0
                     -DDAPPLE_BUILD_TESTS=OFF -DDAPPLE_BUILD_CLI=OFF);;
  *) echo "unknown target $TARGET"; exit 2;;
esac
if [ -n "${DAPPLE_CMAKE_EXTRA:-}" ]; then read -r -a EXTRA <<< "$DAPPLE_CMAKE_EXTRA"; ARGS+=("${EXTRA[@]}"); fi  # e.g. -DDAPPLE_LIBRAW_LOCKPATCH=OFF (stress baseline)
# AI masks (core/ai; docs/research/ai-runtime.md): on when the prebuilt runtimes are staged under tools/ai-rt/dl (ONNX Runtime + TensorFlow Lite C xcframeworks);
# DAPPLE_AI=off builds without. macOS also needs the TFLite C dylib built from source (tools/ai-rt/dl/tf_src/build-macos), else only the ONNX models run there.
if [ "${DAPPLE_AI:-auto}" != off ] && [ -d "$SRC/tools/ai-rt/dl/ios_ort/onnxruntime.xcframework" ]; then ARGS+=(-DDAPPLE_AI=ON); else ARGS+=(-DDAPPLE_AI=OFF); fi
# HDR merge / panorama / sequence detection: OpenCV 4.12.0 built from the pinned source (cmake/OpenCV.cmake, static, modules core imgproc features2d calib3d
# flann stitching photo video, docs/legal/opencv.md). On when the source tree is staged at tools/_dl/opencv (scripts/ios/stage-opencv.sh); DAPPLE_OPENCV=off builds without.
if [ "${DAPPLE_OPENCV:-auto}" != off ] && [ -f "$SRC/tools/_dl/opencv/opencv-4.12.0/CMakeLists.txt" ]; then ARGS+=(-DDAPPLE_WITH_OPENCV=ON); else ARGS+=(-DDAPPLE_WITH_OPENCV=OFF); fi
OMP="$HOME/dapple/tools/libomp/$TARGET"
[ -f "$OMP/lib/libomp.a" ] && ARGS+=(-DDAPPLE_LIBOMP_DIR="$OMP")  # LibRaw OpenMP (build-libomp.sh); absent: single-threaded LibRaw
cmake -S "$SRC" -B "$B" "${ARGS[@]}"
cmake --build "$B" -j8
