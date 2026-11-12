// On-device test of the video core (core/video, FFmpeg 9.x). Build + run: tools/ffmpeg/test-android.sh
// Fixtures: tests/data/video (see README.md there). Colour fixtures: left half RGB(200,40,40), right half RGB(40,80,200).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

#include "../../core/video/dapple_video.h"

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; std::printf("  FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static bool near3(const uint8_t* p, int r, int g, int b, int tol) {
  return std::abs(p[0] - r) <= tol && std::abs(p[1] - g) <= tol && std::abs(p[2] - b) <= tol;
}
static DvOpenOptions SW{0, DV_SRC_AUTO, nullptr};
static DvOpenOptions HW{1, DV_SRC_AUTO, nullptr};

static DvVideo* openF(const std::string& dir, const char* f, DvOpenOptions o, DvInfo* info) {
  char err[256] = {0};
  DvVideo* v = dv_open((dir + "/" + f).c_str(), &o, info, err, sizeof err);
  if (!v) std::printf("  open %s: %s\n", f, err);
  return v;
}

static void colourTest(const std::string& dir, const char* file, int bitDepth, int chroma, const char* codec) {
  std::printf("[%s]\n", file);
  DvInfo info{};
  DvVideo* v = openF(dir, file, SW, &info);
  CHECK(v != nullptr, "open");
  if (!v) return;
  CHECK(info.bitDepth == bitDepth && info.chroma == chroma, "format %d-bit chroma %d", info.bitDepth, info.chroma);
  CHECK(std::strcmp(info.codec, codec) == 0, "codec %s", info.codec);
  CHECK(info.width == 64 && info.height == 48, "size %dx%d", info.width, info.height);
  CHECK(info.hardware == 0, "software requested");

  int w = 0, h = 0;
  CHECK(dv_begin(v, 0, 0, 0, 0, DV_RGBA, 0, &w, &h) == 0, "begin");
  std::vector<uint8_t> px((size_t)w * h * 4);
  int64_t t = dv_next(v, px.data(), (int64_t)px.size());
  CHECK(t >= 0, "next -> %lld", (long long)t);
  const uint8_t* L = &px[((size_t)24 * w + 16) * 4];
  const uint8_t* R = &px[((size_t)24 * w + 48) * 4];
  CHECK(near3(L, 200, 40, 40, 5), "left  = %d %d %d (want 200 40 40)", L[0], L[1], L[2]);
  CHECK(near3(R, 40, 80, 200, 5), "right = %d %d %d (want 40 80 200)", R[0], R[1], R[2]);
  CHECK(L[3] == 255, "alpha");
  CHECK(dv_next(v, px.data(), (int64_t)px.size()) == -1, "single-frame mode ends");

  CHECK(dv_begin(v, 0, 0, 32, 0, DV_RGBA, 0, &w, &h) == 0 && w == 32 && h == 24, "scaled size %dx%d", w, h);
  px.assign((size_t)w * h * 4, 0);
  CHECK(dv_next(v, px.data(), (int64_t)px.size()) >= 0, "scaled next");
  L = &px[((size_t)12 * w + 8) * 4];
  R = &px[((size_t)12 * w + 24) * 4];
  CHECK(near3(L, 200, 40, 40, 6) && near3(R, 40, 80, 200, 6), "scaled colours %d %d %d | %d %d %d", L[0], L[1], L[2], R[0], R[1], R[2]);

  // I420 (8-bit H.264 fallback encoder input): BT.709 limited range, tagged 1/1/1.
  CHECK(dv_begin(v, 0, 0, 0, 0, DV_I420, 0, &w, &h) == 0, "begin i420");
  std::vector<uint8_t> yuv((size_t)w * h * 3 / 2);
  CHECK(dv_next(v, yuv.data(), (int64_t)yuv.size()) >= 0, "next i420");
  int wantL = (int)std::lround(16 + 219 * (0.2126 * 200 + 0.7152 * 40 + 0.0722 * 40) / 255.0);
  int wantR = (int)std::lround(16 + 219 * (0.2126 * 40 + 0.7152 * 80 + 0.0722 * 200) / 255.0);
  int yL = yuv[(size_t)24 * w + 16], yR = yuv[(size_t)24 * w + 48];
  CHECK(std::abs(yL - wantL) <= 3, "Y left %d (want ~%d)", yL, wantL);
  CHECK(std::abs(yR - wantR) <= 3, "Y right %d (want ~%d)", yR, wantR);
  int tp, tt_, tm, tr;
  dv_output_tags(v, &tp, &tt_, &tm, &tr);
  CHECK(tp == 1 && tt_ == 1 && tm == 1 && tr == 1, "tags %d/%d/%d/%d (want 1/1/1/limited)", tp, tt_, tm, tr);

  // P010 (HEVC Main10 encoder input): 10-bit words, value in the top 10 bits; Y' must match the 8-bit result * 4 (+-).
  CHECK(dv_begin(v, 0, 0, 0, 0, DV_P010, 0, &w, &h) == 0, "begin p010");
  std::vector<uint16_t> p010((size_t)w * h * 3 / 2);
  CHECK(dv_frame_bytes(v) == (int64_t)p010.size() * 2, "p010 frame bytes %lld", (long long)dv_frame_bytes(v));
  CHECK(dv_next(v, (uint8_t*)p010.data(), (int64_t)p010.size() * 2) >= 0, "next p010");
  int y10 = p010[(size_t)24 * w + 16] >> 6;
  CHECK(std::abs(y10 - wantL * 4) <= 8, "P010 Y10 left %d (want ~%d)", y10, wantL * 4);
  CHECK((p010[(size_t)24 * w + 16] & 0x3f) == 0, "P010 low 6 bits must be zero");

  CHECK(dv_begin(v, 0, 10000000, 0, 0, DV_RGBA, 0, &w, &h) == 0, "begin range");
  px.assign((size_t)w * h * 4, 0);
  int n = 0;
  int64_t last = -1, tm2;
  bool ordered = true;
  while ((tm2 = dv_next(v, px.data(), (int64_t)px.size())) >= 0) { n++; if (tm2 <= last) ordered = false; last = tm2; }
  CHECK(n == 15 && ordered, "range frames = %d (want 15), ordered=%d", n, (int)ordered);
  CHECK(dv_begin(v, 0, 10000000, 0, 10.0, DV_RGBA, 0, &w, &h) == 0, "begin capped");
  n = 0;
  while (dv_next(v, px.data(), (int64_t)px.size()) >= 0) n++;
  CHECK(n >= 5 && n <= 7, "10 fps cap gives %d frames of 15 @25fps (want 6)", n);
  CHECK(dv_begin(v, 300000, 400000, 0, 0, DV_RGBA, 0, &w, &h) == 0, "begin mid");
  n = 0;
  int64_t first = -1;
  while ((tm2 = dv_next(v, px.data(), (int64_t)px.size())) >= 0) { if (first < 0) first = tm2; n++; }
  CHECK(n >= 3 && n <= 4 && std::llabs(first - 300000) < 40000, "mid range n=%d first=%lld", n, (long long)first);
  dv_close(v);
}

static void hlgTest(const std::string& dir) {
  std::printf("[hlg_hevc_420_10.mp4]\n");
  DvInfo info{};
  DvVideo* v = openF(dir, "hlg_hevc_420_10.mp4", SW, &info);
  CHECK(v != nullptr, "open");
  if (!v) return;
  CHECK(info.hdr == 1 && info.primaries == 9 && info.codec[0] == 'h' && info.colorKind == DV_KIND_HLG, "hdr=%d primaries=%d kind=%d", info.hdr, info.primaries, info.colorKind);
  int w, h;
  // SDR still (tone mapped): HLG grey 0.5 -> sRGB 152 neutral
  CHECK(dv_begin(v, 0, 0, 0, 0, DV_RGBA, 0, &w, &h) == 0, "begin");
  std::vector<uint8_t> px((size_t)w * h * 4);
  CHECK(dv_next(v, px.data(), (int64_t)px.size()) >= 0, "next");
  const uint8_t* p = &px[((size_t)24 * w + 32) * 4];
  CHECK(near3(p, 152, 152, 152, 5), "HLG grey -> %d %d %d (want 152)", p[0], p[1], p[2]);
  // master clip: HLG passes through as 10-bit BT.2020 / HLG, no tone mapping
  CHECK(dv_begin(v, 0, 0, 0, 0, DV_P010, 0, &w, &h) == 0, "begin p010");
  std::vector<uint16_t> q((size_t)w * h * 3 / 2);
  CHECK(dv_next(v, (uint8_t*)q.data(), (int64_t)q.size() * 2) >= 0, "next p010");
  int tp, tt, tm, tr;
  dv_output_tags(v, &tp, &tt, &tm, &tr);
  CHECK(tp == 9 && tt == 18 && tm == 9 && tr == 1, "HLG tags %d/%d/%d/%d (want 9/18/9/limited)", tp, tt, tm, tr);
  int y10 = q[(size_t)24 * w + 32] >> 6;
  // fixture: 8-bit gray 128 scaled to BT.2020 limited 10-bit -> Y' = 64 + 876 * 128/255 = ~504; pass-through must stay near it
  CHECK(std::abs(y10 - 504) <= 12, "HLG passthrough Y10 = %d (want ~504)", y10);
  uint8_t hdr[25];
  (void)dv_hdr_static_info(v, hdr);
  // HDR still: half-float linear, can exceed SDR white; grey 0.5 signal -> 0.317 linear
  CHECK(dv_begin(v, 0, 0, 0, 0, DV_RGBAF16, 0, &w, &h) == 0, "begin f16");
  std::vector<uint16_t> f16((size_t)w * h * 4);
  CHECK(dv_next(v, (uint8_t*)f16.data(), (int64_t)f16.size() * 2) >= 0, "next f16");
  uint16_t hv = f16[((size_t)24 * w + 32) * 4];
  // half 0.317 = 0x3510 .. approximately; decode
  float f = std::ldexp((float)((hv & 0x3ff) | 0x400), (int)((hv >> 10) & 0x1f) - 25);
  CHECK(std::fabs(f - 0.317f) < 0.02f, "HLG F16 grey = %f (want 0.317)", f);
  dv_close(v);
}

// 10-bit precision: a smooth 10-bit ramp must keep > 256 distinct levels through scaling and colour conversion.
static void precisionTest(const std::string& dir) {
  std::printf("[ramp_h264_422_10.mp4]  precision\n");
  DvInfo info{};
  DvVideo* v = openF(dir, "ramp_h264_422_10.mp4", SW, &info);
  CHECK(v != nullptr, "open");
  if (!v) return;
  int w, h;
  CHECK(dv_begin(v, 0, 0, 0, 0, DV_P010, 0, &w, &h) == 0 && w == 1024, "begin p010 (w=%d)", w);
  std::vector<uint16_t> yuv((size_t)w * h * 3 / 2);
  CHECK(dv_next(v, (uint8_t*)yuv.data(), (int64_t)yuv.size() * 2) >= 0, "next");
  std::set<int> lv10, lv8;
  int maxErr = 0;
  for (int x = 0; x < w; x++) {
    int y = yuv[(size_t)8 * w + x] >> 6;
    lv10.insert(y);
    lv8.insert(y >> 2);  // what an 8-bit master would keep
    int want = (int)std::lround(64 + x * 876.0 / 1023.0);
    maxErr = std::max(maxErr, std::abs(y - want));
  }
  std::printf("  P010 luma: %zu distinct 10-bit levels; truncated to 8 bit: %zu\n", lv10.size(), lv8.size());
  CHECK(lv10.size() > 700, "10-bit levels %zu (want > 700, 8-bit truncation allows <= 256)", lv10.size());
  CHECK(lv8.size() <= 256, "8-bit levels %zu", lv8.size());
  CHECK(maxErr <= 10, "ramp max error %d codes (lossy source, want <= 10)", maxErr);
  // scaled to half width: still > 256 levels (scaling happens at 10+ bits)
  CHECK(dv_begin(v, 0, 0, 512, 0, DV_P010, 0, &w, &h) == 0, "begin p010 scaled");
  yuv.assign((size_t)w * h * 3 / 2, 0);
  CHECK(dv_next(v, (uint8_t*)yuv.data(), (int64_t)yuv.size() * 2) >= 0, "next scaled");
  std::set<int> sc;
  for (int x = 0; x < w; x++) sc.insert(yuv[(size_t)(h / 2) * w + x] >> 6);
  CHECK(sc.size() > 256 || (int)sc.size() >= w - 8, "scaled ramp keeps %zu distinct levels over %d px", sc.size(), w);
  // 16-bit RGB still master: R channel distinct levels
  CHECK(dv_begin(v, 0, 0, 0, 0, DV_RGBA64, 0, &w, &h) == 0, "begin rgba64");
  std::vector<uint16_t> rgb((size_t)w * h * 4);
  CHECK(dv_next(v, (uint8_t*)rgb.data(), (int64_t)rgb.size() * 2) >= 0, "next rgba64");
  std::set<int> r16, r8;
  for (int x = 0; x < w; x++) { int r = rgb[((size_t)8 * w + x) * 4]; r16.insert(r); r8.insert(r >> 8); }
  CHECK(r16.size() > 700 && r8.size() <= 256, "RGBA64 R levels %zu (8-bit equivalent %zu)", r16.size(), r8.size());
  // 8-bit dithered output: no banding. Average over 16-px blocks must follow the ramp within 0.6 of an 8-bit step.
  CHECK(dv_begin(v, 0, 0, 0, 0, DV_RGBA, 0, &w, &h) == 0, "begin rgba");
  std::vector<uint8_t> px((size_t)w * h * 4);
  CHECK(dv_next(v, px.data(), (int64_t)px.size()) >= 0, "next rgba");
  double worst = 0;
  for (int x0 = 64; x0 + 16 <= w - 64; x0 += 16) {
    double acc = 0;
    for (int dx = 0; dx < 16; dx++)
      for (int y = 0; y < 16; y++) acc += px[((size_t)y * w + x0 + dx) * 4];
    acc /= 256.0;
    double want = (rgb[((size_t)8 * w + x0 + 8) * 4]) / 257.0;
    worst = std::max(worst, std::fabs(acc - want));
  }
  CHECK(worst < 0.7, "dithered 8-bit block mean deviates %.2f levels from the 16-bit ramp", worst);
  dv_close(v);
}

// S-Log3: Sony tags it BT.709; it is found from the NRT metadata box. Reference values from the published formulas.
static double refSlog3(double code) {
  double x = code / 1023.0;
  return x >= 171.2102946929 / 1023.0 ? std::pow(10.0, (code - 420.0) / 261.5) * 0.19 - 0.01 : (code - 95.0) * 0.01125 / (171.2102946929 - 95.0);
}
static double refDisplay(double lin) {  // documented display transform: exposure 0.8, ACES (Narkowicz), sRGB OETF
  double x = std::max(lin, 0.0) * 0.8;
  double y = std::min(1.0, std::max(0.0, (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14)));
  return y <= 0.0031308 ? 12.92 * y : 1.055 * std::pow(y, 1 / 2.4) - 0.055;
}
static void slogTest(const std::string& dir) {
  std::printf("[slog3_h264_422_10.mp4]\n");
  // published anchor points: 18 % grey at code 420, black at 95, 90 % white at 598
  CHECK(std::fabs(dv_slog3_to_linear(420.0 / 1023.0) - 0.18) < 1e-3, "S-Log3 420 -> %f", dv_slog3_to_linear(420.0 / 1023.0));
  CHECK(std::fabs(dv_slog3_to_linear(95.0 / 1023.0)) < 1e-6, "S-Log3 95 -> %g", dv_slog3_to_linear(95.0 / 1023.0));
  CHECK(std::fabs(dv_slog3_to_linear(598.0 / 1023.0) - 0.90) < 2e-3, "S-Log3 598 -> %f", dv_slog3_to_linear(598.0 / 1023.0));
  // S-Gamut3.Cine -> BT.709: rows sum to 1 (D65 white preserved); published matrix (Sony / OCIO) first row 1.6269474099
  double m[9];
  dv_sgamut3cine_to_709(m);
  std::printf("  S-Gamut3.Cine->709 row0 = %.10f %.10f %.10f\n", m[0], m[1], m[2]);
  for (int r = 0; r < 3; r++) CHECK(std::fabs(m[r * 3] + m[r * 3 + 1] + m[r * 3 + 2] - 1.0) < 1e-6, "row %d sum", r);
  CHECK(std::fabs(m[0] - 1.6269474099) < 2e-4 && std::fabs(m[1] + 0.5401385860) < 2e-4 && std::fabs(m[2] + 0.0868088239) < 2e-4, "matrix differs from the published values");

  DvInfo info{};
  DvVideo* v = openF(dir, "slog3_h264_422_10.mp4", SW, &info);
  CHECK(v != nullptr, "open");
  if (!v) return;
  CHECK(info.slogDetected == 1 && info.colorKind == DV_KIND_SLOG3, "detected=%d kind=%d gamma='%s'", info.slogDetected, info.colorKind, info.captureGamma);
  CHECK(std::strcmp(info.captureGamma, "s-log3-cine") == 0 && std::strcmp(info.capturePrimaries, "s-gamut3-cine") == 0, "gamma '%s' primaries '%s'", info.captureGamma, info.capturePrimaries);
  CHECK(info.matrix == 1, "stream is tagged BT.709 (matrix %d primaries %d trc %d); the colour comes from the NRT metadata", info.matrix, info.primaries, info.trc);
  int w, h;
  CHECK(dv_begin(v, 0, 0, 0, 0, DV_RGBA64, 0, &w, &h) == 0, "begin");
  std::vector<uint16_t> rgb((size_t)w * h * 4);
  CHECK(dv_next(v, (uint8_t*)rgb.data(), (int64_t)rgb.size() * 2) >= 0, "next");
  const double codes[5] = {95, 420, 598, 300, 700};
  double worst = 0;
  for (int i = 0; i < 5; i++) {
    const uint16_t* p = &rgb[((size_t)24 * w + i * 64 + 32) * 4];
    double want = refDisplay(refSlog3(codes[i])) * 65535.0;
    double got = (p[0] + p[1] + p[2]) / 3.0;
    std::printf("  code %4.0f: display %5.0f (ref %5.0f)  rgb %u %u %u\n", codes[i], got, want, p[0], p[1], p[2]);
    // the source is lossy 10-bit 4:2:2 and the sws YUV->RGB rounds: allow 1.2 % of full scale in sRGB-encoded terms
    worst = std::max(worst, std::fabs(got - want) / 65535.0);
    CHECK(std::abs((int)p[0] - (int)p[1]) < 300 && std::abs((int)p[1] - (int)p[2]) < 300, "neutral patch %d is tinted", i);
  }
  CHECK(worst < 0.012, "S-Log3 display error %.4f of full scale", worst);
  double black = rgb[((size_t)24 * w + 32) * 4], grey = rgb[((size_t)24 * w + 96) * 4], white = rgb[((size_t)24 * w + 160) * 4];
  CHECK(black < 3000 && grey > 28000 && grey < 36000 && white > 50000, "not flat: black %.0f grey %.0f white %.0f (16-bit)", black, grey, white);
  dv_close(v);

  // forced standard: the same file is shown flat (no conversion), proving the option does something
  DvOpenOptions std_{0, DV_SRC_STANDARD};
  v = openF(dir, "slog3_h264_422_10.mp4", std_, &info);
  CHECK(v && info.colorKind == DV_KIND_STANDARD, "forced standard kind %d", info.colorKind);
  if (v) {
    CHECK(dv_begin(v, 0, 0, 0, 0, DV_RGBA, 0, &w, &h) == 0, "begin");
    std::vector<uint8_t> px((size_t)w * h * 4);
    CHECK(dv_next(v, px.data(), (int64_t)px.size()) >= 0, "next");
    int g8 = px[((size_t)24 * w + 96) * 4];
    CHECK(std::abs(g8 - (int)std::lround((420 - 64) / 876.0 * 255)) <= 3, "flat grey patch = %d (want ~104)", g8);
    dv_close(v);
  }
  // an ordinary Sony Rec.709 clip with NRT metadata is not S-Log
  v = openF(dir, "sdr_h264_422_10_rec709.mp4", SW, &info);
  CHECK(v && info.slogDetected == 0 && info.colorKind == DV_KIND_STANDARD && std::strcmp(info.captureGamma, "rec709") == 0, "rec709 clip: slog=%d kind=%d gamma='%s'", info.slogDetected, info.colorKind, info.captureGamma);
  if (v) dv_close(v);
  // forced S-Log3 on a file without metadata works too (the tool-page 'S-Log3' choice)
  DvOpenOptions forced{0, DV_SRC_SLOG3};
  v = openF(dir, "sdr_h264_422_10.mp4", forced, &info);
  CHECK(v && info.colorKind == DV_KIND_SLOG3 && info.slogDetected == 0, "forced S-Log3 kind %d", info.colorKind);
  if (v) dv_close(v);
  // S-Log3 master clip is converted to SDR BT.709 (tags 1/1/1), never passed through as BT.709-tagged log
  v = openF(dir, "slog3_h264_422_10.mp4", SW, &info);
  if (v) {
    CHECK(dv_begin(v, 0, 0, 0, 0, DV_P010, 0, &w, &h) == 0, "begin p010");
    std::vector<uint16_t> q((size_t)w * h * 3 / 2);
    CHECK(dv_next(v, (uint8_t*)q.data(), (int64_t)q.size() * 2) >= 0, "next p010");
    int tp, tt, tm, tr;
    dv_output_tags(v, &tp, &tt, &tm, &tr);
    CHECK(tp == 1 && tt == 1 && tm == 1, "tags %d/%d/%d", tp, tt, tm);
    int y10 = q[(size_t)24 * w + 96] >> 6;  // grey patch: display ~0.49 sRGB -> 709 limited ~ 64 + 876*0.49
    CHECK(y10 > 440 && y10 < 560, "S-Log3 grey in the 10-bit master = %d", y10);
    dv_close(v);
  }
}

// Hardware / software choice. The probe's verdict arrives as hwPref; the core only uses MediaCodec for what FFmpeg's
// wrapper can return (8-bit 4:2:0) and falls back to software on any failure.
static void hardwareTest(const std::string& dir) {
  std::printf("[hardware path]\n");
  DvInfo info{};
  // positive control: 8-bit 4:2:0 H.264 takes the hardware (MediaCodec) decoder
  DvVideo* v = openF(dir, "hw_h264_420_8.mp4", HW, &info);
  CHECK(v != nullptr, "open");
  std::vector<uint8_t> hwpx, swpx;
  int w = 0, h = 0;
  if (v) {
    CHECK(info.hardware == 1 && std::strcmp(info.decoder, "h264_mediacodec") == 0, "8-bit 4:2:0 H.264: decoder '%s' hardware=%d (want h264_mediacodec)", info.decoder, info.hardware);
    CHECK(dv_begin(v, 0, 0, 0, 0, DV_RGBA, 0, &w, &h) == 0, "begin");
    hwpx.resize((size_t)w * h * 4);
    CHECK(dv_next(v, hwpx.data(), (int64_t)hwpx.size()) >= 0, "next (hw)");
    char name[32];
    int hw = dv_decoder(v, name, sizeof name);
    std::printf("  after decoding a frame: decoder '%s' hardware=%d\n", name, hw);
    CHECK(hw == 1, "hardware decoder fell back to software while decoding a plain 8-bit clip");
    dv_close(v);
  }
  v = openF(dir, "hw_h264_420_8.mp4", SW, &info);
  if (v) {
    CHECK(info.hardware == 0 && std::strcmp(info.decoder, "h264") == 0, "software decoder '%s'", info.decoder);
    CHECK(dv_begin(v, 0, 0, 0, 0, DV_RGBA, 0, &w, &h) == 0, "begin");
    swpx.resize((size_t)w * h * 4);
    CHECK(dv_next(v, swpx.data(), (int64_t)swpx.size()) >= 0, "next (sw)");
    dv_close(v);
  }
  if (!hwpx.empty() && hwpx.size() == swpx.size()) {
    double d = 0;
    for (size_t i = 0; i < hwpx.size(); i++) if (i % 4 != 3) d += std::abs((int)hwpx[i] - (int)swpx[i]);
    d /= (double)(hwpx.size() * 3 / 4);
    std::printf("  hardware vs software frame: mean abs difference %.2f / 255\n", d);
    CHECK(d < 4.0, "hardware and software frames disagree: mean abs diff %.2f (colour handling differs)", d);
  }
  // HEVC Main 8-bit
  v = openF(dir, "hw_hevc_420_8.mp4", HW, &info);
  CHECK(v != nullptr, "open hevc");
  if (v) {
    std::printf("  HEVC Main 8-bit: decoder '%s' hardware=%d\n", info.decoder, info.hardware);
    CHECK(dv_begin(v, 0, 0, 0, 0, DV_RGBA, 0, &w, &h) == 0, "begin");
    std::vector<uint8_t> px((size_t)w * h * 4);
    CHECK(dv_next(v, px.data(), (int64_t)px.size()) >= 0, "next hevc (hw or fallback)");
    dv_close(v);
  }
  // negative controls: 10-bit 4:2:2 (Sony) and 10-bit 4:2:0 HEVC are software even when hardware is allowed
  v = openF(dir, "sdr_h264_422_10.mp4", HW, &info);
  CHECK(v && info.hardware == 0 && std::strcmp(info.decoder, "h264") == 0, "Sony-like 10-bit 4:2:2 H.264 must use software, got '%s'", info.decoder);
  if (v) dv_close(v);
  v = openF(dir, "hlg_hevc_420_10.mp4", HW, &info);
  CHECK(v && info.hardware == 0, "10-bit HEVC through FFmpeg's wrapper is software (hardware %d)", info.hardware);
  if (v) dv_close(v);
}


// NDK AMediaCodec decoder (hwPref 2 / 3): the path that returns P010 for 10-bit 4:2:0 on devices with a Main10 hardware decoder.
// The emulator has no Main10 decoder, so the plumbing (Annex B conversion, csd, buffer layout, pts, seek/flush, EOS, release)
// is verified on 8-bit streams (hwPref 3 = NDK decoder for any 4:2:0, NV12 output) against the software decoder, and the real
// 10-bit request is checked to either deliver a correct picture (P010) or fall back to software without error.
static int countFrames(DvVideo* v, std::vector<uint8_t>& last, int fmt) {
  int w = 0, h = 0;
  if (dv_begin(v, 0, 1000000, 0, 0, fmt, 0, &w, &h) != 0) return -1;
  last.assign((size_t)dv_frame_bytes(v), 0);
  int n = 0;
  while (dv_next(v, last.data(), (int64_t)last.size()) >= 0) n++;
  return n;
}
static void nativeDecoderTest(const std::string& dir) {
  std::printf("[native AMediaCodec decoder]\n");
  DvOpenOptions NAT{3, DV_SRC_AUTO, nullptr};
  for (const char* f : {"hw_h264_420_8.mp4", "hw_hevc_420_8.mp4"}) {
    DvInfo ni{}, si{};
    DvVideo* a = openF(dir, f, NAT, &ni);
    DvVideo* b = openF(dir, f, SW, &si);
    CHECK(a && b, "open %s", f);
    if (!a || !b) { if (a) dv_close(a); if (b) dv_close(b); continue; }
    std::vector<uint8_t> la, lb;
    int na = countFrames(a, la, DV_RGBA), nb = countFrames(b, lb, DV_RGBA);
    char name[32];
    int hw = dv_decoder(a, name, sizeof name);
    std::printf("  %s: native frames %d vs software %d, decoder '%s' hw=%d (opened as '%s')\n", f, na, nb, name, hw, ni.decoder);
    CHECK(na == nb && na > 0, "%s: frame count %d vs %d", f, na, nb);
    if (hw && la.size() == lb.size()) {
      double d = 0;
      for (size_t i = 0; i < la.size(); i++) if (i % 4 != 3) d += std::abs((int)la[i] - (int)lb[i]);
      d /= (double)(la.size() * 3 / 4);
      std::printf("    last frame, native vs software: mean abs difference %.2f / 255\n", d);
      CHECK(d < 4.0, "%s: native decoder frame differs from software by %.2f", f, d);
    }
    // seek/flush: a second pass from the middle must still deliver frames
    int w = 0, h = 0;
    CHECK(dv_begin(a, 300000, 600000, 0, 0, DV_RGBA, 0, &w, &h) == 0, "second pass begin");
    std::vector<uint8_t> px((size_t)w * h * 4);
    int n2 = 0;
    while (dv_next(a, px.data(), (int64_t)px.size()) >= 0) n2++;
    CHECK(n2 >= 5, "%s: second pass delivered %d frames", f, n2);
    dv_close(a);
    dv_close(b);
  }
  // 10-bit 4:2:0 HEVC, native decoder requested for P010: correct picture either way
  {
    DvOpenOptions N2{2, DV_SRC_AUTO, nullptr};
    DvInfo info{}, si{};
    DvVideo* v = openF(dir, "hlg_hevc_420_10.mp4", N2, &info);
    DvVideo* s = openF(dir, "hlg_hevc_420_10.mp4", SW, &si);
    CHECK(v && s, "open hlg 10-bit");
    if (v && s) {
      std::vector<uint8_t> a, b;
      int na = countFrames(v, a, DV_P010), nb = countFrames(s, b, DV_P010);
      char name[32];
      int hw = dv_decoder(v, name, sizeof name);
      std::printf("  10-bit HEVC P010: decoder '%s' hardware=%d, frames %d vs software %d\n", name, hw, na, nb);
      CHECK(na == nb && na > 0, "10-bit frame count %d vs %d", na, nb);
      if (a.size() == b.size() && na > 0) {
        double d = 0;
        const uint16_t* pa = (const uint16_t*)a.data();
        const uint16_t* pb = (const uint16_t*)b.data();
        for (size_t i = 0; i < a.size() / 2; i++) d += std::abs((int)pa[i] - (int)pb[i]);
        d /= (double)(a.size() / 2);
        std::printf("    P010 words, native-or-fallback vs software: mean abs difference %.2f (16-bit scale)\n", d);
        CHECK(d < 64.0, "10-bit frame differs from software by %.2f", d);
      }
    }
    if (v) dv_close(v);
    if (s) dv_close(s);
  }
  // the self-test asset: a real Main10 stream with a 10-bit ramp (software decode must show > 256 luma levels)
  {
    DvInfo gi{};
    DvVideo* g = openF(dir, "gradient_hevc_main10.mp4", SW, &gi);
    CHECK(g && gi.bitDepth == 10 && gi.chroma == 1 && gi.width == 640 && gi.height == 352, "gradient asset format");
    if (g) {
      std::vector<uint8_t> px;
      int n = countFrames(g, px, DV_P010);
      std::set<int> lv;
      const uint16_t* y = (const uint16_t*)px.data();
      for (int x = 0; x < 640; x++) lv.insert(y[(size_t)176 * 640 + x] >> 6);
      std::printf("  gradient_hevc_main10.mp4: %d frames, %zu distinct 10-bit luma levels in one row\n", n, lv.size());
      CHECK(n == 3 && lv.size() > 256, "gradient asset: %d frames, %zu levels", n, lv.size());
      dv_close(g);
    }
  }
  // report round trip
  dv_set_codec_report("{\"x\":1}");
  char rep[32] = {0};
  CHECK(dv_codec_report(rep, sizeof rep) == 7 && std::strcmp(rep, "{\"x\":1}") == 0, "codec report round trip '%s'", rep);
  dv_set_codec_report("");
}

static void audioTest(const std::string& dir) {
  std::printf("[lpcm_h264_420_8.mp4]  audio\n");
  DvInfo info{};
  DvVideo* v = openF(dir, "lpcm_h264_420_8.mp4", SW, &info);
  CHECK(v != nullptr, "open");
  if (!v) return;
  CHECK(std::strcmp(info.audioCodec, "pcm_s16be") == 0 && info.audioRate == 48000 && info.audioChannels == 2 && info.audioDecodable, "audio %s %d Hz %d ch decodable=%d", info.audioCodec, info.audioRate, info.audioChannels, info.audioDecodable);
  int rate = 0, ch = 0;
  CHECK(dv_audio_begin(v, 0, 1000000, &rate, &ch) == 0 && rate == 48000 && ch == 2, "audio begin %d/%d", rate, ch);
  std::vector<int16_t> all, buf(2 * 4096);
  int n;
  int64_t pts = -1, firstPts = -1;
  while ((n = dv_audio_next(v, buf.data(), 4096, &pts)) > 0) {
    if (firstPts < 0) firstPts = pts;
    all.insert(all.end(), buf.begin(), buf.begin() + (size_t)n * 2);
  }
  size_t frames = all.size() / 2;
  CHECK(frames >= 47000 && frames <= 48200, "1 s of audio -> %zu sample frames", frames);
  int peakL = 0, peakR = 0, zcL = 0, zcR = 0;
  for (size_t i = 1; i < frames; i++) {
    int l = all[i * 2], r = all[i * 2 + 1];
    peakL = std::max(peakL, std::abs(l)); peakR = std::max(peakR, std::abs(r));
    zcL += (all[(i - 1) * 2] < 0) != (l < 0);
    zcR += (all[(i - 1) * 2 + 1] < 0) != (r < 0);
  }
  std::printf("  peak %d / %d, zero crossings %d / %d (440 / 880 Hz sine -> ~880 / ~1760)\n", peakL, peakR, zcL, zcR);
  CHECK(peakL > 3000 && peakR > 3000, "audio present: peaks %d / %d", peakL, peakR);
  CHECK(std::abs(zcL - 880) < 30 && std::abs(zcR - 1760) < 40, "frequencies off: %d / %d", zcL, zcR);
  // trimmed range
  CHECK(dv_audio_begin(v, 200000, 500000, &rate, &ch) == 0, "audio begin range");
  size_t total = 0;
  firstPts = -1;
  while ((n = dv_audio_next(v, buf.data(), 4096, &pts)) > 0) { if (firstPts < 0) firstPts = pts; total += (size_t)n; }
  CHECK(total >= 14000 && total <= 14800 && std::llabs(firstPts - 200000) < 25000, "0.3 s range -> %zu frames, first at %lld us", total, (long long)firstPts);
  dv_close(v);
  // a clip without audio reports none
  v = openF(dir, "hw_h264_420_8.mp4", SW, &info);
  if (v) {
    CHECK(info.audioCodec[0] == 0 && dv_audio_begin(v, 0, 1000000, &rate, &ch) < 0, "no audio expected");
    dv_close(v);
  }
}

// "fd:<n>" input (what the app passes for content:// videos): the metadata scan and the audio demuxer must not disturb the
// file offset FFmpeg reads from (a dup shares it).
static void fdTest(const std::string& dir) {
  std::printf("[fd: input]\n");
  struct Case { const char* file; int frames; bool slog; bool audio; } cases[] = {
      {"slog3_h264_422_10.mp4", 10, true, false}, {"hw_h264_420_8.mp4", 15, false, false}, {"lpcm_h264_420_8.mp4", 25, false, true}};
  for (auto& c : cases) {
    int fd = open((dir + "/" + c.file).c_str(), O_RDONLY);
    CHECK(fd >= 0, "open %s", c.file);
    if (fd < 0) continue;
    DvInfo info{};
    char err[256];
    DvOpenOptions o = HW;
    DvVideo* v = dv_open(("fd:" + std::to_string(fd)).c_str(), &o, &info, err, sizeof err);
    CHECK(v != nullptr, "open fd: %s", err);
    if (!v) continue;
    CHECK((info.slogDetected == 1) == c.slog, "%s: slogDetected=%d through the descriptor", c.file, info.slogDetected);
    int w, h, n = 0;
    CHECK(dv_begin(v, 0, 10000000, 0, 0, DV_RGBA, 0, &w, &h) == 0, "begin");
    std::vector<uint8_t> px((size_t)w * h * 4);
    while (dv_next(v, px.data(), (int64_t)px.size()) >= 0) n++;
    CHECK(n == c.frames, "%s via fd: %d frames (want %d)", c.file, n, c.frames);
    if (c.audio) {
      int rate, ch;
      CHECK(dv_audio_begin(v, 0, 500000, &rate, &ch) == 0, "audio through the descriptor");
      std::vector<int16_t> a(2 * 4096);
      int64_t pts;
      int total = 0, k;
      while ((k = dv_audio_next(v, a.data(), 4096, &pts)) > 0) total += k;
      CHECK(total > 23000 && total < 25000, "0.5 s of audio via fd: %d frames", total);
    }
    // seek back after the pass and decode again (the buffered-seek path)
    CHECK(dv_begin(v, 200000, 200000, 0, 0, DV_RGBA, 0, &w, &h) == 0 && dv_next(v, px.data(), (int64_t)px.size()) >= 0, "second pass");
    dv_close(v);  // closes the descriptor it owns
  }
}

static uint16_t toHalf(float f) {
  union { float f; uint32_t u; } v{f};
  uint32_t s = (v.u >> 16) & 0x8000u;
  int32_t e = (int32_t)((v.u >> 23) & 0xff) - 127 + 15;
  uint32_t m = v.u & 0x7fffffu;
  if (e <= 0) return (uint16_t)s;
  if (e >= 31) return (uint16_t)(s | 0x7bffu);
  uint32_t r = s | ((uint32_t)e << 10) | (m >> 13);
  if (m & 0x1000u) r++;
  return (uint16_t)r;
}
static float fromHalf(uint16_t hv) {
  if ((hv & 0x7fff) == 0) return 0;
  return std::ldexp((float)((hv & 0x3ff) | 0x400), (int)((hv >> 10) & 0x1f) - 25);
}
// Ultra HDR pair: reconstructing with the Android Gainmap formula must give back the HDR input.
static void ultraHdrTest() {
  std::printf("[ultra hdr gain map]\n");
  const int w = 64, h = 32;
  std::vector<uint16_t> src((size_t)w * h * 4);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      float v = 0.02f + 3.5f * (float)x / (w - 1);  // linear 0.02 .. 3.5 (SDR white = 1)
      uint16_t* p = &src[((size_t)y * w + x) * 4];
      p[0] = toHalf(v); p[1] = toHalf(v); p[2] = toHalf(v); p[3] = 0x3c00;
    }
  std::vector<uint8_t> sdr((size_t)w * h * 4), gain((size_t)(w / 2) * (h / 2));
  int gw = 0, gh = 0;
  float meta[5];
  CHECK(dv_make_ultrahdr(src.data(), w, h, sdr.data(), gain.data(), &gw, &gh, meta) == 0 && gw == w / 2 && gh == h / 2, "make");
  std::printf("  ratio %.3f .. %.3f gamma %.1f epsS %.4f epsH %.4f\n", meta[0], meta[1], meta[2], meta[3], meta[4]);
  CHECK(meta[1] > 1.5f && meta[0] <= 1.0f, "ratio range");
  double worst = 0;
  for (int x = 2; x < w - 2; x += 2) {
    float v = 0.02f + 3.5f * (float)x / (w - 1);
    float s = sdr[(size_t)(h / 2) * w * 4 + (size_t)x * 4] / 255.0f;
    float sl = s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f);
    float rec = gain[(size_t)(h / 4) * gw + x / 2] / 255.0f;
    float lg = std::log2(meta[0]) * (1 - rec) + std::log2(meta[1]) * rec;
    float hdr = (sl + meta[3]) * std::exp2(lg) - meta[4];
    worst = std::max(worst, (double)std::fabs(hdr - v) / std::max(v, 0.25f));
    (void)fromHalf;
  }
  CHECK(worst < 0.08, "HDR reconstruction error %.3f (relative)", worst);
  CHECK(sdr[3] == 255, "alpha");
}

int main(int argc, char** argv) {
  std::string dir = argc > 1 ? argv[1] : ".";
  std::printf("ffmpeg %s\n", dv_version());
  colourTest(dir, "sdr_h264_422_10.mp4", 10, 2, "h264");
  colourTest(dir, "sdr_hevc_422_10.mp4", 10, 2, "hevc");
  colourTest(dir, "sdr_h264_420_8.mp4", 8, 1, "h264");
  hlgTest(dir);
  precisionTest(dir);
  slogTest(dir);
  hardwareTest(dir);
  nativeDecoderTest(dir);
  audioTest(dir);
  fdTest(dir);
  ultraHdrTest();
  DvInfo info;
  char err[128];
  DvOpenOptions o{0, 0, nullptr};
  CHECK(dv_open((dir + "/missing.mp4").c_str(), &o, &info, err, sizeof err) == nullptr, "missing file must fail");
  std::printf("%d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
