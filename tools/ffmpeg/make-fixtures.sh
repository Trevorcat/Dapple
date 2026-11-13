#!/usr/bin/env bash
# Regenerates tests/data/video/*.mp4 with the desktop ffmpeg under tools/_dl/ffmpeg-desktop (GPL build, test data only,
# never shipped).
set -e
R="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
E="$(ls "$R"/tools/_dl/ffmpeg-desktop/ffmpeg-*/bin/ffmpeg.exe | head -1)"
cd "$R/tests/data/video"
SRC="color=c=0xc82828:s=64x48:r=25:d=0.6,drawbox=x=32:y=0:w=32:h=48:color=0x2850c8:t=fill"
CONV="scale=out_color_matrix=bt709:out_range=tv"
TAG="-colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv"
"$E" -v error -y -f lavfi -i "$SRC" -vf "$CONV,format=yuv422p10le" -c:v libx264 -profile:v high422 -x264-params keyint=1 -crf 12 $TAG -movflags +faststart sdr_h264_422_10.mp4
"$E" -v error -y -f lavfi -i "$SRC" -vf "$CONV,format=yuv422p10le" -c:v libx265 -x265-params "keyint=1:log-level=error" -crf 12 -tag:v hvc1 $TAG sdr_hevc_422_10.mp4
"$E" -v error -y -f lavfi -i "$SRC" -vf "$CONV,format=yuv420p" -c:v libx264 -profile:v high -x264-params keyint=25 -crf 12 $TAG sdr_h264_420_8.mp4
"$E" -v error -y -f lavfi -i "color=c=gray:s=64x48:r=25:d=0.6" -vf "scale=out_color_matrix=bt2020nc:out_range=tv,format=yuv420p10le" -c:v libx265 -x265-params "keyint=1:log-level=error:colorprim=bt2020:transfer=arib-std-b67:colormatrix=bt2020nc:range=limited" -crf 10 -tag:v hvc1 hlg_hevc_420_10.mp4

# ---- v2 fixtures (FFmpeg 9.0.2 core): precision, S-Log3, audio, hardware controls -------------------------------------
TAGHD="-colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv"
# 10-bit horizontal luma ramp: Y' code 64..940 across 1024 px (876 distinct 10-bit levels), neutral chroma. A decoder that
# truncates to 8 bit leaves at most 220 distinct levels in the video range.
"$E" -v error -y -f lavfi -i "nullsrc=s=1024x16:r=25:d=0.4,format=yuv422p10le,geq=lum='64+X*876/1023':cb=512:cr=512" \
  -c:v libx264 -profile:v high422 -x264-params keyint=1 -crf 6 $TAGHD ramp_h264_422_10.mp4
# S-Log3 patches (Sony tags S-Log3 as plain BT.709): raw luma codes 95 (black), 420 (18 % grey), 598 (90 % white), 300, 700
"$E" -v error -y -f lavfi -i "nullsrc=s=320x48:r=25:d=0.4,format=yuv422p10le,geq=lum='if(lt(X\,64)\,95\,if(lt(X\,128)\,420\,if(lt(X\,192)\,598\,if(lt(X\,256)\,300\,700))))':cb=512:cr=512" \
  -c:v libx264 -profile:v high422 -x264-params keyint=1 -crf 6 $TAGHD slog3_h264_422_10_src.mp4
python -I "$R/tools/ffmpeg/add_nrt_meta.py" slog3_h264_422_10_src.mp4 slog3_h264_422_10.mp4 s-log3-cine s-gamut3-cine
python -I "$R/tools/ffmpeg/add_nrt_meta.py" sdr_h264_422_10.mp4 sdr_h264_422_10_rec709.mp4 rec709 rec709
rm -f slog3_h264_422_10_src.mp4
# 8-bit 4:2:0 at a size platform decoders accept (hardware positive control) + HEVC Main
"$E" -v error -y -f lavfi -i "testsrc2=s=640x360:r=25:d=0.6,format=yuv420p" -c:v libx264 -profile:v high -x264-params keyint=25 -crf 18 $TAGHD hw_h264_420_8.mp4
"$E" -v error -y -f lavfi -i "testsrc2=s=640x360:r=25:d=0.6,format=yuv420p" -c:v libx265 -x265-params "keyint=25:log-level=error" -crf 20 -tag:v hvc1 $TAGHD hw_hevc_420_8.mp4
# audio: Sony-style big-endian 16-bit LPCM ('twos') next to 8-bit H.264; 440 Hz left, 880 Hz right, 48 kHz
"$E" -v error -y -f lavfi -i "testsrc2=s=320x180:r=25:d=1.0,format=yuv420p" \
  -f lavfi -i "sine=f=440:r=48000:d=1.0" -f lavfi -i "sine=f=880:r=48000:d=1.0" -filter_complex "[1:a][2:a]amerge=inputs=2[a]" -map 0:v -map "[a]" \
  -c:v libx264 -x264-params keyint=25 -crf 20 $TAGHD -c:a pcm_s16be -f mov lpcm_h264_420_8.mp4
# synthetic Sony-like 4K30 10-bit 4:2:2 intra (benchmark only, not committed: written next to the build output)
mkdir -p "$R/build/bench"
"$E" -v error -y -f lavfi -i "testsrc2=s=3840x2160:r=30:d=4,format=yuv422p10le" -c:v libx264 -profile:v high422 -x264-params keyint=1 -b:v 150M -maxrate 200M -bufsize 100M $TAGHD \
  "$R/build/bench/synthetic_4k30_422_10.mp4"
python -I "$R/tools/ffmpeg/make_ultrahdr_fixture.py" "$E" "$R/tests/data/video/ultrahdr_cover.jpg"
# 10-bit gradient (Y 64..940 across 640 px), 3 frames: the hardware 10-bit decode self-test asset (app/android/app/src/main/assets/selftest_main10.mp4)
"$E" -v error -y -f lavfi -i "color=c=black:s=640x352:r=30,format=yuv420p10le,geq=lum='64+876*X/639':cb=512:cr=512" -frames:v 3 -c:v libx265 -pix_fmt yuv420p10le -x265-params "keyint=3:log-level=error:colorprim=bt709:transfer=bt709:colormatrix=bt709:range=limited" -crf 12 -tag:v hvc1 gradient_hevc_main10.mp4
# wave 8: HDR luma ramps (Y' 64..940 across 640 px, neutral chroma, BT.2020 NCL): the HDR output policy tests of VideoFrameSession
for T in "hlg arib-std-b67" "pq smpte2084"; do set -- $T
  "$E" -v error -y -f lavfi -i "color=c=black:s=640x352:r=25,format=yuv420p10le,geq=lum='64+876*X/639':cb=512:cr=512" -frames:v 5 -c:v libx265 -pix_fmt yuv420p10le \
    -x265-params "keyint=5:log-level=error:colorprim=bt2020:transfer=$2:colormatrix=bt2020nc:range=limited" -crf 10 -tag:v hvc1 "$R/tests/data/video/${1}_ramp_hevc_420_10.mp4"
done
