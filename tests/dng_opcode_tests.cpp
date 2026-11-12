// DNG opcode tests (core/src/decoders/dng_opcodes.*, libraw_decoder.cpp). Synthetic fixtures only: no phone DNG with opcodes was
// available when this was written (see docs/engine/pipeline-architecture.md section 27); the opcode byte layouts follow the DNG
// specification 1.6 chapter 7, the expected values are computed here from the spec formulas independently of the library code.
//   dapple_dng_tests parse | gainmap | vignette | warp | e2e | all
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <fstream>
#include <string>
#include <vector>

#include "test_tmp.h"
#include "dapple/camera.h"
#include "dapple/encode.h"
#include "color.h"
#include "dapple/decoder.h"
#include "decoders/dng_illuminant.h"
#include "decoders/dng_jxl.h"
#include "dng_thumbnail.h"
#include "decoders/dng_opcodes.h"

using namespace dapple;

static int g_fail = 0;
#define CHECK(c, ...)                                           \
  do {                                                          \
    if (!(c)) {                                                 \
      ++g_fail;                                                 \
      std::printf("FAIL %s:%d  %s  ", __FILE__, __LINE__, #c);  \
      std::printf(__VA_ARGS__);                                 \
      std::printf("\n");                                        \
    }                                                           \
  } while (0)

// ---- big-endian opcode builders -------------------------------------------------------------------------------------------
struct BE {
  std::vector<std::uint8_t> b;
  BE& u32(std::uint32_t v) {
    for (int i = 3; i >= 0; --i) b.push_back(std::uint8_t(v >> (8 * i)));
    return *this;
  }
  BE& i32(int v) { return u32(std::uint32_t(v)); }
  BE& f32(float f) {
    std::uint32_t v;
    std::memcpy(&v, &f, 4);
    return u32(v);
  }
  BE& f64(double d) {
    std::uint64_t v;
    std::memcpy(&v, &d, 8);
    for (int i = 7; i >= 0; --i) b.push_back(std::uint8_t(v >> (8 * i)));
    return *this;
  }
};

static std::vector<std::uint8_t> opcode(std::uint32_t id, std::uint32_t flags, const BE& params) {
  BE o;
  o.u32(id).u32(0x01030000).u32(flags).u32(std::uint32_t(params.b.size()));
  o.b.insert(o.b.end(), params.b.begin(), params.b.end());
  return o.b;
}
static std::vector<std::uint8_t> list(const std::vector<std::vector<std::uint8_t>>& ops) {
  BE l;
  l.u32(std::uint32_t(ops.size()));
  for (auto& o : ops) l.b.insert(l.b.end(), o.begin(), o.end());
  return l.b;
}

// GainMap over the whole W x H image, one map plane, lattice (top, left, pitch 2 x 2) = one Bayer colour, gain(u, v) = fn(u = x/(W-1), v = y/(H-1))
template <class Fn>
static std::vector<std::uint8_t> gainMapOp(int W, int H, int top, int left, int pitch, int pv, int ph, Fn fn, std::uint32_t flags = 0) {
  BE p;
  p.i32(top).i32(left).i32(H).i32(W).i32(0).i32(1).i32(pitch).i32(pitch).i32(pv).i32(ph);
  p.f64(1.0 / (pv - 1)).f64(1.0 / (ph - 1)).f64(0.0).f64(0.0).i32(1);
  for (int i = 0; i < pv; ++i)
    for (int j = 0; j < ph; ++j) p.f32(fn(double(j) / (ph - 1), double(i) / (pv - 1)));
  return opcode(9, flags, p);
}
static std::vector<std::uint8_t> warpOp(const double kr[4], const double kt[2], double cx, double cy, int sets = 1) {
  BE p;
  p.i32(sets);
  for (int s = 0; s < sets; ++s) {
    for (int k = 0; k < 4; ++k) p.f64(kr[k]);
    p.f64(kt[0]).f64(kt[1]);
  }
  p.f64(cx).f64(cy);
  return opcode(1, 0, p);
}
static std::vector<std::uint8_t> vignetteOp(const double k[5], double cx, double cy) {
  BE p;
  for (int i = 0; i < 5; ++i) p.f64(k[i]);
  p.f64(cx).f64(cy);
  return opcode(3, 0, p);
}

// ---- tests ---------------------------------------------------------------------------------------------------------------
static void testParse() {
  const auto gm = gainMapOp(32, 24, 0, 0, 2, 3, 4, [](double u, double v) { return 1.0 + u + v; });
  const double kr[4] = {1, 0.01, 0, 0}, kt[2] = {0, 0}, kv[5] = {0.1, 0, 0, 0, 0};
  BE junk;
  junk.u32(7);
  auto l = list({gm, opcode(999, 1, junk), warpOp(kr, kt, 0.5, 0.5), vignetteOp(kv, 0.5, 0.5), opcode(998, 0, junk)});
  auto pl = dng::parseOpcodeList(l.data(), l.size());
  CHECK(pl.error.empty(), "error '%s'", pl.error.c_str());
  CHECK(pl.ops.size() == 5, "ops %zu", pl.ops.size());
  CHECK(pl.ops[1].optional() && !pl.ops[4].optional(), "flags");
  dng::GainMap g;
  CHECK(dng::parseGainMap(pl.ops[0], g) && g.pointsV == 3 && g.pointsH == 4 && g.rowPitch == 2 && g.gain.size() == 12, "gain map fields");
  CHECK(std::fabs(g.gain[11] - 3.f) < 1e-6f, "last grid gain %g", g.gain[11]);
  dng::WarpRect w;
  CHECK(dng::parseWarpRect(pl.ops[2], w) && std::fabs(w.kr[0][1] - 0.01) < 1e-12 && w.cx == 0.5, "warp fields");
  dng::FixVignette f;
  CHECK(dng::parseFixVignette(pl.ops[3], f) && f.k[0] == 0.1, "vignette fields");
  // truncated list: keeps what parsed, reports the fault, never throws
  auto cut = std::vector<std::uint8_t>(l.begin(), l.begin() + 4 + gm.size() + 10);
  auto tl = dng::parseOpcodeList(cut.data(), cut.size());
  CHECK(!tl.error.empty() && tl.ops.size() == 1, "truncated: '%s' ops %zu", tl.error.c_str(), tl.ops.size());
  auto empty = dng::parseOpcodeList(nullptr, 0);
  CHECK(empty.ops.empty() && empty.error.empty(), "empty");
  // a GainMap with a short payload is rejected, not read out of bounds
  dng::Opcode bad = pl.ops[0];
  bad.params.resize(bad.params.size() - 4);
  CHECK(!dng::parseGainMap(bad, g), "short gain map accepted");
  // unknown optional opcodes are skipped silently-ish (reported), required unknown too (fail soft): neither aborts the others
  ImageF img(32, 24);
  for (auto& v : img.data) v = 0.5f;
  dng::ApplyReport rep;
  dng::applyImage(pl, img, rep);
  CHECK(rep.applied.size() == 3, "applied %zu (%s)", rep.applied.size(), rep.summary().c_str());
  CHECK(rep.skipped.size() == 2, "skipped %zu (%s)", rep.skipped.size(), rep.summary().c_str());
}

static void testGainMap() {
  const int W = 64, H = 48;
  const float black = 256.f;
  auto run = [&](float flat, int rowPitchMode) {
    (void)rowPitchMode;
    std::vector<std::uint16_t> buf(std::size_t(W + 8) * (H + 6), 0);
    for (auto& v : buf) v = 9;  // margin sentinel
    dng::MosaicView v;
    v.px = buf.data();
    v.pitch = W + 8;
    v.top = 2, v.left = 4, v.width = W, v.height = H;
    v.black = {black, black, black, black};
    v.bpr = v.bpc = 2;
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) buf[std::size_t(v.top + y) * v.pitch + v.left + x] = std::uint16_t(flat);
    // four maps, one per Bayer site, different bilinear-exact gain fields; 5 x 4 grid points
    auto gfun = [](int site) {
      return [site](double u, double vv) { return 1.0 + 0.4 * u + 0.2 * vv + 0.1 * site; };
    };
    std::vector<std::vector<std::uint8_t>> ops;
    for (int s = 0; s < 4; ++s) ops.push_back(gainMapOp(W, H, s / 2, s % 2, 2, 4, 5, gfun(s)));
    auto l = list(ops);
    auto pl = dng::parseOpcodeList(l.data(), l.size());
    dng::ApplyReport rep;
    dng::applyMosaic(pl, v, rep);
    CHECK(rep.applied.size() == 4 && rep.skipped.empty(), "report %s", rep.summary().c_str());
    int bad = 0;
    double worst = 0;
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) {
        const int site = (y & 1) * 2 + (x & 1);
        const double u = double(x) / (W - 1), vv = double(y) / (H - 1);
        const double expect = black + (flat - black) * gfun(site)(u, vv);
        const double got = buf[std::size_t(v.top + y) * v.pitch + v.left + x];
        worst = std::max(worst, std::fabs(got - expect));
        if (std::fabs(got - expect) > 1.0) ++bad;
      }
    CHECK(bad == 0, "flat %g: %d pixels off by > 1 count (worst %.2f)", flat, bad, worst);
    // outside the active area nothing moved
    CHECK(buf[0] == 9 && buf[std::size_t(v.top - 1) * v.pitch + v.left] == 9 && buf[std::size_t(v.top) * v.pitch + v.left - 1] == 9 &&
              buf[std::size_t(v.top + H) * v.pitch + v.left] == 9,
          "margin touched");
  };
  run(2000.f, 0);
  run(black, 0);  // black stays black whatever the gain
  // a map that does not cover the image: values replicate outside (origin 0.25 spacing 0.25, 3 x 3 points)
  {
    std::vector<std::uint16_t> buf(std::size_t(W) * H, 1256);
    dng::MosaicView v;
    v.px = buf.data(), v.pitch = W, v.width = W, v.height = H, v.black = {0.f}, v.bpr = v.bpc = 1;
    BE p;
    p.i32(0).i32(0).i32(H).i32(W).i32(0).i32(1).i32(1).i32(1).i32(3).i32(3).f64(0.25).f64(0.25).f64(0.25).f64(0.25).i32(1);
    for (int i = 0; i < 9; ++i) p.f32(1.f + 0.5f * float(i));  // 1.0 .. 5.0 row major
    auto l = list({opcode(9, 0, p)});
    auto pl = dng::parseOpcodeList(l.data(), l.size());
    dng::ApplyReport rep;
    dng::applyMosaic(pl, v, rep);
    CHECK(buf[0] == 1256, "top-left corner replicates gain 1.0, got %d", buf[0]);                              // before the map origin
    CHECK(buf[std::size_t(H - 1) * W + W - 1] == std::uint16_t(1256 * 5.0), "bottom-right replicates 5.0, got %d", buf[std::size_t(H - 1) * W + W - 1]);
    // inside the map: gain bilinear in the 3 x 3 grid (1.0 .. 5.0 row major, point (i, j) at relative 0.25 + 0.25 * index)
    const int px = 32, py = 24;
    const double tu = (double(px) / (W - 1) - 0.25) / 0.25, tv = (double(py) / (H - 1) - 0.25) / 0.25;
    const double expect = 1.0 + 0.5 * tu + 1.5 * tv;
    CHECK(std::fabs(buf[std::size_t(py) * W + px] / 1256.0 - expect) < 0.01, "inside gain %g vs %g", buf[std::size_t(py) * W + px] / 1256.0, expect);
  }
}

static void testVignette() {
  const int W = 65, H = 49;
  std::vector<std::uint16_t> buf(std::size_t(W) * H, 1000);
  dng::MosaicView v;
  v.px = buf.data(), v.pitch = W, v.width = W, v.height = H, v.black = {0.f}, v.bpr = v.bpc = 1;
  const double k[5] = {0.5, 0.25, 0, 0, 0.1};
  auto l = list({vignetteOp(k, 0.5, 0.5)});
  auto pl = dng::parseOpcodeList(l.data(), l.size());
  dng::ApplyReport rep;
  dng::applyMosaic(pl, v, rep);
  CHECK(rep.applied.size() == 1, "report %s", rep.summary().c_str());
  CHECK(buf[std::size_t(H / 2) * W + W / 2] == 1000, "centre untouched");
  const double expect = 1000.0 * (1 + 0.5 + 0.25 + 0.1);  // corner: r = 1 (spec: farthest pixel)
  CHECK(std::fabs(buf[0] - expect) <= 1.0, "corner %d vs %g", buf[0], expect);
  const double r2 = (16.0 * 16.0) / (32.0 * 32.0 + 24.0 * 24.0);  // pixel (x = 48, y = 24): dx = 16
  const double g = 1 + 0.5 * r2 + 0.25 * r2 * r2 + 0.1 * r2 * r2 * r2 * r2 * r2;
  CHECK(std::fabs(buf[std::size_t(24) * W + 48] - 1000 * g) <= 1.0, "mid-edge %d vs %g", buf[std::size_t(24) * W + 48], 1000 * g);
}

// expected source position straight from the spec formulas (chapter 7, WarpRectilinear)
static void specSource(const double kr[4], const double kt[2], double cxh, double cyh, int W, int H, double x, double y, double& sx, double& sy) {
  const double cx = cxh * (W - 1), cy = cyh * (H - 1);
  const double mx = std::max(std::fabs(0 - cx), std::fabs(W - 1 - cx)), my = std::max(std::fabs(0 - cy), std::fabs(H - 1 - cy));
  const double m = std::sqrt(mx * mx + my * my);
  const double dx = (x - cx) / m, dy = (y - cy) / m;
  const double r2 = dx * dx + dy * dy;
  const double f = kr[0] + kr[1] * r2 + kr[2] * r2 * r2 + kr[3] * r2 * r2 * r2;
  const double dxr = f * dx, dyr = f * dy;
  const double dxt = kt[0] * (2 * dx * dy) + kt[1] * (r2 + 2 * dx * dx);
  const double dyt = kt[1] * (2 * dx * dy) + kt[0] * (r2 + 2 * dy * dy);
  sx = cx + m * (dxr + dxt);
  sy = cy + m * (dyr + dyt);
}

static void perfWarp() {
  if (!std::getenv("DNG_PERF")) return;
  ImageF img(4032, 3024);
  for (std::size_t i = 0; i < img.data.size(); ++i) img.data[i] = float(i % 977) / 977.f;
  dng::WarpRect w;
  w.kr[0][0] = 1.0, w.kr[0][1] = 0.05, w.kr[0][2] = -0.01;
  const auto t0 = std::chrono::steady_clock::now();
  ImageF out = dng::warpRectilinear(img, w);
  const auto t1 = std::chrono::steady_clock::now();
  std::printf("  warp 12 MP (4032x3024 RGB float): %.0f ms (checksum %.3f)\n", std::chrono::duration<double, std::milli>(t1 - t0).count(), double(out.data[12345]));
}

static void testWarp() {
  const int W = 200, H = 150;
  // identity coefficients: bit-exact copy
  {
    ImageF img(W, H);
    for (std::size_t i = 0; i < img.data.size(); ++i) img.data[i] = float((i * 2654435761u) % 1000) / 1000.f;
    const double kr[4] = {1, 0, 0, 0}, kt[2] = {0, 0};
    auto l = list({warpOp(kr, kt, 0.5, 0.5)});
    auto pl = dng::parseOpcodeList(l.data(), l.size());
    ImageF out = img;
    dng::ApplyReport rep;
    dng::applyImage(pl, out, rep);
    CHECK(out.data == img.data, "identity warp changed pixels");
  }
  // ramps: plane 0 = x, plane 1 = y, plane 2 = x + 2y. A resampler that is exact on linear data returns the source coordinates.
  const double kr[4] = {1.0, 0.06, -0.03, 0.01}, kt[2] = {0.004, -0.003};
  const double cx = 0.47, cy = 0.52;
  {
    ImageF img(W, H);
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) {
        float* p = img.row(y) + x * 3;
        p[0] = float(x) / 100.f, p[1] = float(y) / 100.f, p[2] = (float(x) + 2.f * float(y)) / 100.f;
      }
    auto l = list({warpOp(kr, kt, cx, cy)});
    auto pl = dng::parseOpcodeList(l.data(), l.size());
    ImageF out = img;
    dng::ApplyReport rep;
    dng::applyImage(pl, out, rep);
    CHECK(rep.applied.size() == 1, "report %s", rep.summary().c_str());
    double worst = 0;
    int n = 0;
    for (int y = 4; y < H - 4; ++y)
      for (int x = 4; x < W - 4; ++x) {
        double sx, sy;
        specSource(kr, kt, cx, cy, W, H, x, y, sx, sy);
        if (sx < 2 || sy < 2 || sx > W - 3 || sy > H - 3) continue;
        const float* p = out.row(y) + x * 3;
        worst = std::max({worst, std::fabs(p[0] - sx / 100.0), std::fabs(p[1] - sy / 100.0), std::fabs(p[2] - (sx + 2 * sy) / 100.0)});
        ++n;
      }
    CHECK(n > W * H / 2 && worst < 2e-4, "ramp mapping: %d px, worst error %.2e (x100 = %.4f px)", n, worst, worst * 100);
    std::printf("  warp ramp: %d px compared, worst %.4f px\n", n, worst * 100);
  }
  // grid pattern: 1-px lines every 16 px (source), radial distortion only. Find the lines along the centre row and compare with the
  // analytic position (the dest x whose source x equals the line), sub-pixel via the centroid.
  {
    ImageF img(W, H);
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) {
        const float v = (x % 16 == 0 || y % 16 == 0) ? 1.f : 0.f;
        float* p = img.row(y) + x * 3;
        p[0] = p[1] = p[2] = v;
      }
    const double kr2[4] = {1.0, 0.08, 0.0, 0.0}, kt0[2] = {0, 0};
    auto l = list({warpOp(kr2, kt0, 0.5, 0.5)});
    auto pl = dng::parseOpcodeList(l.data(), l.size());
    ImageF out = img;
    dng::ApplyReport rep;
    dng::applyImage(pl, out, rep);
    const int yc = 75;  // not a grid row: pure vertical-line cross-section; centre row of the optical centre is (H-1)/2 = 74.5
    int found = 0;
    double worst = 0;
    for (int xl = 16; xl <= 176; xl += 16) {
      // analytic dest x: solve sx(x, yc) = xl by bisection on the spec formula (monotone for these coefficients)
      double lo = 0, hi = W - 1;
      for (int it = 0; it < 60; ++it) {
        const double mid = 0.5 * (lo + hi);
        double sx, sy;
        specSource(kr2, kt0, 0.5, 0.5, W, H, mid, yc, sx, sy);
        (sx < xl ? lo : hi) = mid;
      }
      const double expect = 0.5 * (lo + hi);
      // measured centroid within +-3 px
      double sw = 0, sxw = 0;
      for (int x = int(expect) - 3; x <= int(expect) + 4; ++x) {
        if (x < 0 || x >= W) continue;
        const double v = out.row(yc)[x * 3];
        sw += v, sxw += v * x;
      }
      if (sw < 0.3) continue;
      ++found;
      worst = std::max(worst, std::fabs(sxw / sw - expect));
    }
    CHECK(found >= 8 && worst < 0.3, "grid lines: %d found, worst centroid error %.3f px", found, worst);
    std::printf("  warp grid: %d lines, worst centroid error %.3f px\n", found, worst);
  }
}

// ---- end to end: synthetic DNG through LibRaw and the decoder ---------------------------------------------------------------
namespace fs = std::filesystem;

struct Tag {
  std::uint16_t id, type;
  std::uint32_t count;
  std::vector<std::uint8_t> data;  // little-endian payload
};
static void put16(std::vector<std::uint8_t>& b, std::uint32_t v) { b.push_back(v & 255), b.push_back((v >> 8) & 255); }
static void put32(std::vector<std::uint8_t>& b, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(std::uint8_t(v >> (8 * i)));
}
static Tag tagShort(int id, std::vector<int> v) {
  Tag t{std::uint16_t(id), 3, std::uint32_t(v.size()), {}};
  for (int x : v) put16(t.data, x);
  return t;
}
static Tag tagLong(int id, std::vector<std::uint32_t> v) {
  Tag t{std::uint16_t(id), 4, std::uint32_t(v.size()), {}};
  for (auto x : v) put32(t.data, x);
  return t;
}
static Tag tagBytes(int id, std::vector<std::uint8_t> v, int type = 1) { return Tag{std::uint16_t(id), std::uint16_t(type), std::uint32_t(v.size()), std::move(v)}; }
static Tag tagAscii(int id, const std::string& s) {
  std::vector<std::uint8_t> v(s.begin(), s.end());
  v.push_back(0);
  return Tag{std::uint16_t(id), 2, std::uint32_t(v.size()), v};
}
static Tag tagSRational(int id, std::vector<double> v, bool sign) {
  Tag t{std::uint16_t(id), std::uint16_t(sign ? 10 : 5), std::uint32_t(v.size()), {}};
  for (double d : v) put32(t.data, std::uint32_t(std::int32_t(std::lround(d * 1000000)))), put32(t.data, 1000000);
  return t;
}

// colour tags of the synthetic DNG; dual = ColorMatrix1 (illuminant A) + ColorMatrix2 (D65)
struct DngColor {
  bool dual = false;
  double cm1[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, cm2[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  double neutral[3] = {0.5, 1.0, 0.6};
  double baselineEv = 0;  // BaselineExposure tag (50730) when != 0
  double whiteXy[2] = {0, 0};  // > 0: AsShotWhiteXY instead of AsShotNeutral
};
static DngColor g_color;

// Bayer RGGB 16-bit uncompressed DNG, flat raw value `flat` (black 64, white 4095).
static void writeDng(const fs::path& path, int W, int H, int orientation, bool crop, const std::vector<std::uint8_t>& l2, const std::vector<std::uint8_t>& l3,
                     std::vector<std::uint16_t> pixels) {
  std::vector<Tag> tags;
  const std::uint32_t imgBytes = std::uint32_t(W) * H * 2;
  tags.push_back(tagLong(256, {std::uint32_t(W)}));
  tags.push_back(tagLong(257, {std::uint32_t(H)}));
  tags.push_back(tagShort(258, {16}));
  tags.push_back(tagShort(259, {1}));
  tags.push_back(tagShort(262, {32803}));
  tags.push_back(tagAscii(271, "Dapple"));
  tags.push_back(tagAscii(272, "Synthetic DNG"));
  tags.push_back(tagLong(273, {0}));  // patched below
  tags.push_back(tagShort(274, {orientation}));
  tags.push_back(tagShort(277, {1}));
  tags.push_back(tagLong(278, {std::uint32_t(H)}));
  tags.push_back(tagLong(279, {imgBytes}));
  tags.push_back(tagShort(284, {1}));
  tags.push_back(tagShort(33421, {2, 2}));
  tags.push_back(tagBytes(33422, {0, 1, 1, 2}));
  tags.push_back(tagBytes(50706, {1, 4, 0, 0}));
  tags.push_back(tagBytes(50707, {1, 1, 0, 0}));
  tags.push_back(tagAscii(50708, "Dapple Synthetic DNG"));
  tags.push_back(tagShort(50714, {64}));
  tags.push_back(tagLong(50717, {4095}));
  if (crop) {
    tags.push_back(tagLong(50719, {10, 6}));
    tags.push_back(tagLong(50720, {std::uint32_t(W - 30), std::uint32_t(H - 18)}));
  }
  if (g_color.dual) {
    tags.push_back(tagSRational(50721, std::vector<double>(g_color.cm1, g_color.cm1 + 9), true));
    tags.push_back(tagSRational(50722, std::vector<double>(g_color.cm2, g_color.cm2 + 9), true));
    tags.push_back(tagShort(50778, {17}));
    tags.push_back(tagShort(50779, {21}));
  } else {
    tags.push_back(tagSRational(50721, {1, 0, 0, 0, 1, 0, 0, 0, 1}, true));
    tags.push_back(tagShort(50778, {21}));
  }
  if (g_color.baselineEv != 0) tags.push_back(tagSRational(50730, {g_color.baselineEv}, true));
  if (g_color.whiteXy[0] > 0) tags.push_back(tagSRational(50729, {g_color.whiteXy[0], g_color.whiteXy[1]}, false));
  else tags.push_back(tagSRational(50728, std::vector<double>(g_color.neutral, g_color.neutral + 3), false));
  if (!l2.empty()) tags.push_back(tagBytes(51009, l2, 7));
  if (!l3.empty()) tags.push_back(tagBytes(51022, l3, 7));
  std::sort(tags.begin(), tags.end(), [](const Tag& a, const Tag& b) { return a.id < b.id; });
  const std::uint32_t ifdSize = 2 + 12 * std::uint32_t(tags.size()) + 4;
  std::uint32_t off = 8 + ifdSize;
  std::vector<std::uint32_t> dataOff(tags.size());
  for (std::size_t i = 0; i < tags.size(); ++i)
    if (tags[i].data.size() > 4) {
      dataOff[i] = off;
      off += std::uint32_t(tags[i].data.size() + (tags[i].data.size() & 1));
    }
  const std::uint32_t imgOff = off;
  for (auto& t : tags)
    if (t.id == 273) t.data.clear(), put32(t.data, imgOff);
  std::vector<std::uint8_t> f = {'I', 'I', 42, 0, 8, 0, 0, 0};
  put16(f, std::uint32_t(tags.size()));
  for (std::size_t i = 0; i < tags.size(); ++i) {
    put16(f, tags[i].id), put16(f, tags[i].type), put32(f, tags[i].count);
    if (tags[i].data.size() > 4) put32(f, dataOff[i]);
    else {
      auto d = tags[i].data;
      d.resize(4, 0);
      f.insert(f.end(), d.begin(), d.end());
    }
  }
  put32(f, 0);
  for (std::size_t i = 0; i < tags.size(); ++i)
    if (tags[i].data.size() > 4) {
      f.insert(f.end(), tags[i].data.begin(), tags[i].data.end());
      if (tags[i].data.size() & 1) f.push_back(0);
    }
  for (std::uint16_t v : pixels) put16(f, v);
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(f.data()), std::streamsize(f.size()));
}

static void testEndToEnd() {
  const fs::path dir = fs::temp_directory_path() / (std::string("dapple_dng_work_") + testUniqueTag() + "");
  fs::create_directories(dir);
  const int W = 256, H = 192;
  // smooth, clearly non-flat content so the gain maps and the warp have something to act on, no clipping after gain <= 1.6
  std::vector<std::uint16_t> px(std::size_t(W) * H);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) px[std::size_t(y) * W + x] = std::uint16_t(64 + 1200 + 600 * std::sin(x * 0.03) * std::cos(y * 0.04) + 0.5);
  auto gfun = [](double u, double v) { return 1.0 + 0.4 * u + 0.2 * v; };  // bilinear-exact
  std::vector<std::vector<std::uint8_t>> gms;
  for (int s = 0; s < 4; ++s) gms.push_back(gainMapOp(W, H, s / 2, s % 2, 2, 4, 5, gfun));
  const auto l2 = list(gms);
  const double kid[4] = {1, 0, 0, 0}, kt0[2] = {0, 0};
  const auto l3id = list({warpOp(kid, kt0, 0.5, 0.5)});

  auto decode = [&](const fs::path& p, DecodeQuality q) {
    auto dec = makeLibRawDecoder();
    auto r = dec->decode(p, DecodeOptions{q});
    if (!r) std::printf("  decode failed: %s\n", r.error().message.c_str());
    return r;
  };

  // 1. lens shading: ratio (with gain map) / (without) at interior points equals the analytic gain
  writeDng(dir / "plain.dng", W, H, 1, false, {}, {}, px);
  writeDng(dir / "shade.dng", W, H, 1, false, l2, {}, px);
  auto a = decode(dir / "plain.dng", DecodeQuality::Full), b = decode(dir / "shade.dng", DecodeQuality::Full);
  CHECK(a && b, "decode");
  if (a && b) {
    CHECK(a->linear.width == W && b->linear.width == W && b->linear.height == H, "size %dx%d", b->linear.width, b->linear.height);
    CHECK(b->meta.dngCorrections.find("GainMap x4") != std::string::npos, "report '%s'", b->meta.dngCorrections.c_str());
    CHECK(a->meta.dngCorrections.empty(), "plain file reported '%s'", a->meta.dngCorrections.c_str());
    double worst = 0, maxGain = 0;
    int n = 0;
    for (int y = 12; y < H - 12; y += 7)
      for (int x = 12; x < W - 12; x += 7) {
        const float* pa = a->linear.row(y) + x * 3;
        const float* pb = b->linear.row(y) + x * 3;
        if (pa[1] < 0.02f || pb[0] > 0.97f || pb[2] > 0.97f) continue;  // white-balanced red / blue clipped at 1.0 after the gain
        const double ratio = pb[1] / double(pa[1]), expect = gfun(double(x) / (W - 1), double(y) / (H - 1));
        if (std::getenv("DNG_DEBUG") && (x % 49 == 12) && (y % 49 == 12)) std::printf("   (%d,%d) plain %.4f %.4f %.4f shade %.4f %.4f %.4f ratio %.3f expect %.3f\n", x, y, pa[0], pa[1], pa[2], pb[0], pb[1], pb[2], ratio, expect);
        worst = std::max(worst, std::fabs(ratio / expect - 1.0));
        maxGain = std::max(maxGain, ratio);
        ++n;
      }
    CHECK(n > 100 && worst < 0.03, "gain ratio vs analytic: %d samples, worst relative error %.4f", n, worst);
    std::printf("  lens shading e2e: %d samples, worst relative error vs analytic gain %.4f (max applied gain %.2f)\n", n, worst, maxGain);
  }

  // 2. warp (identity coefficients) + default crop + orientation 6 (rotate 90 cw): must equal LibRaw's own orientation of the cropped
  //    full frame exactly, and the half-size preview must be exactly half of the crop
  writeDng(dir / "orient_full.dng", W, H, 6, false, {}, {}, px);
  writeDng(dir / "orient_crop.dng", W, H, 6, true, {}, l3id, px);
  auto f = decode(dir / "orient_full.dng", DecodeQuality::Full), c = decode(dir / "orient_crop.dng", DecodeQuality::Full);
  CHECK(f && c, "decode orient");
  if (f && c) {
    // crop origin / size after rounding to even: x0 = 10, y0 = 6, w = W - 30, h = H - 18
    const int x0 = 10, y0 = 6, cw = W - 30, ch = H - 18;
    CHECK(c->linear.width == ch && c->linear.height == cw, "cropped+rotated size %dx%d (want %dx%d)", c->linear.width, c->linear.height, ch, cw);
    CHECK(f->linear.width == H && f->linear.height == W, "uncropped rotated size %dx%d", f->linear.width, f->linear.height);
    CHECK(c->meta.sensorWidth == ch && c->meta.sensorHeight == cw, "meta size %dx%d", c->meta.sensorWidth, c->meta.sensorHeight);
    if (c->linear.width == ch && c->linear.height == cw && f->linear.width == H) {
      double worst = 0;
      for (int cyp = 0; cyp < ch; ++cyp)
        for (int cxp = 0; cxp < cw; ++cxp) {
          const float* pc = c->linear.row(cxp) + (ch - 1 - cyp) * 3;
          const float* pf = f->linear.row(x0 + cxp) + (H - 1 - (y0 + cyp)) * 3;
          for (int k = 0; k < 3; ++k) worst = std::max(worst, std::fabs(double(pc[k]) - pf[k]));
        }
      CHECK(worst < 1e-6, "crop+rotate vs LibRaw orientation: worst difference %.3g", worst);
      std::printf("  crop + orientation vs LibRaw: worst difference %.3g\n", worst);
    }
    auto h = decode(dir / "orient_crop.dng", DecodeQuality::Preview);
    CHECK(h && h->linear.width == ch / 2 && h->linear.height == cw / 2, "preview size %dx%d (want %dx%d)", h ? h->linear.width : 0, h ? h->linear.height : 0, ch / 2, cw / 2);
  }

  // 3. DAPPLE_DNG_OPCODES is read once per process, so the fail-soft path is exercised with a corrupt list instead: the decode still works
  {
    auto bad = l2;
    bad.resize(bad.size() / 2);
    writeDng(dir / "corrupt.dng", W, H, 1, false, bad, {0, 0, 0, 5, 0, 0, 0, 1}, px);
    auto r = decode(dir / "corrupt.dng", DecodeQuality::Full);
    CHECK(r && r->linear.width == W, "corrupt opcode list must not fail the decode");
    if (r) std::printf("  corrupt list: decode ok, report '%s'\n", r->meta.dngCorrections.c_str());
  }
  std::error_code ec;
  fs::remove_all(dir, ec);
}

// DNG spec chapter 6 solve against its own forward model, then through a synthetic DNG and the decoder's probe
static void testIlluminant() {
  const double cmA[9] = {0.9210, -0.2510, -0.0702, -0.4503, 1.3012, 0.1801, -0.0402, 0.1013, 0.4624};
  const double cmD[9] = {0.7866, -0.2108, -0.0555, -0.4192, 1.2090, 0.2336, -0.0642, 0.1259, 0.5817};
  DngCalibration cal[2];
  std::memcpy(cal[0].cm, cmA, sizeof cmA);
  std::memcpy(cal[1].cm, cmD, sizeof cmD);
  cal[0].cct = illuminantCodeCct(17), cal[1].cct = illuminantCodeCct(21);
  const double ab[3] = {1, 1, 1};
  auto forward = [&](double x, double y, double neutral[3]) {  // spec: neutral = AB*CC*CM(weight at the true xy) * XYZ(xy, Y = 1)
    const double T = std::clamp(xyToCct(x, y), 1500.0, 20000.0);
    const double w = std::clamp((1.0 / T - 1.0 / cal[1].cct) / (1.0 / cal[0].cct - 1.0 / cal[1].cct), 0.0, 1.0);
    const double X = x / y, Z = (1 - x - y) / y;
    for (int r = 0; r < 3; ++r) neutral[r] = (w * cmA[r * 3] + (1 - w) * cmD[r * 3]) * X + (w * cmA[r * 3 + 1] + (1 - w) * cmD[r * 3 + 1]) + (w * cmA[r * 3 + 2] + (1 - w) * cmD[r * 3 + 2]) * Z;
  };
  double worstMired = 0;
  for (double T : {2300.0, 2856.0, 3300.0, 4000.0, 4800.0, 5500.0, 6504.0, 7500.0, 9000.0}) {
    double x, y;
    cctToXy(T, x, y);
    double n[3];
    forward(x, y, n);
    double sx, sy;
    int it = 0;
    CHECK(solveIlluminantFromNeutral(cal, 2, ab, n, sx, sy, &it), "solve failed at %g K", T);
    const double dm = std::fabs(1e6 / xyToCct(sx, sy) - 1e6 / xyToCct(x, y));
    worstMired = std::max(worstMired, dm);
    CHECK(std::fabs(sx - x) < 2e-5 && std::fabs(sy - y) < 2e-5 && it < 25, "%g K: solved (%.5f, %.5f) want (%.5f, %.5f), %d passes", T, sx, sy, x, y, it);
  }
  std::printf("  illuminant solve vs forward model: worst %.4f mired over 2300-9000 K\n", worstMired);
  // one calibration only = the plain matrix inverse; a unit-matrix camera returns the neutral's own chromaticity
  {
    DngCalibration one;
    const double id[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    std::memcpy(one.cm, id, sizeof id);
    one.cct = 6504;
    double sx, sy;
    const double n[3] = {0.9, 1.0, 0.8};
    CHECK(solveIlluminantFromNeutral(&one, 1, ab, n, sx, sy), "single");
    CHECK(std::fabs(sx - 0.9 / 2.7) < 1e-9 && std::fabs(sy - 1.0 / 2.7) < 1e-9, "single matrix xy (%g, %g)", sx, sy);
    const double bad[3] = {0, 1, 1};
    CHECK(!solveIlluminantFromNeutral(&one, 1, ab, bad, sx, sy), "non-physical neutral accepted");
  }
  // through the decoder: a DNG carrying both matrices and an as-shot neutral made from 3300 K light
  {
    const fs::path dir = fs::temp_directory_path() / (std::string("dapple_dng_work_ill_") + testUniqueTag() + "");
    fs::create_directories(dir);
    g_color.dual = true;
    std::memcpy(g_color.cm1, cmA, sizeof cmA);
    std::memcpy(g_color.cm2, cmD, sizeof cmD);
    double x, y;
    cctToXy(3300.0, x, y);
    forward(x, y, g_color.neutral);
    const double nmax = std::max({g_color.neutral[0], g_color.neutral[1], g_color.neutral[2]});
    { const double g = g_color.neutral[1]; for (double& v : g_color.neutral) v /= g; }  // AsShotNeutral is relative; scale does not matter
    (void)nmax;
    std::vector<std::uint16_t> px(64 * 48, 800);
    writeDng(dir / "dual.dng", 64, 48, 1, false, {}, {}, px);
    auto pr = makeLibRawDecoder()->probe(dir / "dual.dng");
    CHECK(pr.has_value(), "probe failed");
    if (pr) {
      const double mired = std::fabs(1e6 / pr->asShotCct - 1e6 / xyToCct(x, y));
      CHECK(pr->illuminantSource == "dng-dual" && mired < 1.0, "source '%s' cct %.1f (want %.1f) off by %.3f mired", pr->illuminantSource.c_str(), pr->asShotCct, xyToCct(x, y), mired);
      std::printf("  DNG probe: source %s, CCT %.1f K (truth %.1f K), xy (%.4f, %.4f)\n", pr->illuminantSource.c_str(), pr->asShotCct, xyToCct(x, y), pr->asShotIlluminantXy[0], pr->asShotIlluminantXy[1]);
    }
    // the same light recorded as AsShotWhiteXY only (no AsShotNeutral): the illuminant is the recorded point, the multipliers follow from the matrices
    g_color.whiteXy[0] = x, g_color.whiteXy[1] = y;
    writeDng(dir / "whitexy.dng", 64, 48, 1, false, {}, {}, px);
    auto pw = makeLibRawDecoder()->probe(dir / "whitexy.dng");
    CHECK(pw.has_value(), "whitexy probe failed");
    if (pw) {
      CHECK(pw->illuminantSource == "dng-whitexy" && std::fabs(pw->asShotIlluminantXy[0] - x) < 1e-4 && std::fabs(pw->asShotIlluminantXy[1] - y) < 1e-4,
            "whitexy source '%s' xy (%.4f, %.4f) want (%.4f, %.4f)", pw->illuminantSource.c_str(), pw->asShotIlluminantXy[0], pw->asShotIlluminantXy[1], x, y);
      double nn[3];
      forward(x, y, nn);  // multipliers = G / neutral
      CHECK(std::fabs(pw->asShotWb[0] - nn[1] / nn[0]) < 1e-3 && std::fabs(pw->asShotWb[2] - nn[1] / nn[2]) < 1e-3, "whitexy WB %.4f %.4f want %.4f %.4f", pw->asShotWb[0], pw->asShotWb[2],
            nn[1] / nn[0], nn[1] / nn[2]);
      std::printf("  AsShotWhiteXY-only DNG: source %s, CCT %.1f K, WB R %.3f B %.3f\n", pw->illuminantSource.c_str(), pw->asShotCct, pw->asShotWb[0], pw->asShotWb[2]);
    }
    g_color = DngColor{};
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
}

// exposure anchor of a file without an embedded preview: tier 2 = the BaselineExposure tag, tier 3 = scene statistics (tag absent or the default 0)
static void testAnchor() {
  const fs::path dir = fs::temp_directory_path() / (std::string("dapple_dng_work_anchor_") + testUniqueTag() + "");
  fs::create_directories(dir);
  auto decode = [&](const char* name) { return makeLibRawDecoder()->decode(dir / name, DecodeOptions{DecodeQuality::Full}); };
  std::vector<std::uint16_t> px(64 * 48, 800);  // flat: (800 - 64) / (4095 - 64) = 0.18 of white
  g_color = DngColor{};
  writeDng(dir / "none.dng", 64, 48, 1, false, {}, {}, px);
  g_color.baselineEv = 0.5;
  writeDng(dir / "tag.dng", 64, 48, 1, false, {}, {}, px);
  g_color = DngColor{};
  auto a = decode("none.dng");
  CHECK(a.has_value(), "decode none");
  if (a) {
    const auto& m = a->meta;
    CHECK(m.exposureAnchorTier == 3, "tier %d (want 3: no preview, no tag)", m.exposureAnchorTier);
    CHECK(m.exposureAnchorEv >= -1.5f && m.exposureAnchorEv <= 3.5f && m.exposureAnchorConfidence < 0.5f, "stats EV %.2f conf %.2f", m.exposureAnchorEv, m.exposureAnchorConfidence);
    std::printf("  anchor, no preview / no tag: tier %d, %+.2f EV, confidence %.2f (%s)%s", m.exposureAnchorTier, m.exposureAnchorEv, m.exposureAnchorConfidence, m.exposureAnchorNote.c_str(), "\n");
  }
  // a reference rendering of the same frame (RAW+JPEG sibling / DecodeOptions::referenceImage): tier 1, and the EV follows the reference level
  {
    auto jpegLevel = [&](const char* name, int level) {
      Image8 im;
      im.width = 256, im.height = 192;
      im.data.assign(std::size_t(256) * 192 * 3, std::uint8_t(level));
      CHECK(writeJpeg(im, dir / name, JpegOptions{97}).has_value(), "write reference jpeg");
    };
    jpegLevel("ref128.jpg", 128);
    jpegLevel("ref160.jpg", 160);
    DecodeOptions o128{DecodeQuality::Full}, o160{DecodeQuality::Full};
    o128.referenceImage = dir / "ref128.jpg";
    o160.referenceImage = dir / "ref160.jpg";
    auto r128 = makeLibRawDecoder()->decode(dir / "none.dng", o128), r160 = makeLibRawDecoder()->decode(dir / "none.dng", o160);
    CHECK(r128.has_value() && r160.has_value(), "decode with reference");
    if (r128 && r160) {
      const float want = std::log2(color::srgbDecode(160.f / 255.f) / color::srgbDecode(128.f / 255.f));
      CHECK(r128->meta.exposureAnchorTier == 1 && r128->meta.exposureAnchorSource == "reference-image", "tier %d source '%s'", r128->meta.exposureAnchorTier, r128->meta.exposureAnchorSource.c_str());
      CHECK(std::fabs((r160->meta.exposureAnchorEv - r128->meta.exposureAnchorEv) - want) < 0.05f, "EV step %.3f want %.3f", r160->meta.exposureAnchorEv - r128->meta.exposureAnchorEv, want);
      std::printf("  anchor from a reference JPEG: level 128 -> %+.2f EV, 160 -> %+.2f EV (step %.3f, want %.3f)%s", r128->meta.exposureAnchorEv, r160->meta.exposureAnchorEv, r160->meta.exposureAnchorEv - r128->meta.exposureAnchorEv, want, "\n");
    }
    // RAW-only capture: the JPEG embedded into the DNG itself (dng_thumbnail.h) is the embedded preview afterwards
    {
      fs::copy_file(dir / "none.dng", dir / "emb.dng", fs::copy_options::overwrite_existing);
      std::ifstream jf(dir / "ref160.jpg", std::ios::binary);
      std::vector<std::uint8_t> jb((std::istreambuf_iterator<char>(jf)), std::istreambuf_iterator<char>());
      std::string err;
      CHECK(dapple::embedDngThumbnail(dir / "emb.dng", jb, 256, 192, &err), "embed: %s", err.c_str());
      CHECK(!dapple::embedDngThumbnail(dir / "emb.dng", jb, 256, 192, &err), "a second embed must be refused (SubIFDs present)");
      auto pv = extractEmbeddedPreview(dir / "emb.dng", 0);
      CHECK(pv.has_value() && pv->width == 256 && pv->height == 192, "embedded preview not found by LibRaw");
      auto e = makeLibRawDecoder()->decode(dir / "emb.dng", DecodeOptions{DecodeQuality::Full});
      CHECK(e.has_value() && e->meta.exposureAnchorTier == 1 && e->meta.exposureAnchorSource == "embedded-preview", "tier %d source '%s'", e ? e->meta.exposureAnchorTier : -1,
            e ? e->meta.exposureAnchorSource.c_str() : "");
      CHECK(e.has_value() && r160 && std::fabs(e->meta.exposureAnchorEv - r160->meta.exposureAnchorEv) < 0.02f, "embedded anchor %.3f vs reference anchor %.3f", e ? e->meta.exposureAnchorEv : 0.f,
            r160 ? r160->meta.exposureAnchorEv : 0.f);
      // the raw data of the patched file is untouched
      auto plain = decode("none.dng"), patched = makeLibRawDecoder()->decode(dir / "emb.dng", DecodeOptions{DecodeQuality::Full});
      CHECK(plain && patched && plain->linear.data == patched->linear.data, "patched DNG decodes differently");
    }
    // sibling discovery: none.jpg next to none.dng
    fs::copy_file(dir / "ref128.jpg", dir / "none.jpg", fs::copy_options::overwrite_existing);
    auto sib = decode("none.dng");
    CHECK(sib.has_value() && sib->meta.exposureAnchorTier == 1 && sib->meta.exposureAnchorSource == "reference-image", "sibling JPEG not picked up");
  }
  auto b = decode("tag.dng");
  CHECK(b.has_value(), "decode tag");
  if (b) {
    CHECK(b->meta.exposureAnchorTier == 2 && std::fabs(b->meta.exposureAnchorEv - 0.5f) < 1e-3f, "tier %d EV %.3f (want 2, 0.5)", b->meta.exposureAnchorTier, b->meta.exposureAnchorEv);
    CameraRegistry reg = CameraRegistry::withBuiltins();
    CHECK(std::fabs(effectiveCamera(reg, b->meta).baselineExposureEv - 0.5f) < 1e-3f, "effectiveCamera does not carry the anchor");
    ShotMetadata none;
    CHECK(effectiveCamera(reg, none).baselineExposureEv == reg.resolve("", "").baselineExposureEv, "no anchor: the profile value must stay");
  }
  std::error_code ec;
  fs::remove_all(dir, ec);
}

// DNG 1.7 JPEG XL: the public Samsung S22 sample (CC0, not in the repo: build/rawsamples, see scripts/samples/download.sh) is decoded by libjxl through
// our tile reader and through the decoder; skipped (exit 77) when the file or the JPEG XL decoder is absent.
static int testJxl() {
  const fs::path f = fs::path("build/rawsamples/Samsung/SM-S901E/20250305_173647.dng");
  std::error_code ec;
  if (!fs::exists(f, ec)) {
    std::printf("  JPEG XL: sample not present, skipped%s", "\n");
    return 77;
  }
  if (!dng::isJxlDng(f)) {
    CHECK(false, "the sample is not recognised as a JPEG XL DNG");
    return 1;
  }
  const dng::JxlRaw r = dng::decodeJxlDng(f);
  if (!r.error.empty() && r.error.find("no JPEG XL decoder") != std::string::npos) {
    std::printf("  JPEG XL: built without libjxl, skipped%s", "\n");
    return 77;
  }
  CHECK(r.error.empty(), "decode: %s", r.error.c_str());
  CHECK(r.width == 4000 && r.height == 3000 && r.channels == 3 && r.data.size() == std::size_t(4000) * 3000 * 3, "size %dx%d x%d", r.width, r.height, r.channels);
  if (r.error.empty()) {
    double sum = 0;
    std::uint16_t mx = 0;
    for (std::uint16_t v : r.data) sum += v, mx = std::max(mx, v);
    CHECK(sum / double(r.data.size()) > 500 && mx > 20000, "samples look empty: mean %.0f max %u", sum / double(r.data.size()), unsigned(mx));
    std::printf("  JPEG XL tiles decoded: %dx%d, 3 samples, mean %.0f, max %u%s", r.width, r.height, sum / double(r.data.size()), unsigned(mx), "\n");
  }
  auto dec = makeLibRawDecoder()->decode(f, DecodeOptions{DecodeQuality::Preview});
  CHECK(dec.has_value(), "decoder: %s", dec ? "" : dec.error().message.c_str());
  if (dec) {
    CHECK(dec->linear.width == 1500 && dec->linear.height == 2000, "preview decode %dx%d (want 1500x2000, orientation 6, half size)", dec->linear.width, dec->linear.height);
    CHECK(dec->meta.exposureAnchorTier > 0, "no exposure anchor");
  }
  return g_fail ? 1 : 0;
}

// FixBadPixelsConstant (OpcodeList1): pixels equal to the constant are replaced by the mean of their same-colour neighbours two pixels away
static void testBadPixels() {
  constexpr int W = 12, H = 12;
  std::vector<std::uint16_t> px(std::size_t(W) * H);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) px[std::size_t(y) * W + x] = std::uint16_t(1000 + 10 * (x % 2) + 20 * (y % 2));  // a flat Bayer pattern: 4 plane values
  const std::uint16_t kBad = 65535;
  px[std::size_t(5) * W + 5] = kBad;  // interior
  px[std::size_t(0) * W + 0] = kBad;  // corner: only two neighbours
  px[std::size_t(5) * W + 7] = kBad;  // two bad pixels in a row of the same colour: the neighbour two to the left is bad, the others are used
  px[std::size_t(5) * W + 5 + 2 * 0] = kBad;
  std::vector<std::uint8_t> params = {0x00, 0x00, 0xFF, 0xFF, 0, 0, 0, 0};  // Constant 65535, BayerPhase 0 (big endian)
  dng::OpcodeList list;
  dng::Opcode op;
  op.id = dng::kFixBadPixelsConstant, op.version = 0x01030000, op.flags = 1, op.params = params;
  list.ops.push_back(op);
  dng::MosaicView v;
  v.px = px.data(), v.pitch = W, v.width = W, v.height = H;
  dng::ApplyReport rep;
  dng::applyMosaic(list, v, rep);
  int bad = 0;
  for (std::uint16_t s : px) bad += s == kBad;
  CHECK(bad == 0, "%d bad pixels left", bad);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) CHECK(px[std::size_t(y) * W + x] == 1000 + 10 * (x % 2) + 20 * (y % 2), "pixel (%d,%d) = %u", x, y, unsigned(px[std::size_t(y) * W + x]));
  CHECK(rep.summary().find("FixBadPixelsConstant (3 px)") != std::string::npos, "report '%s'", rep.summary().c_str());
  std::printf("  FixBadPixelsConstant: 3 bad pixels restored to their plane value, report '%s'%s", rep.summary().c_str(), "\n");
}

int main(int argc, char** argv) {
  const std::string which = argc > 1 ? argv[1] : "all";
  auto want = [&](const char* n) { return which == "all" || which == n; };
  if (want("parse")) testParse();
  if (want("gainmap")) testGainMap();
  if (want("vignette")) testVignette();
  if (want("warp")) testWarp(), perfWarp();
  if (want("e2e")) testEndToEnd();
  if (want("illuminant")) testIlluminant();
  if (want("anchor")) testAnchor();
  if (want("badpixels")) testBadPixels();
  if (which == "jxl") return testJxl();
  std::printf(g_fail ? "FAILED (%d)\n" : "OK\n", g_fail);
  return g_fail ? 1 : 0;
}
