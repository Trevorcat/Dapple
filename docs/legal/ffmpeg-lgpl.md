# FFmpeg (LGPL-2.1-or-later) in Dapple

Dapple uses FFmpeg as the single video decoder of the Motion Photo (动态照片) tool (`core/video`): Android MediaCodec
hardware decoding first (FFmpeg's `h264_mediacodec` / `hevc_mediacodec`), FFmpeg's own software H.264 / HEVC decoders for
what no phone decoder handles correctly (Sony XAVC S / S-I / HS: H.264 High 4:2:2 Intra and 10-bit, HEVC 10-bit 4:2:2),
and the PCM audio decoders for Sony's LPCM soundtrack. It is a small separate build, not the FFmpeg that ships with any
other product. Dapple does not encode with FFmpeg: the clip is encoded with the platform MediaCodec encoders.
For 10-bit 4:2:0 streams on devices whose hardware decoder passed the one-time self-test (docs/engine/codec-capability.md), the core drives the platform decoder directly through the NDK `AMediaCodec` API (output `COLOR_FormatYUVP010`; FFmpeg's MediaCodec wrapper only returns 8-bit) and uses FFmpeg's libswscale for the conversion; that is Dapple code (`core/video/dapple_video.cpp`) calling the platform's `libmediandk`, no FFmpeg code is involved in the decoding itself.

## What is used

| Item | Value |
|---|---|
| Version | FFmpeg 9.0.2 (release 2026-09-18, `ffmpeg-9.0.2.tar.xz`, https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz) |
| SHA-256 of the tarball | `8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e` (computed after download) |
| Signature | `ffmpeg-9.0.2.tar.xz.asc` verified with GnuPG against the FFmpeg release signing key `FCF9 86EA 15E6 E293 A564 4F10 B432 2F04 D676 58D8` (`ffmpeg-devel@ffmpeg.org`, https://ffmpeg.org/ffmpeg-devel.asc): "Good signature", made 2026-09-18. ffmpeg.org publishes no separate `.sha256` file for this release (the URL returns 404), so the signature is the authenticity check and the SHA-256 above is the recorded fingerprint of the verified file. Keyring and tarball: `tools/_dl/ffmpeg9/`. |
| Licence reported by `configure` | `LGPL version 2.1 or later` (`FFMPEG_LICENSE` in config.h); `CONFIG_GPL 0`, `CONFIG_NONFREE 0` |
| Libraries | `libavutil` (61), `libavcodec` (63), `libavformat` (63), `libswscale` (10), `libswresample` (7) as shared libraries |
| ABIs | arm64-v8a (NEON, dotprod, i8mm), x86_64 (emulator / test builds, NASM assembly) |
| NDK | r28 (28.2.13676358), API level 29 |
| Source and build script | `tools/_dl/ffmpeg/ffmpeg-9.0.2` (unmodified), `tools/ffmpeg/build.sh` |
| Dapple code that calls it | `core/video/dapple_video.cpp` (+ JNI `dapple_video_jni.cpp`), `app/android/.../SwVideo.kt`, `SoftwareMedia.kt`, `LiveBridge.kt` |

FFmpeg is not patched. Nothing from FFmpeg's GPL parts is compiled in: the build is configured without
`--enable-gpl`, `--enable-version3` and `--enable-nonfree`, and without any external library (no x264, x265,
fdk-aac, ...). The only dependencies beyond libc are the NDK's own `libmediandk` (MediaCodec) and `libandroid`, which are
platform libraries.

### Configure flags (exact)

```
--enable-cross-compile --target-os=android --arch=<aarch64|x86_64> --cc=<ndk clang, api 29>
--enable-shared --disable-static --enable-pic
--disable-everything --disable-programs --disable-doc --disable-debug --disable-autodetect --disable-network
--disable-avdevice --disable-avfilter
--enable-avcodec --enable-avformat --enable-avutil --enable-swscale --enable-swresample
--enable-jni --enable-mediacodec
--enable-decoder=h264,hevc,h264_mediacodec,hevc_mediacodec,pcm_s16be,pcm_s16le,pcm_s24be,pcm_s24le,pcm_s32be,pcm_s32le,pcm_f32be,pcm_f32le
--enable-parser=h264,hevc --enable-demuxer=mov
--enable-protocol=file,fd --disable-symver --extra-cflags=-O3
--enable-neon --enable-asm                      (arm64-v8a)
--enable-asm --x86asmexe=<NASM 3.02>             (x86_64; FFmpeg 9's x86 assembly needs NASM, YASM is too old)
```

Reproduce with `tools/ffmpeg/build.sh [arm64-v8a] [x86_64]` (Windows host, Git Bash, the NDK's `make`; NASM 3.02 from
https://www.nasm.us unpacked under `tools/_dl/nasm`, BSD-2-Clause, build tool only, not shipped). The only post-configure
step is renaming the shared objects to plain `lib<name>.so` (no `.so.N` chain), which Android packaging requires.

### MediaCodec

`--enable-jni --enable-mediacodec` adds FFmpeg's Android MediaCodec decoders. In the app `libdapple_video.so` registers the
JavaVM (`av_jni_set_java_vm` in `JNI_OnLoad`) so FFmpeg picks the codec by profile through the platform's `MediaCodecList`;
without a JavaVM (native test executables) FFmpeg uses the NDK `AMediaCodec` API. Its buffer-mode output is 8-bit NV12 /
I420 only, so Dapple's core asks the hardware decoder for 8-bit 4:2:0 H.264 / HEVC and uses the software decoders for
everything else (10-bit, 4:2:2) and whenever the hardware decoder fails to open or deliver a frame.

## Maintenance rule

Always track the **latest point release of the newest maintained FFmpeg branch** (today 9.0.x; move to 9.1 / the next
branch when it is released and has had its first point release). FFmpeg parses and decodes untrusted files (video from
cameras, other people's phones, downloads), and security fixes for the demuxers and decoders land in the point releases.
Every release of Dapple therefore:

1. checks https://ffmpeg.org/releases/ for a newer point release of the newest branch (and a new branch),
2. downloads the `.tar.xz` and its `.asc`, verifies the signature against the release signing key above (and records the
   SHA-256 here),
3. rebuilds with `tools/ffmpeg/build.sh`, bumps the version in this file, in the in-app notice
   (`app/lib/screens/library/licenses_page.dart`) and in `tools/ffmpeg/build.sh`, and re-runs `tools/ffmpeg/test-android.sh`.

Do not stay on an old branch for convenience, and never carry local patches.

## How the LGPL is satisfied

1. **Dynamic linking.** FFmpeg is never linked statically into `libdapple.so` or `libdapple_video.so`. The five
   `libav*.so` / `libsw*.so` files are shipped as separate files in the APK's `lib/<abi>/` directory and are
   loaded by the Android dynamic linker. A user can therefore replace them with their own build of FFmpeg (same
   major versions: avutil 61, avcodec 63, avformat 63, swscale 10, swresample 7) and re-sign / repackage the app, which is
   the relinking right LGPL section 6 requires.
2. **Source.** The complete corresponding source is the unmodified upstream tarball above plus the configure flags
   in this file and `tools/ffmpeg/build.sh`. On request (and in the repository under `tools/ffmpeg/` and
   `docs/legal/`) Dapple provides these, for at least three years after the release that contains them.
3. **Notice.** The licences screen (设置 > 开源许可) shows the notice text below, with a link to the licence.
   The store listing / website "About" page must carry the same text.
4. **No restriction on reverse engineering** for debugging modifications of the library is imposed by Dapple's
   terms of use for the FFmpeg part.
5. **Licence text.** The full LGPL-2.1 text is `tools/_dl/ffmpeg/ffmpeg-9.0.2/COPYING.LGPLv2.1`; a copy must be
   included in the distribution (iOS and desktop builds too, if FFmpeg is ever used there; on iOS static
   linking is not allowed by the App Store terms in combination with the LGPL, so use dynamic frameworks or a
   different decoder there).

## Notice text (shown in the app)

> This software uses libraries from the FFmpeg project (https://ffmpeg.org) under the GNU Lesser General Public
> License version 2.1 or later: libavutil, libavcodec, libavformat, libswscale and libswresample, version 9.0.2, built
> with only the H.264 / HEVC decoders (software and Android MediaCodec), the PCM audio decoders, the MP4/MOV demuxer, the
> scaler and the resampler, and used as separate shared libraries that can be replaced. FFmpeg is a trademark of Fabrice
> Bellard, originator of the FFmpeg project. The source code of the version used, the build configuration and the build
> script are available from the Dapple project on request.
> A copy of the licence: https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html

## Ultra HDR / gain maps

The Ultra HDR still (SDR JPEG + gain map) is written by the Android platform (`Bitmap.compress` of a bitmap with a
`Gainmap`, Android 14+); the gain map itself is computed in `core/video` (`dv_make_ultrahdr`). No Google libultrahdr
(Apache-2.0) is bundled. The S-Log3 / S-Gamut3.Cine conversion uses Sony's published formulas ("Technical Summary for
S-Gamut3.Cine / S-Log3 and S-Gamut3"); the matrix is derived from the published primaries in code and checked against
the published constants in `tests/video/dapple_video_test.cpp`.

## Patents

H.264 / HEVC decoding is covered by third-party patent pools in some jurisdictions. The LGPL grants no patent
licence. Decoding the user's own camera files on their own device is the intended use; check the distribution
regions before publishing. Hardware decode and encode go through the platform's own codecs.

## Test data

`tests/data/video/*.mp4` were produced with a desktop FFmpeg 9.0.2 build (GPL, with libx264/libx265) kept in
`tools/_dl/ffmpeg-desktop`. That build is a test-data generator only; it is not part of the app and nothing from it is
shipped. Regenerate with `tools/ffmpeg/make-fixtures.sh`.
