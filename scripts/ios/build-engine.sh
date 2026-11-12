#!/usr/bin/env bash
# Build the engine (libdapple) for Apple targets ON THE MAC (run via scripts/ios/mac.sh or directly there).
#   build-engine.sh <macos|ios-sim|ios-device> [gpu=on|off]
# Output: ~/dapple/build/<target>/bin/ (macos: libdapple.dylib + tests; ios-*: dapple.framework)
set -euo pipefail
source "$HOME/dapple/src/scripts/ios/env.sh"
TARGET="${1:-ios-sim}"; GPU="${2:-on}"
SRC="$HOME/dapple/src"; B="$HOME/dapple/build/$TARGET"
GLSLC="$HOME/dapple/tools/vulkansdk/macOS/bin/glslc"
ARGS=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DDAPPLE_WITH_OPENCV=OFF -DDAPPLE_GLSLC="$GLSLC")
[ "$GPU" = on ] && ARGS+=(-DDAPPLE_GPU=ON) || ARGS+=(-DDAPPLE_GPU=OFF)
case "$TARGET" in
  macos)      ARGS+=(-DCMAKE_OSX_ARCHITECTURES=arm64 -DDAPPLE_BUILD_TESTS=ON -DDAPPLE_BUILD_CLI=ON);;
  ios-sim)    ARGS+=(-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0
                     -DDAPPLE_BUILD_TESTS=OFF -DDAPPLE_BUILD_CLI=OFF -DCMAKE_PROJECT_INCLUDE="$SRC/cmake/apple_framework.cmake");;
  ios-device) ARGS+=(-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0
                     -DDAPPLE_BUILD_TESTS=OFF -DDAPPLE_BUILD_CLI=OFF -DCMAKE_PROJECT_INCLUDE="$SRC/cmake/apple_framework.cmake");;
  *) echo "unknown target $TARGET"; exit 2;;
esac
cmake -S "$SRC" -B "$B" "${ARGS[@]}"
cmake --build "$B" -j8
