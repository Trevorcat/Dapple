#include "dapple_video.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
// Desktop test build (llvm-mingw): pread / O_BINARY shims. The file position is restored; the desktop decoder is single threaded per clip.
#include <io.h>
static ssize_t pread(int fd, void* buf, size_t n, off_t off) {
  const long long cur = _lseeki64(fd, 0, SEEK_CUR);
  if (_lseeki64(fd, off, SEEK_SET) < 0) return -1;
  const int r = _read(fd, buf, (unsigned)n);
  _lseeki64(fd, cur, SEEK_SET);
  return r;
}
#define DV_O_BINARY O_BINARY
#else
#define DV_O_BINARY 0
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef __ANDROID__
#include <android/log.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#define DV_LOG(...) __android_log_print(ANDROID_LOG_WARN, "DappleVideo", __VA_ARGS__)
#else
#define DV_LOG(...) std::fprintf(stderr, __VA_ARGS__)
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace {

// ---- colour model -------------------------------------------------------------------------------------------------
// Everything between the decoder and the final quantisation runs at >= 10 bits: frames are scaled / converted to
// 16-bit RGB (RGB48LE) or kept as 10-bit YUV (P010). Output sRGB / BT.709 unless the stream is HLG / PQ and the
// caller asks for YUV, in which case the HDR signal passes through untouched (BT.2020 + its own transfer).
enum Kind { kStd = DV_KIND_STANDARD, kSdr2020 = DV_KIND_SDR2020, kHlg = DV_KIND_HLG, kPq = DV_KIND_PQ, kSlog3 = DV_KIND_SLOG3 };

constexpr int kOutLut = 16384;
constexpr float kXMax = 8.0f;

float hlgInv(float e) {  // ARIB STD-B67 inverse OETF, scene linear, 75 % signal (203 nit) -> 1.0, OOTF ignored
  const float a = 0.17883277f, b = 0.28466892f, c = 0.55991073f;
  float lin = e <= 0.5f ? e * e / 3.0f : (std::exp((e - c) / a) + b) / 12.0f;
  return lin / 0.2650f;
}
float pqInv(float e) {  // SMPTE ST 2084 EOTF in nits / 203
  const double m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
  double p = std::pow((double)e, 1.0 / m2);
  double n = std::max(p - c1, 0.0) / (c2 - c3 * p);
  return (float)(std::pow(n, 1.0 / m1) * 10000.0 / 203.0);
}
float softKnee(float x) {
  const float k = 0.7f;
  if (x <= k) return x;
  return k + (1.0f - k) * (1.0f - std::exp(-(x - k) / (1.0f - k)));
}
float srgbOetf(float v) {
  v = std::min(std::max(v, 0.0f), 1.0f);
  return v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}
float srgbEotf(float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); }

// S-Log3 (Sony, "Technical Summary for S-Gamut3.Cine / S-Log3 and S-Gamut3"): x = code value / 1023 (10-bit code values
// as recorded; for a limited-range file that means the raw code, not a range-expanded one).
double slog3ToLinear(double x) {
  if (x >= 171.2102946929 / 1023.0) return std::pow(10.0, (x * 1023.0 - 420.0) / 261.5) * (0.18 + 0.01) - 0.01;
  return (x * 1023.0 - 95.0) * 0.01125 / (171.2102946929 - 95.0);
}

// Linear RGB -> XYZ from primaries + D65 white (standard derivation).
void rgbToXyz(const double p[6], const double w[2], double m[9]) {
  double xr = p[0], yr = p[1], xg = p[2], yg = p[3], xb = p[4], yb = p[5];
  double X[3] = {xr / yr, xg / yg, xb / yb}, Z[3] = {(1 - xr - yr) / yr, (1 - xg - yg) / yg, (1 - xb - yb) / yb};
  double Xw = w[0] / w[1], Zw = (1 - w[0] - w[1]) / w[1];
  // solve [X;1;Z] columns * S = white
  double a[9] = {X[0], X[1], X[2], 1, 1, 1, Z[0], Z[1], Z[2]};
  double det = a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6]) + a[2] * (a[3] * a[7] - a[4] * a[6]);
  double wv[3] = {Xw, 1.0, Zw}, S[3];
  for (int c = 0; c < 3; c++) {
    double t[9];
    std::memcpy(t, a, sizeof t);
    for (int r = 0; r < 3; r++) t[r * 3 + c] = wv[r];
    S[c] = (t[0] * (t[4] * t[8] - t[5] * t[7]) - t[1] * (t[3] * t[8] - t[5] * t[6]) + t[2] * (t[3] * t[7] - t[4] * t[6])) / det;
  }
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < 3; c++) m[r * 3 + c] = a[r * 3 + c] * S[c];
}
void inv3(const double a[9], double o[9]) {
  double det = a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6]) + a[2] * (a[3] * a[7] - a[4] * a[6]);
  o[0] = (a[4] * a[8] - a[5] * a[7]) / det; o[1] = (a[2] * a[7] - a[1] * a[8]) / det; o[2] = (a[1] * a[5] - a[2] * a[4]) / det;
  o[3] = (a[5] * a[6] - a[3] * a[8]) / det; o[4] = (a[0] * a[8] - a[2] * a[6]) / det; o[5] = (a[2] * a[3] - a[0] * a[5]) / det;
  o[6] = (a[3] * a[7] - a[4] * a[6]) / det; o[7] = (a[1] * a[6] - a[0] * a[7]) / det; o[8] = (a[0] * a[4] - a[1] * a[3]) / det;
}
void mul3(const double a[9], const double b[9], double o[9]) {
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < 3; c++) o[r * 3 + c] = a[r * 3 + 0] * b[0 * 3 + c] + a[r * 3 + 1] * b[1 * 3 + c] + a[r * 3 + 2] * b[2 * 3 + c];
}
// Published primaries (CIE xy), white D65 (0.3127, 0.3290).
const double kD65[2] = {0.3127, 0.3290};
const double kP709[6] = {0.640, 0.330, 0.300, 0.600, 0.150, 0.060};
const double kP2020[6] = {0.708, 0.292, 0.170, 0.797, 0.131, 0.046};
const double kSGamut3Cine[6] = {0.766, 0.275, 0.225, 0.800, 0.089, -0.087};
void gamutTo709(const double src[6], double m[9]) {
  double a[9], b[9], bi[9];
  rgbToXyz(src, kD65, a);
  rgbToXyz(kP709, kD65, b);
  inv3(b, bi);
  mul3(bi, a, m);
}

// Display transform for scene-linear S-Log3 footage (18 % grey = 0.18, BT.709 primaries): exposure 0.8, then the
// Narkowicz ACES filmic fit (grey 0.18 -> sRGB 0.49, 90 % white -> 0.87, smooth shoulder, slight toe contrast).
double slogTonemap(double x) {
  x = std::max(x, 0.0) * 0.8;
  double y = (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14);
  return std::min(std::max(y, 0.0), 1.0);
}

uint16_t toHalf(float f) {
  union { float f; uint32_t u; } v{f};
  uint32_t s = (v.u >> 16) & 0x8000u;
  int32_t e = (int32_t)((v.u >> 23) & 0xff) - 127 + 15;
  uint32_t m = v.u & 0x7fffffu;
  if (e <= 0) {
    if (e < -10) return (uint16_t)s;
    m |= 0x800000u;
    uint32_t t = 14 - e;
    uint32_t r = (m >> t) + ((m >> (t - 1)) & 1u);
    return (uint16_t)(s | r);
  }
  if (e >= 31) return (uint16_t)(s | 0x7bffu);
  uint32_t r = s | ((uint32_t)e << 10) | (m >> 13);
  if (m & 0x1000u) r++;
  return (uint16_t)r;
}

const uint8_t kBayer8[64] = {0,  32, 8,  40, 2,  34, 10, 42, 48, 16, 56, 24, 50, 18, 58, 26, 12, 44, 4,  36, 14, 46, 6,  38, 60, 28,
                             52, 20, 62, 30, 54, 22, 3,  35, 11, 43, 1,  33, 9,  41, 51, 19, 59, 27, 49, 17, 57, 25, 15, 47, 7,  39,
                             13, 45, 5,  37, 63, 31, 55, 23, 61, 29, 53, 21};
inline uint8_t quant8(uint16_t v, int x, int y) {
  // 16-bit -> 8-bit with an 8x8 ordered dither: no banding in smooth gradients, exact for flat colours.
  float t = (kBayer8[((y & 7) << 3) | (x & 7)] + 0.5f) / 64.0f;
  int q = (int)(v * (255.0f / 65535.0f) + t);
  return (uint8_t)(q > 255 ? 255 : q);
}

struct Tables {
  std::vector<float> in;   // 65536: signal -> linear (source primaries); empty for Standard
  std::vector<float> out;  // kOutLut + 2: linear display -> sRGB-encoded 0..1
  double mat[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  bool gamut = false;
  Kind kind = kStd;
  void build(Kind k, bool bt2020Primaries, bool limited, int bits) {
    kind = k;
    gamut = false;
    in.clear();
    if (k != kStd) {
      in.resize(65536);
      for (int i = 0; i < 65536; i++) {
        float e = i / 65535.0f;
        switch (k) {
          case kHlg: in[i] = hlgInv(e); break;
          case kPq: in[i] = pqInv(e); break;
          case kSdr2020: in[i] = std::pow(e, 2.4f); break;
          default: {  // S-Log3
            double off = (double)(16 << (bits - 8)), span = (double)(219 << (bits - 8)), mx = (double)((1 << bits) - 1);
            double x = limited ? (off + span * e) / mx : e;
            in[i] = (float)slog3ToLinear(x);
          }
        }
      }
    }
    if (k == kSlog3) {
      gamutTo709(kSGamut3Cine, mat);
      gamut = true;
    } else if (bt2020Primaries) {
      gamutTo709(kP2020, mat);
      gamut = true;
    }
    out.resize(kOutLut + 2);
    for (int i = 0; i < kOutLut + 2; i++) {
      float x = (float)i * kXMax / kOutLut;
      float d;
      switch (k) {
        case kSdr2020: d = std::min(x, 1.0f); break;
        case kSlog3: d = (float)slogTonemap(x); break;
        default: d = softKnee(x);
      }
      out[i] = srgbOetf(d);
    }
  }
  inline float oetf(float x) const {
    if (!(x > 0)) return 0;
    if (x >= kXMax) x = kXMax - 1e-4f;
    float p = x * (kOutLut / kXMax);
    int i = (int)p;
    float f = p - i;
    return out[i] + (out[i + 1] - out[i]) * f;
  }
};

int swsCs(int cs, int h) {
  switch (cs) {
    case AVCOL_SPC_BT709: return SWS_CS_ITU709;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M: return SWS_CS_ITU601;
    case AVCOL_SPC_SMPTE240M: return SWS_CS_SMPTE240M;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL: return SWS_CS_BT2020;
    default: return h >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601;  // unspecified: HD is 709, SD is 601
  }
}

// ---- Sony metadata ---------------------------------------------------------------------------------------------------
bool readAt(int fd, int64_t off, void* dst, size_t n) {
  size_t got = 0;
  while (got < n) {
    ssize_t r = pread(fd, (char*)dst + got, n - got, (off_t)(off + (int64_t)got));
    if (r <= 0) return false;
    got += (size_t)r;
  }
  return true;
}
std::string itemValue(const std::string& x, const char* name) {
  std::string key = std::string("name=\"") + name + "\"";
  size_t a = x.find(key);
  if (a == std::string::npos) return {};
  size_t e = x.find('>', a);
  size_t v = x.find("value=\"", a);
  if (v == std::string::npos || (e != std::string::npos && v > e)) return {};
  v += 7;
  size_t q = x.find('"', v);
  return q == std::string::npos ? std::string() : x.substr(v, q - v);
}
// NRT XML in the MP4: a top-level 'meta' box (handler 'nrtm') after / before 'moov'; some files carry it in moov/udta.
int nrtFromFd(int fd, std::string& gamma, std::string& prim) {
  // never lseek here: a dup of the descriptor FFmpeg reads shares its file offset; pread + fstat leave it alone
  struct stat sb;
  if (fstat(fd, &sb) != 0) return 0;
  const off_t size = sb.st_size;
  if (size < 16) return 0;
  int64_t pos = 0;
  int guard = 0;
  while (pos + 8 <= size && guard++ < 4096) {
    uint8_t h[16];
    if (!readAt(fd, pos, h, 8)) break;
    uint64_t sz = ((uint32_t)h[0] << 24) | (h[1] << 16) | (h[2] << 8) | h[3];
    char t[5] = {(char)h[4], (char)h[5], (char)h[6], (char)h[7], 0};
    uint64_t hdr = 8;
    if (sz == 1) {
      if (!readAt(fd, pos + 8, h + 8, 8)) break;
      sz = 0;
      for (int i = 8; i < 16; i++) sz = (sz << 8) | h[i];
      hdr = 16;
    } else if (sz == 0) {
      sz = (uint64_t)(size - pos);
    }
    if (sz < hdr) break;
    bool want = !std::strcmp(t, "meta") || !std::strcmp(t, "moov") || !std::strcmp(t, "uuid");
    if (want && sz <= (8u << 20)) {
      std::string body(sz - hdr, '\0');
      if (readAt(fd, pos + (int64_t)hdr, &body[0], body.size())) {
        size_t a = body.find("CaptureGammaEquation");
        if (a != std::string::npos) {
          // take the XML from its start so itemValue() sees whole tags
          size_t s = body.rfind("<Item", a);
          std::string x = body.substr(s == std::string::npos ? a : s);
          gamma = itemValue(x, "CaptureGammaEquation");
          prim = itemValue(x, "CaptureColorPrimaries");
          if (!gamma.empty()) return 1;
        }
      }
    }
    pos += (int64_t)sz;
  }
  return 0;
}
int sidecar(const std::string& path, std::string& gamma, std::string& prim) {
  size_t slash = path.find_last_of("/\\");
  size_t dot = path.find_last_of('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return 0;
  std::string stem = path.substr(0, dot);
  for (const char* suf : {"M01.XML", "M01.xml", "m01.xml"}) {
    FILE* f = std::fopen((stem + suf).c_str(), "rb");
    if (!f) continue;
    std::string x;
    char b[4096];
    size_t n;
    while ((n = std::fread(b, 1, sizeof b, f)) > 0 && x.size() < (4u << 20)) x.append(b, n);
    std::fclose(f);
    gamma = itemValue(x, "CaptureGammaEquation");
    prim = itemValue(x, "CaptureColorPrimaries");
    if (!gamma.empty()) return 2;
  }
  return 0;
}
bool isSlog3(const std::string& g) {
  std::string l = g;
  for (auto& c : l) c = (char)std::tolower((unsigned char)c);
  return l.find("s-log3") != std::string::npos || l.find("slog3") != std::string::npos;
}

}  // namespace

struct PreadIo {
  int fd = -1;
  int64_t pos = 0, size = 0;
};

struct DvVideo {
  AVFormatContext* fmt = nullptr;
  AVCodecContext* dec = nullptr;
  AVStream* st = nullptr;
  int vs = -1;
  AVPacket* pkt = nullptr;
  bool pktPending = false;
  AVFrame* frm = nullptr;
  DvInfo info{};
  DvOpenOptions opts{};
  int64_t startPts = 0;  // stream start in time_base units
  int threads = 1;
  bool hw = false;
  int hwFrames = 0;
  int idle = 0;
  std::string url;
  int audioFd = -1;
  // NDK AMediaCodec decoder (10-bit 4:2:0 -> P010); v->dec is null while it is active
  bool nat = false;
#ifdef __ANDROID__
  AMediaCodec* nmc = nullptr;
#endif
  int natNalLen = 4, natHeld = -1, natFmt = 0, natW = 0, natH = 0, natStride = 0, natSlice = 0, natCropL = 0, natCropT = 0;
  bool natFmtKnown = false, natEos = false;
  std::vector<uint8_t> natIn;
  int srcFd = -1;  // dup of the source descriptor, for the sidecar-less NRT scan and the audio demuxer

  // pass state
  int64_t from = 0, to = 0, lastOut = -1, lastPts = -1;
  double minGap = 0, frameUs = 33333, nextTick = 0;
  int outFmt = DV_RGBA, ow = 0, oh = 0, fast = 0;
  bool single = false, eofSent = false, done = true;

  SwsContext* sws = nullptr;   // decoded -> final YUV (pass-through) or RGB48
  SwsContext* sws2 = nullptr;  // RGB path: RGB48 -> YUV
  int swsKey[12] = {0};
  Kind kind = kStd;
  bool passthrough = false;
  int tagPri = 1, tagTrc = 1, tagMat = 1;
  Tables tables;
  std::vector<uint16_t> rgb48, rgb16;

  // audio
  AVFormatContext* afmt = nullptr;
  void* aio = nullptr;  // PreadIo of the audio demuxer
  AVCodecContext* adec = nullptr;
  int as = -1;
  SwrContext* swr = nullptr;
  AVPacket* apkt = nullptr;
  AVFrame* afrm = nullptr;
  int arate = 0, ach = 0;
  int64_t aFrom = 0, aTo = 0, aPos = -1;
  bool aEof = false;
  std::vector<int16_t> aq;  // queued interleaved samples
  int64_t aqPts = 0;
};

static void setErr(char* err, int n, const std::string& s) {
  if (err && n > 0) std::snprintf(err, (size_t)n, "%s", s.c_str());
}
static std::string av(int r) {
  char b[128];
  av_strerror(r, b, sizeof b);
  return b;
}

extern "C" const char* dv_version(void) { return av_version_info(); }
extern "C" double dv_slog3_to_linear(double v) { return slog3ToLinear(v); }
extern "C" void dv_sgamut3cine_to_709(double m[9]) { gamutTo709(kSGamut3Cine, m); }
extern "C" double dv_slog3_tonemap(double x) { return slogTonemap(x); }

extern "C" int dv_detect_sony_gamma(const char* path, char* gamma, int gammaLen, char* primaries, int primLen) {
  std::string g, p;
  int r = 0;
  int fd = open(path, O_RDONLY | DV_O_BINARY);
  if (fd >= 0) {
    r = nrtFromFd(fd, g, p);
    close(fd);
  }
  if (!r) r = sidecar(path, g, p);
  if (gamma && gammaLen > 0) std::snprintf(gamma, (size_t)gammaLen, "%s", g.c_str());
  if (primaries && primLen > 0) std::snprintf(primaries, (size_t)primLen, "%s", p.c_str());
  return r;
}

static bool openSoftware(DvVideo* v, std::string* err) {
  const AVCodec* codec = avcodec_find_decoder(v->st->codecpar->codec_id);
  if (!codec) { if (err) *err = std::string("no decoder for ") + avcodec_get_name(v->st->codecpar->codec_id); return false; }
  if (v->dec) avcodec_free_context(&v->dec);
  v->dec = avcodec_alloc_context3(codec);
  avcodec_parameters_to_context(v->dec, v->st->codecpar);
  unsigned hwc = std::thread::hardware_concurrency();
  v->threads = (int)std::min(std::max(hwc, 1u), 8u);
  v->dec->thread_count = v->threads;
  v->dec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
  int r = avcodec_open2(v->dec, codec, nullptr);
  if (r < 0) { if (err) *err = "decoder: " + av(r); return false; }
  v->hw = false;
  std::snprintf(v->info.decoder, sizeof v->info.decoder, "%s", codec->name);
  v->info.hardware = 0;
  return true;
}

static bool openHardware(DvVideo* v) {
  AVCodecID id = v->st->codecpar->codec_id;
  const char* name = id == AV_CODEC_ID_H264 ? "h264_mediacodec" : id == AV_CODEC_ID_HEVC ? "hevc_mediacodec" : nullptr;
  if (!name) return false;
  const AVCodec* codec = avcodec_find_decoder_by_name(name);
  if (!codec) return false;
  AVCodecContext* c = avcodec_alloc_context3(codec);
  avcodec_parameters_to_context(c, v->st->codecpar);
  c->thread_count = 1;
  if (avcodec_open2(c, codec, nullptr) < 0) { avcodec_free_context(&c); return false; }
  if (v->dec) avcodec_free_context(&v->dec);
  v->dec = c;
  v->hw = true;
  v->threads = 1;
  std::snprintf(v->info.decoder, sizeof v->info.decoder, "%s", name);
  v->info.hardware = 1;
  return true;
}


// ---- NDK AMediaCodec decoder ----------------------------------------------------------------------------------------
// FFmpeg's MediaCodec wrapper maps only 8-bit NV12 / I420 buffers. For 10-bit 4:2:0 the codec is driven directly and asked
// for COLOR_FormatYUVP010 output; the buffer is handed to swscale as AV_PIX_FMT_P010LE (stride / slice-height come from the
// output format, never assumed). Input is Annex B: the MP4 length prefixes are rewritten to start codes here.
#ifdef __ANDROID__
static const int kColorP010 = 54, kColorNV12 = 21, kColorI420 = 19, kColorFlex = 0x7F420888;

static void annexB(std::vector<uint8_t>& out, const uint8_t* p, int n) {
  static const uint8_t sc[4] = {0, 0, 0, 1};
  out.insert(out.end(), sc, sc + 4);
  out.insert(out.end(), p, p + n);
}

// avcC / hvcC -> codec-specific data (start-code NAL units); returns the NAL length size, 0 on a malformed record
static int parseExtradata(AVCodecID id, const uint8_t* e, int n, std::vector<uint8_t>& csd0, std::vector<uint8_t>& csd1) {
  if (!e || n < 7) return 0;
  if (id == AV_CODEC_ID_H264) {
    if (e[0] != 1) return 0;
    int pos = 5, ns = e[pos++] & 31;
    for (int i = 0; i < ns && pos + 2 <= n; i++) { int l = (e[pos] << 8) | e[pos + 1]; pos += 2; if (pos + l > n) return 0; annexB(csd0, e + pos, l); pos += l; }
    if (pos >= n) return 0;
    int np = e[pos++];
    for (int i = 0; i < np && pos + 2 <= n; i++) { int l = (e[pos] << 8) | e[pos + 1]; pos += 2; if (pos + l > n) return 0; annexB(csd1, e + pos, l); pos += l; }
    return (e[4] & 3) + 1;
  }
  if (n < 23 || e[0] != 1) return 0;
  int pos = 22, arrays = e[pos++];
  for (int a = 0; a < arrays && pos + 3 <= n; a++) {
    pos++;
    int cnt = (e[pos] << 8) | e[pos + 1]; pos += 2;
    for (int i = 0; i < cnt && pos + 2 <= n; i++) { int l = (e[pos] << 8) | e[pos + 1]; pos += 2; if (pos + l > n) return 0; annexB(csd0, e + pos, l); pos += l; }
  }
  return (e[21] & 3) + 1;
}

static void natRelease(DvVideo* v) {
  if (v->nmc && v->natHeld >= 0) { AMediaCodec_releaseOutputBuffer(v->nmc, (size_t)v->natHeld, false); v->natHeld = -1; }
}
static void natClose(DvVideo* v) {
  if (v->nmc) {
    natRelease(v);
    AMediaCodec_stop(v->nmc);
    AMediaCodec_delete(v->nmc);
    v->nmc = nullptr;
  }
  v->nat = false;
}
static void natFlush(DvVideo* v) {
  if (!v->nmc) return;
  natRelease(v);
  AMediaCodec_flush(v->nmc);
  v->natEos = false;
}

static bool openNative(DvVideo* v) {
  AVCodecParameters* cp = v->st->codecpar;
  const char* mime = cp->codec_id == AV_CODEC_ID_H264 ? "video/avc" : cp->codec_id == AV_CODEC_ID_HEVC ? "video/hevc" : nullptr;
  if (!mime) return false;
  std::vector<uint8_t> c0, c1;
  int nl = parseExtradata(cp->codec_id, cp->extradata, cp->extradata_size, c0, c1);
  if (!nl || c0.empty()) { DV_LOG("native decoder: no avcC/hvcC extradata"); return false; }
  AMediaFormat* f = AMediaFormat_new();
  AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, mime);
  AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, cp->width);
  AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, cp->height);
  AMediaFormat_setBuffer(f, "csd-0", c0.data(), c0.size());
  if (!c1.empty()) AMediaFormat_setBuffer(f, "csd-1", c1.data(), c1.size());
  const bool ten = v->info.bitDepth > 8;
  AMediaFormat_setInt32(f, "color-format", ten ? kColorP010 : kColorNV12);
  const char* nm = v->opts.nativeDecoder;
  AMediaCodec* mc = (nm && *nm) ? AMediaCodec_createCodecByName(nm) : AMediaCodec_createDecoderByType(mime);
  if (!mc) { AMediaFormat_delete(f); DV_LOG("native decoder: cannot create %s", nm && *nm ? nm : mime); return false; }
  media_status_t st = AMediaCodec_configure(mc, f, nullptr, nullptr, 0);
  AMediaFormat_delete(f);
  if (st == AMEDIA_OK) st = AMediaCodec_start(mc);
  if (st != AMEDIA_OK) { AMediaCodec_delete(mc); DV_LOG("native decoder: configure/start failed (%d)", (int)st); return false; }
  if (v->dec) avcodec_free_context(&v->dec);
  v->nmc = mc;
  v->nat = true;
  v->hw = true;
  v->threads = 1;
  v->natNalLen = nl;
  v->natFmtKnown = false;
  v->natEos = false;
  v->natHeld = -1;
  std::string nmn = nm && *nm ? nm : "default";
  std::snprintf(v->info.decoder, sizeof v->info.decoder, "mc:%.26s", nmn.c_str());
  v->info.hardware = 1;
  return true;
}

// Queues one packet (or end of stream when pkt is null): 0 queued, EAGAIN no input buffer free, < 0 error.
static int natSend(DvVideo* v, const AVPacket* pkt, int64_t waitUs = 0) {
  ssize_t ii = AMediaCodec_dequeueInputBuffer(v->nmc, pkt ? 0 : waitUs);
  if (ii < 0) return ii == AMEDIACODEC_INFO_TRY_AGAIN_LATER ? AVERROR(EAGAIN) : AVERROR(EIO);
  if (!pkt) return AMediaCodec_queueInputBuffer(v->nmc, (size_t)ii, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) == AMEDIA_OK ? 0 : AVERROR(EIO);
  v->natIn.clear();
  const uint8_t* d = pkt->data;
  int n = pkt->size;
  if (n >= 4 && d[0] == 0 && d[1] == 0 && d[2] == 0 && d[3] == 1) v->natIn.assign(d, d + n);  // already Annex B
  else {
    int pos = 0;
    while (pos + v->natNalLen <= n) {
      uint32_t l = 0;
      for (int k = 0; k < v->natNalLen; k++) l = (l << 8) | d[pos + k];
      pos += v->natNalLen;
      if (l == 0 || pos + (int)l > n) break;
      annexB(v->natIn, d + pos, (int)l);
      pos += (int)l;
    }
  }
  size_t cap = 0;
  uint8_t* ib = AMediaCodec_getInputBuffer(v->nmc, (size_t)ii, &cap);
  if (!ib || cap < v->natIn.size()) {
    AMediaCodec_queueInputBuffer(v->nmc, (size_t)ii, 0, 0, 0, 0);
    return AVERROR(EIO);
  }
  std::memcpy(ib, v->natIn.data(), v->natIn.size());
  int64_t ts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
  int64_t us = ts == AV_NOPTS_VALUE ? 0 : av_rescale_q(ts, v->st->time_base, AV_TIME_BASE_Q);
  return AMediaCodec_queueInputBuffer(v->nmc, (size_t)ii, 0, v->natIn.size(), (uint64_t)us, 0) == AMEDIA_OK ? 0 : AVERROR(EIO);
}

// Output -> an AVFrame whose planes point into the codec's buffer (valid until the next call). 0 frame, EAGAIN, AVERROR_EOF, < 0.
static int natReceive(DvVideo* v, AVFrame* f) {
  natRelease(v);
  // end of stream not accepted yet (no free input buffer at that moment): keep trying, decoders with a deep reorder queue only flush on it
  if (v->eofSent && !v->natEos && natSend(v, nullptr) == 0) v->natEos = true;
  for (;;) {
    AMediaCodecBufferInfo bi;
    ssize_t oi = AMediaCodec_dequeueOutputBuffer(v->nmc, &bi, 0);
    if (oi == AMEDIACODEC_INFO_TRY_AGAIN_LATER) return AVERROR(EAGAIN);
    if (oi == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
    if (oi == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
      AMediaFormat* of = AMediaCodec_getOutputFormat(v->nmc);
      auto geti = [&](const char* k, int def) { int32_t x = def; return AMediaFormat_getInt32(of, k, &x) ? (int)x : def; };
      v->natFmt = geti("color-format", 0);
      v->natW = geti("width", v->info.width);
      v->natH = geti("height", v->info.height);
      v->natStride = geti("stride", 0);
      v->natSlice = geti("slice-height", 0);
      int cl = geti("crop-left", 0), ct = geti("crop-top", 0), cr = geti("crop-right", v->natW - 1), cb = geti("crop-bottom", v->natH - 1);
      v->natCropL = cl; v->natCropT = ct;
      v->natW = cr - cl + 1; v->natH = cb - ct + 1;
      v->natFmtKnown = true;
      DV_LOG("native decoder output: color-format %d %dx%d stride %d slice %d crop %d,%d", v->natFmt, v->natW, v->natH, v->natStride, v->natSlice, cl, ct);
      AMediaFormat_delete(of);
      continue;
    }
    if (oi < 0) return AVERROR(EIO);
    if (bi.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) { AMediaCodec_releaseOutputBuffer(v->nmc, (size_t)oi, false); return AVERROR_EOF; }
    if (bi.size <= 0) { AMediaCodec_releaseOutputBuffer(v->nmc, (size_t)oi, false); continue; }
    size_t cap = 0;
    uint8_t* ob = AMediaCodec_getOutputBuffer(v->nmc, (size_t)oi, &cap);
    if (!ob) { AMediaCodec_releaseOutputBuffer(v->nmc, (size_t)oi, false); return AVERROR(EIO); }
    if (!v->natFmtKnown) { v->natW = v->info.width; v->natH = v->info.height; }
    const bool ten = v->natFmt == kColorP010;
    const bool nv12 = v->natFmt == kColorNV12 || v->natFmt == kColorFlex;
    const bool i420 = v->natFmt == kColorI420;
    if (!(ten || nv12 || i420) || (v->info.bitDepth > 8 && !ten)) {
      // an 8-bit (or unknown) buffer for a 10-bit stream would be a silently truncated picture: refuse
      DV_LOG("native decoder returned color-format %d for a %d-bit stream", v->natFmt, v->info.bitDepth);
      AMediaCodec_releaseOutputBuffer(v->nmc, (size_t)oi, false);
      return AVERROR(ENOTSUP);
    }
    const int bps = ten ? 2 : 1;
    int stride = v->natStride > 0 ? v->natStride : (v->natCropL + v->natW) * bps;
    int slice = v->natSlice > 0 ? v->natSlice : v->natCropT + v->natH;
    size_t need = (size_t)bi.offset + (size_t)stride * slice + (size_t)stride * ((slice + 1) / 2);
    if (need > cap) {
      DV_LOG("native decoder buffer too small: %zu < %zu (stride %d slice %d)", cap, need, stride, slice);
      AMediaCodec_releaseOutputBuffer(v->nmc, (size_t)oi, false);
      return AVERROR(EIO);
    }
    uint8_t* base = ob + bi.offset;
    f->format = ten ? AV_PIX_FMT_P010LE : (nv12 ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P);
    f->width = v->natW;
    f->height = v->natH;
    f->data[0] = base + (size_t)v->natCropT * stride + (size_t)v->natCropL * bps;
    f->linesize[0] = stride;
    uint8_t* chroma = base + (size_t)stride * slice;
    if (i420) {
      int cs = stride / 2, cslice = slice / 2;
      f->data[1] = chroma + (size_t)(v->natCropT / 2) * cs + v->natCropL / 2;
      f->data[2] = chroma + (size_t)cs * cslice + (size_t)(v->natCropT / 2) * cs + v->natCropL / 2;
      f->linesize[1] = f->linesize[2] = cs;
    } else {
      f->data[1] = chroma + (size_t)(v->natCropT / 2) * stride + (size_t)(v->natCropL / 2) * 2 * bps;
      f->linesize[1] = stride;
    }
    f->best_effort_timestamp = av_rescale_q(bi.presentationTimeUs, AV_TIME_BASE_Q, v->st->time_base);
    f->pts = f->best_effort_timestamp;
    v->natHeld = (int)oi;
    return 0;
  }
}
#else
static void natClose(DvVideo*) {}
static void natFlush(DvVideo*) {}
static bool openNative(DvVideo*) { return false; }
static int natSend(DvVideo*, const AVPacket*, int64_t = 0) { return AVERROR(ENOSYS); }
static int natReceive(DvVideo*, AVFrame*) { return AVERROR(ENOSYS); }
#endif

static std::mutex gReportMu;
static std::string gReport;
extern "C" void dv_set_codec_report(const char* json) {
  std::lock_guard<std::mutex> l(gReportMu);
  gReport = json ? json : "";
}
extern "C" int dv_codec_report(char* dst, int n) {
  std::lock_guard<std::mutex> l(gReportMu);
  if (dst && n > 0) std::snprintf(dst, (size_t)n, "%s", gReport.c_str());
  return (int)gReport.size();
}

extern "C" DvVideo* dv_open(const char* url, const DvOpenOptions* opts, DvInfo* info, char* err, int errLen) {
  av_log_set_level(AV_LOG_ERROR);
  auto* v = new DvVideo();
  if (opts) v->opts = *opts;
  AVDictionary* o = nullptr;
  std::string u = url;
  v->url = u;
  std::string path;
  if (u.rfind("fd:", 0) == 0 && u.size() > 3) {
    int fd = std::atoi(u.c_str() + 3);
    v->srcFd = dup(fd);   // second descriptor for the audio demuxer and the metadata scan
    av_dict_set(&o, "fd", u.c_str() + 3, 0);
    u = "fd:";
  } else {
    path = u;
    v->srcFd = open(u.c_str(), O_RDONLY | DV_O_BINARY);
  }
  int r = avformat_open_input(&v->fmt, u.c_str(), nullptr, &o);
  av_dict_free(&o);
  if (r < 0) { setErr(err, errLen, "open: " + av(r)); if (v->srcFd >= 0) close(v->srcFd); delete v; return nullptr; }
  if ((r = avformat_find_stream_info(v->fmt, nullptr)) < 0) { setErr(err, errLen, "probe: " + av(r)); dv_close(v); return nullptr; }
  v->vs = av_find_best_stream(v->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (v->vs < 0) { setErr(err, errLen, "no video stream"); dv_close(v); return nullptr; }
  v->st = v->fmt->streams[v->vs];
  AVCodecParameters* cp = v->st->codecpar;
  v->pkt = av_packet_alloc();
  v->frm = av_frame_alloc();
  v->startPts = v->st->start_time == AV_NOPTS_VALUE ? 0 : v->st->start_time;

  DvInfo& i = v->info;
  i.width = cp->width;
  i.height = cp->height;
  const AVPacketSideData* sd = av_packet_side_data_get(cp->coded_side_data, cp->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
  if (sd && sd->size >= 36) {
    double a = -av_display_rotation_get((const int32_t*)sd->data);  // clockwise
    if (std::isnan(a)) a = 0;
    int q = ((int)std::lround(a / 90.0) % 4 + 4) % 4;
    i.rotation = q * 90;
  }
  AVRational fr = v->st->avg_frame_rate.num ? v->st->avg_frame_rate : v->st->r_frame_rate;
  i.fps = fr.num && fr.den ? av_q2d(fr) : 30.0;
  v->frameUs = 1e6 / i.fps;
  int64_t dur = v->st->duration != AV_NOPTS_VALUE ? av_rescale_q(v->st->duration, v->st->time_base, AV_TIME_BASE_Q) : v->fmt->duration;
  i.durationUs = dur == AV_NOPTS_VALUE ? 0 : dur;
  const AVPixFmtDescriptor* d = av_pix_fmt_desc_get((AVPixelFormat)cp->format);
  if (d) {
    i.bitDepth = d->comp[0].depth;
    i.chroma = d->nb_components < 3 ? 0 : (d->log2_chroma_w == 0 && d->log2_chroma_h == 0) ? 3 : (d->log2_chroma_h == 0 ? 2 : 1);
  }
  i.primaries = cp->color_primaries;
  i.trc = cp->color_trc;
  i.matrix = cp->color_space;
  i.range = cp->color_range == AVCOL_RANGE_JPEG ? 2 : (cp->color_range == AVCOL_RANGE_MPEG ? 1 : 0);
  i.hdr = i.trc == AVCOL_TRC_ARIB_STD_B67 ? 1 : (i.trc == AVCOL_TRC_SMPTE2084 ? 2 : 0);
  std::snprintf(i.codec, sizeof i.codec, "%s", avcodec_get_name(cp->codec_id));
  i.profile = cp->profile;

  // decoder: hardware first when the caller's probe allows it and FFmpeg's MediaCodec wrapper can return the format
  // (8-bit 4:2:0 only: it maps NV12 / I420 buffers); software otherwise or when hardware fails to open.
  std::string e;
  bool hwEligible = v->opts.hwPref && i.bitDepth == 8 && i.chroma == 1 && (cp->codec_id == AV_CODEC_ID_H264 || cp->codec_id == AV_CODEC_ID_HEVC);
  const bool natEligible = (v->opts.hwPref >= 3 || (v->opts.hwPref == 2 && i.bitDepth == 10)) && i.chroma == 1 &&
                           (cp->codec_id == AV_CODEC_ID_H264 || cp->codec_id == AV_CODEC_ID_HEVC);
  if (natEligible && openNative(v)) {
    // opened
  } else if (!(hwEligible && openHardware(v)) && !openSoftware(v, &e)) { setErr(err, errLen, e); dv_close(v); return nullptr; }

  // Sony metadata: S-Log3 is tagged as plain BT.709 in the stream.
  std::string g, p;
  int det = v->srcFd >= 0 ? nrtFromFd(v->srcFd, g, p) : 0;
  if (!det && !path.empty()) det = sidecar(path, g, p);
  i.slogDetected = det && isSlog3(g) ? det : 0;
  std::snprintf(i.captureGamma, sizeof i.captureGamma, "%s", g.c_str());
  std::snprintf(i.capturePrimaries, sizeof i.capturePrimaries, "%s", p.c_str());
  bool slog = v->opts.source == DV_SRC_SLOG3 || (v->opts.source == DV_SRC_AUTO && i.slogDetected);
  if (slog) v->kind = kSlog3;
  else if (v->opts.source == DV_SRC_HLG || (v->opts.source != DV_SRC_STANDARD && i.hdr == 1)) v->kind = kHlg;
  else if (v->opts.source != DV_SRC_STANDARD && i.hdr == 2) v->kind = kPq;
  else if (i.primaries == AVCOL_PRI_BT2020 && v->opts.source != DV_SRC_STANDARD) v->kind = kSdr2020;
  else v->kind = kStd;
  i.colorKind = v->kind;

  // first audio stream
  int as = av_find_best_stream(v->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  if (as >= 0) {
    AVCodecParameters* ap = v->fmt->streams[as]->codecpar;
    i.audioCodecId = (int)ap->codec_id;
    i.audioRate = ap->sample_rate;
    i.audioChannels = ap->ch_layout.nb_channels;
    i.audioBits = ap->bits_per_coded_sample ? ap->bits_per_coded_sample : ap->bits_per_raw_sample;
    std::snprintf(i.audioCodec, sizeof i.audioCodec, "%s", avcodec_get_name(ap->codec_id));
    i.audioDecodable = avcodec_find_decoder(ap->codec_id) != nullptr;
  }
  if (info) *info = i;
  return v;
}

extern "C" void dv_close(DvVideo* v) {
  if (!v) return;
  if (v->sws) sws_freeContext(v->sws);
  if (v->sws2) sws_freeContext(v->sws2);
  if (v->swr) swr_free(&v->swr);
  av_frame_free(&v->afrm);
  av_packet_free(&v->apkt);
  if (v->adec) avcodec_free_context(&v->adec);
  if (v->afmt) {
    AVIOContext* apb = v->afmt->pb;
    avformat_close_input(&v->afmt);
    if (apb) { av_freep(&apb->buffer); avio_context_free(&apb); }
  }
  delete (PreadIo*)v->aio;
  av_frame_free(&v->frm);
  av_packet_free(&v->pkt);
  natClose(v);
  if (v->dec) avcodec_free_context(&v->dec);
  if (v->fmt) avformat_close_input(&v->fmt);
  if (v->srcFd >= 0) close(v->srcFd);
  delete v;
}

static int64_t frameBytes(int fmt, int w, int h) {
  switch (fmt) {
    case DV_RGBA: return (int64_t)w * h * 4;
    case DV_I420: return (int64_t)w * h * 3 / 2;
    case DV_P010: return (int64_t)w * h * 3;
    default: return (int64_t)w * h * 8;  // RGBA64, RGBAF16
  }
}
extern "C" int dv_decoder(const DvVideo* v, char* name, int n) {
  if (name && n > 0) std::snprintf(name, (size_t)n, "%s", v->info.decoder);
  return v->hw ? 1 : 0;
}
extern "C" int64_t dv_frame_bytes(const DvVideo* v) { return v ? frameBytes(v->outFmt, v->ow, v->oh) : 0; }

extern "C" void dv_output_tags(const DvVideo* v, int* pri, int* trc, int* mat, int* range) {
  if (pri) *pri = v->tagPri;
  if (trc) *trc = v->tagTrc;
  if (mat) *mat = v->tagMat;
  if (range) *range = 1;
}

extern "C" int dv_hdr_static_info(const DvVideo* v, uint8_t out[25]) {
  const AVCodecParameters* cp = v->st->codecpar;
  const AVPacketSideData* md = av_packet_side_data_get(cp->coded_side_data, cp->nb_coded_side_data, AV_PKT_DATA_MASTERING_DISPLAY_METADATA);
  const AVPacketSideData* cl = av_packet_side_data_get(cp->coded_side_data, cp->nb_coded_side_data, AV_PKT_DATA_CONTENT_LIGHT_LEVEL);
  if (!md && !cl) return 0;
  std::memset(out, 0, 25);
  out[0] = 0;  // static metadata type 1
  auto put16 = [&](int off, unsigned x) { out[off] = (uint8_t)(x & 0xff); out[off + 1] = (uint8_t)(x >> 8); };
  if (md && md->size >= sizeof(AVMasteringDisplayMetadata)) {
    const auto* m = (const AVMasteringDisplayMetadata*)md->data;
    if (m->has_primaries) {
      // order in the Android blob: Rx Ry Gx Gy Bx By Wx Wy (units 0.00002), then max lum (cd/m2), min lum (0.0001 cd/m2)
      const int idx[3] = {0, 1, 2};
      for (int k = 0; k < 3; k++) {
        put16(1 + k * 4, (unsigned)std::lround(av_q2d(m->display_primaries[idx[k]][0]) * 50000));
        put16(3 + k * 4, (unsigned)std::lround(av_q2d(m->display_primaries[idx[k]][1]) * 50000));
      }
      put16(13, (unsigned)std::lround(av_q2d(m->white_point[0]) * 50000));
      put16(15, (unsigned)std::lround(av_q2d(m->white_point[1]) * 50000));
    }
    if (m->has_luminance) {
      put16(17, (unsigned)std::lround(av_q2d(m->max_luminance)));
      put16(19, (unsigned)std::lround(av_q2d(m->min_luminance) * 10000));
    }
  }
  if (cl && cl->size >= sizeof(AVContentLightMetadata)) {
    const auto* c = (const AVContentLightMetadata*)cl->data;
    put16(21, c->MaxCLL);
    put16(23, c->MaxFALL);
  }
  return 1;
}

extern "C" int dv_begin(DvVideo* v, int64_t startUs, int64_t endUs, int maxEdge, double maxFps, int format, int fast, int* w, int* h) {
  if (!v) return -1;
  int sw = v->info.width, sh = v->info.height;
  double k = (maxEdge > 0 && std::max(sw, sh) > maxEdge) ? (double)maxEdge / std::max(sw, sh) : 1.0;
  v->ow = std::max(2, ((int)std::lround(sw * k)) & ~1);
  v->oh = std::max(2, ((int)std::lround(sh * k)) & ~1);
  v->outFmt = format;
  v->fast = fast;
  v->from = std::max<int64_t>(0, startUs);
  v->to = endUs;
  v->single = endUs <= startUs;
  v->minGap = maxFps > 0 ? 1e6 / maxFps : 0;
  v->lastOut = -1;
  v->nextTick = 0;
  v->lastPts = -1;
  v->eofSent = false;
  v->pktPending = false;
  v->idle = 0;
  v->done = false;
  if (!v->hw && v->dec) v->dec->skip_loop_filter = fast ? AVDISCARD_ALL : AVDISCARD_DEFAULT;
  int64_t ts = av_rescale_q(v->from, AV_TIME_BASE_Q, v->st->time_base) + v->startPts;
  int r = av_seek_frame(v->fmt, v->vs, ts, AVSEEK_FLAG_BACKWARD);
  if (r < 0) r = av_seek_frame(v->fmt, v->vs, v->startPts, AVSEEK_FLAG_BACKWARD);
  if (r < 0) return r;
  if (v->nat) natFlush(v); else avcodec_flush_buffers(v->dec);
  // output colour tags: HDR passes through for YUV outputs, everything else is delivered as BT.709 SDR
  bool yuvOut = format == DV_I420 || format == DV_P010;
  const bool hdrKeep = format == DV_P010;  // 8-bit I420 is always delivered as SDR BT.709 (fallback / compatibility encoders)
  v->passthrough = false;
  v->tagPri = v->tagTrc = v->tagMat = 1;
  if (hdrKeep && (v->kind == kHlg || v->kind == kPq) && v->info.hdr != 0 && v->info.matrix == AVCOL_SPC_BT2020_NCL) {
    v->passthrough = true;
    v->tagPri = AVCOL_PRI_BT2020;
    v->tagTrc = v->kind == kHlg ? AVCOL_TRC_ARIB_STD_B67 : AVCOL_TRC_SMPTE2084;
    v->tagMat = AVCOL_SPC_BT2020_NCL;
  } else if (yuvOut && v->kind == kStd && (v->info.matrix == AVCOL_SPC_BT709 || (v->info.matrix == AVCOL_SPC_UNSPECIFIED && sh >= 720))) {
    v->passthrough = true;  // same matrix both sides: only scaling / bit depth / chroma siting change, no RGB round trip
  }
  if (w) *w = v->ow;
  if (h) *h = v->oh;
  return 0;
}

static void ensureScalers(DvVideo* v, const AVFrame* f) {
  // The stream's own tags win over what a decoder reports per frame (MediaCodec reports its own defaults).
  int cs = v->info.matrix != AVCOL_SPC_UNSPECIFIED ? v->info.matrix : f->colorspace;
  int rg = v->info.range == 2 ? AVCOL_RANGE_JPEG : v->info.range == 1 ? AVCOL_RANGE_MPEG : f->color_range;
  int pri = v->info.primaries != AVCOL_PRI_UNSPECIFIED ? v->info.primaries : f->color_primaries;
  bool full = rg == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P || f->format == AV_PIX_FMT_YUVJ422P || f->format == AV_PIX_FMT_YUVJ444P;
  bool toYuv = v->outFmt == DV_I420 || v->outFmt == DV_P010;
  int key[12] = {f->format, f->width, f->height, cs, full ? 1 : 0, (int)v->kind + 10 * (pri == AVCOL_PRI_BT2020), v->ow, v->oh, v->outFmt, v->fast, v->passthrough ? 1 : 0, 0};
  if (v->sws && std::memcmp(key, v->swsKey, sizeof key) == 0) return;
  std::memcpy(v->swsKey, key, sizeof key);
  if (v->sws) { sws_freeContext(v->sws); v->sws = nullptr; }
  if (v->sws2) { sws_freeContext(v->sws2); v->sws2 = nullptr; }
  AVPixelFormat yuvFmt = v->outFmt == DV_P010 ? AV_PIX_FMT_P010LE : AV_PIX_FMT_YUV420P;
  AVPixelFormat dstFmt = v->passthrough ? yuvFmt : AV_PIX_FMT_RGB48LE;
  int flags = v->fast ? SWS_FAST_BILINEAR : (SWS_BICUBIC | SWS_ACCURATE_RND);
  if (!v->fast && dstFmt == AV_PIX_FMT_RGB48LE) flags |= SWS_FULL_CHR_H_INT;
  auto make = [&](AVPixelFormat sf, int sw_, int sh_, AVPixelFormat df, int dw, int dh) {
    SwsContext* s = sws_alloc_context();
    av_opt_set_int(s, "srcw", sw_, 0); av_opt_set_int(s, "srch", sh_, 0); av_opt_set_int(s, "src_format", sf, 0);
    av_opt_set_int(s, "dstw", dw, 0); av_opt_set_int(s, "dsth", dh, 0); av_opt_set_int(s, "dst_format", df, 0);
    av_opt_set_int(s, "sws_flags", flags, 0);
    av_opt_set_int(s, "threads", v->threads, 0);
    if (sws_init_context(s, nullptr, nullptr) < 0) { sws_freeContext(s); return (SwsContext*)nullptr; }
    return s;
  };
  v->sws = make((AVPixelFormat)f->format, f->width, f->height, dstFmt, v->ow, v->oh);
  if (!v->sws) return;
  const int* srcCo = sws_getCoefficients(swsCs(cs, f->height));
  if (v->passthrough) {
    // same matrix both sides (BT.709 or BT.2020 NCL); only range / depth / chroma format change
    sws_setColorspaceDetails(v->sws, srcCo, full ? 1 : 0, srcCo, 0, 0, 1 << 16, 1 << 16);
    return;
  }
  // source matrix + range -> full-range RGB48 (the stream's own transfer is still applied)
  sws_setColorspaceDetails(v->sws, srcCo, full ? 1 : 0, sws_getCoefficients(SWS_CS_ITU709), 1, 0, 1 << 16, 1 << 16);
  bool limitedSig = !full;
  v->tables.build(v->kind, pri == AVCOL_PRI_BT2020 && v->kind != kSlog3, limitedSig, std::max(8, v->info.bitDepth));
  if (toYuv) {
    v->sws2 = make(AV_PIX_FMT_RGB48LE, v->ow, v->oh, yuvFmt, v->ow, v->oh);
    if (v->sws2) sws_setColorspaceDetails(v->sws2, sws_getCoefficients(SWS_CS_ITU709), 1, sws_getCoefficients(SWS_CS_ITU709), 0, 0, 1 << 16, 1 << 16);
  }
}

// RGB48 (decoded, source transfer) -> display-referred 16-bit RGB (sRGB-encoded, BT.709 primaries); or linear F16.
static void transform(DvVideo* v, uint16_t* dst, bool f16) {
  const Tables& t = v->tables;
  const size_t n = (size_t)v->ow * v->oh;
  const uint16_t* s = v->rgb48.data();
  const bool g = t.gamut;
  const float* m = nullptr;
  float mf[9];
  for (int i = 0; i < 9; i++) mf[i] = (float)t.mat[i];
  m = mf;
  for (size_t i = 0; i < n; i++, s += 3, dst += 4) {
    float r, gg, b;
    if (t.kind == kStd) {  // gamma-encoded already; linearise only for float output
      r = f16 ? srgbEotf(s[0] / 65535.0f) : s[0] / 65535.0f;
      gg = f16 ? srgbEotf(s[1] / 65535.0f) : s[1] / 65535.0f;
      b = f16 ? srgbEotf(s[2] / 65535.0f) : s[2] / 65535.0f;
      if (!f16) { dst[0] = s[0]; dst[1] = s[1]; dst[2] = s[2]; dst[3] = 65535; continue; }
    } else {
      r = t.in[s[0]]; gg = t.in[s[1]]; b = t.in[s[2]];
      if (g) {
        float R = m[0] * r + m[1] * gg + m[2] * b;
        float G = m[3] * r + m[4] * gg + m[5] * b;
        float B = m[6] * r + m[7] * gg + m[8] * b;
        r = R; gg = G; b = B;
      }
    }
    if (f16) {
      dst[0] = toHalf(std::min(std::max(r, 0.0f), kXMax));
      dst[1] = toHalf(std::min(std::max(gg, 0.0f), kXMax));
      dst[2] = toHalf(std::min(std::max(b, 0.0f), kXMax));
      dst[3] = 0x3c00;  // 1.0
    } else {
      dst[0] = (uint16_t)(t.oetf(r) * 65535.0f + 0.5f);
      dst[1] = (uint16_t)(t.oetf(gg) * 65535.0f + 0.5f);
      dst[2] = (uint16_t)(t.oetf(b) * 65535.0f + 0.5f);
      dst[3] = 65535;
    }
  }
}

static bool convert(DvVideo* v, const AVFrame* f, uint8_t* dst) {
  ensureScalers(v, f);
  if (!v->sws) return false;
  const int W = v->ow, H = v->oh;
  if (v->passthrough) {
    uint8_t* planes[4] = {dst, nullptr, nullptr, nullptr};
    int strides[4] = {0, 0, 0, 0};
    if (v->outFmt == DV_P010) {
      planes[1] = dst + (size_t)W * H * 2;
      strides[0] = W * 2; strides[1] = W * 2;
    } else {
      planes[1] = dst + (size_t)W * H; planes[2] = planes[1] + (size_t)(W / 2) * (H / 2);
      strides[0] = W; strides[1] = W / 2; strides[2] = W / 2;
    }
    return sws_scale(v->sws, f->data, f->linesize, 0, f->height, planes, strides) > 0;
  }
  v->rgb48.resize((size_t)W * H * 3);
  uint8_t* p48[4] = {(uint8_t*)v->rgb48.data(), nullptr, nullptr, nullptr};
  int s48[4] = {W * 6, 0, 0, 0};
  if (sws_scale(v->sws, f->data, f->linesize, 0, f->height, p48, s48) <= 0) return false;
  const bool f16 = v->outFmt == DV_RGBAF16;
  if (v->outFmt == DV_RGBA64 || f16) { transform(v, (uint16_t*)dst, f16); return true; }
  v->rgb16.resize((size_t)W * H * 4);
  transform(v, v->rgb16.data(), false);
  if (v->outFmt == DV_RGBA) {
    const uint16_t* s = v->rgb16.data();
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++, s += 4, dst += 4) {
        dst[0] = quant8(s[0], x, y); dst[1] = quant8(s[1], x, y); dst[2] = quant8(s[2], x, y); dst[3] = 255;
      }
    return true;
  }
  // YUV out from the converted RGB: pack RGB48 (drop alpha) and let swscale do matrix + chroma + depth
  uint16_t* q = v->rgb48.data();
  const uint16_t* s = v->rgb16.data();
  for (size_t i = 0, n = (size_t)W * H; i < n; i++, s += 4, q += 3) { q[0] = s[0]; q[1] = s[1]; q[2] = s[2]; }
  const uint8_t* sp[4] = {(const uint8_t*)v->rgb48.data(), nullptr, nullptr, nullptr};
  int ss[4] = {W * 6, 0, 0, 0};
  uint8_t* dp[4] = {dst, nullptr, nullptr, nullptr};
  int ds[4] = {0, 0, 0, 0};
  if (v->outFmt == DV_P010) { dp[1] = dst + (size_t)W * H * 2; ds[0] = W * 2; ds[1] = W * 2; }
  else { dp[1] = dst + (size_t)W * H; dp[2] = dp[1] + (size_t)(W / 2) * (H / 2); ds[0] = W; ds[1] = W / 2; ds[2] = W / 2; }
  return v->sws2 && sws_scale(v->sws2, sp, ss, 0, H, dp, ds) > 0;
}

// Hardware decoder gave up (no frame / error): continue the same pass in software.
static bool fallbackToSoftware(DvVideo* v) {
  if (!v->hw) return false;
  DV_LOG("hardware decoder gave no frames / failed: falling back to software");
  natClose(v);
  std::string e;
  if (!openSoftware(v, &e)) return false;
  v->dec->skip_loop_filter = v->fast ? AVDISCARD_ALL : AVDISCARD_DEFAULT;
  int64_t ts = av_rescale_q(v->from, AV_TIME_BASE_Q, v->st->time_base) + v->startPts;
  if (av_seek_frame(v->fmt, v->vs, ts, AVSEEK_FLAG_BACKWARD) < 0) av_seek_frame(v->fmt, v->vs, v->startPts, AVSEEK_FLAG_BACKWARD);
  avcodec_flush_buffers(v->dec);
  v->pktPending = false;
  v->eofSent = false;
  v->idle = 0;
  v->lastPts = -1;
  if (v->sws) { sws_freeContext(v->sws); v->sws = nullptr; std::memset(v->swsKey, 0, sizeof v->swsKey); }
  return true;
}

extern "C" int dv_force_software(DvVideo* v) { return v && fallbackToSoftware(v) ? 1 : 0; }

extern "C" int64_t dv_next(DvVideo* v, uint8_t* dst, int64_t dstSize) {
  if (!v || v->done) return -1;
  if (dstSize < frameBytes(v->outFmt, v->ow, v->oh)) return -2;
  for (;;) {
    int r = v->nat ? natReceive(v, v->frm) : avcodec_receive_frame(v->dec, v->frm);
    if (r == 0) {
      AVFrame* f = v->frm;
      v->idle = 0;
      if (v->hw) v->hwFrames++;
      int64_t pts = f->best_effort_timestamp;
      int64_t us;
      if (pts == AV_NOPTS_VALUE) us = v->lastPts < 0 ? 0 : v->lastPts + (int64_t)v->frameUs;
      else us = av_rescale_q(pts - v->startPts, v->st->time_base, AV_TIME_BASE_Q);
      v->lastPts = us;
      bool skip = us < v->from - (int64_t)(v->frameUs / 2);
      if (!skip && !v->single && us > v->to) { v->done = true; av_frame_unref(f); return -1; }
      // frame-rate cap: keep the frame closest to each tick of a grid with spacing minGap (50 fps -> 30 fps keeps 3 of 5)
      if (!skip && v->minGap > 0 && v->lastOut >= 0 && (double)us + v->frameUs / 2 < v->nextTick) skip = true;
      if (skip) { av_frame_unref(f); continue; }
      bool ok = convert(v, f, dst);
      if (!ok) DV_LOG("convert failed: decoder %s frame format %s (%d) %dx%d", v->info.decoder, av_get_pix_fmt_name((AVPixelFormat)f->format) ? av_get_pix_fmt_name((AVPixelFormat)f->format) : "?", f->format, f->width, f->height);
      av_frame_unref(f);
      if (!ok) return -2;
      v->lastOut = us;
      if (v->minGap > 0) {
        v->nextTick = (v->nextTick == 0 ? (double)us : v->nextTick) + v->minGap;
        if (v->nextTick < (double)us) v->nextTick = (double)us + v->minGap;
      }
      if (v->single) v->done = true;
      return us;
    }
    if (r == AVERROR_EOF) {
      if (v->hw && v->hwFrames == 0 && fallbackToSoftware(v)) continue;
      v->done = true;
      return -1;
    }
    if (r != AVERROR(EAGAIN)) {
      DV_LOG("receive_frame failed (%s): %s", v->info.decoder, av(r).c_str());
      if (v->hw && fallbackToSoftware(v)) continue;
      return -2;
    }
    // needs input (or, for the asynchronous MediaCodec decoder, time)
    if (v->eofSent) {
      if (++v->idle > 3000) {
        if (v->hw && v->hwFrames == 0 && fallbackToSoftware(v)) continue;
        v->done = true;
        return -1;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }
    if (!v->pktPending) {
      int rr = av_read_frame(v->fmt, v->pkt);
      if (rr < 0) {
        v->eofSent = true;
        if (v->nat) { if (natSend(v, nullptr, 200000) == 0) v->natEos = true; } else avcodec_send_packet(v->dec, nullptr);
        continue;
      }
      if (v->pkt->stream_index != v->vs) { av_packet_unref(v->pkt); continue; }
      v->pktPending = true;
    }
    int sr = v->nat ? natSend(v, v->pkt) : avcodec_send_packet(v->dec, v->pkt);
    if (sr == 0) {
      av_packet_unref(v->pkt);
      v->pktPending = false;
    } else if (sr == AVERROR(EAGAIN)) {
      // decoder queue full: let it produce output first
      if (++v->idle > 3000) {
        if (v->hw && fallbackToSoftware(v)) continue;
        return -2;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } else {
      av_packet_unref(v->pkt);
      v->pktPending = false;
      if (sr == AVERROR_INVALIDDATA) continue;
      if (v->hw && fallbackToSoftware(v)) continue;
      return -2;
    }
    // a MediaCodec decoder that has taken many packets without returning any frame is not working
    if (v->hw && v->hwFrames == 0 && ++v->idle > 1500) {
      if (fallbackToSoftware(v)) continue;
    }
  }
}

// ---- audio ---------------------------------------------------------------------------------------------------------
// Independent reader (own position, pread) over the source descriptor: the audio demuxer must not share a file offset with
// the video demuxer, and re-opening /proc/self/fd is refused for descriptors handed over by the media provider.
static int preadRead(void* o, uint8_t* buf, int n) {
  auto* io = (PreadIo*)o;
  ssize_t r = pread(io->fd, buf, (size_t)n, (off_t)io->pos);
  if (r < 0) return AVERROR(EIO);
  if (r == 0) return AVERROR_EOF;
  io->pos += r;
  return (int)r;
}
static int64_t preadSeek(void* o, int64_t off, int whence) {
  auto* io = (PreadIo*)o;
  if (whence & AVSEEK_SIZE) return io->size;
  switch (whence & ~AVSEEK_FORCE) {
    case SEEK_SET: io->pos = off; break;
    case SEEK_CUR: io->pos += off; break;
    case SEEK_END: io->pos = io->size + off; break;
    default: return -1;
  }
  return io->pos;
}

extern "C" int dv_audio_begin(DvVideo* v, int64_t startUs, int64_t endUs, int* rate, int* channels) {
  if (!v || !v->info.audioCodecId || !v->info.audioDecodable) return -1;
  if (!v->afmt) {
    if (v->srcFd < 0) return -2;
    auto* io = new PreadIo();
    io->fd = v->srcFd;
    struct stat sb;
    io->size = fstat(v->srcFd, &sb) == 0 ? sb.st_size : 0;
    v->aio = io;
    uint8_t* ibuf = (uint8_t*)av_malloc(65536);
    AVIOContext* pb = avio_alloc_context(ibuf, 65536, 0, io, preadRead, nullptr, preadSeek);
    v->afmt = avformat_alloc_context();
    v->afmt->pb = pb;
    v->afmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    int r = avformat_open_input(&v->afmt, "", nullptr, nullptr);
    if (r < 0) { DV_LOG("audio open: %s", av(r).c_str()); v->afmt = nullptr; return -2; }
    if (avformat_find_stream_info(v->afmt, nullptr) < 0) return -2;
    v->as = av_find_best_stream(v->afmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (v->as < 0) return -1;
    const AVCodecParameters* ap = v->afmt->streams[v->as]->codecpar;
    const AVCodec* c = avcodec_find_decoder(ap->codec_id);
    if (!c) return -1;
    v->adec = avcodec_alloc_context3(c);
    avcodec_parameters_to_context(v->adec, ap);
    if (avcodec_open2(v->adec, c, nullptr) < 0) return -2;
    v->apkt = av_packet_alloc();
    v->afrm = av_frame_alloc();
    v->arate = ap->sample_rate;
    v->ach = std::min(ap->ch_layout.nb_channels, 2);
    AVChannelLayout out;
    av_channel_layout_default(&out, v->ach);
    int sr = swr_alloc_set_opts2(&v->swr, &out, AV_SAMPLE_FMT_S16, v->arate, &ap->ch_layout, (AVSampleFormat)ap->format, v->arate, 0, nullptr);
    if (sr < 0 || swr_init(v->swr) < 0) return -2;
  }
  AVStream* st = v->afmt->streams[v->as];
  int64_t start = st->start_time == AV_NOPTS_VALUE ? 0 : st->start_time;
  int64_t ts = av_rescale_q(std::max<int64_t>(0, startUs), AV_TIME_BASE_Q, st->time_base) + start;
  if (av_seek_frame(v->afmt, v->as, ts, AVSEEK_FLAG_BACKWARD) < 0) av_seek_frame(v->afmt, v->as, start, AVSEEK_FLAG_BACKWARD);
  avcodec_flush_buffers(v->adec);
  swr_close(v->swr); swr_init(v->swr);
  v->aFrom = std::max<int64_t>(0, startUs);
  v->aTo = endUs;
  v->aEof = false;
  v->aq.clear();
  v->aqPts = v->aFrom;
  v->aPos = -1;
  if (rate) *rate = v->arate;
  if (channels) *channels = v->ach;
  return 0;
}

extern "C" int dv_audio_next(DvVideo* v, int16_t* dst, int maxFrames, int64_t* ptsUs) {
  if (!v || !v->afmt) return -1;
  const int ch = v->ach;
  AVStream* st = v->afmt->streams[v->as];
  int64_t start = st->start_time == AV_NOPTS_VALUE ? 0 : st->start_time;
  while ((int)(v->aq.size() / ch) < maxFrames && !v->aEof) {
    int rr = av_read_frame(v->afmt, v->apkt);
    if (rr < 0) { v->aEof = true; break; }
    if (v->apkt->stream_index != v->as) { av_packet_unref(v->apkt); continue; }
    avcodec_send_packet(v->adec, v->apkt);
    av_packet_unref(v->apkt);
    while (avcodec_receive_frame(v->adec, v->afrm) == 0) {
      int64_t fus = v->afrm->pts == AV_NOPTS_VALUE ? -1 : av_rescale_q(v->afrm->pts - start, st->time_base, AV_TIME_BASE_Q);
      int n = v->afrm->nb_samples;
      std::vector<int16_t> tmp((size_t)n * ch);
      uint8_t* o[1] = {(uint8_t*)tmp.data()};
      int got = swr_convert(v->swr, o, n, (const uint8_t**)v->afrm->extended_data, n);
      av_frame_unref(v->afrm);
      if (got <= 0) continue;
      if (v->aPos < 0) v->aPos = fus >= 0 ? fus : v->aFrom;
      int64_t t0 = v->aPos;
      v->aPos += (int64_t)got * 1000000 / v->arate;
      // trim to [aFrom, aTo]
      int64_t skipFrames = 0, keep = got;
      if (t0 < v->aFrom) skipFrames = std::min<int64_t>(got, (v->aFrom - t0) * v->arate / 1000000);
      if (v->aTo > 0 && t0 + (int64_t)got * 1000000 / v->arate > v->aTo) {
        int64_t lim = std::max<int64_t>(0, (v->aTo - t0) * v->arate / 1000000);
        keep = std::min<int64_t>(got, lim);
        v->aEof = true;
      }
      if (keep > skipFrames) {
        if (v->aq.empty()) v->aqPts = t0 + skipFrames * 1000000 / v->arate;
        v->aq.insert(v->aq.end(), tmp.begin() + skipFrames * ch, tmp.begin() + keep * ch);
      }
      if (v->aEof) break;
    }
  }
  int avail = (int)(v->aq.size() / ch);
  int n = std::min(avail, maxFrames);
  if (n <= 0) return 0;
  if (ptsUs) *ptsUs = v->aqPts;
  std::memcpy(dst, v->aq.data(), (size_t)n * ch * sizeof(int16_t));
  v->aq.erase(v->aq.begin(), v->aq.begin() + (size_t)n * ch);
  v->aqPts += (int64_t)n * 1000000 / v->arate;
  return n;
}

// ---- Ultra HDR -------------------------------------------------------------------------------------------------------
extern "C" int dv_make_ultrahdr(const uint16_t* rgbaF16, int w, int h, uint8_t* sdr, uint8_t* gain, int* gw, int* gh, float meta[5]) {
  if (!rgbaF16 || w < 2 || h < 2) return -1;
  // half -> float
  auto h2f = [](uint16_t hv) {
    uint32_t s = (hv & 0x8000u) << 16, e = (hv >> 10) & 0x1f, m = hv & 0x3ff, u;
    if (e == 0) { if (m == 0) u = s; else { e = 1; while (!(m & 0x400)) { m <<= 1; e--; } m &= 0x3ff; u = s | ((e + 127 - 15) << 23) | (m << 13); } }
    else if (e == 31) u = s | 0x7f800000u | (m << 13);
    else u = s | ((e + 127 - 15) << 23) | (m << 13);
    union { uint32_t u; float f; } c{u};
    return c.f;
  };
  const float epsS = 1.0f / 64.0f, epsH = 1.0f / 64.0f;
  const int GW = (w + 1) / 2, GH = (h + 1) / 2;
  std::vector<float> lg((size_t)w * h);
  float lo = 0.0f, hi = 0.0f;
  bool first = true;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      const uint16_t* p = rgbaF16 + ((size_t)y * w + x) * 4;
      float r = h2f(p[0]), g = h2f(p[1]), b = h2f(p[2]);
      float sr = softKnee(r), sg = softKnee(g), sb = softKnee(b);
      uint8_t* o = sdr + ((size_t)y * w + x) * 4;
      o[0] = quant8((uint16_t)(srgbOetf(sr) * 65535.0f + 0.5f), x, y);
      o[1] = quant8((uint16_t)(srgbOetf(sg) * 65535.0f + 0.5f), x, y);
      o[2] = quant8((uint16_t)(srgbOetf(sb) * 65535.0f + 0.5f), x, y);
      o[3] = 255;
      // gain from luminance of what the 8-bit base actually shows (decoded back to linear), so the pair reconstructs
      float ys = 0.2126f * srgbEotf(o[0] / 255.0f) + 0.7152f * srgbEotf(o[1] / 255.0f) + 0.0722f * srgbEotf(o[2] / 255.0f);
      float yh = 0.2126f * r + 0.7152f * g + 0.0722f * b;
      float l = std::log2((yh + epsH) / (ys + epsS));
      lg[(size_t)y * w + x] = l;
      if (first) { lo = hi = l; first = false; } else { lo = std::min(lo, l); hi = std::max(hi, l); }
    }
  if (hi - lo < 1e-3f) hi = lo + 1e-3f;
  for (int gy = 0; gy < GH; gy++)
    for (int gx = 0; gx < GW; gx++) {
      float acc = 0; int n = 0;
      for (int dy = 0; dy < 2; dy++)
        for (int dx = 0; dx < 2; dx++) {
          int x = std::min(w - 1, gx * 2 + dx), y = std::min(h - 1, gy * 2 + dy);
          acc += lg[(size_t)y * w + x]; n++;
        }
      float rec = (acc / n - lo) / (hi - lo);
      gain[(size_t)gy * GW + gx] = (uint8_t)std::lround(std::min(std::max(rec, 0.0f), 1.0f) * 255.0f);
    }
  if (gw) *gw = GW;
  if (gh) *gh = GH;
  meta[0] = std::exp2(lo); meta[1] = std::exp2(hi); meta[2] = 1.0f; meta[3] = epsS; meta[4] = epsH;
  return 0;
}
