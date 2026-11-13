#!/usr/bin/env bash
# Builds a minimal LGPL-2.1+ FFmpeg for Android (shared libs) inside tools/.
#   tools/ffmpeg/build.sh [arm64-v8a] [x86_64]     (default: both)
# Source: tools/_dl/ffmpeg/ffmpeg-9.0.2 (tarball tools/_dl/ffmpeg-9.0.2.tar.xz, sha256 in docs/legal/ffmpeg-lgpl.md).
# Output: tools/ffmpeg/install/<abi>/{include,lib}; libs are also copied to tools/ffmpeg/jniLibs/<abi> (Gradle jniLibs source dir)
# NOT enabled: --enable-gpl, --enable-nonfree, --enable-version3, any external library.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
T="$(cd "$HERE/.." && pwd)"
VER=9.0.2
SRC="$(cygpath -m "$T")/_dl/ffmpeg/ffmpeg-$VER"
if [ ! -d "$T/_dl/ffmpeg/ffmpeg-$VER" ]; then
  mkdir -p "$T/_dl/ffmpeg"
  mkdir -p "$T/_dl/ffmpeg9"; [ -f "$T/_dl/ffmpeg9/ffmpeg-$VER.tar.xz" ] || curl -L -o "$T/_dl/ffmpeg9/ffmpeg-$VER.tar.xz" "https://ffmpeg.org/releases/ffmpeg-$VER.tar.xz"
  tar -xJf "$T/_dl/ffmpeg9/ffmpeg-$VER.tar.xz" -C "$T/_dl/ffmpeg"
fi
NDK="$T/android-sdk/ndk/28.2.13676358"
TC="$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin"
export PATH="$NDK/prebuilt/windows-x86_64/bin:$TC:$PATH"   # make.exe, yasm.exe, llvm-*
API=29
mkdir -p "$HERE/tmp"; export TMPDIR="$HERE/tmp" TEMP="$HERE/tmp" TMP="$HERE/tmp"
ABIS=("$@"); [ ${#ABIS[@]} -eq 0 ] && ABIS=(arm64-v8a x86_64)

for ABI in "${ABIS[@]}"; do
  case "$ABI" in
    arm64-v8a) ARCH=aarch64; TRIPLE=aarch64-linux-android; EXTRA=(--enable-neon --enable-asm) ;;
    x86_64)    ARCH=x86_64;  TRIPLE=x86_64-linux-android;  EXTRA=(--enable-asm --x86asmexe="$T/_dl/nasm/nasm-3.02/nasm.exe") ;;
    *) echo "bad abi $ABI"; exit 1 ;;
  esac
  B="$HERE/build/$ABI"; I="$HERE/install/$ABI"
  rm -rf "$B" "$I"; mkdir -p "$B" "$I"; cd "$B"
  "$SRC/configure" \
    --prefix="$(cygpath -m "$I")" --enable-cross-compile --target-os=android --arch=$ARCH \
    --host-cc="$T/llvm-mingw-20261020-ucrt-x86_64/bin/clang.exe" --cc="$TC/${TRIPLE}${API}-clang" --cxx="$TC/${TRIPLE}${API}-clang++" \
    --ar="$TC/llvm-ar.exe" --ranlib="$TC/llvm-ranlib.exe" --strip="$TC/llvm-strip.exe" --nm="$TC/llvm-nm.exe" \
    --enable-shared --disable-static --enable-pic \
    --disable-everything --disable-programs --disable-doc --disable-debug --disable-autodetect --disable-network \
    --disable-avdevice --disable-avfilter \
    --enable-avcodec --enable-avformat --enable-avutil --enable-swscale --enable-swresample \
    --enable-jni --enable-mediacodec \
    --enable-decoder=h264,hevc,h264_mediacodec,hevc_mediacodec,pcm_s16be,pcm_s16le,pcm_s24be,pcm_s24le,pcm_s32be,pcm_s32le,pcm_f32be,pcm_f32le \
    --enable-parser=h264,hevc --enable-demuxer=mov \
    --enable-protocol=file,fd \
    --disable-symver --extra-cflags="-O3" "${EXTRA[@]}"
  # Android wants plain lib<name>.so (no .so.N symlink chain): keep the plain name lib<name>.so.
  sed -i \
    -e 's|^SLIBNAME_WITH_MAJOR=.*|SLIBNAME_WITH_MAJOR=$(SLIBNAME)|' \
    -e 's|^SLIB_INSTALL_NAME=.*|SLIB_INSTALL_NAME=$(SLIBNAME_WITH_MAJOR)|' \
    -e 's|^SLIB_INSTALL_LINKS=.*|SLIB_INSTALL_LINKS=|' \
    ffbuild/config.mak
  # NDK make.exe is a native Windows binary: it needs F:/ paths, configure wrote /f/ ones.
  sed -i "s|/f/projects|F:/projects|g" Makefile ffbuild/config.mak
  make -j"$(nproc)"
  # `make install` is unreliable with the Windows make; copy the public headers + libs by hand.
  mkdir -p "$I/lib" "$I/include"
  for L in libavutil libavcodec libavformat libswscale libswresample; do
    mkdir -p "$I/include/$L"
    for h in "$T"/_dl/ffmpeg/ffmpeg-$VER/$L/*.h; do
      case "$(basename "$h")" in *_internal.h|internal.h|*_template.h|hwcontext_*|d3d*|dxva*|vdpau*|videotoolbox*|mediacodec*|qsv*|xvmc*|vaapi*) continue;; esac
      cp "$h" "$I/include/$L/"
    done
    cp "$B/$L"/*.h "$I/include/$L/" 2>/dev/null || true
    cp "$B/$L/$L.so" "$I/lib/"
    mkdir -p "$HERE/jniLibs/$ABI"; cp "$B/$L/$L.so" "$HERE/jniLibs/$ABI/"
  done
  "$TC/llvm-strip.exe" --strip-unneeded "$I"/lib/*.so 2>/dev/null || true
  echo "== $ABI built:"; ls -l "$I"/lib/*.so
done
