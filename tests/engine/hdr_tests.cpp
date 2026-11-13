// Single-image HDR (EditParams::hdr, core/src/hdr_field.h, node NodeType::Hdr). Docs: pipeline-architecture.md "Single-image HDR".
//   dapple_hdr_tests <id>   ids: identity lowrange anchors midband tiles monotonic halo noise [parity bench with -DDAPPLE_GPU=ON; exit 77: no GPU]
//   dapple_hdr_tests rank <files...>   base-layer dynamic range (p1..p99 of the base EV) of RAW files, highest first
//     identity   hdr = 0 bit-identical, old Renderer ignores hdr, JSON clamp / round trip
//     lowrange   a scene whose base range fits what the tone curve shows well is not touched at strength 100
//     anchors    black point and mid-grey do not move; the base map never inverts (slope >= 0.3)
//     midband    mid-frequency band energy of log luminance at 50 / 100 relative to 0 is >= 1
//     tiles      TiledExport == Whole at hdr 100
//     monotonic  on the 12 EV scene: shadow percentile rises, highlight percentile falls with the strength; clipped stays white
//     halo       synthetic step edge with texture: gain next to the edge deviates from its plateau by < 0.15 EV
//     noise      dark noisy patch next to a bright area: noise amplification bounded
//     parity     GPU executor vs CPU executor, same frame: dE2000 mean <= 0.15, p99 <= 1.5
//     bench      frame cost with and without hdr
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "../deterministic.h"
#include "colour_metrics.h"
#include "dapple/camera.h"
#include "dapple/decoder.h"
#include "dapple/edit_params.h"
#include "dapple/renderer.h"
#include "engine/exec/cpu_executor.h"
#include "engine/exec/engine_session.h"
#include "hdr_field.h"
#ifdef DAPPLE_HDR_GPU
#include "engine/exec/gpu_executor.h"
#include "gpu_rig.h"
#include "parity.h"
#endif

namespace {

using namespace dapple;
using namespace dapple::engine;
using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

int g_failures = 0;
#define CHECK(cond)                                                                  \
  do {                                                                               \
    if (!(cond)) {                                                                   \
      std::printf("  CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
      ++g_failures;                                                                  \
    }                                                                                \
  } while (0)

const CameraProfile& cam() {
  static const CameraRegistry reg = CameraRegistry::withBuiltins();
  return reg.resolve("", "");
}

// The hard case: a dark textured interior (about 0.0007 .. 0.003, i.e. 11 EV under the window), a mid floor, a bright textured window with a
// clipped core, a lit wall patch, noise. `wide` = false: the same picture squeezed into 2.5 EV (an overcast day).
SourceImage makeScene(int w, int h, bool wide = true) {
  SourceImage s;
  s.sceneReferred = true;
  s.meta.iso = 200.f;
  s.linear = ImageF(w, h);
  std::mt19937 rng(7);
  std::normal_distribution<float> nd(0.f, 1.f);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const float u = float(x) / float(w - 1), v = float(y) / float(h - 1);
      float lum = 0.003f + 0.008f * u;                                   // dark room
      if (v > 0.62f) lum = 0.05f + 0.06f * u;                            // floor
      if (u > 0.55f && u < 0.9f && v > 0.12f && v < 0.55f) lum = 0.7f;   // window
      if (u > 0.62f && u < 0.78f && v > 0.2f && v < 0.4f) lum = 1.0f;  // clipped core
      if (u < 0.3f && v < 0.5f) lum = 0.12f;                             // lit wall patch
      if (!wide) lum = 0.06f * std::pow(lum / 0.06f, 0.2f);              // 12 EV -> 2.4 EV around 0.06
      const float tex = (((x / 5) + (y / 5)) & 1) ? 1.10f : 0.91f;
      const float n = 1.f + 0.03f * nd(rng);
      float* p = s.linear.row(y) + 3 * x;
      p[0] = std::min(1.f, lum * tex * n * 1.05f);
      p[1] = std::min(1.f, lum * tex * n);
      p[2] = std::min(1.f, lum * tex * n * 0.9f);
    }
  return s;
}

Image8 renderRef(const EditParams& p, const SourceImage& src) { return Renderer(p, cam(), nullptr).render(src); }

bool exactRender(EngineSession& es, const EditParams& p, const SourceImage& src, ExecMode mode, int tile, Image8& out, ExecStats* st = nullptr) {
  const Renderer r(p, cam(), nullptr);
  EngineSession::ExactOptions o;
  o.mode = mode;
  o.tile = tile;
  o.sessionCache = false;
  std::string err;
  const bool ok = es.renderExact(p, r, src, o, out, &err, st);
  if (!ok) std::printf("  renderExact failed: %s\n", err.c_str());
  return ok;
}

float lumaOf(const Image8& im, std::size_t i) {
  return (0.2126f * im.data[3 * i] + 0.7152f * im.data[3 * i + 1] + 0.0722f * im.data[3 * i + 2]) / 255.f;
}
float percentile(const Image8& im, float q) {
  std::vector<float> v(std::size_t(im.width) * im.height);
  for (std::size_t i = 0; i < v.size(); ++i) v[i] = lumaOf(im, i);
  std::sort(v.begin(), v.end());
  return v[std::size_t(q * float(v.size() - 1))];
}

// ---- identity ----------------------------------------------------------------------------------------------------------------
void testIdentity() {
  const SourceImage src = makeScene(613, 421);
  EditParams p;
  p.exposure = 0.2f;
  p.contrast = 8.f;
  EngineSession es;
  Image8 ref = renderRef(p, src), got;
  CHECK(exactRender(es, p, src, ExecMode::Whole, 0, got));
  CHECK(ref.data == got.data);
  std::printf("  hdr 0: engine exact == Renderer::render: %s\n", ref.data == got.data ? "byte-identical" : "DIFFERENT");
  EditParams q = p;
  q.hdr = 100.f;
  const Image8 oldPath = renderRef(q, src);
  CHECK(oldPath.data == ref.data);
  std::printf("  hdr 100 through the old Renderer (DAPPLE_ENGINE=off path): %s\n", oldPath.data == ref.data ? "ignored (identical)" : "CHANGED");
  Image8 on;
  CHECK(exactRender(es, q, src, ExecMode::Whole, 0, on));
  CHECK(on.data != ref.data);
  // parameter: declared 0..100, clamped, serialised, round trip
  {
    const auto r = fromJson("{\"hdr\": 60}");
    CHECK(r && r->hdr == 60.f);
    const auto hi = fromJson("{\"hdr\": 250}");
    CHECK(hi && hi->hdr == 100.f);
    const auto lo = fromJson("{\"hdr\": -5}");
    CHECK(lo && lo->hdr == 0.f);
    EditParams q2;
    q2.hdr = 35.f;
    const auto back = fromJson(toJson(q2));
    CHECK(back && back->hdr == 35.f);
    CHECK(toJsonDiff(EditParams{}).find("hdr") == std::string::npos);
    CHECK(toJsonDiff(q2).find("\"hdr\"") != std::string::npos);
  }
  // the graph of hdr = 0 has no Hdr node
  FramePlan fp;
  const Renderer r0(p, cam(), nullptr);
  r0.hdrBind(src, fp.hdr);
  CHECK(!fp.hdr.active());
  const Renderer r1(q, cam(), nullptr);
  const auto lut1 = r1.bake(src, 33);
  r1.hdrBind(src, fp.hdr, &lut1->tone);
  CHECK(fp.hdr.active());
}

// ---- tiles -------------------------------------------------------------------------------------------------------------------
void testTiles() {
  const SourceImage src = makeScene(613, 421);
  EditParams p;
  p.exposure = 0.2f;
  p.contrast = 8.f;
  p.hdr = 100.f;
  p.clarity = 20.f;
  p.crop = "4:5";
  p.straighten = 2.f;
  EngineSession es;
  Image8 whole;
  CHECK(exactRender(es, p, src, ExecMode::Whole, 0, whole));
  for (int tile : {96, 200}) {
    Image8 t;
    ExecStats st;
    CHECK(exactRender(es, p, src, ExecMode::TiledExport, tile, t, &st));
    CHECK(t.width == whole.width && t.height == whole.height);
    int maxCode = 0;
    std::size_t differing = 0;
    for (std::size_t i = 0; i < whole.data.size() && i < t.data.size(); ++i) {
      const int d = std::abs(int(whole.data[i]) - int(t.data[i]));
      maxCode = std::max(maxCode, d);
      differing += d != 0;
    }
    std::printf("  tile %3d: %d tiles, max code %d, differing bytes %zu\n", tile, st.tiles, maxCode, differing);
    CHECK(st.tiles > 1);
    CHECK(maxCode <= 1);
  }
}

// ---- monotonic ---------------------------------------------------------------------------------------------------------------
void testMonotonic() {
  const SourceImage src = makeScene(640, 432);
  EngineSession es;
  float prevLo = -1.f, prevHi = 2.f, core0 = 0.f, black0 = 0.f;
  for (float s : {0.f, 25.f, 50.f, 75.f, 100.f}) {
    EditParams p;
    p.hdr = s;
    Image8 im;
    CHECK(exactRender(es, p, src, ExecMode::Whole, 0, im));
    const float p5 = percentile(im, 0.05f), lo = percentile(im, 0.30f), hi = percentile(im, 0.95f), mean = [&] {
      double a = 0;
      for (std::size_t i = 0; i < std::size_t(im.width) * im.height; ++i) a += lumaOf(im, i);
      return float(a / (double(im.width) * im.height));
    }();
    // the clipped core of the window (u 0.62..0.78, v 0.2..0.4): stays white
    double cs = 0;
    int cn = 0;
    for (int y = int(0.25f * float(im.height)); y < int(0.35f * float(im.height)); ++y)
      for (int x = int(0.66f * float(im.width)); x < int(0.74f * float(im.width)); ++x, ++cn) cs += lumaOf(im, std::size_t(y) * im.width + x);
    const float core = float(cs / cn);
    std::printf("  hdr %3.0f: p5 %.4f  p30 %.4f  p95 %.4f  mean %.4f  clipped core %.3f\n", s, p5, lo, hi, mean, core);
    if (s == 0.f) black0 = p5;
    else CHECK(std::abs(p5 - black0) < 0.012f);  // black does not rise
    CHECK(lo >= prevLo - 1e-4f);
    CHECK(hi <= prevHi + 1e-4f);
    if (s == 0.f) core0 = core;
    else CHECK(std::abs(core - core0) < 0.02f);  // a clipped highlight is not greyed out
    if (s > 0.f) {
      CHECK(lo > prevLo || prevLo < 0.f);
      CHECK(hi < prevHi);
    }
    prevLo = lo;
    prevHi = hi;
  }
}

// ---- lowrange ----------------------------------------------------------------------------------------------------------------
double meanDE(const Image8& a, const Image8& b, double* maxOut = nullptr) {
  double sum = 0, mx = 0;
  const std::size_t n = std::size_t(a.width) * a.height;
  for (std::size_t i = 0; i < n; ++i) {
    const float fa[3] = {a.data[3 * i] / 255.f, a.data[3 * i + 1] / 255.f, a.data[3 * i + 2] / 255.f};
    const float fb[3] = {b.data[3 * i] / 255.f, b.data[3 * i + 1] / 255.f, b.data[3 * i + 2] / 255.f};
    const double de = metrics::deltaE2000(metrics::srgbToLab(fa), metrics::srgbToLab(fb));
    sum += de;
    mx = std::max(mx, de);
  }
  if (maxOut) *maxOut = mx;
  return sum / double(n);
}

void testLowRange() {
  EngineSession es;
  EditParams a, b;
  b.hdr = 100.f;
  // an overcast scene (2.4 EV): not touched at all at strength 100
  {
    const SourceImage src = makeScene(640, 432, false);
    Image8 ia, ib;
    CHECK(exactRender(es, a, src, ExecMode::Whole, 0, ia));
    CHECK(exactRender(es, b, src, ExecMode::Whole, 0, ib));
    FramePlan fp;
    const Renderer rb0(b, cam(), nullptr);
    const auto lut0 = rb0.bake(src, 33);
    rb0.hdrBind(src, fp.hdr, &lut0->tone);
    double mx = 0;
    const double m = meanDE(ia, ib, &mx);
    std::printf("  %.1f EV scene (shown range %.1f EV) at hdr 100: node %s, dE2000 mean %.4f max %.3f\n", fp.hdr.range, fp.hdr.shown, fp.hdr.active() ? "active" : "not in the graph", m, mx);
    CHECK(m < 0.05 && mx < 0.5);
  }
  // a middling scene (the scene squeezed to about 6 EV, 1.5 x what the tone curve shows well): moves little
  {
    SourceImage mid = makeScene(640, 432, true);
    for (float& v : mid.linear.data) v = 0.05f * std::pow(std::max(v, 1e-6f) / 0.05f, 0.8f);
    Image8 ia, ib;
    CHECK(exactRender(es, a, mid, ExecMode::Whole, 0, ia));
    CHECK(exactRender(es, b, mid, ExecMode::Whole, 0, ib));
    FramePlan fp;
    const Renderer rb(b, cam(), nullptr);
    const auto lutb = rb.bake(mid, 33);
    rb.hdrBind(mid, fp.hdr, &lutb->tone);
    const double m = meanDE(ia, ib);
    std::printf("  %.1f EV scene (shown range %.1f EV) at hdr 100: node %s, dE2000 mean %.3f\n", fp.hdr.range, fp.hdr.shown, fp.hdr.active() ? "active" : "not in the graph", m);
    CHECK(m < 3.5);
  }
}

// ---- anchors -----------------------------------------------------------------------------------------------------------------
HdrParams hardParams(const SourceImage& src, std::shared_ptr<const HdrField>& f, float s = 1.f, float T = 4.2f) {
  const float wb[3] = {1.f, 1.f, 1.f};
  f = hdr::fieldFor(src, wb);
  return hdr::resolve(*f, s, 0.f, src.meta.iso, wb, T);
}

void testAnchors() {
  const SourceImage src = makeScene(640, 432);
  std::shared_ptr<const HdrField> f;
  const HdrParams p = hardParams(src, f);
  std::printf("  12 EV scene: base range %.1f .. %.1f EV (R %.1f), key %.1f, lift %.2f EV, pull %.2f EV\n", p.lo, p.hi, p.hi - p.lo, p.key, p.ls, p.hp);
  CHECK(p.active());
  float slope = 1.f, minSlope = 9.f;
  for (float b = p.lo - 2.f; b < p.hi + 2.f; b += 0.05f) {
    float sl;
    const float g = hdr::baseGain(p, b, &sl);
    minSlope = std::min(minSlope, sl);
    if (b <= p.lo + 1e-3f) CHECK(std::abs(g) < 1e-4f);  // black does not move (nor anything below it)
  }
  CHECK(minSlope >= 0.29f);
  CHECK(std::abs(hdr::baseGain(p, p.key, &slope)) < 1e-4f && std::abs(slope - 1.f) < 1e-3f);  // mid-grey does not move, local slope 1
  CHECK(hdr::baseGain(p, p.lo + 0.33f * (p.key - p.lo)) > 0.f);                                // lifted ABOVE black
  CHECK(hdr::baseGain(p, p.hi) < -0.05f);                                                      // highlights pulled
  std::printf("  base map slope >= %.2f everywhere; gain(black) 0, gain(key) 0, lift at a third %.2f EV, pull at the top %.2f EV\n", minSlope,
              hdr::baseGain(p, p.lo + 0.33f * (p.key - p.lo)), -hdr::baseGain(p, p.hi));
  {
    EditParams bp;
    bp.hdr = 100.f;
    Renderer rr(bp, cam(), nullptr);
    const auto lut = rr.bake(src, 33);
    FramePlan fp;
    rr.hdrBind(src, fp.hdr, &lut->tone);
    std::printf("  with the real tone curve: R %.1f EV, shown range T %.1f EV, lift %.2f EV, pull %.2f EV\n", fp.hdr.range, fp.hdr.shown, fp.hdr.ls, fp.hdr.hp);
  }
  // end to end: the darkest 1 % of the frame comes out where it was without hdr
  EngineSession es;
  EditParams a, b;
  b.hdr = 100.f;
  Image8 ia, ib;
  CHECK(exactRender(es, a, src, ExecMode::Whole, 0, ia));
  CHECK(exactRender(es, b, src, ExecMode::Whole, 0, ib));
  const float p1a = percentile(ia, 0.01f), p1b = percentile(ib, 0.01f);
  std::printf("  darkest percentile of the output: %.4f at 0, %.4f at 100 (8-bit step 0.004)\n", p1a, p1b);
  CHECK(std::abs(p1a - p1b) < 0.012f);
}

// ---- midband -----------------------------------------------------------------------------------------------------------------
std::vector<float> gaussBlur(const std::vector<float>& in, int W, int H, float sigma) {
  const int r = std::max(1, int(std::ceil(3 * sigma)));
  std::vector<float> k(std::size_t(2 * r + 1)), a(in.size()), b(in.size());
  float sum = 0;
  for (int i = -r; i <= r; ++i) sum += k[std::size_t(i + r)] = std::exp(-0.5f * float(i * i) / (sigma * sigma));
  for (float& v : k) v /= sum;
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      float acc = 0;
      for (int t = -r; t <= r; ++t) acc += k[std::size_t(t + r)] * in[std::size_t(y) * W + std::clamp(x + t, 0, W - 1)];
      a[std::size_t(y) * W + x] = acc;
    }
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      float acc = 0;
      for (int t = -r; t <= r; ++t) acc += k[std::size_t(t + r)] * a[std::size_t(std::clamp(y + t, 0, H - 1)) * W + x];
      b[std::size_t(y) * W + x] = acc;
    }
  return b;
}

void testMidband() {
  const int W = 640, H = 432;
  const SourceImage src = makeScene(W, H);
  // luminance plane with mid-scale structure (blobs of 6 .. 24 px at +-0.25 EV) on top of the scene
  std::vector<float> lum(std::size_t(W) * H), L0(lum.size());
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      const float* px = src.linear.row(y) + 3 * x;
      const float blob = 0.25f * std::sin(float(x) * 0.35f) * std::sin(float(y) * 0.30f) + 0.25f * std::sin(float(x) * 0.12f + 1.f) * std::sin(float(y) * 0.11f);
      lum[std::size_t(y) * W + x] = (0.2880402f * px[0] + 0.7118741f * px[1] + 0.0000857f * px[2]) * std::exp2(blob);
      L0[std::size_t(y) * W + x] = std::log2(lum[std::size_t(y) * W + x]);
    }
  auto band = [&](const std::vector<float>& L) {
    const auto a = gaussBlur(L, W, H, 1.5f), b = gaussBlur(L, W, H, 6.f);
    std::vector<float> d(L.size());
    for (std::size_t i = 0; i < d.size(); ++i) d[i] = a[i] - b[i];
    return d;
  };
  const auto coarse = gaussBlur(L0, W, H, 6.f);
  std::vector<char> keep(lum.size());
  for (std::size_t i = 0; i < keep.size(); ++i) keep[i] = std::abs(L0[i] - coarse[i]) < 0.8f;  // away from the edges of the scene
  const auto b0 = band(L0);
  double e0 = 0;
  for (std::size_t i = 0; i < b0.size(); ++i)
    if (keep[i]) e0 += double(b0[i]) * b0[i];
  for (float s : {0.5f, 1.f}) {
    std::shared_ptr<const HdrField> f2;
    const HdrParams p = hardParams(src, f2, s);
    std::vector<float> L1(L0.size());
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) {
        const float l = lum[std::size_t(y) * W + x];
        float rgb[3] = {l, l, l};
        hdr::apply(*f2, p, (float(x) + 0.5f) / float(W), (float(y) + 0.5f) / float(H), rgb);
        L1[std::size_t(y) * W + x] = std::log2(std::max(rgb[1], 1e-9f));
      }
    const auto b1 = band(L1);
    double e1 = 0;
    for (std::size_t i = 0; i < b1.size(); ++i)
      if (keep[i]) e1 += double(b1[i]) * b1[i];
    std::printf("  strength %3.0f: mid band (sigma 1.5 - 6 px, log luminance) energy ratio vs 0: %.3f\n", s * 100.f, e1 / e0);
    CHECK(e1 / e0 >= 1.0);
  }
}

// ---- synthsheet: writes the 12 EV synthetic scene at strength 0 / 50 / 100 as PPM (dapple_hdr_tests synthsheet <dir>) -------------------------
void testSynthSheet(const char* dir) {
  SourceImage src = makeScene(1800, 1200);
  // the hard case of the brief: interior about 12 EV under the window (the base scene has 8 EV), noise on top
  std::mt19937 rng(5);
  std::normal_distribution<float> nd(0.f, 1.f);
  for (int y = 0; y < src.linear.height; ++y)
    for (int x = 0; x < src.linear.width; ++x) {
      float* p = src.linear.row(y) + 3 * x;
      const float u = float(x) / float(src.linear.width - 1), v = float(y) / float(src.linear.height - 1);
      const bool interior = v < 0.62f && !(u > 0.55f && u < 0.9f && v > 0.12f && v < 0.55f) && !(u < 0.3f && v < 0.5f);
      if (interior)
        for (int c = 0; c < 3; ++c) p[c] = std::max(0.f, p[c] * 0.25f * (1.f + 0.1f * nd(rng)));  // 0.003..0.011 -> 0.0008..0.003
    }
  EngineSession es;
  for (int s : {0, 50, 100}) {
    EditParams p;
    p.hdr = float(s);
    Image8 im;
    CHECK(exactRender(es, p, src, ExecMode::Whole, 0, im));
    const std::string f = std::string(dir) + "/synth_h" + std::to_string(s) + ".ppm";
    if (FILE* fp = std::fopen(f.c_str(), "wb")) {
      std::fprintf(fp, "P6\n%d %d\n255\n", im.width, im.height);
      std::fwrite(im.data.data(), 1, im.data.size(), fp);
      std::fclose(fp);
    }
  }
}

// ---- rank --------------------------------------------------------------------------------------------------------------------
int rankFiles(int argc, char** argv) {
  const DecoderRegistry reg = DecoderRegistry::withBuiltins();
  struct Row {
    float r, lo, hi;
    std::string f;
  };
  std::vector<Row> rows;
  for (int i = 2; i < argc; ++i) {
    const ImageDecoder* d = reg.find(argv[i]);
    if (!d) continue;
    auto img = d->decode(argv[i], DecodeOptions{DecodeQuality::Preview});
    if (!img) continue;
    const float wb[3] = {1.f, 1.f, 1.f};
    const auto f = hdr::fieldFor(*img, wb);
    if (f) rows.push_back({f->hi0 - f->lo0, f->lo0, f->hi0, argv[i]});
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.r > b.r; });
  for (const Row& r : rows) std::printf("%5.1f EV  (p1 %6.1f  p99 %6.1f)  %s\n", r.r, r.lo, r.hi, r.f.c_str());
  return 0;
}

// ---- halo --------------------------------------------------------------------------------------------------------------------
// The per-pixel gain next to a step edge against the plateau of the same side, in EV. A halo (base leaking across the edge) shows as a
// band of gain that differs from the plateau.
void testHalo() {
  const int W = 512, H = 340, edge = 256;
  struct Case {
    const char* name;
    float dark, bright;
  };
  for (const Case& c : {Case{"window / wall  (6.8 EV)", 0.008f, 0.9f}, Case{"sky / foliage (4 EV)", 0.06f, 0.9f}, Case{"mild step (1.3 EV)", 0.2f, 0.5f}}) {
    std::vector<float> lum(std::size_t(W) * H);
    std::mt19937 rng(3);
    std::normal_distribution<float> nd(0.f, 1.f);
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) {
        const float base = y < 10 ? 0.0002f : (x < edge ? c.dark : c.bright);  // a black strip gives the frame its black end
        const float tex = 1.f + 0.10f * (((x / 4 + y / 4) & 1) ? 1.f : -1.f) + 0.02f * nd(rng);
        lum[std::size_t(y) * W + x] = base * tex;
      }
    const auto field = hdr::buildField(lum, W, H);
    const float gain3[3] = {1.f, 1.f, 1.f};
    const HdrParams hp = hdr::resolve(*field, 1.f, 0.f, 100.f, gain3, 1.f);  // T = 1 EV: forces a strong compression whatever the range
    // gain at every pixel of the middle rows
    std::vector<double> evgain(std::size_t(W), 0.0);
    for (int x = 0; x < W; ++x) {
      double a = 0;
      int n = 0;
      for (int y = H / 2 - 20; y < H / 2 + 20; ++y) {
        const float l = lum[std::size_t(y) * W + x];
        float rgb[3] = {l, l, l};
        hdr::apply(*field, hp, (float(x) + 0.5f) / float(W), (float(y) + 0.5f) / float(H), rgb);
        a += std::log2(double(rgb[1]) / double(l));
        ++n;
      }
      evgain[std::size_t(x)] = a / n;
    }
    auto mean = [&](int a, int b) {
      double s = 0;
      for (int x = a; x < b; ++x) s += evgain[std::size_t(x)];
      return s / (b - a);
    };
    const double plD = mean(edge - 140, edge - 70), plB = mean(edge + 70, edge + 140);
    double worstD = 0, worstB = 0;
    for (int d = 1; d <= 24; ++d) {
      worstD = std::max(worstD, std::abs(mean(edge - d - 1, edge - d + 1) - plD));
      worstB = std::max(worstB, std::abs(mean(edge + d, edge + d + 2) - plB));
    }
    std::printf("  %-24s plateau gain dark %+.2f EV bright %+.2f EV | worst deviation next to the edge: dark side %.3f EV, bright side %.3f EV\n", c.name, plD, plB, worstD,
                worstB);
    // steps of >= 3 EV are edges of the bilateral base (no leak at all); a 1.5 EV step is inside the range sigma and is smoothed on purpose
    if (c.bright / c.dark > 8.f) CHECK(worstD < 0.15 && worstB < 0.15);  // the mild step is informational (T forced to 1 EV here)
    if (c.dark < 0.01f) CHECK(plD > 0.3 && plB < -0.3);
  }
}

// ---- noise -------------------------------------------------------------------------------------------------------------------
void testNoise() {
  const int W = 512, H = 340;
  for (float iso : {100.f, 1600.f}) {
    const float dark = 0.0035f, sigma = 0.0012f;
    std::vector<float> lum(std::size_t(W) * H);
    std::vector<float> src(std::size_t(W) * H);
    std::mt19937 rng(11);
    std::normal_distribution<float> nd(0.f, 1.f);
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) {
        const bool bright = x > W * 3 / 4;
        const float v = bright ? 0.35f : std::max(1e-5f, dark + sigma * nd(rng));
        src[std::size_t(y) * W + x] = v;
        lum[std::size_t(y) * W + x] = v;
      }
    const auto field = hdr::buildField(lum, W, H);
    const float gain3[3] = {1.f, 1.f, 1.f};
    const HdrParams hp = hdr::resolve(*field, 1.f, 0.f, iso, gain3, 1.f);
    double si = 0, so = 0, mi = 0, mo = 0;
    std::size_t n = 0;
    for (int y = 20; y < H - 20; ++y)
      for (int x = 20; x < W / 2; ++x) {
        const float l = src[std::size_t(y) * W + x];
        float rgb[3] = {l, l, l};
        hdr::apply(*field, hp, (float(x) + 0.5f) / float(W), (float(y) + 0.5f) / float(H), rgb);
        si += double(l) * l, so += double(rgb[1]) * rgb[1], mi += l, mo += rgb[1];
        ++n;
      }
    mi /= double(n), mo /= double(n);
    const double sdIn = std::sqrt(si / double(n) - mi * mi), sdOut = std::sqrt(so / double(n) - mo * mo);
    std::printf("  ISO %4.0f dark patch %.4f +- %.4f (noise floor %.4f): mean gain %.2fx, noise gain %.2fx (std %.5f -> %.5f)\n", iso, dark, sigma, hp.yn, mo / mi, sdOut / sdIn,
                sdIn, sdOut);
    CHECK(sdOut / sdIn < 3.0);
    CHECK(mo / mi >= 0.999);
  }
}

#ifdef DAPPLE_HDR_GPU
std::shared_ptr<engine::gpu::Context> gpuContext() {
  std::string err;
  engine::gpu::ContextOptions co;
  co.device.allowSoftware = std::getenv("DAPPLE_GPU_SOFTWARE") != nullptr;
  co.device.profile = std::getenv("DAPPLE_GPU_PROFILE") != nullptr;  // per-pass device timestamps (gpuMs)
  auto ctx = engine::gpu::Context::create(co, err);
  if (!ctx) {
    std::printf("no usable GPU context: %s\n", err.c_str());
    std::exit(77);
  }
  std::printf("device: %s\n", ctx->core().dev().caps().text().c_str());
  return ctx;
}

void testParity() {
  auto ctx = gpuContext();
  struct Case {
    const char* name;
    std::function<void(EditParams&)> set;
    bool interactive;
  };
  const std::vector<Case> cases = {
      {"hdr 100", [](EditParams& p) { p.hdr = 100.f; }, false},
      {"hdr 50 + exposure -0.7, contrast", [](EditParams& p) { p.hdr = 50.f; p.exposure = -0.7f; p.contrast = 20.f; }, false},
      {"hdr 100 + crop, straighten, vignette", [](EditParams& p) { p.hdr = 100.f; p.crop = "4:5"; p.straighten = 3.f; p.vignette = -25.f; }, false},
      {"hdr 80 + clarity, sharpen, NR", [](EditParams& p) { p.hdr = 80.f; p.clarity = 30.f; p.sharpen = 50.f; p.nrLuma = 30.f; }, false},
      {"hdr 100 interactive", [](EditParams& p) { p.hdr = 100.f; }, true},
  };
  for (const std::pair<int, int>& sz : {std::pair<int, int>{640, 448}, std::pair<int, int>{1536, 1024}}) {
    const SourceImage src = makeScene(sz.first, sz.second);
    dapple::gpu::Rig rig(ctx, src, cam());
    EngineSession cpuEs;
    for (const Case& c : cases) {
      EditParams p;
      c.set(p);
      Renderer r(p, cam(), nullptr);
      FastOptions fo;
      fo.spatial = c.interactive ? SpatialQuality::Interactive : SpatialQuality::Full;
      fo.noise = false;
      std::string err;
      int gw = 0, gh = 0;
      engine::GpuStats st;
      std::uint8_t* gpuPx = rig.render(p, r, src, fo, 33, gw, gh, &st, err);
      FrameJob job{p, r, src, nullptr, cam(), nullptr, {}, {}, {}};
      job.request.bakeSize = 33;
      job.request.fastAnalysis = false;
      job.request.noise = fo.noise;
      job.fastOptions = fo;
      FrameResult res;
      const bool okCpu = cpuEs.renderFrame(job, res);
      if (!gpuPx || !okCpu || res.w != gw || res.h != gh) {
        std::printf("  %-38s %dx%d: render failed (%s %s)\n", c.name, sz.first, sz.second, err.c_str(), res.error.c_str());
        ++g_failures;
      } else {
        const dapple::gpu::DeStats d = dapple::gpu::compareRgba8(gpuPx, res.rgba, std::size_t(gw) * gh);
        std::printf("  %-38s %4dx%-4d mean %.4f p99 %.3f max %.2f  differing %.1f%%  (%s)\n", c.name, gw, gh, d.mean, d.p99, d.max, d.differing * 100, res.executor.c_str());
        CHECK(d.mean <= 0.15 && d.p99 <= 1.5);
        if (!c.interactive) {  // the baked CPU frame against the exact (per-pixel) CPU render of the same edit: same HDR, baked tone / colour
          Image8 ex;
          EngineSession es2;
          if (exactRender(es2, p, src, ExecMode::Whole, 0, ex) && ex.width == gw && ex.height == gh) {
            std::vector<std::uint8_t> rgba(std::size_t(gw) * gh * 4, 255);
            for (std::size_t i = 0; i < std::size_t(gw) * gh; ++i) std::memcpy(&rgba[4 * i], &ex.data[3 * i], 3);
            const dapple::gpu::DeStats e = dapple::gpu::compareRgba8(rgba.data(), res.rgba, std::size_t(gw) * gh);
            std::printf("      CPU baked frame vs CPU exact render: mean %.4f p99 %.3f\n", e.mean, e.p99);
          }
        }
      }
      std::free(gpuPx);
      std::free(res.rgba);
    }
  }
}

double medianGpuMs(dapple::gpu::Rig& rig, const EditParams& p, const SourceImage& src, const FastOptions& fo, int srcLong, int& w, int& h) {
  std::vector<double> v;
  Renderer r(p, cam(), nullptr);
  for (int i = 0; i < 9; ++i) {
    std::string err;
    engine::GpuStats st;
    std::uint8_t* px = rig.render(p, r, src, fo, 33, w, h, &st, err, srcLong);
    if (!px) {
      std::printf("  render failed: %s\n", err.c_str());
      return -1;
    }
    std::free(px);
    if (i >= 2) v.push_back(st.gpuMs > 0 ? st.gpuMs : st.wallMs);
  }
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

void testBench() {
  auto ctx = gpuContext();
  const SourceImage big = makeScene(6000, 4000);
  dapple::gpu::Rig rig(ctx, big, cam());
  struct Mode {
    const char* name;
    int srcLong;
    SpatialQuality q;
  };
  for (const Mode& m : {Mode{"1080 px interactive", 1080, SpatialQuality::Interactive}, Mode{"full resolution 6000x4000", 0, SpatialQuality::Full}}) {
    const SourceImage cpuSrc = m.srcLong ? [&] {
      SourceImage s = big;
      s.linear = resizeToLongEdge(big.linear, m.srcLong);
      return s;
    }()
                                         : big;
    FastOptions fo;
    fo.spatial = m.q;
    fo.noise = false;
    EditParams off, on;
    on.hdr = 100.f;
    int w = 0, h = 0;
    const double a = medianGpuMs(rig, off, cpuSrc, fo, 0, w, h), b = medianGpuMs(rig, on, cpuSrc, fo, 0, w, h);
    std::printf("  GPU %-26s %dx%d: hdr 0 %.3f ms, hdr 100 %.3f ms, added %.3f ms\n", m.name, w, h, a, b, b - a);
  }
}
#endif

void testCpuBench() {
  const SourceImage src = [] {
    SourceImage s = makeScene(1920, 1280);
    return s;
  }();
  EditParams off, on;
  on.hdr = 100.f;
  EngineSession es;
  for (int pass = 0; pass < 2; ++pass) {
    const auto t0 = Clock::now();
    const std::shared_ptr<const HdrField> f = [&] {
      const float wb[3] = {1.f, 1.f, 1.f};
      return hdr::fieldFor(src, wb);
    }();
    if (pass == 1) std::printf("  field (cache hit) %.3f ms\n", msSince(t0));
    else std::printf("  field build from a 1920x1280 source: %.2f ms\n", msSince(t0));
  }
  for (const EditParams* p : {&off, &on}) {
    std::vector<double> v;
    for (int i = 0; i < 7; ++i) {
      Image8 o;
      const auto t0 = Clock::now();
      exactRender(es, *p, src, ExecMode::Whole, 0, o);
      v.push_back(msSince(t0));
    }
    std::sort(v.begin(), v.end());
    std::printf("  CPU exact render 1920x1280, hdr %s: median %.1f ms\n", p == &on ? "100" : "0", v[v.size() / 2]);
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  const std::string id = argc >= 2 ? argv[1] : "";
  if (id == "rank") return rankFiles(argc, argv);
  if (id == "synthsheet" && argc > 2) {
    testSynthSheet(argv[2]);
    return g_failures ? 1 : 0;
  }
  struct T {
    const char* name;
    void (*fn)();
  };
  const T tests[] = {{"identity", testIdentity}, {"lowrange", testLowRange}, {"anchors", testAnchors}, {"midband", testMidband}, {"tiles", testTiles}, {"monotonic", testMonotonic}, {"halo", testHalo}, {"noise", testNoise},
                     {"cpubench", testCpuBench},
#ifdef DAPPLE_HDR_GPU
                     {"parity", testParity}, {"bench", testBench},
#endif
  };
  bool ran = false;
  for (const T& t : tests)
    if (id.empty() || id == t.name) {
      if (id.empty() && std::string(t.name) == "cpubench") continue;
      std::printf("[%s]\n", t.name);
      t.fn();
      ran = true;
    }
  if (!ran) {
    std::printf("unknown id '%s'\n", id.c_str());
    return 2;
  }
  std::printf(g_failures ? "FAILED: %d check(s)\n" : "ok\n", g_failures);
  return g_failures ? 1 : 0;
}
