// Display P3 export (colorSpace "p3" of dapple_session_export_jpeg_ex / _hdr_ex).
// `dapple_p3_tests <id>`; ids: identity tags chroma direct inside hue parity hdr fraction evidence.
// Everything goes through the C API on a synthetic Display P3 chart (a JPEG with the P3 profile: hue x chroma x lightness, plus a grey ramp), so the
// import (colour managed, section 35), the render, the export engine and the encoder are the shipped ones. Bounds (dE = CIE76 on D65 linear sRGB):
//   inside   P3 export converted to sRGB against the sRGB export, over the pixels whose P3 result lies inside sRGB: max <= 1.6, mean <= 0.35 (two
//            8-bit quantisations of different primaries; the matching is exact in float).
//   chroma   P3 export against the (decoded) P3 source, default edit: p99 <= 2.0, max <= 3.5 (dark saturated corners, dE76 amplifies 8-bit steps; the source JPEG's own 8-bit grid and the pipeline's rounding).
//   hue      OKLab hue of a +70 saturation edit against the source: P3 mode no worse than sRGB mode + 2 degrees (99th percentile).
//   parity   CPU against GPU render of the P3 export: max 8-bit difference <= 2.
// `evidence` writes build/p3/ (PNG maps, numbers) for the chart and any RAW samples that exist.

#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "color.h"
#include "dapple/dapple_c.h"
#include "dapple/camera.h"
#include "dapple/decoder.h"
#include "dapple/encode.h"
#include "dapple/edit_params.h"
#include "dapple/renderer.h"
#include "dapple/ultrahdr.h"
#include "exif_writer.h"

namespace fs = std::filesystem;
using namespace dapple;
using Bytes = std::vector<std::uint8_t>;

static int g_fail = 0;
#define CHECK(c)                                                  \
  do {                                                            \
    if (!(c)) {                                                   \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); \
      ++g_fail;                                                   \
    }                                                             \
  } while (0)

static fs::path root() {
#ifdef DAPPLE_SOURCE_DIR
  return fs::path(DAPPLE_SOURCE_DIR);
#else
  return fs::current_path();
#endif
}
static Bytes readAll(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
static void writeAll(const fs::path& p, const Bytes& b) {
  fs::create_directories(p.parent_path());
  std::ofstream o(p, std::ios::binary);
  o.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}

// ---- colour maths (independent of the renderer's own code paths, except the matrices) ---------------------------------------------------------
using color::srgbDecode;
using color::srgbEncode;
struct Rgb8 {
  int w = 0, h = 0;
  std::vector<std::uint8_t> px;
};
static Rgb8 decodeJpeg(const Bytes& b) {
  Rgb8 o;
  int c = 0;
  std::uint8_t* d = stbi_load_from_memory(b.data(), int(b.size()), &o.w, &o.h, &c, 3);
  if (d) o.px.assign(d, d + std::size_t(o.w) * o.h * 3);
  stbi_image_free(d);
  return o;
}
// the 8-bit pixel i of an image holding `p3` (Display P3) or sRGB codes -> linear sRGB (unbounded)
static void linSrgb(const Rgb8& im, std::size_t i, bool p3, float out[3]) {
  const float v[3] = {srgbDecode(im.px[3 * i] / 255.f), srgbDecode(im.px[3 * i + 1] / 255.f), srgbDecode(im.px[3 * i + 2] / 255.f)};
  if (p3) color::p3ToSrgb().apply(v, out);
  else out[0] = v[0], out[1] = v[1], out[2] = v[2];
}
static float cbrtS(float x) { return std::cbrt(x); }
static void lab(const float s[3], float o[3]) {
  const float X = 0.4124564f * s[0] + 0.3575761f * s[1] + 0.1804375f * s[2];
  const float Y = 0.2126729f * s[0] + 0.7151522f * s[1] + 0.0721750f * s[2];
  const float Z = 0.0193339f * s[0] + 0.1191920f * s[1] + 0.9503041f * s[2];
  auto f = [](float t) { return t > 0.008856f ? cbrtS(t) : 7.787f * t + 16.f / 116.f; };
  const float fx = f(X / 0.95047f), fy = f(Y), fz = f(Z / 1.08883f);
  o[0] = 116.f * fy - 16.f;
  o[1] = 500.f * (fx - fy);
  o[2] = 200.f * (fy - fz);
}
static float dE(const float a[3], const float b[3]) {
  float la[3], lb[3];
  lab(a, la);
  lab(b, lb);
  return std::sqrt((la[0] - lb[0]) * (la[0] - lb[0]) + (la[1] - lb[1]) * (la[1] - lb[1]) + (la[2] - lb[2]) * (la[2] - lb[2]));
}
static bool insideSrgb(const float s[3], float tol = 0.004f) {
  return std::min({s[0], s[1], s[2]}) >= -tol && std::max({s[0], s[1], s[2]}) <= 1.f + tol;
}
static void oklch(const float s[3], float& L, float& C, float& hDeg) {
  float l[3];
  color::linearSrgbToOklab(s, l);
  L = l[0];
  C = std::hypot(l[1], l[2]);
  hDeg = std::atan2(l[2], l[1]) * 180.f / color::kPi;
  if (hDeg < 0) hDeg += 360.f;
}

// ---- the chart --------------------------------------------------------------------------------------------------------------------------------
// 1800 x 1200: three lightness bands (OKLab L 0.35 / 0.55 / 0.75) of 360 hues x a chroma ramp 0.01 .. 0.40 (clipped to the P3 cube, so the saturated
// end is the P3 boundary), then a grey ramp. `limit` caps the chroma (modern renderings are measured on the part away from the sRGB knee).
static constexpr int kW = 1800, kH = 1200, kPatch = 24;
// Flat 24 px patches, compared on their 12 px interiors: the exported JPEG (4:2:0 chroma) is faithful there, so the numbers below measure the colour
// pipeline, not the codec.
static int g_colStep = 1;  // the small twins keep every third hue column of the chart (one patch in three, the same colours): a third of the pixels
static bool interior(std::size_t i) {
  const int cw = kW / g_colStep;
  const int x = int(i % std::size_t(cw)) % kPatch, y = int(i / std::size_t(cw)) % kPatch;
  return x >= 6 && x < 18 && y >= 6 && y < 18;
}
static Image8 makeChart(float maxChroma) {
  Image8 im;
  const int cw = kW / g_colStep;
  im.width = cw;
  im.height = kH;
  im.data.assign(std::size_t(cw) * kH * 3, 0);
  const int bandH = 360, rampY = 3 * bandH;
  const float Ls[3] = {0.35f, 0.55f, 0.75f};
  for (int y = 0; y < kH; ++y)
    for (int xc = 0; xc < cw; ++xc) {
      const int x = (xc / kPatch) * kPatch * g_colStep + xc % kPatch;  // the column of the full chart this one stands for
      float p[3];
      if (y < rampY) {
        const int b = y / bandH;
        const float C = 0.01f + (maxChroma - 0.01f) * float((y % bandH) / kPatch) / (bandH / kPatch - 1);
        const float h = float(x / kPatch * kPatch + kPatch / 2) / kW * 2.f * color::kPi;
        const float labv[3] = {Ls[b], C * std::cos(h), C * std::sin(h)};
        float s[3];
        color::oklabToLinearSrgb(labv, s);
        color::srgbToP3().apply(s, p);
      } else {
        const float g = srgbDecode(float(x / kPatch * kPatch + kPatch / 2) / (kW - 1));
        p[0] = p[1] = p[2] = g;
      }
      for (int c = 0; c < 3; ++c) im.data[(std::size_t(y) * cw + xc) * 3 + c] = std::uint8_t(srgbEncode(std::clamp(p[c], 0.f, 1.f)) * 255.f + 0.5f);
    }
  return im;
}
static fs::path outDir() {
  const fs::path d = root() / "build" / "p3-test";
  fs::create_directories(d);
  return d;
}
static fs::path chartPath(float maxChroma, const char* name) {
  const fs::path p = outDir() / name;
  JpegOptions jo;
  jo.quality = 100;
  jo.iccSegment = buildIccSegment(IccSpace::DisplayP3);
  writeAll(p, *encodeJpeg(makeChart(maxChroma), jo));
  return p;
}

struct Session {
  DappleSession* s = nullptr;
  explicit Session(const fs::path& f, int longEdge = 0) { s = dapple_session_open(f.string().c_str(), DAPPLE_QUALITY_FULL, longEdge); }
  ~Session() {
    if (s) dapple_session_close(s);
  }
  Rgb8 exportAs(const std::string& params, const char* space, int longEdge, Bytes* raw = nullptr, const char* name = "x.jpg") const {
    const fs::path o = outDir() / name;
    const std::string opt = std::string("{\"colorSpace\":\"") + space + "\"}";
    const int rc = dapple_session_export_jpeg_ex(s, params.c_str(), "", longEdge, 100, o.string().c_str(), opt.c_str());
    if (rc != DAPPLE_OK) std::printf("  export failed: %s\n", dapple_last_error());
    CHECK(rc == DAPPLE_OK);
    const Bytes b = readAll(o);
    if (raw) *raw = b;
    return decodeJpeg(b);
  }
};

struct Stats {
  double sum = 0, max = 0;
  std::size_t n = 0;
  std::vector<float> all;
  void add(float v) {
    sum += v;
    max = std::max<double>(max, v);
    ++n;
    all.push_back(v);
  }
  double mean() const { return n ? sum / double(n) : 0; }
  double pct(double p) {
    if (all.empty()) return 0;
    const std::size_t k = std::min(all.size() - 1, std::size_t(double(all.size()) * p));
    std::nth_element(all.begin(), all.begin() + std::ptrdiff_t(k), all.end());
    return all[k];
  }
};

// What the 8-bit grid alone can explain: the largest dE between the pixel and the same pixel with every channel moved by half a code (8 corners).
static float quantAllow(const Rgb8& im, std::size_t i, bool p3) {
  float c0[3];
  linSrgb(im, i, p3, c0);
  float worst = 0.f;
  for (int m = 0; m < 8; ++m) {
    float v[3];
    for (int c = 0; c < 3; ++c) v[c] = srgbDecode(std::clamp((im.px[3 * i + std::size_t(c)] + ((m >> c & 1) ? 0.5f : -0.5f)) / 255.f, 0.f, 1.f));
    float s[3];
    if (p3) color::p3ToSrgb().apply(v, s);
    else s[0] = v[0], s[1] = v[1], s[2] = v[2];
    worst = std::max(worst, dE(c0, s));
  }
  return worst;
}

// P3 export against the sRGB export over the pixels whose P3 result lies inside sRGB (up to 0.0015 linear: the P3 grid's half step at the dark end).
// Two measures: the raw dE76, and the excess over what two 8-bit grids can explain (dE minus the half-code uncertainty of both pixels, >= 0).
static void insideStats(const Rgb8& p3, const Rgb8& sr, Stats& st, Stats& excess, std::size_t* nOut = nullptr, std::size_t* nWide = nullptr) {
  const std::size_t n = std::size_t(p3.w) * p3.h;
  std::size_t out = 0;
  float worstEx = 0.f;
  for (std::size_t i = 0; i < n; ++i) {
    if (!interior(i)) continue;
    float a[3], b[3];
    linSrgb(p3, i, true, a);
    linSrgb(sr, i, false, b);
    if (!insideSrgb(a, -0.004f)) {  // comfortably inside: the 0.004 band at the cube faces is where the sRGB rule pulls a hair-outside colour towards grey
      ++out;
      continue;
    }
    const float d = dE(a, b);
    st.add(d);
    const float ex = std::max(0.f, d - quantAllow(p3, i, true) - quantAllow(sr, i, false));
    excess.add(ex);
    if (ex > worstEx && std::getenv("P3_DEBUG")) {
      worstEx = ex;
      std::printf("    worst excess %.2f (dE %.2f) at (%zu,%zu) p3 lin [%.4f %.4f %.4f] srgb [%.4f %.4f %.4f] codes p3 %d %d %d sr %d %d %d\n", ex, d, i % std::size_t(p3.w), i / std::size_t(p3.w), a[0], a[1], a[2], b[0], b[1], b[2], p3.px[3 * i], p3.px[3 * i + 1], p3.px[3 * i + 2], sr.px[3 * i], sr.px[3 * i + 1], sr.px[3 * i + 2]);
    }
  }
  if (nOut) *nOut = out;
  if (nWide) *nWide = n;
}

// ---- tests ----------------------------------------------------------------------------------------------------------------------------------------
static void testIdentity() {
  const fs::path f = chartPath(0.40f, "chart.jpg");
  Session S(f);
  CHECK(S.s != nullptr);
  if (!S.s) return;
  const char* ps[] = {"{}", "{\"contrast\":30,\"saturation\":20}", "{\"look\":\"teal\"}"};
  for (const char* p : ps) {
    Bytes a, b;
    const Rgb8 x = S.exportAs(p, "srgb", 600, &a, "id_a.jpg");
    const fs::path o = outDir() / "id_b.jpg";
    CHECK(dapple_session_export_jpeg_ex(S.s, p, "", 600, 100, o.string().c_str(), "{}") == DAPPLE_OK);  // no colorSpace key = sRGB
    b = readAll(o);
    CHECK(a == b);
    const Rgb8 y = decodeJpeg(b);
    CHECK(x.px == y.px);
  }
  // the P3 option with a bad name is still an error
  const fs::path bad = outDir() / "bad.jpg";
  CHECK(dapple_session_export_jpeg_ex(S.s, "{}", "", 600, 92, bad.string().c_str(), "{\"colorSpace\":\"rec2020\"}") == DAPPLE_ERROR);
}

static bool contains(const Bytes& b, const char* s) {
  const std::size_t n = std::strlen(s);
  if (std::search(b.begin(), b.end(), s, s + n) != b.end()) return true;
  Bytes u16;  // the profile description is a UTF-16BE 'mluc' string
  for (std::size_t i = 0; i < n; ++i) u16.insert(u16.end(), {0, std::uint8_t(s[i])});
  return std::search(b.begin(), b.end(), u16.begin(), u16.end()) != b.end();
}
static void testTags() {
  const fs::path f = chartPath(0.40f, "chart.jpg");
  Session S(f);
  CHECK(S.s != nullptr);
  if (!S.s) return;
  Bytes p3, sr;
  S.exportAs("{}", "p3", 600, &p3, "t_p3.jpg");
  S.exportAs("{}", "srgb", 600, &sr, "t_sr.jpg");
  const std::string icc = "ICC_PROFILE";
  CHECK(contains(p3, "ICC_PROFILE") && contains(p3, "Display P3") && !contains(sr, "Display P3"));
  const std::uint8_t cs[10] = {0x01, 0xA0, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0xFF, 0xFF};  // Exif ColorSpace (0xA001) SHORT 1 = 0xFFFF (little endian)
  const std::uint8_t cs1[10] = {0x01, 0xA0, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00};
  CHECK(std::search(p3.begin(), p3.end(), cs, cs + 10) != p3.end());
  CHECK(std::search(sr.begin(), sr.end(), cs1, cs1 + 10) != sr.end());
  // the profile is the 476 byte v4 matrix / TRC one of exif_writer: the primaries are the P3 ones (red X 0.5151 in the D50 PCS)
  CHECK(buildIccSegment(IccSpace::DisplayP3).size() > 400);
}

static void testChroma() {
  const fs::path f = chartPath(0.40f, "chart.jpg");
  Session S(f);
  CHECK(S.s != nullptr);
  if (!S.s) return;
  const Rgb8 src = decodeJpeg(readAll(f));
  const Rgb8 p3 = S.exportAs("{}", "p3", 0, nullptr, "c_p3.jpg");
  const Rgb8 sr = S.exportAs("{}", "srgb", 0, nullptr, "c_sr.jpg");
  CHECK(p3.w == kW && sr.w == kW && src.w == kW);
  const std::size_t n = std::size_t(kW) * kH;
  Stats eP3, eSr, inP3, inSr;
  std::size_t wide = 0;
  float minLin = 0.f;
  for (std::size_t i = 0; i < n; ++i) {
    if (!interior(i)) continue;
    float s[3], a[3], b[3];
    linSrgb(src, i, true, s);
    linSrgb(p3, i, true, a);
    linSrgb(sr, i, false, b);
    if (std::getenv("P3_PIX") && (i == 8 * kW + 8 || i == 12 * kW + 899)) std::printf("    pix %zu src codes %d %d %d lin [%.4f %.4f %.4f]; p3 export codes %d %d %d; srgb export codes %d %d %d lin [%.4f %.4f %.4f]\n", i, src.px[3 * i], src.px[3 * i + 1], src.px[3 * i + 2], s[0], s[1], s[2], p3.px[3 * i], p3.px[3 * i + 1], p3.px[3 * i + 2], sr.px[3 * i], sr.px[3 * i + 1], sr.px[3 * i + 2], b[0], b[1], b[2]);
    if (insideSrgb(s, 0.0f)) {  // inside sRGB: both exports against the source
      inP3.add(dE(s, a));
      inSr.add(dE(s, b));
      continue;
    }
    if (insideSrgb(s, 0.03f)) continue;  // colour the source holds outside sRGB (the part sRGB export loses)
    ++wide;
    eP3.add(dE(s, a));
    eSr.add(dE(s, b));
    minLin = std::min(minLin, std::min({a[0], a[1], a[2]}));
  }
  std::printf("  chroma: %zu of %zu source pixels outside sRGB; dE to the source: P3 export mean %.2f p99 %.2f max %.2f, sRGB export mean %.2f max %.2f; min linear sRGB of P3 export %.3f\n",
              wide, n, eP3.mean(), eP3.pct(0.99), eP3.max, eSr.mean(), eSr.max, minLin);
  std::printf("  chroma: inside sRGB, dE to the source: P3 export mean %.3f p99 %.2f max %.2f; sRGB export mean %.3f p99 %.2f max %.2f\n", inP3.mean(), inP3.pct(0.99), inP3.max, inSr.mean(), inSr.pct(0.99), inSr.max);
  CHECK(wide > n / 40);
  CHECK(eP3.max <= 3.5 && eP3.pct(0.99) <= 2.0);
  CHECK(eSr.mean() > 3.0 * std::max(0.3, eP3.mean()));  // the sRGB export really loses it
  CHECK(minLin < -0.05f);                                // and the P3 file carries values past sRGB
}

// ---- the pipeline itself, without codec or 8-bit grid: Renderer::renderFloat in both output spaces on the chart (float, 0..1 encoded) --------------
static SourceImage chartSource(float maxChroma) {
  const Image8 c = makeChart(maxChroma);
  SourceImage src;
  src.sceneReferred = false;  // a display-referred picture, like a JPEG
  src.linear = ImageF(c.width, c.height);
  static const color::Mat3 p3ToPro = color::mul(color::kXyzD50ToProPhoto, color::mul(color::kD65ToD50, color::kP3ToXyzD65));
  for (std::size_t i = 0; i < std::size_t(c.width) * c.height; ++i) {
    const float v[3] = {srgbDecode(c.data[3 * i] / 255.f), srgbDecode(c.data[3 * i + 1] / 255.f), srgbDecode(c.data[3 * i + 2] / 255.f)};
    float p[3];
    p3ToPro.apply(v, p);
    for (int k = 0; k < 3; ++k) src.linear.data[3 * i + std::size_t(k)] = p[k];
  }
  return src;
}
static ImageF renderDirect(const SourceImage& src, const std::string& json, int space) {
  static const CameraRegistry reg = CameraRegistry::withBuiltins();
  auto e = fromJson(json);
  EditParams p = e ? *e : EditParams{};
  p.outputSpace = space;
  return Renderer(p, reg.resolve("", ""), nullptr).renderFloat(src, true);
}
static void linOf(const ImageF& im, std::size_t i, bool p3, float out[3]) {
  const float v[3] = {srgbDecode(std::clamp(im.data[3 * i], 0.f, 1.f)), srgbDecode(std::clamp(im.data[3 * i + 1], 0.f, 1.f)),
                      srgbDecode(std::clamp(im.data[3 * i + 2], 0.f, 1.f))};
  if (p3) color::p3ToSrgb().apply(v, out);
  else out[0] = v[0], out[1] = v[1], out[2] = v[2];
}

// How far a colour (linear sRGB, unbounded) is towards the sRGB boundary at constant OKLab lightness and hue: 1 = on it (the renderer's modern chroma compression
// starts at 0.88).
static float boundaryShare(const float lin[3]) {
  float l[3];
  color::linearSrgbToOklab(lin, l);
  if (std::hypot(l[1], l[2]) < 2e-3f) return 0.f;
  auto fits = [&](float k) {
    const float lab3[3] = {l[0], k * l[1], k * l[2]};
    float r[3];
    color::oklabToLinearSrgb(lab3, r);
    return std::min({r[0], r[1], r[2]}) >= -2e-4f && std::max({r[0], r[1], r[2]}) <= 1.0002f;
  };
  float lo = 0.f, hi = 4.f;
  if (fits(hi)) return 0.f;
  for (int i = 0; i < 14; ++i) {
    const float mid = 0.5f * (lo + hi);
    (fits(mid) ? lo : hi) = mid;
  }
  return lo > 1e-4f ? 1.f / lo : 1e4f;
}

struct Direct {
  const char* name;
  std::string params;
  float maxChroma;
  double maxBound;
  float maxShare = 0.f;                 // > 0: only colours whose boundary share (P3 result) is below it (the modern chroma compression)
  const char* maskParams = nullptr;     // the pixels to leave out are those of this edit's P3 render (the vignette darkens a colour into sRGB afterwards)
};
static bool wanted(const std::vector<std::string>& only, const char* name) { return only.empty() || std::find(only.begin(), only.end(), std::string(name)) != only.end(); }
static void insideDirectImpl(const std::vector<std::string>& only) {
  const Direct cases[] = {
      {"standard", "{}", 0.40f, 0.05},
      {"contrast+sat+vib+highlights", "{\"contrast\":40,\"saturation\":25,\"vibrance\":30,\"highlights\":-30}", 0.40f, 0.05},
      {"hsl + temp + tint", "{\"hsl_red_s\":30,\"hsl_blue_h\":-20,\"temp\":15,\"tint\":-10}", 0.40f, 0.05},
      {"parametric curves", "{\"cv_rgb_H\":40,\"cv_rgb_S\":-30,\"cv_r_L\":25}", 0.40f, 0.05},
      {"grade", "{\"gr_shadows_h\":210,\"gr_shadows_s\":30,\"gr_highlights_h\":40,\"gr_highlights_s\":25}", 0.40f, 0.05},
      {"vignette", "{\"vignette\":-40}", 0.40f, 0.05, 0.f, "{}"},
      {"vignette (light)", "{\"vignette\":50}", 0.40f, 0.05, 0.f, "{}"},
      {"look warm", "{\"look\":\"warm\"}", 0.40f, 0.05},
      {"look amber", "{\"look\":\"amber\"}", 0.40f, 0.05},
      {"look rose", "{\"look\":\"rose\"}", 0.40f, 0.05},
      {"look neutral", "{\"look\":\"neutral\"}", 0.40f, 0.05},
      {"look cool", "{\"look\":\"cool\"}", 0.40f, 0.05},
      {"look teal", "{\"look\":\"teal\"}", 0.40f, 0.05},
      // grain is additive noise on the output-encoded values (clamped per channel): the same noise on P3-encoded values differs by its own amplitude
      {"look film (grain)", "{\"look\":\"film\"}", 0.40f, 12.0},
      {"look bw", "{\"look\":\"bw\"}", 0.40f, 0.05},
      // the modern renderings compress chroma from 88 % of the output gamut's boundary on: colours beyond 85 % of sRGB's boundary are kept more by P3
      // (the point of the target); the rest must be equal
      {"modern_clean", "{\"film\":\"modern_clean\"}", 0.40f, 1.0, 0.85f},
      {"modern_vivid", "{\"film\":\"modern_vivid\"}", 0.40f, 1.0, 0.85f},
      {"modern_soft", "{\"film\":\"modern_soft\"}", 0.40f, 1.0, 0.85f},
  };
  for (const Direct& c : cases) {
    if (!wanted(only, c.name)) continue;
    const SourceImage src = chartSource(c.maxChroma);
    const ImageF sr = renderDirect(src, c.params, 0), p3 = renderDirect(src, c.params, 1);
    const ImageF p3m = c.maskParams ? renderDirect(src, c.maskParams, 1) : ImageF();
    const std::size_t n = std::size_t(src.linear.width) * src.linear.height;
    Stats st;
    std::size_t out = 0;
    for (std::size_t i = 0; i < n; ++i) {
      float a[3], b[3];
      linOf(p3, i, true, a);
      linOf(sr, i, false, b);
      float am[3];
      if (c.maskParams) linOf(p3m, i, true, am);
      if (!insideSrgb(c.maskParams ? am : a, -2e-4f) || (c.maxShare > 0.f && boundaryShare(a) > c.maxShare)) {  // the colour sRGB could not hold (and its rule pulled)
        ++out;
        continue;
      }
      st.add(dE(a, b));
    }
    std::printf("  inside %-30s: dE76 mean %.4f p99.9 %.3f max %.3f over %zu pixels; %zu (%.1f %%) carry colour outside sRGB\n", c.name, st.mean(), st.pct(0.999), st.max, st.n, out,
                100.0 * double(out) / double(n));
    CHECK(st.n > n / 4);
    CHECK(st.max <= c.maxBound);
  }
}
static void testInsideDirect() { insideDirectImpl({}); }
// one edit of each kind (tone and colour, HSL, grade, a vignette that darkens into sRGB, a look, a modern chroma compression): P3 against sRGB in the float pipeline
static void testInsideDirectSmall() {
  g_colStep = 3;
  insideDirectImpl({"standard", "hsl + temp + tint", "vignette", "look cool", "modern_vivid"});
  g_colStep = 1;
}

struct Case {
  const char* name;
  std::string params;
  float maxChroma;
  double maxBound, meanBound;
};
static void insideImpl(const std::vector<std::string>& only) {
  // Film stocks (and their blend with the normal render) go through the C API: they are rendered as sRGB and converted (see c_api.cpp), measured on
  // the exported JPEGs (flat patches; the 8-bit grids of both files are allowed for, see quantAllow).
  if (!dapple_film_list_json) return;
  const Case cases[] = {
      {"film portra, no grain", "{\"film\":\"kodak_portra_400\",\"film_grain\":0}", 0.40f, 2.5, 0.05},
      {"film velvia, no grain", "{\"film\":\"fujifilm_velvia_100\",\"film_grain\":0}", 0.40f, 2.5, 0.05},
      {"film portra half + teal", "{\"film\":\"kodak_portra_400\",\"film_strength\":50,\"look\":\"teal\",\"film_grain\":0}", 0.40f, 2.5, 0.05},
      {"film portra with grain", "{\"film\":\"kodak_portra_400\"}", 0.40f, 4.0, 0.15},
  };
  for (const Case& c : cases) {
    if (!wanted(only, c.name)) continue;
    const std::string sfx = g_colStep > 1 ? "_s" : "";  // the small twin runs beside the full test: its own files
    const fs::path f = chartPath(c.maxChroma, (std::string(c.maxChroma > 0.2f ? "chart" : "chart_low") + sfx + ".jpg").c_str());
    Session S(f);
    CHECK(S.s != nullptr);
    if (!S.s) return;
    const Rgb8 p3 = S.exportAs(c.params, "p3", 0, nullptr, ("i_p3" + sfx + ".jpg").c_str());
    const Rgb8 sr = S.exportAs(c.params, "srgb", 0, nullptr, ("i_sr" + sfx + ".jpg").c_str());
    Stats st, ex;
    std::size_t nOut = 0, n = 0;
    insideStats(p3, sr, st, ex, &nOut, &n);
    std::printf("  inside %-24s: dE76 mean %.3f max %.3f; beyond the 8-bit grids: mean %.4f p99.9 %.3f max %.3f over %zu pixels; %zu (%.1f %%) carry colour outside sRGB\n", c.name,
                st.mean(), st.max, ex.mean(), ex.pct(0.999), ex.max, st.n, nOut, 100.0 * double(nOut) / double(n));
    CHECK(st.n > n / 16);
    CHECK(ex.max <= c.maxBound);
    CHECK(ex.mean() <= c.meanBound);
  }
}
static void testInside() { insideImpl({}); }
// the film export through the C API in P3 against sRGB on two of the four edits (a negative stock, a blend with a look): in the default run
static void testInsideSmall() {
  g_colStep = 3;
  insideImpl({"film velvia, no grain", "film portra half + teal"});
  g_colStep = 1;
}

static void testHue() {
  const fs::path f = chartPath(0.40f, "chart.jpg");
  Session S(f);
  CHECK(S.s != nullptr);
  if (!S.s) return;
  const Rgb8 src = decodeJpeg(readAll(f));
  const std::string ed = "{\"saturation\":70,\"vibrance\":50}";
  const Rgb8 p3 = S.exportAs(ed, "p3", 0, nullptr, "h_p3.jpg");
  const Rgb8 sr = S.exportAs(ed, "srgb", 0, nullptr, "h_sr.jpg");
  Stats hP3, hSr;
  std::size_t clipP3 = 0, clipSr = 0, n = 0;
  for (std::size_t i = 0; i < std::size_t(kW) * kH; ++i) {
    if (!interior(i)) continue;
    float s[3], a[3], b[3];
    linSrgb(src, i, true, s);
    linSrgb(p3, i, true, a);
    linSrgb(sr, i, false, b);
    float Ls, Cs, hs, La, Ca, ha, Lb, Cb, hb;
    oklch(s, Ls, Cs, hs);
    if (Cs < 0.08f || Ls < 0.2f || Ls > 0.85f) continue;
    oklch(a, La, Ca, ha);
    oklch(b, Lb, Cb, hb);
    auto dh = [](float x, float y) {
      float d = std::fabs(x - y);
      return d > 180.f ? 360.f - d : d;
    };
    if (Ca > 0.05f) hP3.add(dh(hs, ha));
    if (Cb > 0.05f) hSr.add(dh(hs, hb));
    ++n;
    clipP3 += (p3.px[3 * i] == 0 || p3.px[3 * i + 1] == 0 || p3.px[3 * i + 2] == 0 || p3.px[3 * i] == 255 || p3.px[3 * i + 1] == 255 || p3.px[3 * i + 2] == 255);
    clipSr += (sr.px[3 * i] == 0 || sr.px[3 * i + 1] == 0 || sr.px[3 * i + 2] == 0 || sr.px[3 * i] == 255 || sr.px[3 * i + 1] == 255 || sr.px[3 * i + 2] == 255);
  }
  std::printf("  hue shift against the source (deg): P3 mean %.2f p99 %.2f max %.2f; sRGB mean %.2f p99 %.2f max %.2f; pixels on a cube face: P3 %.1f %% sRGB %.1f %% of %zu\n",
              hP3.mean(), hP3.pct(0.99), hP3.max, hSr.mean(), hSr.pct(0.99), hSr.max, 100.0 * double(clipP3) / double(n), 100.0 * double(clipSr) / double(n), n);
  CHECK(hP3.pct(0.99) <= hSr.pct(0.99) + 2.0);
  CHECK(hP3.mean() <= hSr.mean() + 0.5);
}

static void testParity() {
  const fs::path f = chartPath(0.40f, "chart.jpg");
  Session S(f);
  CHECK(S.s != nullptr);
  if (!S.s) return;
  const int gpu = dapple_gpu_set_enabled(1);
  if (!gpu) {
    std::printf("  SKIP no GPU backend / device\n");
    dapple_gpu_set_enabled(-1);
    std::exit(77);
  }
  const char* ps[] = {"{}", "{\"saturation\":60,\"look\":\"teal\"}", "{\"film\":\"modern_vivid\"}", "{\"film\":\"kodak_portra_400\"}"};
  for (const char* p : ps) {
    dapple_gpu_set_enabled(1);
    const Rgb8 g = S.exportAs(p, "p3", 0, nullptr, "g_gpu.jpg");
    const Rgb8 gs = S.exportAs(p, "srgb", 0, nullptr, "g_gpu_s.jpg");
    dapple_gpu_set_enabled(0);
    const Rgb8 c = S.exportAs(p, "p3", 0, nullptr, "g_cpu.jpg");
    const Rgb8 cs = S.exportAs(p, "srgb", 0, nullptr, "g_cpu_s.jpg");
    auto diff = [](const Rgb8& a, const Rgb8& b) {
      int m = 0;
      double s = 0;
      std::size_t cnt = 0, wi = 0;
      for (std::size_t i = 0; i < std::size_t(a.w) * a.h; ++i) {
        if (!interior(i)) continue;  // the patch interiors: the JPEG codec is faithful there
        // Not on a face of the output cube: the baked lattice (65^3 tetrahedra) cannot follow the clamp's kink exactly (a chart clipped to the P3 cube puts
        // whole patches on it; photographs hardly ever), in either output space.
        if (std::min({b.px[3 * i], b.px[3 * i + 1], b.px[3 * i + 2]}) < 4 || std::max({b.px[3 * i], b.px[3 * i + 1], b.px[3 * i + 2]}) > 251) continue;
        for (int k = 0; k < 3; ++k) {
          const int d = std::abs(int(a.px[3 * i + std::size_t(k)]) - int(b.px[3 * i + std::size_t(k)]));
          if (d > m) m = d, wi = i;
          s += d;
          ++cnt;
        }
      }
      if (std::getenv("P3_DEBUG")) std::printf(("    worst at (%zu,%zu): a %d %d %d  b %d %d %d" + std::string(1, char(92)) + "n").c_str(), wi % kW, wi / kW, a.px[3 * wi], a.px[3 * wi + 1], a.px[3 * wi + 2], b.px[3 * wi], b.px[3 * wi + 1], b.px[3 * wi + 2]);
      return std::pair<int, double>(m, s / double(cnt));
    };
    const auto dp = diff(g, c), ds = diff(gs, cs);
    std::printf("  parity %-34s: CPU vs GPU max %d mean %.3f (P3)   max %d mean %.3f (sRGB)\n", p, dp.first, dp.second, ds.first, ds.second);
    CHECK(dp.first <= std::max(2, ds.first + 1));
  }
  dapple_gpu_set_enabled(-1);
}

static void testHdr() {
  const fs::path f = root() / "build" / "rawsamples" / "Canon/EOS-R6-Mark-II/canon-r6m2-img_0346-raw-normal-full.cr3";
  if (!fs::exists(f)) {
    std::printf("  SKIP no build/rawsamples\n");
    std::exit(77);
  }
  Session S(f, 1600);
  CHECK(S.s != nullptr);
  if (!S.s) return;
  const std::string ed = "{\"exposureEv\":1.0}";
  const fs::path sdrP = outDir() / "hdr_sdr.jpg", hdrP = outDir() / "hdr_hdr.jpg";
  CHECK(dapple_session_export_jpeg_ex(S.s, ed.c_str(), "", 1200, 92, sdrP.string().c_str(), "{\"colorSpace\":\"p3\"}") == DAPPLE_OK);
  DappleHdrInfo info{};
  CHECK(dapple_session_export_jpeg_hdr_ex(S.s, ed.c_str(), "", 1200, 92, hdrP.string().c_str(), 2.f, &info, "{\"colorSpace\":\"p3\"}") == DAPPLE_OK);
  const Bytes sdr = readAll(sdrP), hdr = readAll(hdrP);
  std::printf("  p3 + hdr written=%d H=%.2f\n", info.written, info.headroom_stops);
  CHECK(contains(hdr, "Display P3"));
  if (info.written) {
    auto p = uhdr::parse(hdr);
    CHECK(p.has_value() && p->base == sdr);  // the base image is the P3 SDR export byte for byte
  } else {
    CHECK(hdr == sdr);
  }
  // the luminance weights of the gain map: P3 differs from Rec.709 on a saturated colour
  Image8 a, b;
  a.width = b.width = 1;
  a.height = b.height = 1;
  a.data = {255, 0, 0};
  b.data = {255, 255, 255};
  const float pBt = uhdr::brightPeak(a, 0.f, false), pP3 = uhdr::brightPeak(a, 0.f, true);
  CHECK(std::fabs(pBt - 0.2126f) < 1e-3f && std::fabs(pP3 - 0.2289746f) < 1e-3f);
  CHECK(std::fabs(uhdr::brightPeak(b, 0.f, true) - 1.f) < 1e-3f);  // white stays 1
}

static void testFraction() {
  const fs::path f = chartPath(0.40f, "chart.jpg");
  Session S(f);
  CHECK(S.s != nullptr);
  if (!S.s) return;
  float fr = -1, frFilm = -1, frLow = -1;
  CHECK(dapple_session_wide_gamut_fraction(S.s, "{}", "", &fr) == DAPPLE_OK);
  CHECK(dapple_session_wide_gamut_fraction(S.s, "{\"film\":\"kodak_portra_400\"}", "", &frFilm) == DAPPLE_OK);
  const fs::path g = chartPath(0.10f, "chart_low.jpg");
  Session T(g);
  CHECK(T.s && dapple_session_wide_gamut_fraction(T.s, "{}", "", &frLow) == DAPPLE_OK);
  std::printf("  wide-gamut fraction: chart %.3f, chart with film %.3f, low-chroma chart %.4f\n", fr, frFilm, frLow);
  CHECK(fr > 0.1f && frFilm < 0.001f && frLow < 0.002f);
}

// ---- evidence ----------------------------------------------------------------------------------------------------------------------------------
static void writeMap(const fs::path& png, const Rgb8& p3, const Rgb8& sr, std::size_t& nOut, double& meanIn, double& maxIn, double& maxOutExcess) {
  std::vector<std::uint8_t> map(std::size_t(p3.w) * p3.h * 3);
  const std::size_t n = std::size_t(p3.w) * p3.h;
  nOut = 0;
  double sum = 0;
  std::size_t nin = 0;
  maxIn = 0;
  maxOutExcess = 0;
  for (std::size_t i = 0; i < n; ++i) {
    float a[3], b[3];
    linSrgb(p3, i, true, a);
    linSrgb(sr, i, false, b);
    const bool in = insideSrgb(a);
    const float g = 0.35f * (0.2126f * b[0] + 0.7151f * b[1] + 0.0722f * b[2]);
    float r = g, gg = g, bb = g;
    if (!in) {
      ++nOut;
      const float ex = std::max(-std::min({a[0], a[1], a[2]}), std::max({a[0], a[1], a[2]}) - 1.f);
      maxOutExcess = std::max<double>(maxOutExcess, ex);
      const float k = std::clamp(0.4f + ex * 4.f, 0.f, 1.f);
      r = 1.f * k + g * (1 - k);
      gg = 0.1f * k + g * (1 - k);
      bb = 0.6f * k + g * (1 - k);
    } else {
      const float d = dE(a, b);
      sum += d;
      ++nin;
      maxIn = std::max<double>(maxIn, d);
    }
    map[3 * i] = std::uint8_t(std::clamp(srgbEncode(r), 0.f, 1.f) * 255.f + 0.5f);
    map[3 * i + 1] = std::uint8_t(std::clamp(srgbEncode(gg), 0.f, 1.f) * 255.f + 0.5f);
    map[3 * i + 2] = std::uint8_t(std::clamp(srgbEncode(bb), 0.f, 1.f) * 255.f + 0.5f);
  }
  meanIn = nin ? sum / double(nin) : 0;
  stbi_write_png(png.string().c_str(), p3.w, p3.h, 3, map.data(), p3.w * 3);
}
static void testEvidence() {
  const fs::path dir = root() / "build" / "p3";
  fs::create_directories(dir);
  std::ofstream num(dir / "numbers.txt");
  auto one = [&](const char* tag, const fs::path& file, int longEdge, const std::string& params) {
    Session S(file, longEdge);
    if (!S.s) {
      num << tag << ": cannot open " << file.string() << "\n";
      return;
    }
    Bytes bs, bp;
    const Rgb8 sr = S.exportAs(params, "srgb", longEdge, &bs, "ev_sr.jpg");
    const Rgb8 p3 = S.exportAs(params, "p3", longEdge, &bp, "ev_p3.jpg");
    writeAll(dir / (std::string(tag) + "_srgb.jpg"), bs);
    writeAll(dir / (std::string(tag) + "_p3.jpg"), bp);
    std::size_t nOut = 0;
    double meanIn = 0, maxIn = 0, maxEx = 0;
    writeMap(dir / (std::string(tag) + "_diff.png"), p3, sr, nOut, meanIn, maxIn, maxEx);
    float fr = 0;
    dapple_session_wide_gamut_fraction(S.s, params.c_str(), "", &fr);
    char line[512];
    std::snprintf(line, sizeof line,
                  "%s (%dx%d, params %s): %.2f %% of the pixels carry colour outside sRGB (max excess %.3f linear); inside sRGB the two 8-bit JPEG exports (codec and grid noise included) differ by dE76 mean %.3f max %.3f; "
                  "wide_gamut_fraction %.3f; files %zu / %zu bytes\n",
                  tag, p3.w, p3.h, params.c_str(), 100.0 * double(nOut) / double(std::size_t(p3.w) * p3.h), maxEx, meanIn, maxIn, fr, bs.size(), bp.size());
    std::fputs(line, stdout);
    num << line;
  };
  one("chart", chartPath(0.40f, "chart.jpg"), 0, "{}");
  one("chart_sat", chartPath(0.40f, "chart.jpg"), 0, "{\"saturation\":40,\"vibrance\":30}");
  const struct {
    const char* tag;
    const char* rel;
  } raws[] = {{"raw_sony_a7r4", "Sony/ILCE-7RM4/DSC00396.ARW"}, {"raw_sony_a6700", "Sony/ILCE-6700/DSC00002.ARW"}};
  for (const auto& r : raws) {
    const fs::path f = root() / "build" / "rawsamples" / r.rel;
    if (fs::exists(f)) one(r.tag, f, 2400, "{}");
    else num << r.tag << ": missing sample " << f.string() << "\n";
  }
}

static void testPx() {
  static const CameraRegistry reg = CameraRegistry::withBuiltins();
  const CameraProfile& cam = reg.resolve("", "");
  SourceImage src;
  src.sceneReferred = false;
  src.linear = ImageF(2, 1);
  const float l[2][3] = {{0.0564f, 0.0362f, 0.0425f}, {0.3f, 0.5f, 0.2f}};
  for (int k = 0; k < 2; ++k) {
    float p[3];
    color::srgbToProPhoto().apply(l[k], p);
    for (int c = 0; c < 3; ++c) src.linear.data[3 * k + c] = p[c];
  }
  for (int sp = 0; sp < 2; ++sp) {
    EditParams e;
    e.outputSpace = sp;
    Renderer r(e, cam, nullptr);
    const Image8 o = r.render(src);
    std::printf("  space %d: px0 %d %d %d  px1 %d %d %d\n", sp, o.data[0], o.data[1], o.data[2], o.data[3], o.data[4], o.data[5]);
  }
  std::printf("  want sRGB codes %d %d %d\n", int(srgbEncode(l[0][0]) * 255 + 0.5f), int(srgbEncode(l[0][1]) * 255 + 0.5f), int(srgbEncode(l[0][2]) * 255 + 0.5f));
}

// The full pipeline on real RAW files (preview-quality decode), both output spaces, as float (no codec, no 8-bit grid). Inside-sRGB equality with the default
// detail stages and with strong ones (sharpen, clarity, texture, grain run on output-encoded values, so they are the part that is not exactly commutative).
static void testRawDirect() {
  const char* files[] = {"Sony/ILCE-7RM4/DSC00396.ARW", "Sony/ILCE-6700/DSC00002.ARW"};
  const std::string detail = std::string("{") + char(34) + "sharpen" + char(34) + ":120," + char(34) + "clarity" + char(34) + ":40," + char(34) + "texture" + char(34) + ":40," + char(34) + "grain" + char(34) + ":30," +
                             char(34) + "saturation" + char(34) + ":20}";
  std::ofstream num(root() / "build" / "p3" / "numbers.txt", std::ios::app);
  for (const char* rel : files) {
    const fs::path f = root() / "build" / "rawsamples" / rel;
    if (!fs::exists(f)) continue;
    auto dec = makeLibRawDecoder()->decode(f, DecodeOptions{});
    if (!dec) {
      std::printf("  decode failed %s\n", rel);
      ++g_fail;
      continue;
    }
    static const CameraRegistry reg = CameraRegistry::withBuiltins();
    const CameraProfile& cam = reg.resolve(dec->meta.make, dec->meta.model);
    for (const std::string& ed : {std::string("{}"), detail, std::string("{}") + " "}) {
      const bool spatial = ed.size() != 3;
      auto e = fromJson(ed);
      EditParams p0 = e ? *e : EditParams{};
      EditParams p1 = p0;
      p1.outputSpace = 1;
      const ImageF sr = Renderer(p0, cam, nullptr).renderFloat(*dec, spatial), p3 = Renderer(p1, cam, nullptr).renderFloat(*dec, spatial);
      const std::size_t n = std::size_t(sr.width) * sr.height;
      Stats st;
      std::size_t out = 0;
      for (std::size_t i = 0; i < n; ++i) {
        float a[3], b[3];
        linOf(p3, i, true, a);
        linOf(sr, i, false, b);
        if (!insideSrgb(a, -2e-4f)) {
          ++out;
          continue;
        }
        st.add(dE(a, b));
      }
      char line[400];
      std::snprintf(line, sizeof line, "  rawdirect %s %s: %zu pixels, %.1f %% outside sRGB; inside: dE76 mean %.4f p99.9 %.3f max %.3f" "\n", rel, ed.size() > 4 ? "strong detail" : (spatial ? "default edit" : "default edit, no spatial stages"), n,
                    100.0 * double(out) / double(n), st.mean(), st.pct(0.999), st.max);
      std::fputs(line, stdout);
      num << line;
      CHECK(st.mean() < 0.15);
    }
  }
}

// Share of colour outside sRGB of every RAW sample under build/rawsamples (the app's probe: preview session, 384 px render), to pick the evidence files.
static void testScan() {
  for (const auto& e : fs::recursive_directory_iterator(root() / "build" / "rawsamples")) {
    if (!e.is_regular_file()) continue;
    std::string x = e.path().extension().string();
    for (char& c : x) c = char(std::tolower(c));
    if (x != ".cr3" && x != ".arw" && x != ".nef" && x != ".raf" && x != ".dng" && x != ".orf" && x != ".rw2") continue;
    DappleSession* s = dapple_session_open(e.path().string().c_str(), DAPPLE_QUALITY_PREVIEW, 1600);
    if (!s) continue;
    float f = -1;
    dapple_session_wide_gamut_fraction(s, "{}", "", &f);
    std::printf("  SCAN %.4f %s\n", f, e.path().generic_string().c_str());
    dapple_session_close(s);
  }
}

int main(int argc, char** argv) {
  const std::string id = argc > 1 ? argv[1] : "";
  const fs::path pack = root() / "core" / "data" / "film" / "dapple.dfp";
  if (fs::exists(pack) && dapple_film_load_pack(pack.string().c_str()) != DAPPLE_OK) std::printf("  film pack not loaded: %s\n", dapple_last_error());
  std::printf("p3_export_tests %s\n", id.c_str());
  if (id == "identity") testIdentity();
  else if (id == "tags") testTags();
  else if (id == "chroma") testChroma();
  else if (id == "inside") testInside();
  else if (id == "inside_small") testInsideSmall();
  else if (id == "direct") testInsideDirect();
  else if (id == "direct_small") testInsideDirectSmall();
  else if (id == "hue") testHue();
  else if (id == "parity") testParity();
  else if (id == "hdr") testHdr();
  else if (id == "fraction") testFraction();
  else if (id == "evidence") testEvidence();
  else if (id == "px") testPx();
  else if (id == "scan") testScan();
  else if (id == "rawdirect") testRawDirect();
  else {
    std::printf("unknown id\n");
    return 2;
  }
  std::printf("%s (%d failures)\n", g_fail ? "FAILED" : "ok", g_fail);
  return g_fail ? 1 : 0;
}
