#!/usr/bin/env bash
# ON THE MAC: LLVM OpenMP runtime (libomp, Apache-2.0 with LLVM exception) as a static library for macOS arm64, iOS device and iOS simulator
# (arm64), into ~/dapple/tools/libomp/<macos|ios-device|ios-sim>/{lib/libomp.a,include/omp.h}. Same LLVM major as the NDK's libomp (19).
# Usage: build-libomp.sh   (idempotent; downloads the two source tarballs once into ~/dapple/tools/_dl)
set -euo pipefail
source "$HOME/dapple/src/scripts/ios/env.sh"
V=19.1.7; D="$HOME/dapple/tools/_dl"; R="$HOME/dapple/tools/libomp"; mkdir -p "$D" "$R"
U="https://github.com/llvm/llvm-project/releases/download/llvmorg-$V"
for n in openmp cmake; do
  [ -f "$D/$n-$V.src.tar.xz" ] || curl -fsSL -o "$D/$n-$V.src.tar.xz" "$U/$n-$V.src.tar.xz"
  [ -d "$R/src/$n" ] || { mkdir -p "$R/src/$n"; tar -xJf "$D/$n-$V.src.tar.xz" -C "$R/src/$n" --strip-components=1; }
done
build() {  # name, extra cmake args
  local name="$1"; shift
  [ -f "$R/$name/lib/libomp.a" ] && return 0
  cmake -S "$R/src/openmp" -B "$R/b-$name" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_INSTALL_PREFIX="$R/$name" \
    -DOPENMP_STANDALONE_BUILD=ON -DLIBOMP_ENABLE_SHARED=OFF -DLIBOMP_OMPT_SUPPORT=OFF -DLIBOMP_OMPD_SUPPORT=OFF -DLIBOMP_USE_HWLOC=OFF \
    -DLIBOMP_FORTRAN_MODULES=OFF -DOPENMP_ENABLE_LIBOMPTARGET=OFF -DOPENMP_ENABLE_OMPT_TOOLS=OFF -DOPENMP_ENABLE_TESTS=OFF "$@"
  cmake --build "$R/b-$name" -j8 --target omp
  mkdir -p "$R/$name/lib" "$R/$name/include"
  cp "$(find "$R/b-$name" -name libomp.a | head -1)" "$R/$name/lib/libomp.a"
  cp "$(find "$R/b-$name" -name omp.h | head -1)" "$R/$name/include/omp.h"
}
build macos
build ios-device -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0
build ios-sim -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0
ls -la "$R"/*/lib/libomp.a
