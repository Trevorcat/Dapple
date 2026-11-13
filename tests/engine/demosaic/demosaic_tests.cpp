// Demosaic node tests (ctest "demosaic.*"; docs/engine/pipeline-architecture.md section 32).
//   dapple_demosaic_tests <id> ...   ids: flat edges zoneplate tiles threads cache gpu_parity gpu_tiles gpu_real (see --list)
// Synthetic scenes have a known answer; "real" tests use the public sample files under build/rawsamples when present (exit 77 = skipped).
#include <algorithm>
#include <atomic>
#include <thread>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "de2000.h"
#include "dapple/mosaic.h"
#include "engine/nodes/demosaic/demosaic.h"
#include "engine/nodes/demosaic/demosaic_desc.h"
#include "engine/nodes/demosaic/demosaic_session.h"
#ifdef DAPPLE_HAVE_GPU
#include "engine/exec/gpu_runtime.h"
#include "engine/hal/select.h"
#include "engine/nodes/demosaic/demosaic_backend.h"
#include "engine/nodes/demosaic/demosaic_gpu.h"
#endif

using namespace dapple;
using namespace dapple::demosaic;

namespace {
int g_fail = 0;
#define CHECK(cond, ...)                                              \
  do {                                                                \
    if (!(cond)) {                                                    \
      std::printf("  FAIL %s:%d: %s  ", __FILE__, __LINE__, #cond);   \
      std::printf(__VA_ARGS__);                                       \
      std::printf("\n");                                              \
      ++g_fail;                                                       \
    }                                                                 \
  } while (0)

const char* kXTransRows[6] = {"GBGGRG", "RGRBGB", "GBGGRG", "GRGGBG", "BGBRGR", "GRGGBG"};

// A synthetic sensor: truth image (float RGB in [0,1]) sampled through the CFA, 14-bit levels, identity white balance and colour matrix.
struct Synth {
  int w = 0, h = 0;
  Cfa cfa = Cfa::Bayer;
  std::vector<float> truth;          // w * h * 3
  std::vector<std::uint16_t> raw;
  std::vector<float> black{512.f};
  Desc d;
  Synth(int w_, int h_, Cfa c, const std::vector<float>& t) : w(w_), h(h_), cfa(c), truth(t) {
    d.cfa = c;
    d.period = c == Cfa::Bayer ? 2 : 6;
    for (int y = 0; y < d.period; ++y)
      for (int x = 0; x < d.period; ++x) {
        int col;
        if (c == Cfa::Bayer) col = (y == 0 && x == 0) ? 0 : (y == 1 && x == 1) ? 2 : 1;  // RGGB
        else col = kXTransRows[y][x] == 'R' ? 0 : kXTransRows[y][x] == 'G' ? 1 : 2;
        d.cfa3[y * d.period + x] = std::uint8_t(col);
        d.cfa4[y * d.period + x] = std::uint8_t(col == 1 && c == Cfa::Bayer && (x + y) == 1 && y == 1 ? 3 : col);
      }
    raw.resize(std::size_t(w) * h);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        const int col = d.cfa3[(y % d.period) * d.period + x % d.period];
        const float v = std::clamp(truth[(std::size_t(y) * w + x) * 3 + col], 0.f, 1.f);
        raw[std::size_t(y) * w + x] = std::uint16_t(std::lround(512.f + v * (16383.f - 512.f)));
      }
    d.width = w, d.height = h, d.data = raw.data();
    d.blackRows = d.blackCols = 1;
    d.black = black.data();
    d.invSpan = 1.f / (16383.f - 512.f);
  }
  Synth(const Synth&) = delete;
};

std::vector<float> flatScene(int w, int h, float r, float g, float b) {
  std::vector<float> t(std::size_t(w) * h * 3);
  for (std::size_t i = 0; i < std::size_t(w) * h; ++i) t[3 * i] = r, t[3 * i + 1] = g, t[3 * i + 2] = b;
  return t;
}

// Zone plate: the local frequency grows linearly with the radius and reaches Nyquist at 0.9 of the half diagonal; the three channels are the same
// pattern with different phases (so there is chroma at every frequency).
std::vector<float> zoneScene(int w, int h) {
  std::vector<float> t(std::size_t(w) * h * 3);
  const double R = 0.9 * 0.5 * std::sqrt(double(w) * w + double(h) * h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const double dx = x - 0.5 * w, dy = y - 0.5 * h, ph = 3.14159265358979 * (dx * dx + dy * dy) / (2.0 * R);
      float* p = &t[(std::size_t(y) * w + x) * 3];
      p[0] = float(0.5 + 0.4 * std::cos(ph));
      p[1] = float(0.5 + 0.4 * std::cos(ph + 0.7));
      p[2] = float(0.5 + 0.4 * std::cos(ph + 1.4));
    }
  return t;
}

// Grey (R = G = B) scene: steps, a 1 px and a 2 px line, a diagonal: the classic zipper / false colour probe. Any chroma in the output is an artefact.
std::vector<float> greyScene(int w, int h) {
  std::vector<float> t(std::size_t(w) * h * 3);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      float v = 0.2f;
      if (x > w / 4) v = 0.8f;
      if (y > h / 2 && x < w / 2) v = 0.15f;
      if ((x + y) > (w + h) * 3 / 4 && x > w / 2) v = 0.9f;
      if (x == 3 * w / 5 || (y == h / 3 && x < w / 2) || y == h / 3 + 9 || y == h / 3 + 10) v = 0.05f;
      float* p = &t[(std::size_t(y) * w + x) * 3];
      p[0] = p[1] = p[2] = v;
    }
  return t;
}

std::vector<float> edgeScene(int w, int h) {  // vertical, horizontal and diagonal steps in a coloured field
  std::vector<float> t(std::size_t(w) * h * 3);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      float* p = &t[(std::size_t(y) * w + x) * 3];
      const bool a = x > w / 3, b = y > h / 2, c = (x + y) > (w + h) / 2;
      p[0] = a ? 0.8f : 0.2f;
      p[1] = b ? 0.7f : 0.25f;
      p[2] = c ? 0.9f : 0.3f;
    }
  return t;
}

struct Err { double mae = 0, maxe = 0, psnr = 0; };
Err compare(const Synth& s, const std::vector<float>& out, int border) {
  Err e;
  double sum = 0, sq = 0;
  std::size_t n = 0;
  for (int y = border; y < s.h - border; ++y)
    for (int x = border; x < s.w - border; ++x)
      for (int c = 0; c < 3; ++c) {
        const double d = std::fabs(double(out[(std::size_t(y) * s.w + x) * 3 + c]) - double(s.truth[(std::size_t(y) * s.w + x) * 3 + c]));
        sum += d, sq += d * d, ++n;
        e.maxe = std::max(e.maxe, d);
      }
  e.mae = sum / double(n);
  e.psnr = 10.0 * std::log10(1.0 / std::max(sq / double(n), 1e-12));
  return e;
}

std::vector<float> run(const Desc& d, Rect r, int threads = 0) {
  std::vector<float> out(std::size_t(r.w()) * r.h() * 3);
  dapple::demosaic::demosaic(d, r, out.data(), std::size_t(r.w()) * 3, threads);
  return out;
}

// ---- tests -------------------------------------------------------------------------------------------------------------------

void testFlat() {
  for (Cfa c : {Cfa::Bayer, Cfa::XTrans}) {
    for (const std::array<float, 3>& col : std::vector<std::array<float, 3>>{{0.5f, 0.5f, 0.5f}, {0.8f, 0.4f, 0.1f}, {0.05f, 0.9f, 0.3f}}) {
      Synth s(96, 80, c, flatScene(96, 80, col[0], col[1], col[2]));
      const auto out = run(s.d, {0, 0, s.w, s.h});
      const Err e = compare(s, out, 0);  // including the borders
      std::printf("  flat %s (%.2f %.2f %.2f): max error %.6f\n", c == Cfa::Bayer ? "bayer " : "xtrans", col[0], col[1], col[2], e.maxe);
      CHECK(e.maxe < 2e-4, "flat field not reproduced (incl. borders): max error %.6f", e.maxe);
    }
  }
  // black level: a frame at the black level is exactly black; above white is exactly 1
  {
    Synth s(48, 48, Cfa::Bayer, flatScene(48, 48, 0.f, 0.f, 0.f));
    const auto out = run(s.d, {0, 0, 48, 48});
    for (float v : out) CHECK(v == 0.f, "black frame %.6f", v);
    Synth s2(48, 48, Cfa::XTrans, flatScene(48, 48, 1.f, 1.f, 1.f));
    for (auto& v : s2.raw) v = 16383;
    const auto o2 = run(s2.d, {0, 0, 48, 48});
    for (float v : o2) CHECK(v == 1.f, "saturated frame %.6f", v);
  }
}

double chromaError(const std::vector<float>& out, int w, int h, int border) {  // mean |R - G| + |B - G| (truth is grey)
  double sum = 0;
  std::size_t n = 0;
  for (int y = border; y < h - border; ++y)
    for (int x = border; x < w - border; ++x) {
      const float* p = &out[(std::size_t(y) * w + x) * 3];
      sum += std::fabs(p[0] - p[1]) + std::fabs(p[2] - p[1]), ++n;
    }
  return sum / double(n);
}

void testEdges() {
  for (Cfa c : {Cfa::Bayer, Cfa::XTrans}) {
    Synth s(180, 150, c, edgeScene(180, 150));
    const auto out = run(s.d, {0, 0, s.w, s.h});
    const Err e = compare(s, out, 4);
    std::vector<float> bil(std::size_t(s.w) * s.h * 3);
    bilinear(s.d, {0, 0, s.w, s.h}, bil.data(), std::size_t(s.w) * 3);
    const Err b = compare(s, bil, 4);
    std::printf("  colour edges %s: node MAE %.5f PSNR %.1f dB | bilinear MAE %.5f PSNR %.1f dB\n", c == Cfa::Bayer ? "bayer " : "xtrans", e.mae, e.psnr, b.mae, b.psnr);
    CHECK(e.psnr >= b.psnr, "node must not be worse than bilinear on step edges (%.1f vs %.1f)", e.psnr, b.psnr);
    CHECK(e.psnr > 30.0, "edge PSNR too low: %.1f", e.psnr);
    // grey scene: false colour
    Synth g(200, 160, c, greyScene(200, 160));
    const auto go = run(g.d, {0, 0, g.w, g.h});
    std::vector<float> gb(std::size_t(g.w) * g.h * 3);
    bilinear(g.d, {0, 0, g.w, g.h}, gb.data(), std::size_t(g.w) * 3);
    const double cn = chromaError(go, g.w, g.h, 8), cb = chromaError(gb, g.w, g.h, 8);
    const Err ge = compare(g, go, 8), gbe = compare(g, gb, 8);
    std::printf("  grey edges / lines %s: false colour node %.5f bilinear %.5f | PSNR node %.1f bilinear %.1f dB\n", c == Cfa::Bayer ? "bayer " : "xtrans", cn, cb, ge.psnr, gbe.psnr);
    CHECK(cn < cb * 0.75, "false colour on grey edges not clearly below bilinear (%.5f vs %.5f)", cn, cb);
    CHECK(ge.psnr > gbe.psnr + 1.0, "grey edges: node must beat bilinear by 1 dB (%.1f vs %.1f)", ge.psnr, gbe.psnr);
  }
}

// A hot pixel (one raw sample at white in a flat grey field): how far does it spread, how coloured is it, compared with bilinear?
void testHotPixel() {
  for (Cfa c : {Cfa::Bayer, Cfa::XTrans}) {
    for (int phase = 0; phase < 3; ++phase) {  // hot sample at a red, a green and a blue site
      Synth s(64, 64, c, flatScene(64, 64, 0.3f, 0.3f, 0.3f));
      int hx = 30, hy = 30;
      for (int k = 0; k < 36; ++k) {  // first site of the wanted colour near the centre
        const int x = 30 + k % 6, y = 30 + k / 6;
        if (s.d.cfa3[(y % s.d.period) * s.d.period + x % s.d.period] == phase) { hx = x, hy = y; break; }
      }
      s.raw[std::size_t(hy) * s.w + hx] = 16383;
      const auto out = run(s.d, {0, 0, s.w, s.h});
      std::vector<float> bil(out.size());
      bilinear(s.d, {0, 0, s.w, s.h}, bil.data(), std::size_t(s.w) * 3);
      auto spread = [&](const std::vector<float>& o, double* peakChroma, double* peak) {
        int n = 0;
        *peakChroma = *peak = 0;
        for (int y = 0; y < s.h; ++y)
          for (int x = 0; x < s.w; ++x) {
            const float* p = &o[(std::size_t(y) * s.w + x) * 3];
            const double dev = std::max({std::fabs(p[0] - 0.3), std::fabs(p[1] - 0.3), std::fabs(p[2] - 0.3)});
            if (dev > 0.02) ++n;
            *peak = std::max(*peak, dev);
            *peakChroma = std::max(*peakChroma, double(std::max(std::fabs(p[0] - p[1]), std::fabs(p[2] - p[1]))));
          }
        return n;
      };
      double pcN, pkN, pcB, pkB;
      const int nN = spread(out, &pcN, &pkN), nB = spread(bil, &pcB, &pkB);
      std::printf("  hot pixel %s at a %c site: affected pixels node %d bilinear %d | peak deviation node %.2f bilinear %.2f | peak chroma node %.2f bilinear %.2f\n", c == Cfa::Bayer ? "bayer " : "xtrans", "RGB"[phase], nN, nB, pkN, pkB, pcN, pcB);
      CHECK(nN <= 60, "a single hot sample spreads over %d pixels", nN);
      CHECK(pkN <= 1.0 && pcN <= 1.0, "bounded");
    }
  }
}

#ifdef DAPPLE_HAVE_GPU
std::unique_ptr<hal::Device> makeDevice(std::string& why);
double meanDe(const std::vector<float>& a, const std::vector<float>& b, double* p99, double* maxAbs);
#endif

// Optional isolated-outlier filter on the mosaic: a hot sample at any site disappears, a clipped highlight blob is untouched, tiles agree with the whole frame.
void testHotFilter() {
  for (Cfa c : {Cfa::Bayer, Cfa::XTrans}) {
    for (int phase = 0; phase < 3; ++phase) {
      Synth s(64, 64, c, flatScene(64, 64, 0.3f, 0.3f, 0.3f));
      int hx = 30, hy = 30;
      for (int k = 0; k < 36; ++k) {
        const int x = 30 + k % 6, y = 30 + k / 6;
        if (s.d.cfa3[(y % s.d.period) * s.d.period + x % s.d.period] == phase) { hx = x, hy = y; break; }
      }
      s.raw[std::size_t(hy) * s.w + hx] = 16383;
      s.d.hot = true;
      const auto out = run(s.d, {0, 0, s.w, s.h});
      int n = 0;
      for (std::size_t i = 0; i < out.size(); ++i) n += std::fabs(out[i] - 0.3f) > 0.02f;
      std::printf("  hot filter %s at a %c site: affected values %d\n", c == Cfa::Bayer ? "bayer " : "xtrans", "RGB"[phase], n);
      CHECK(n == 0, "hot sample survived the filter (%d values)", n);
    }
    // a clipped 6 x 6 blob in a dark field must not be filtered
    std::vector<float> t = flatScene(64, 64, 0.1f, 0.1f, 0.1f);
    for (int y = 28; y < 34; ++y)
      for (int x = 28; x < 34; ++x) t[(std::size_t(y) * 64 + x) * 3] = t[(std::size_t(y) * 64 + x) * 3 + 1] = t[(std::size_t(y) * 64 + x) * 3 + 2] = 1.f;
    Synth b(64, 64, c, t);
    const auto off = run(b.d, {0, 0, 64, 64});
    b.d.hot = true;
    const auto on = run(b.d, {0, 0, 64, 64});
    CHECK(off == on, "the filter changed a real (clipped) highlight");
    // tiles with the filter on
    Synth z(150, 130, c, zoneScene(150, 130));
    z.d.hot = true;
    const auto whole = run(z.d, {0, 0, z.w, z.h}, 1);
    const Rect r{13, 21, 97, 88};
    const auto part = run(z.d, r, 2);
    int bad = 0;
    for (int y = r.y0; y < r.y1; ++y)
      for (int x = r.x0; x < r.x1; ++x)
        for (int ch = 0; ch < 3; ++ch) bad += part[(std::size_t(y - r.y0) * r.w() + (x - r.x0)) * 3 + ch] != whole[(std::size_t(y) * z.w + x) * 3 + ch];
    CHECK(bad == 0, "tiles differ with the hot filter on: %d", bad);
#ifdef DAPPLE_HAVE_GPU
    std::string why;
    if (auto dev = makeDevice(why)) {
      std::string err;
      auto gpu = GpuDemosaic::create(*dev, false, err);
      Synth g(150, 130, c, zoneScene(150, 130));
      g.d.hot = true;
      g.raw[60 * 150 + 61] = 16383;
      const auto cpu = run(g.d, {0, 0, g.w, g.h});
      std::vector<float> gp(cpu.size());
      CHECK(gpu->run(g.d, {0, 0, g.w, g.h}, gp.data(), std::size_t(g.w) * 3, 64) == hal::Status::Ok, "gpu hot run");
      double mx = 0;
      for (std::size_t i = 0; i < cpu.size(); ++i) mx = std::max(mx, double(std::fabs(cpu[i] - gp[i])));
      std::printf("  hot filter gpu vs cpu %s: max abs %.2e\n", c == Cfa::Bayer ? "bayer " : "xtrans", mx);
      CHECK(mx < 2e-3, "gpu / cpu differ with the hot filter on: %.2e", mx);
    }
#endif
  }
}

// Clipped highlights: a blob whose red and blue are clipped (scaled 1.0) while green is not (0.62): the data is magenta there and the node, like
// LibRaw with highlight = 0, must reproduce exactly that (no extra hue shift or ringing from interpolating across the clipped samples), and
// nothing may leave [0, 1].
void testClip() {
  for (Cfa c : {Cfa::Bayer, Cfa::XTrans}) {
    std::vector<float> t = flatScene(96, 96, 0.3f, 0.3f, 0.3f);
    for (int y = 30; y < 66; ++y)
      for (int x = 30; x < 66; ++x) t[(std::size_t(y) * 96 + x) * 3] = t[(std::size_t(y) * 96 + x) * 3 + 1] = t[(std::size_t(y) * 96 + x) * 3 + 2] = 1.f;
    Synth s(96, 96, c, t);
    // a white balance that clips red and blue earlier than green (as on every camera): scaled red / blue saturate at 0.55 of the raw range
    s.d.wb[0] = 1.8f, s.d.wb[2] = 1.6f;
    for (int y = 0; y < 96; ++y)
      for (int x = 0; x < 96; ++x) {
        const int col = s.d.cfa3[(y % s.d.period) * s.d.period + x % s.d.period];
        const bool in = x >= 30 && x < 66 && y >= 30 && y < 66;
        const float v = in ? (col == 1 ? 0.62f : 1.f) : 0.3f / (col == 0 ? 1.8f : col == 2 ? 1.6f : 1.f) * 1.f;  // raw: green below clip inside the blob
        s.raw[std::size_t(y) * 96 + x] = std::uint16_t(std::lround(512.f + v * (16383.f - 512.f)));
      }
    const auto out = run(s.d, {0, 0, 96, 96});
    int bad = 0;
    double worst = 0;
    for (int y = 34; y < 62; ++y)
      for (int x = 34; x < 62; ++x) {
        const float* p = &out[(std::size_t(y) * 96 + x) * 3];
        const double ch = std::max({std::fabs(p[0] - 1.0), std::fabs(p[1] - 0.62), std::fabs(p[2] - 1.0)});  // the data itself: R, B clipped, G at 0.62
        worst = std::max(worst, ch);
        if (ch > 0.03) ++bad;
      }
    for (float v : out) CHECK(v >= 0.f && v <= 1.f && v == v, "output outside [0, 1]");
    std::printf("  clipped blob %s: interior pixels deviating > 0.03 from the data colour: %d of %d (worst %.3f)\n", c == Cfa::Bayer ? "bayer " : "xtrans", bad, 28 * 28, worst);
    CHECK(bad == 0, "colour shift inside a clipped highlight: %d pixels", bad);
  }
}

// ---- wiring: the decoder's mosaic path (decodeFullFromMosaic / decodeRawRegion), real sample files, exit 77 when none is present ----
#if defined(_WIN32)
void setEnvVar(const char* k, const char* v) { _putenv_s(k, v); }
#else
void setEnvVar(const char* k, const char* v) { setenv(k, v, 1); }
#endif

std::vector<std::filesystem::path> sampleFiles() {
  std::vector<std::filesystem::path> v;
  for (const char* f : {"build/rawsamples/Fujifilm/X-S10/Fujifilm-X-S10-compressed.RAF", "build/rawsamples/Canon/EOS-R10/IMG_4361.CR3", "build/rawsamples/Sony/ILCE-7M5/full_compressed.ARW",
                        "build/rawsamples/Nikon/D850/Nikon-D850-14bit-lossless-compressed.NEF", "build/rawsamples/Google/Pixel-7-Pro/PXL_20221204_063131635.dng"})
    if (std::filesystem::exists(f)) v.push_back(f);
  return v;
}

int testWireRegion() {
  const auto files = sampleFiles();
  if (files.empty()) return 77;
  setEnvVar("DAPPLE_ANCHOR", "off");
  for (const auto& f : files) {
    std::string why;
    auto full = decodeFullFromMosaic(f, {}, &why);
    if (!full) {
      std::printf("  %s: not on the mosaic path (%s)\n", f.filename().string().c_str(), why.c_str());
      continue;
    }
    const ImageF& im = full->linear;
    if (auto probe = decodeRawRegion(f, 0, 0, 32, 32, {}, &why); !probe && why.find("OpcodeList3") != std::string::npos) {
      std::printf("  %s: lens-warped DNG, a region is refused as designed (%s)\n", f.filename().string().c_str(), why.c_str());
      continue;
    }
    long bad = 0, px = 0;
    unsigned seed = 11;
    auto rnd = [&](int n) { seed = seed * 1664525u + 1013904223u; return int((seed >> 8) % unsigned(std::max(1, n))); };
    for (int k = 0; k < 6; ++k) {
      const int w = 16 + rnd(900), h = 16 + rnd(700), x0 = k == 0 ? 0 : rnd(im.width - w), y0 = k == 1 ? im.height - h : rnd(im.height - h);
      auto reg = decodeRawRegion(f, x0, y0, x0 + w, y0 + h, {}, &why);
      CHECK(bool(reg), "region open failed: %s", why.c_str());
      if (!reg) continue;
      CHECK(reg->linear.width == w && reg->linear.height == h, "region size %dx%d", reg->linear.width, reg->linear.height);
      for (int y = 0; y < h && reg->linear.height == h; ++y)
        for (int x = 0; x < w; ++x)
          for (int c = 0; c < 3; ++c) bad += reg->linear.row(y)[3 * x + c] != im.row(y0 + y)[3 * (x0 + x) + c], ++px;
    }
    std::printf("  %s: 6 regions against the full open, %ld values, %ld differing\n", f.filename().string().c_str(), px, bad);
    CHECK(bad == 0, "a region differs from the same rectangle of the full open (%ld values)", bad);
  }
  return g_fail ? 1 : 0;
}

int testWireAnchor() {
  const auto files = sampleFiles();
  if (files.empty()) return 77;
  setEnvVar("DAPPLE_ANCHOR", "on");
  double worst = 0;
  for (const auto& f : files) {
    clearMosaicCache();
    std::string why;
    setEnvVar("DAPPLE_DEMOSAIC", "on");
    auto m = decodeFullFromMosaic(f, {}, &why);
    setEnvVar("DAPPLE_DEMOSAIC", "off");  // the RGB path, LibRaw does everything
    DecodeOptions o;
    o.quality = DecodeQuality::Full;
    auto r = makeLibRawDecoder()->decode(f, o);
    setEnvVar("DAPPLE_DEMOSAIC", "on");
    if (!m || !r) {
      std::printf("  %s: skipped (%s)\n", f.filename().string().c_str(), m ? "RGB decode failed" : why.c_str());
      continue;
    }
    const double d = std::fabs(double(m->meta.exposureAnchorEv) - double(r->meta.exposureAnchorEv));
    worst = std::max(worst, d);
    std::printf("  %s: anchor mosaic path %+.3f EV (tier %d) RGB path %+.3f EV (tier %d), difference %.4f EV\n", f.filename().string().c_str(), m->meta.exposureAnchorEv, m->meta.exposureAnchorTier, r->meta.exposureAnchorEv, r->meta.exposureAnchorTier, d);
    CHECK(m->meta.exposureAnchorTier == r->meta.exposureAnchorTier, "anchor tier differs");
    CHECK(d <= 0.02, "anchor differs by %.4f EV", d);
  }
  std::printf("  worst difference %.4f EV\n", worst);
  return g_fail ? 1 : 0;
}

int testWireCache() {
  const auto files = sampleFiles();
  if (files.empty()) return 77;
  setEnvVar("DAPPLE_ANCHOR", "off");
  clearMosaicCache();
  unsigned long long h0, m0, h1, m1, h2, m2;
  mosaicCacheCounters(&h0, &m0);
  std::string why;
  auto t0 = std::chrono::steady_clock::now();
  auto a = decodeRawRegion(files[0], 100, 100, 600, 500, {}, &why);
  const double ms1 = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  mosaicCacheCounters(&h1, &m1);
  t0 = std::chrono::steady_clock::now();
  auto b = decodeRawRegion(files[0], 700, 300, 1200, 700, {}, &why);
  const double ms2 = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  auto c = decodeFullFromMosaic(files[0], {}, &why);
  mosaicCacheCounters(&h2, &m2);
  CHECK(a && b && c, "decode failed: %s", why.c_str());
  CHECK(m1 == m0 + 1 && h1 == h0, "first open must decode once");
  CHECK(m2 == m1 && h2 == h1 + 2, "re-opens (region, full) must be served from the cache: hits %llu misses %llu", h2 - h1, m2 - m1);
  std::printf("  first region open %.0f ms (decompress + demosaic), second region %.0f ms (cached mosaic), then a full open: %llu hits, %llu new decodes\n", ms1, ms2, h2 - h1, m2 - m1);
  return g_fail ? 1 : 0;
}

int testWireCancel() {
  const char* f = "build/rawsamples/Fujifilm/X-T50/DSCF0119.RAF";
  if (!std::filesystem::exists(f)) return 77;
  setEnvVar("DAPPLE_ANCHOR", "off");
  std::string why;
  decodeRawRegion(f, 0, 0, 64, 64, {}, &why);  // the mosaic is cached now: only the demosaic is timed
  std::atomic<std::uint64_t> gen{1};
  {
    std::atomic<std::uint64_t> other{2};  // already cancelled before it starts
    auto r = decodeRawRegion(f, 0, 0, 3000, 2000, {}, &why, &other, 1);
    CHECK(!r && why == "cancelled", "a cancelled open must fail with 'cancelled' (%s)", why.c_str());
  }
  auto t0 = std::chrono::steady_clock::now();
  std::thread th([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    gen.store(2);
  });
  auto r = decodeRawRegion(f, 0, 0, 7752, 5178, {}, &why, &gen, 1);  // the whole frame on this thread pool: seconds without the cancel
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  th.join();
  CHECK(!r, "the open was not cancelled");
  CHECK(ms < 800.0, "cancel took %.0f ms to take effect", ms);
  std::printf("  cancelled a whole-frame open after 30 ms: returned after %.0f ms (%s)\n", ms, why.c_str());
  return g_fail ? 1 : 0;
}

#ifdef DAPPLE_HAVE_GPU
// the decoder's GPU path: backend installed (own device), runtime switch on; a region and the full picture against the CPU node
int testWireGpu() {
  const auto files = sampleFiles();
  if (files.empty()) return 77;
  std::string why;
  if (!makeDevice(why)) return 77;
  setEnvVar("DAPPLE_ANCHOR", "off");
  engine::gpu::setRuntimeEnabled(1);
  for (const auto& f : {files[0], files[1]}) {
    clearMosaicCache();
    demosaic::setGpuBackend(nullptr);
    auto cpu = decodeRawRegion(f, 300, 200, 1500, 1100, {}, &why);
    demosaic::installGpuBackend();
    auto gpu = decodeRawRegion(f, 300, 200, 1500, 1100, {}, &why);
    CHECK(cpu && gpu, "decode failed %s", why.c_str());
    if (!cpu || !gpu) continue;
    double p99, mx;
    const double mean = meanDe(gpu->linear.data, cpu->linear.data, &p99, &mx);
    std::printf("  %s: GPU backend vs CPU node on a 1200 x 900 region: mean dE2000 %.4f p99 %.4f max abs %.2e\n", f.filename().string().c_str(), mean, p99, mx);
    CHECK(mean <= 0.1 && p99 <= 1.0, "decoder GPU path off the CPU node");
  }
  demosaic::setGpuBackend(nullptr);
  return g_fail ? 1 : 0;
}
#endif

void testZonePlate() {
  for (Cfa c : {Cfa::Bayer, Cfa::XTrans}) {
    Synth s(256, 256, c, zoneScene(256, 256));
    const auto out = run(s.d, {0, 0, s.w, s.h});
    std::vector<float> bil(std::size_t(s.w) * s.h * 3);
    bilinear(s.d, {0, 0, s.w, s.h}, bil.data(), std::size_t(s.w) * 3);
    const Err e = compare(s, out, 8), b = compare(s, bil, 8);
    std::printf("  zone plate %s: node PSNR %.1f dB MAE %.4f | bilinear PSNR %.1f dB MAE %.4f\n", c == Cfa::Bayer ? "bayer " : "xtrans", e.psnr, e.mae, b.psnr, b.mae);
    CHECK(e.psnr > b.psnr, "node must beat bilinear on the zone plate (%.1f vs %.1f dB)", e.psnr, b.psnr);
  }
}

void testTiles() {
  for (Cfa c : {Cfa::Bayer, Cfa::XTrans}) {
    Synth s(301, 257, c, zoneScene(301, 257));  // odd sizes: tile edges and picture edges at every phase
    const auto whole = run(s.d, {0, 0, s.w, s.h}, 1);
    int rois = 0, bad = 0;
    unsigned seed = 7;
    auto rnd = [&](int n) { seed = seed * 1664525u + 1013904223u; return int((seed >> 8) % unsigned(n)); };
    for (int k = 0; k < 40; ++k) {
      Rect r;
      r.x0 = k < 6 ? (k & 1 ? 0 : s.w - 17) : rnd(s.w - 8);
      r.y0 = k < 6 ? (k & 2 ? 0 : s.h - 13) : rnd(s.h - 8);
      r.x1 = std::min(s.w, r.x0 + 1 + rnd(120));
      r.y1 = std::min(s.h, r.y0 + 1 + rnd(120));
      const auto part = run(s.d, r, 2);
      for (int y = r.y0; y < r.y1; ++y)
        for (int x = r.x0; x < r.x1; ++x)
          for (int ch = 0; ch < 3; ++ch)
            if (part[(std::size_t(y - r.y0) * r.w() + (x - r.x0)) * 3 + ch] != whole[(std::size_t(y) * s.w + x) * 3 + ch]) ++bad;
      ++rois;
    }
    std::printf("  tiles %s: %d random regions of interest vs the whole frame, %d differing values\n", c == Cfa::Bayer ? "bayer " : "xtrans", rois, bad);
    CHECK(bad == 0, "region of interest differs from the whole frame in %d values (halo too small?)", bad);
  }
}

void testThreads() {
  for (Cfa c : {Cfa::Bayer, Cfa::XTrans}) {
    Synth s(400, 300, c, zoneScene(400, 300));
    const auto a = run(s.d, {0, 0, s.w, s.h}, 1), b = run(s.d, {0, 0, s.w, s.h}, 7);
    CHECK(a == b, "1 thread and 7 threads differ");
  }
  std::printf("  threads: identical output for 1 and 7 threads\n");
}

void testCache() {
  MosaicCache cache(std::size_t(64) << 20);
  Synth s(128, 96, Cfa::XTrans, zoneScene(128, 96));
  auto mos = std::make_shared<MosaicImage>();
  mos->width = s.w, mos->height = s.h, mos->cfa = CfaKind::XTrans, mos->period = 6;
  mos->data = s.raw;
  for (int i = 0; i < 36; ++i) mos->cfaIdx[i] = s.d.cfa4[i];
  mos->blackRows = mos->blackCols = 1, mos->black = {512.f}, mos->white = 16383.f, mos->blackMin = 512.f;
  mos->key[0] = 0x1234, mos->key[1] = 0x5678;
  std::shared_ptr<const MosaicImage> cm = mos;
  bool hit = true;
  auto a = cache.full(cm, 0, &hit);
  CHECK(!hit, "first demosaic must miss");
  hit = false;
  auto b = cache.full(cm, 0, &hit);
  CHECK(hit && a.get() == b.get(), "second request must hit and return the cached result (no second demosaic)");
  auto other = std::make_shared<MosaicImage>(*mos);
  other->key[0] = 99;
  bool hit2 = true;
  cache.full(other, 0, &hit2);
  CHECK(!hit2, "a different file key must miss");
  const CacheStats st = cache.stats();
  CHECK(st.fullHits == 1 && st.fullMisses == 2, "stats %llu hits %llu misses", (unsigned long long)st.fullHits, (unsigned long long)st.fullMisses);
  // eviction: a budget of one result keeps the newest only; a consumer still holds its shared_ptr
  MosaicCache small(128 * 96 * 12 + 100);
  auto r1 = small.full(cm, 0), r2 = small.full(other, 0);
  bool h3 = true;
  small.full(cm, 0, &h3);
  CHECK(!h3 && small.stats().evictions >= 1, "evicted entry must be recomputed");
  CHECK(r1 && r1->width == 128, "evicted result stays valid for its holder");
  std::printf("  cache: hit / miss / eviction semantics ok (hits %llu, misses %llu, evictions %llu)\n", (unsigned long long)st.fullHits, (unsigned long long)st.fullMisses, (unsigned long long)small.stats().evictions);
  // the mosaic of a real file: decode once, then served from the cache
  const std::filesystem::path f = "build/rawsamples/Fujifilm/X-S10/Fujifilm-X-S10-compressed.RAF";
  if (std::filesystem::exists(f)) {
    MosaicCache c2;
    bool h = true;
    auto t0 = std::chrono::steady_clock::now();
    auto m1 = c2.mosaic(f, {}, &h);
    const double ms1 = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(m1 && !h, "first open must decode");
    h = false;
    t0 = std::chrono::steady_clock::now();
    auto m2 = c2.mosaic(f, {}, &h);
    const double ms2 = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(m2 && h && m1->get() == m2->get(), "re-open must be a cache hit (same object)");
    std::printf("  cache (X-S10 compressed RAF): first open %.0f ms, re-open %.3f ms (hit %d)\n", ms1, ms2, int(h));
  }
}

#ifdef DAPPLE_HAVE_GPU
std::unique_ptr<hal::Device> makeDevice(std::string& why) {
  hal::DeviceOptions o;
  if (const char* v = std::getenv("DAPPLE_GPU_SOFTWARE")) o.allowSoftware = v[0] == '1';
  hal::Selection sel = hal::selectDevice(o);
  if (!sel.device) {
    for (const hal::Attempt& a : sel.attempts) why += (why.empty() ? "" : "; ") + a.detail;
    return nullptr;
  }
  return std::move(sel.device);
}

double meanDe(const std::vector<float>& a, const std::vector<float>& b, double* p99, double* maxAbs) {
  std::vector<float> de;
  double ma = 0;
  for (std::size_t i = 0; i + 2 < a.size(); i += 3) {
    de.push_back(float(demo::dE2000(demo::toLab(&a[i], 4.f), demo::toLab(&b[i], 4.f))));
    for (int c = 0; c < 3; ++c) ma = std::max(ma, double(std::fabs(a[i + c] - b[i + c])));
  }
  double s = 0;
  for (float v : de) s += v;
  std::nth_element(de.begin(), de.begin() + std::ptrdiff_t(de.size() * 99 / 100), de.end());
  *p99 = de[de.size() * 99 / 100];
  *maxAbs = ma;
  return s / double(de.size());
}

int testGpuParity() {
  std::string why;
  auto dev = makeDevice(why);
  if (!dev) {
    std::printf("  no GPU device (%s)\n", why.c_str());
    return 77;
  }
  std::printf("  device: %s\n", dev->caps().text().c_str());
  std::string err;
  auto gpu = GpuDemosaic::create(*dev, false, err);
  auto gpu16 = GpuDemosaic::create(*dev, true, err);
  for (Cfa c : {Cfa::Bayer, Cfa::XTrans}) {
    Synth s(333, 271, c, zoneScene(333, 271));
    // a non-trivial colour matrix and multipliers so those paths are covered
    const float mtx[9] = {1.3f, -0.2f, -0.1f, -0.1f, 1.2f, -0.1f, 0.0f, -0.3f, 1.3f};
    std::memcpy(s.d.m, mtx, sizeof mtx);
    s.d.wb[0] = 1.8f, s.d.wb[2] = 1.4f;
    const auto cpu = run(s.d, {0, 0, s.w, s.h});
    std::vector<float> g(cpu.size());
    GpuStats st;
    hal::Status rc = gpu->run(s.d, {0, 0, s.w, s.h}, g.data(), std::size_t(s.w) * 3, 128, &st);
    CHECK(rc == hal::Status::Ok, "gpu run failed: %s", gpu->error().c_str());
    if (rc != hal::Status::Ok) continue;
    double p99, mx;
    const double mean = meanDe(g, cpu, &p99, &mx);
    std::printf("  gpu vs cpu %s (fp32 output): mean dE2000 %.5f p99 %.5f max abs %.2e, %d tiles\n", c == Cfa::Bayer ? "bayer " : "xtrans", mean, p99, mx, st.tiles);
    CHECK(mean <= 0.1 && p99 <= 1.0, "parity budget exceeded: mean %.4f p99 %.4f", mean, p99);
    CHECK(mx < 2e-3, "max abs difference %.2e", mx);
    std::vector<float> g16(cpu.size());
    rc = gpu16->run(s.d, {0, 0, s.w, s.h}, g16.data(), std::size_t(s.w) * 3, 128, &st);
    CHECK(rc == hal::Status::Ok, "gpu fp16 run failed");
    if (rc == hal::Status::Ok) {
      const double m16 = meanDe(g16, cpu, &p99, &mx);
      std::printf("  gpu vs cpu %s (RGBA16F output): mean dE2000 %.5f p99 %.5f max abs %.2e\n", c == Cfa::Bayer ? "bayer " : "xtrans", m16, p99, mx);
      CHECK(m16 <= 0.1 && p99 <= 1.0, "fp16 parity budget exceeded");
    }
  }
  return g_fail ? 1 : 0;
}

int testGpuTiles() {
  std::string why;
  auto dev = makeDevice(why);
  if (!dev) return 77;
  std::string err;
  auto gpu = GpuDemosaic::create(*dev, false, err);
  for (Cfa c : {Cfa::Bayer, Cfa::XTrans}) {
    Synth s(301, 257, c, zoneScene(301, 257));
    std::vector<float> whole(std::size_t(s.w) * s.h * 3), tiled(whole.size()), part;
    CHECK(gpu->run(s.d, {0, 0, s.w, s.h}, whole.data(), std::size_t(s.w) * 3, 512) == hal::Status::Ok, "whole frame");
    CHECK(gpu->run(s.d, {0, 0, s.w, s.h}, tiled.data(), std::size_t(s.w) * 3, 64) == hal::Status::Ok, "tiled");
    std::size_t bad = 0;
    for (std::size_t i = 0; i < whole.size(); ++i) bad += whole[i] != tiled[i];
    std::printf("  gpu tiles %s: whole frame (1 tile) vs 64 px tiles: %zu differing values\n", c == Cfa::Bayer ? "bayer " : "xtrans", bad);
    CHECK(bad == 0, "tile seams: %zu differing values", bad);
    // a region of interest alone
    const Rect r{37, 41, 211, 199};
    part.assign(std::size_t(r.w()) * r.h() * 3, 0.f);
    CHECK(gpu->run(s.d, r, part.data(), std::size_t(r.w()) * 3, 100) == hal::Status::Ok, "roi");
    std::size_t bad2 = 0;
    for (int y = r.y0; y < r.y1; ++y)
      for (int x = r.x0; x < r.x1; ++x)
        for (int ch = 0; ch < 3; ++ch) bad2 += part[(std::size_t(y - r.y0) * r.w() + (x - r.x0)) * 3 + ch] != whole[(std::size_t(y) * s.w + x) * 3 + ch];
    CHECK(bad2 == 0, "roi differs from the whole frame in %zu values", bad2);
  }
  return g_fail ? 1 : 0;
}

int testGpuReal() {
  const char* files[] = {"build/rawsamples/Fujifilm/X-T50/DSCF0119.RAF", "build/rawsamples/Sony/ILCE-7RM4/DSC00396.ARW"};
  std::string why;
  auto dev = makeDevice(why);
  if (!dev) return 77;
  std::string err;
  auto gpu = GpuDemosaic::create(*dev, false, err);
  int ran = 0;
  for (const char* f : files) {
    if (!std::filesystem::exists(f)) continue;
    auto mr = decodeRawMosaic(f);
    if (!mr || mr->cfa == CfaKind::None) continue;
    const Desc d = makeDesc(*mr);
    const Rect roi{mr->width / 2 - 600, mr->height / 2 - 400, mr->width / 2 + 600, mr->height / 2 + 400};
    const auto cpu = run(d, roi);
    std::vector<float> g(cpu.size());
    CHECK(gpu->run(d, roi, g.data(), std::size_t(roi.w()) * 3, 512) == hal::Status::Ok, "gpu real");
    double p99, mx;
    const double mean = meanDe(g, cpu, &p99, &mx);
    std::printf("  gpu vs cpu on a 1200 x 800 crop of %s: mean dE2000 %.5f p99 %.5f max abs %.2e\n", std::filesystem::path(f).filename().string().c_str(), mean, p99, mx);
    CHECK(mean <= 0.1 && p99 <= 1.0, "real-file parity budget exceeded");
    ++ran;
  }
  if (!ran) return 77;
  return g_fail ? 1 : 0;
}
#endif
}  // namespace

int main(int argc, char** argv) {
  const std::vector<std::string> all = {"flat", "edges", "zoneplate", "hotpixel", "hotfilter", "clip", "tiles", "threads", "cache", "wire_region", "wire_anchor", "wire_cache", "wire_cancel", "gpu_parity", "gpu_tiles", "gpu_real", "wire_gpu"};
  std::vector<std::string> ids;
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--list") {
      for (auto& a : all) std::printf("%s\n", a.c_str());
      return 0;
    }
    ids.push_back(argv[i]);
  }
  if (ids.empty()) ids = {"flat", "edges", "zoneplate", "hotpixel", "hotfilter", "clip", "tiles", "threads", "cache"};
  int rc = 0;
  for (const auto& id : ids) {
    std::printf("[%s]\n", id.c_str());
    const int before = g_fail;
    if (id == "flat") testFlat();
    else if (id == "edges") testEdges();
    else if (id == "zoneplate") testZonePlate();
    else if (id == "hotpixel") testHotPixel();
    else if (id == "clip") testClip();
    else if (id == "hotfilter") testHotFilter();
    else if (id == "tiles") testTiles();
    else if (id == "threads") testThreads();
    else if (id == "cache") testCache();
    else if (id == "wire_region") { const int r = testWireRegion(); if (r == 77 && g_fail == before) return 77; }
    else if (id == "wire_anchor") { const int r = testWireAnchor(); if (r == 77 && g_fail == before) return 77; }
    else if (id == "wire_cache") { const int r = testWireCache(); if (r == 77 && g_fail == before) return 77; }
    else if (id == "wire_cancel") { const int r = testWireCancel(); if (r == 77 && g_fail == before) return 77; }
#ifdef DAPPLE_HAVE_GPU
    else if (id == "gpu_parity") { const int r = testGpuParity(); if (r == 77 && g_fail == before) return 77; }
    else if (id == "gpu_tiles") { const int r = testGpuTiles(); if (r == 77 && g_fail == before) return 77; }
    else if (id == "gpu_real") { const int r = testGpuReal(); if (r == 77 && g_fail == before) return 77; }
    else if (id == "wire_gpu") { const int r = testWireGpu(); if (r == 77 && g_fail == before) return 77; }
#else
    else if (id.rfind("gpu_", 0) == 0) return 77;
#endif
    else {
      std::printf("unknown test %s\n", id.c_str());
      return 2;
    }
    std::printf("%s %s\n", g_fail == before ? "PASS" : "FAIL", id.c_str());
  }
  rc = g_fail ? 1 : 0;
  return rc;
}
