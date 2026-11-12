#!/usr/bin/env bash
# usage: build_probe.sh <cmake build dir with core/libdapple_core.a> <source root that dir was built from>  -> <build dir>/bin/rawprobe.exe
set -e
R="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"; B="$(cd "$1" && pwd)"; S="$(cd "$2" && pwd)"
source "$R/tools/env.sh"
LR=$(grep -E '^FETCHCONTENT_SOURCE_DIR_LIBRAW' "$B/CMakeCache.txt" | cut -d= -f2); [ -d "$LR" ] || LR="$B/_deps/libraw-src"
clang++ -O2 -std=c++23 -DLIBRAW_NODLL -DDAPPLE_FILM_HAVE_AVX2=1 -I"$S/core/src" -I"$S/core/include" -I"$S/core" -I"$LR" "$R/scripts/samples/rawprobe.cpp" \
  -static -o "$B/bin/rawprobe.exe" "$B/core/libdapple_core.a" "$B/core/libdapple.dll.a" "$B/libdapple_libraw.a" -lws2_32 -lole32 -luuid -ladvapi32 -lshell32
