// Video decoding for Motion Photo creation: THE one place that decodes video (FFmpeg 9.x libavformat / libavcodec /
// libswscale / libswresample, LGPL, dynamically linked; see docs/legal/ffmpeg-lgpl.md).
//
//  * Decoder choice: hardware first (FFmpeg's h264_mediacodec / hevc_mediacodec, Android MediaCodec through the NDK or
//    JNI) when the caller's probe says the device decodes the stream and the stream is 8-bit 4:2:0 (what FFmpeg's
//    MediaCodec wrapper can return); software (FFmpeg h264 / hevc, any bit depth / chroma) otherwise, and also when the
//    hardware decoder fails to open or to deliver a frame.
//  * Precision: every frame is scaled and colour-converted at >= 10-bit (16-bit RGB or 10-bit YUV); 8-bit output is
//    produced last, with dithering. Colour matrix / range / transfer / primaries are handled here and nowhere else.
//  * Source colour: standard (BT.709 / 601), HLG, PQ, S-Log3 (S-Gamut3.Cine). Sony tags S-Log3 as plain BT.709, so
//    it is detected from the NRT metadata Sony writes into the MP4 (top-level 'meta' box, handler 'nrtm', item
//    CaptureGammaEquation) or from the clip's M01.XML sidecar; the caller can also force a source colour.
//  * Audio: LPCM (Sony records big-endian 16 / 24 bit PCM, 'twos' / 'in24') is decoded to interleaved 16-bit stereo
//    for the AAC encoder.
//
// Pull API: open -> begin(range, size, format) -> next()* -> close.
#pragma once
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DvVideo DvVideo;

enum DvFormat {
  DV_RGBA = 0,     // 8-bit sRGB RGBA, dithered from 16-bit (thumbnails, SDR stills)
  DV_I420 = 1,     // 8-bit 4:2:0 planar, always SDR BT.709 limited (H.264 fallback / compatibility encoder); Y then U then V, tightly packed
  DV_RGBA64 = 2,   // 16-bit little-endian sRGB RGBA, full range (SDR still master, no banding)
  DV_P010 = 3,     // 10-bit 4:2:0, Y plane then interleaved UV plane, 16-bit LE words, value in the top 10 bits; HLG / PQ pass through (BT.2020)
  DV_RGBAF16 = 4   // half-float linear RGBA, BT.709 primaries, 1.0 = SDR reference white, may exceed 1 (HDR sources)
};

enum DvSource { DV_SRC_AUTO = 0, DV_SRC_STANDARD = 1, DV_SRC_SLOG3 = 2, DV_SRC_HLG = 3 };

// How the stream's colour was classified (DvInfo.colorKind).
enum DvColorKind { DV_KIND_STANDARD = 0, DV_KIND_SDR2020 = 1, DV_KIND_HLG = 2, DV_KIND_PQ = 3, DV_KIND_SLOG3 = 4 };

// hwPref: 0 = software only; 1 = FFmpeg's MediaCodec wrapper for 8-bit 4:2:0 hardware decode; 2 = additionally the NDK
// AMediaCodec decoder (nativeDecoder, or the default decoder of the MIME type) for 10-bit 4:2:0, output COLOR_FormatYUVP010
// (FFmpeg's wrapper can only return 8-bit); 3 = the NDK decoder for any 4:2:0 stream (tests, 8-bit goes through NV12).
// Which of these a device may use is decided by the Kotlin codec ladder (declared capability + one-time self-test).
// Any hardware failure continues the pass in FFmpeg software.
typedef struct DvOpenOptions {
  int hwPref;
  int source;                  // DvSource
  const char* nativeDecoder;   // MediaCodec name for hwPref >= 2, or NULL for the default decoder of the stream's MIME type
} DvOpenOptions;

typedef struct DvInfo {
  int width, height;       // coded picture size (before rotation)
  int rotation;            // degrees clockwise to apply for display: 0/90/180/270
  double fps;
  int64_t durationUs;
  int bitDepth;            // luma bits
  int chroma;              // 1 = 4:2:0, 2 = 4:2:2, 3 = 4:4:4, 0 = mono
  int primaries, trc, matrix, range;  // AVCOL_* values as signalled (2 = unspecified), range 1 = limited, 2 = full
  int hdr;                 // 0 SDR, 1 HLG, 2 PQ
  char codec[16];
  int profile;
  // decoder actually in use
  int hardware;            // 1 = MediaCodec decoder
  char decoder[32];        // FFmpeg decoder name, e.g. "h264", "h264_mediacodec"
  // colour classification
  int colorKind;           // DvColorKind used for conversion
  int slogDetected;        // 0 none, 1 NRT metadata inside the MP4, 2 sidecar M01.XML
  char captureGamma[32];   // CaptureGammaEquation as found ("" when there is no Sony metadata)
  char capturePrimaries[32];
  // audio (first audio stream)
  int audioCodecId;        // AVCodecID, 0 = none
  int audioRate, audioChannels, audioBits;
  char audioCodec[16];
  int audioDecodable;      // 1 when the library can decode it (PCM)
} DvInfo;

// url is a file path, or "fd:<n>" for a descriptor the library then owns (closed by dv_close).
DvVideo* dv_open(const char* url, const DvOpenOptions* opts, DvInfo* info, char* err, int errLen);
void dv_close(DvVideo*);

// Starts a decode pass: frames with start - half frame <= pts <= end, in presentation order.
// maxEdge > 0 scales the longer side down to that many pixels (never up). maxFps > 0 drops frames to at most that
// rate. fast = cheaper decode for small thumbnails (loop filter skipped, bilinear scaling).
// startUs == endUs delivers just the frame nearest to that time. Output size is returned in *w / *h (even numbers).
int dv_begin(DvVideo*, int64_t startUs, int64_t endUs, int maxEdge, double maxFps, int format, int fast, int* w, int* h);

// Decoder in use right now (it can change from hardware to software while decoding): 1 = hardware; name receives the
// FFmpeg decoder name.
int dv_decoder(const DvVideo*, char* name, int nameLen);

// Gives up the hardware decoder and continues in software (what happens automatically when it fails). Returns 1 if switched.
int dv_force_software(DvVideo*);

// Bytes per frame for the pass started by dv_begin.
int64_t dv_frame_bytes(const DvVideo*);

// Next frame into dst. Returns its timestamp in us (relative to the stream start), -1 at the end of the range,
// -2 on error.
int64_t dv_next(DvVideo*, uint8_t* dst, int64_t dstSize);

// Colour tags the YUV output of the current pass must carry (AVCOL_* values: primaries, trc, matrix; range 1 = limited).
// SDR and converted sources: BT.709 (1, 1, 1); HLG / PQ passed through: BT.2020 (9, 18 or 16, 9).
void dv_output_tags(const DvVideo*, int* primaries, int* trc, int* matrix, int* range);

// CTA-861.3 style HDR static metadata (25 bytes: type, 3x primaries, white point, max / min luminance, MaxCLL, MaxFALL)
// for MediaFormat.KEY_HDR_STATIC_INFO. Returns 1 when the stream carries mastering / content-light data, else 0.
int dv_hdr_static_info(const DvVideo*, uint8_t out[25]);

// ---- audio -----------------------------------------------------------------------------------------------------
// Decodes [startUs, endUs] of the first audio stream to interleaved signed 16-bit PCM (at most 2 channels, source
// sample rate). Returns 0 and the format, or < 0 when there is no (decodable) audio.
int dv_audio_begin(DvVideo*, int64_t startUs, int64_t endUs, int* rate, int* channels);
// Up to maxFrames sample frames into dst (frames * channels int16). Returns the count, 0 at the end, < 0 on error.
// *ptsUs receives the stream time of the first returned sample frame.
int dv_audio_next(DvVideo*, int16_t* dst, int maxFrames, int64_t* ptsUs);

// ---- stills ----------------------------------------------------------------------------------------------------
// Ultra HDR helper: from a DV_RGBAF16 frame (linear, 1.0 = SDR white) builds the SDR base image (8-bit sRGB RGBA,
// tone mapped and dithered) and a single-channel gain map (w/2 x h/2 rounded up, 8-bit) per the Ultra HDR / Android
// Gainmap model:  recovery = (gain8/255)^(1/gamma) mapped between log2 min and max ratio;
//   hdr_linear = (sdr_linear + epsSdr) * 2^(logMin + recovery * (logMax - logMin)) - epsHdr.
// meta receives: [0] ratioMin, [1] ratioMax (content boost, linear), [2] gamma, [3] epsSdr, [4] epsHdr.
// Returns the gain map size in gw / gh; 0 on success.
int dv_make_ultrahdr(const uint16_t* rgbaF16, int w, int h, uint8_t* sdrRgba, uint8_t* gain, int* gw, int* gh, float meta[5]);

// ---- codec capability report ---------------------------------------------------------------------------------------
// The platform layer (Kotlin: MediaCodecList query + one-time self-tests, see CodecLadder.kt) hands its JSON report to the
// core so it is available through the C API next to the decoder state; the core keeps its own copy.
void dv_set_codec_report(const char* json);
// Copies the last report (empty string when none was set) into dst; returns the full length (excluding the terminator).
int dv_codec_report(char* dst, int dstLen);

// ---- tools / tests -----------------------------------------------------------------------------------------------
const char* dv_version(void);
// S-Log3 code value (0..1) -> scene linear (18 % grey = 0.18), Sony's published formula.
double dv_slog3_to_linear(double v);
// Linear S-Gamut3.Cine -> linear BT.709 matrix (row major, derived from the published primaries).
void dv_sgamut3cine_to_709(double m[9]);
// Display transform used for S-Log3: scene linear BT.709 -> display-referred linear (0..1).
double dv_slog3_tonemap(double linear);
// Reads Sony NRT metadata (path or "fd:" untouched; path only) and the M01.XML sidecar. Returns 0 none, 1 in file, 2 sidecar.
int dv_detect_sony_gamma(const char* path, char* gamma, int gammaLen, char* primaries, int primLen);

#ifdef __cplusplus
}
#endif
