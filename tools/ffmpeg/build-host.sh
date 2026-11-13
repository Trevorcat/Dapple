#!/usr/bin/env bash
# Builds the same minimal LGPL-2.1+ FFmpeg as build.sh, for the Windows desktop (llvm-mingw, shared DLLs), for `dapple motion`
# and the desktop video tests. Output: tools/ffmpeg/install/windows-x64/{include,lib,bin}. Encoders are test-only intermediates (ffv1, prores_ks is not
# enabled): the production encoder is the platform one (MediaCodec) chosen by the Kotlin codec ladder.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
T="$(cd "$HERE/.." && pwd)"
VER=9.0.2
SRC="$(cygpath -m "$T")/_dl/ffmpeg/ffmpeg-$VER"
LLVM="$T/llvm-mingw-20261020-ucrt-x86_64/bin"
NDK="$T/android-sdk/ndk/28.2.13676358"
export PATH="$NDK/prebuilt/windows-x86_64/bin:$LLVM:$PATH"
mkdir -p "$HERE/tmp"; export TMPDIR="$HERE/tmp" TEMP="$HERE/tmp" TMP="$HERE/tmp"
B="$HERE/build/windows-x64"; I="$HERE/install/windows-x64"
rm -rf "$B" "$I"; mkdir -p "$B" "$I"; cd "$B"
"$SRC/configure" \
  --prefix="$(cygpath -m "$I")" --target-os=mingw32 --arch=x86_64 \
  --cc="$LLVM/clang.exe" --cxx="$LLVM/clang++.exe" --ar="$LLVM/llvm-ar.exe" --ranlib="$LLVM/llvm-ranlib.exe" --nm="$LLVM/llvm-nm.exe" --strip="$LLVM/llvm-strip.exe" \
  --enable-shared --disable-static \
  --disable-everything --disable-programs --disable-doc --disable-debug --disable-autodetect --disable-network \
  --disable-avdevice --disable-avfilter \
  --enable-avcodec --enable-avformat --enable-avutil --enable-swscale --enable-swresample \
  --enable-decoder=h264,hevc,ffv1,rawvideo,pcm_s16be,pcm_s16le,pcm_s24be,pcm_s24le,pcm_s32be,pcm_s32le,pcm_f32be,pcm_f32le \
  --enable-encoder=ffv1,rawvideo --enable-muxer=mov,matroska --enable-parser=h264,hevc --enable-demuxer=mov,matroska \
  --enable-protocol=file --x86asmexe="$T/_dl/nasm/nasm-3.02/nasm.exe" --extra-cflags="-O3"
# configure records /f/... (MSYS) paths, which the native make cannot read: rewrite them to F:/...
sed -i "s#/${T:1:1}/#${T:1:1}:/#g" Makefile ffbuild/config.mak ffbuild/config.sh
make -j4
mkdir -p "$I/lib" "$I/include" "$I/bin"
for L in libavutil libavcodec libavformat libswscale libswresample; do
  mkdir -p "$I/include/$L"
  for h in "$T"/_dl/ffmpeg/ffmpeg-$VER/$L/*.h; do
    case "$(basename "$h")" in *_internal.h|internal.h|*_template.h|hwcontext_*|d3d*|dxva*|vdpau*|videotoolbox*|mediacodec*|qsv*|xvmc*|vaapi*) continue;; esac
    cp "$h" "$I/include/$L/"
  done
  cp "$B/$L"/*.h "$I/include/$L/" 2>/dev/null || true
  cp "$B/$L"/*.dll "$I/bin/"; cp "$B/$L"/*.lib "$I/lib/" 2>/dev/null || cp "$B/$L"/*.dll.a "$I/lib/" 2>/dev/null || true
done
ls -l "$I"/bin "$I"/lib
