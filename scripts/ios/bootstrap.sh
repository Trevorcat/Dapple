#!/usr/bin/env bash
# Bootstrap the Mac build environment under ~/dapple (no Homebrew, no sudo, nothing outside ~/dapple).
# Run on the Mac:   bash bootstrap.sh          or from the PC:   ssh dapple-mac 'bash -s' < scripts/ios/bootstrap.sh
# Idempotent. Records versions in ~/dapple/tools/VERSIONS.txt.
set -euo pipefail
D="$HOME/dapple"; T="$D/tools"; DL="$D/dl"; SRC="$D/src"
CMAKE_VER=4.4.3; NINJA_VER=1.13.1; FLUTTER_VER=3.47.8; VKSDK_VER=1.4.309.0; SPIRVCROSS_TAG=vulkan-sdk-1.4.309.0
VKHDR_TAG=v1.4.309; VOLK_TAG=1.4.304
mkdir -p "$T" "$DL" "$SRC/tools/_dl" "$D/build" "$D/out"
fetch() { [ -s "$2" ] || curl -fL --retry 3 -sS -o "$2.part" "$1" && mv -f "$2.part" "$2" 2>/dev/null || true; }
get() { if [ ! -s "$2" ]; then curl -fL --retry 3 -sS -o "$2.part" "$1"; mv "$2.part" "$2"; fi; }

# CMake (official universal tarball)
if [ ! -x "$T/cmake/CMake.app/Contents/bin/cmake" ]; then
  get "https://github.com/Kitware/CMake/releases/download/v$CMAKE_VER/cmake-$CMAKE_VER-macos-universal.tar.gz" "$DL/cmake.tgz"
  rm -rf "$T/cmake" "$T/cmake-tmp"; mkdir "$T/cmake-tmp"; tar -xzf "$DL/cmake.tgz" -C "$T/cmake-tmp"; mv "$T/cmake-tmp/cmake-$CMAKE_VER-macos-universal" "$T/cmake"; rmdir "$T/cmake-tmp"
fi
# Ninja
if [ ! -x "$T/ninja/ninja" ]; then
  get "https://github.com/ninja-build/ninja/releases/download/v$NINJA_VER/ninja-mac.zip" "$DL/ninja.zip"
  rm -rf "$T/ninja"; mkdir "$T/ninja"; unzip -q -o "$DL/ninja.zip" -d "$T/ninja"; chmod +x "$T/ninja/ninja"
fi
# Flutter (official stable archive for macOS arm64)
if [ ! -x "$T/flutter/bin/flutter" ]; then
  get "https://storage.googleapis.com/flutter_infra_release/releases/stable/macos/flutter_macos_arm64_$FLUTTER_VER-stable.zip" "$DL/flutter.zip"
  rm -rf "$T/flutter"; unzip -q -o "$DL/flutter.zip" -d "$T"
fi
# LunarG Vulkan SDK: only for glslc / spirv-val / spirv-opt (installed headless into ~/dapple/tools/vulkansdk, no sudo)
if [ ! -x "$T/vulkansdk/macOS/bin/glslc" ]; then
  get "https://sdk.lunarg.com/sdk/download/$VKSDK_VER/mac/vulkansdk-macos-$VKSDK_VER.zip" "$DL/vulkansdk.zip"
  rm -rf "$DL/vksdk-inst"; mkdir -p "$DL/vksdk-inst"; unzip -q -o "$DL/vulkansdk.zip" -d "$DL/vksdk-inst"
  INST="$(ls -d "$DL"/vksdk-inst/*.app | head -1)"
  "$INST/Contents/MacOS/$(basename "$INST" .app)" --root "$T/vulkansdk" --accept-licenses --default-answer --confirm-command install \
     com.lunarg.vulkan.core || "$INST/Contents/MacOS/$(basename "$INST" .app)" --root "$T/vulkansdk" --accept-licenses --default-answer --confirm-command install
fi
# Sources the CMake files expect under tools/_dl (the PC keeps them there; tools/ is not copied from the PC)
if [ ! -d "$SRC/tools/_dl/spirv-cross/SPIRV-Cross-$SPIRVCROSS_TAG" ]; then
  get "https://github.com/KhronosGroup/SPIRV-Cross/archive/refs/tags/$SPIRVCROSS_TAG.tar.gz" "$DL/spirv-cross.tgz"
  mkdir -p "$SRC/tools/_dl/spirv-cross"; tar -xzf "$DL/spirv-cross.tgz" -C "$SRC/tools/_dl/spirv-cross"
fi
if [ ! -d "$SRC/tools/_dl/vulkan/Vulkan-Headers-1.4.309" ]; then
  get "https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/$VKHDR_TAG.tar.gz" "$DL/vkhdr.tgz"
  get "https://github.com/zeux/volk/archive/refs/tags/$VOLK_TAG.tar.gz" "$DL/volk.tgz"
  mkdir -p "$SRC/tools/_dl/vulkan"; tar -xzf "$DL/vkhdr.tgz" -C "$SRC/tools/_dl/vulkan"; tar -xzf "$DL/volk.tgz" -C "$SRC/tools/_dl/vulkan"
fi
# versions
{
  echo "date: $(date)"; echo "macOS: $(sw_vers -productVersion) $(uname -m)"; xcodebuild -version | tr '\n' ' '; echo
  "$T/cmake/CMake.app/Contents/bin/cmake" --version | head -1; echo "ninja $("$T/ninja/ninja" --version)"
  "$T/flutter/bin/flutter" --version --suppress-analytics 2>/dev/null | head -3 || true
  "$T/vulkansdk/macOS/bin/glslc" --version 2>/dev/null | head -2 || true
  xcrun --show-sdk-version; xcrun simctl list runtimes | grep -i ios || true
} | tee "$T/VERSIONS.txt"
