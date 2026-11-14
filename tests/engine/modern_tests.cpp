// The modern rendering family (EditParams::filmStock = modern_clean / modern_vivid / modern_soft, strength = film_strength).
// Docs: pipeline-architecture.md section 37.
//   dapple_modern_tests <id>   ids: identity off json ends fog midgrey hue skin monotonic tiles [parity bench with -DDAPPLE_GPU=ON; exit 77: no GPU]
//     identity   strength 0 is the standard rendering byte for byte; film stock ids still reach the film code
//     off        DAPPLE_ENGINE=off ignores the family (the old Renderer renders the standard picture)
//     json       selector / strength round trip, unknown id harmless, film and modern exclusive by construction
//     ends       a high-range scene reaches display black and white (percentiles of the output luminance)
//     fog        a low-contrast scene is stretched by at most the stated rule (guard holds)
//     highkey    large bright background: its chroma and lightness spread stay at the standard level; lowkey / gap likewise
//     midgrey    a neutral 18 % patch keeps its standard value at every strength and rendering
//     hue        OKLab hue error along saturated ramps from dark up to white, chroma rolls off, no per-channel clipping signature
//     skin       skin patches keep hue and chroma inside a window in all three renderings
//     monotonic  black falls, white rises, chroma rises with the strength
//     tiles      tiled export == whole frame
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

#include "color.h"
#include "dapple/camera.h"
#include "dapple/decoder.h"
#include "dapple/edit_params.h"
#include "dapple/modern.h"
#include "dapple/renderer.h"
#include "engine/exec/engine_session.h"
#ifdef DAPPLE_MODERN_GPU
#include "engine/exec/gpu_executor.h"
#include "gpu_rig.h"
#include "parity.h"
#endif

namespace {

using namespace dapple;
using namespace dapple::engine;
using namespace dapple::color;
using Clock = std::chrono::steady_clock;

int g_failures = 0;
#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("  CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                         \
    }                                                                       \
  } while (0)

const CameraProfile& cam() {
  static const CameraRegistry reg = CameraRegistry::withBuiltins();
  return reg.resolve("", "");
}

const char* kIds[3] = {"modern_clean", "modern_vivid", "modern_soft"};

// ---- scenes ------------------------------------------------------------------------------------------------------------------
// Rows 0 .. 0.45: a neutral log ramp (-7.5 .. +2.4 EV about mid-grey, clipped at 1, slight texture and noise). Rows 0.45 .. 0.93: six
// saturated hue bands, each a ramp of constant chromaticity from 1 % to 6x of the colour (past gamut, up to clipped white). Bottom strip:
// four skin patches (mid-tone). The mid-grey patch is the neutral ramp at EV 0 (x = 0.757).
constexpr float kEvLo = -7.5f, kEvHi = 2.4f;
constexpr int kBands = 6;
const float kBandSrgb[kBands][3] = {{0.9f, 0.08f, 0.06f}, {0.9f, 0.45f, 0.04f}, {0.15f, 0.8f, 0.1f},
                                    {0.05f, 0.7f, 0.85f}, {0.1f, 0.2f, 0.95f}, {0.8f, 0.1f, 0.7f}};
const float kSkinSrgb[4][3] = {{0.745f, 0.42f, 0.30f}, {0.60f, 0.32f, 0.22f}, {0.86f, 0.60f, 0.48f}, {0.36f, 0.18f, 0.12f}};  // linear sRGB

float rampX(float ev) { return (ev - kEvLo) / (kEvHi - kEvLo); }

SourceImage makeScene(int w, int h) {
  SourceImage s;
  s.sceneReferred = true;
  s.meta.iso = 100.f;
  s.linear = ImageF(w, h);
  std::mt19937 rng(11);
  std::normal_distribution<float> nd(0.f, 1.f);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const float u = float(x) / float(w - 1), v = float(y) / float(h - 1);
      float rgb[3];  // linear sRGB
      if (v < 0.45f) {
        const float ev = kEvLo + (kEvHi - kEvLo) * u;
        const float l = std::min(1.f, 0.18f * std::exp2(ev));
        rgb[0] = rgb[1] = rgb[2] = l;
      } else if (v < 0.93f) {
        const int b = std::min(kBands - 1, int((v - 0.45f) / 0.48f * kBands));
        const float sc = 0.01f * std::pow(600.f, u);  // 0.01 .. 6
        for (int c = 0; c < 3; ++c) rgb[c] = kBandSrgb[b][c] * sc;
      } else {
        const int k = std::min(3, int(u * 4.f));
        for (int c = 0; c < 3; ++c) rgb[c] = kSkinSrgb[k][c];
      }
      const float n = 1.f + 0.01f * nd(rng);
      float pp[3];
      for (int c = 0; c < 3; ++c) rgb[c] *= n;
      srgbToProPhoto().apply(rgb, pp);
      float* p = s.linear.row(y) + 3 * x;
      for (int c = 0; c < 3; ++c) p[c] = std::clamp(pp[c], 0.f, 1.f);  // the sensor clips
    }
  return s;
}

// A foggy day: everything between 0.22 and 0.42 (1 EV), a touch of colour.
SourceImage makeFog(int w, int h) {
  SourceImage s;
  s.sceneReferred = true;
  s.meta.iso = 100.f;
  s.linear = ImageF(w, h);
  std::mt19937 rng(5);
  std::normal_distribution<float> nd(0.f, 1.f);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const float u = float(x) / float(w - 1), v = float(y) / float(h - 1);
      const float l = (0.22f + 0.20f * (0.6f * u + 0.4f * v)) * (1.f + 0.01f * nd(rng));
      float* p = s.linear.row(y) + 3 * x;
      p[0] = l * 1.02f, p[1] = l, p[2] = l * 0.97f;
    }
  return s;
}

// A high-key picture: 55 % of the frame (right side) a bright, smooth, out-of-focus green background (Y about 0.45 with soft blotches), a darker
// subject on the left (skin patch, dark clothes, one dark corner).
SourceImage makeHighKey(int w, int h) {
  SourceImage s;
  s.sceneReferred = true;
  s.meta.iso = 100.f;
  s.linear = ImageF(w, h);
  std::mt19937 rng(21);
  std::normal_distribution<float> nd(0.f, 1.f);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const float u = float(x) / float(w - 1), v = float(y) / float(h - 1);
      float rgb[3];
      if (u > 0.45f) {
        const float blot = 1.f + 0.28f * std::sin(9.f * u + 3.f * v) * std::cos(7.f * v - 2.f * u);
        const float l = 0.45f * blot * (0.9f + 0.2f * v);
        rgb[0] = 0.55f * l, rgb[1] = 1.0f * l, rgb[2] = 0.30f * l;  // green
      } else if (v < 0.5f) {
        for (int c = 0; c < 3; ++c) rgb[c] = kSkinSrgb[0][c] * (0.9f + 0.2f * u);
      } else {
        const float l = (u < 0.2f && v > 0.85f) ? 0.004f : 0.035f + 0.05f * u;
        rgb[0] = 0.8f * l, rgb[1] = 1.0f * l, rgb[2] = 1.1f * l;
      }
      const float n = 1.f + 0.01f * nd(rng);
      float pp[3];
      for (int c = 0; c < 3; ++c) rgb[c] *= n;
      srgbToProPhoto().apply(rgb, pp);
      float* p = s.linear.row(y) + 3 * x;
      for (int c = 0; c < 3; ++c) p[c] = std::clamp(pp[c], 0.f, 1.f);
    }
  return s;
}

// A low-key picture: 70 % deep, dark-blue, textured background (Y 0.01 .. 0.05), a small lit subject and a bright lamp.
SourceImage makeLowKey(int w, int h) {
  SourceImage s;
  s.sceneReferred = true;
  s.meta.iso = 100.f;
  s.linear = ImageF(w, h);
  std::mt19937 rng(23);
  std::normal_distribution<float> nd(0.f, 1.f);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const float u = float(x) / float(w - 1), v = float(y) / float(h - 1);
      float rgb[3];
      const bool lit = u > 0.62f && u < 0.85f && v > 0.35f && v < 0.9f, lamp = u > 0.9f && u < 0.94f && v > 0.1f && v < 0.16f;
      if (lamp) rgb[0] = rgb[1] = rgb[2] = 2.0f;
      else if (lit) {
        for (int c = 0; c < 3; ++c) rgb[c] = kSkinSrgb[1][c] * 1.3f;
      } else {
        const float tex = 1.f + 0.35f * std::sin(23.f * u) * std::sin(19.f * v);
        const float l = (0.012f + 0.035f * (1.f - v)) * tex;
        rgb[0] = 0.55f * l, rgb[1] = 0.8f * l, rgb[2] = 1.4f * l;
      }
      const float n = 1.f + 0.015f * nd(rng);
      float pp[3];
      for (int c = 0; c < 3; ++c) rgb[c] *= n;
      srgbToProPhoto().apply(rgb, pp);
      float* p = s.linear.row(y) + 3 * x;
      for (int c = 0; c < 3; ++c) p[c] = std::clamp(pp[c], 0.f, 1.f);
    }
  return s;
}

// A foggy field: light grey sky (top 55 %), a dark brown-green ground, nothing in between, little colour.
SourceImage makeFogField(int w, int h) {
  SourceImage s;
  s.sceneReferred = true;
  s.meta.iso = 100.f;
  s.linear = ImageF(w, h);
  std::mt19937 rng(29);
  std::normal_distribution<float> nd(0.f, 1.f);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const float u = float(x) / float(w - 1), v = float(y) / float(h - 1);
      float rgb[3];
      if (v < 0.55f) {
        const float l = 0.62f + 0.12f * v;
        rgb[0] = l, rgb[1] = l * 1.01f, rgb[2] = l * 1.03f;
      } else {
        const float l = (0.035f + 0.03f * u) * (1.f + 0.4f * std::sin(40.f * u * v));
        rgb[0] = 1.1f * l, rgb[1] = 1.0f * l, rgb[2] = 0.8f * l;
      }
      const float n = 1.f + 0.01f * nd(rng);
      float pp[3];
      for (int c = 0; c < 3; ++c) rgb[c] *= n;
      srgbToProPhoto().apply(rgb, pp);
      float* p = s.linear.row(y) + 3 * x;
      for (int c = 0; c < 3; ++c) p[c] = std::clamp(pp[c], 0.f, 1.f);
    }
  return s;
}

EditParams withBase(const char* id, float strength = 100.f) {
  EditParams p;
  p.filmStock = id;
  p.filmStrength = strength;
  return p;
}

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

Image8 render(const EditParams& p, const SourceImage& src) {
  static EngineSession es;
  Image8 im;
  exactRender(es, p, src, ExecMode::Whole, 0, im);
  return im;
}

float encLuma(const Image8& im, int x, int y) {
  const std::uint8_t* p = &im.data[(std::size_t(y) * im.width + x) * 3];
  return (0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]) / 255.f;
}
void lab8(const Image8& im, int x, int y, float lab[3]) {
  const std::uint8_t* p = &im.data[(std::size_t(y) * im.width + x) * 3];
  const float rgb[3] = {srgbDecode(p[0] / 255.f), srgbDecode(p[1] / 255.f), srgbDecode(p[2] / 255.f)};
  linearSrgbToOklab(rgb, lab);
}
float hueDeg(const float lab[3]) {
  float h = std::atan2(lab[2], lab[1]) * 180.f / kPi;
  return h < 0 ? h + 360.f : h;
}
float hueErr(float a, float b) {
  float d = std::fabs(a - b);
  return d > 180.f ? 360.f - d : d;
}
float chromaOf(const float lab[3]) { return std::hypot(lab[1], lab[2]); }

// percentile of the encoded luminance over the neutral ramp rows (rows 0 .. 0.45)
float rampPercentile(const Image8& im, float q) {
  std::vector<float> v;
  for (int y = 0; y < int(0.45f * im.height); ++y)
    for (int x = 0; x < im.width; ++x) v.push_back(encLuma(im, x, y));
  std::sort(v.begin(), v.end());
  return v[std::size_t(q * float(v.size() - 1))];
}

// ---- identity / off / json ----------------------------------------------------------------------------------------------------
void testIdentity() {
  const SourceImage src = makeScene(384, 256);
  EditParams a;
  a.contrast = 10.f;
  a.clarity = 10.f;
  Image8 ref = render(a, src);
  for (const char* id : kIds) {
    EditParams p = a;
    p.filmStock = id;
    p.filmStrength = 0.f;
    const Image8 got = render(p, src);
    CHECK(got.data == ref.data);
    EditParams q = a;
    q.filmStock = id;
    const Image8 full = render(q, src);
    CHECK(full.data != ref.data);
    std::printf("  %-13s strength 0 identical: %s, strength 100 differs: %s\n", id, got.data == ref.data ? "yes" : "NO", full.data != ref.data ? "yes" : "NO");
  }
  // a modern id is no film stock: the film code path stays off
  Renderer r(withBase("modern_vivid"), cam(), nullptr);
  CHECK(!r.filmActive());
}

void testOff() {
#ifdef _WIN32
  _putenv_s("DAPPLE_ENGINE", "off");
#else
  setenv("DAPPLE_ENGINE", "off", 1);
#endif
  const SourceImage src = makeScene(320, 224);
  EditParams a;
  const Image8 ref = Renderer(a, cam(), nullptr).render(src);
  for (const char* id : kIds) {
    const Image8 got = Renderer(withBase(id), cam(), nullptr).render(src);
    CHECK(got.data == ref.data);
    std::printf("  DAPPLE_ENGINE=off, %s: %s\n", id, got.data == ref.data ? "ignored (identical to standard)" : "APPLIED");
  }
}

void testJson() {
  EditParams p = withBase("modern_soft", 60.f);
  const std::string j = toJson(p);
  CHECK(j.find("\"film\": \"modern_soft\"") != std::string::npos || j.find("\"film\":\"modern_soft\"") != std::string::npos);
  const auto back = fromJson(j);
  CHECK(back && back->filmStock == "modern_soft" && std::fabs(back->filmStrength - 60.f) < 1e-3f);
  const auto q = fromJson("{\"film\":\"modern_vivid\",\"film_strength\":35}");
  CHECK(q && q->filmStock == "modern_vivid" && std::fabs(q->filmStrength - 35.f) < 1e-3f);
  CHECK(modernKind("modern_clean") == ModernKind::Clean && modernKind("kodak_portra_400") == ModernKind::None && modernKind("") == ModernKind::None);
  CHECK(toJsonDiff(p).find("modern_soft") != std::string::npos);
  CHECK(modernJson().find("modern_vivid") != std::string::npos);
  // exclusive: the selector holds ONE id; setting the other replaces it
  EditParams m = p;
  m.filmStock = "kodak_portra_400";
  CHECK(!isModernId(m.filmStock));
  std::printf("  selector: film = \"modern_soft\", strength film_strength = 60; json: %s\n", toJsonDiff(p).c_str());
}

// ---- ends ----------------------------------------------------------------------------------------------------------------------
void testEnds() {
  const SourceImage src = makeScene(768, 512);
  const Image8 std8 = render(EditParams{}, src);
  const float sb = rampPercentile(std8, 0.005f), sw = rampPercentile(std8, 0.995f);
  std::printf("  standard: neutral ramp output P0.5 %.4f  P99.5 %.4f\n", sb, sw);
  for (const char* id : kIds) {
    const Image8 im = render(withBase(id), src);
    const float b = rampPercentile(im, 0.005f), w = rampPercentile(im, 0.995f);
    const float b2 = rampPercentile(im, 0.02f), w2 = rampPercentile(im, 0.998f);
    std::printf("  %-13s P0.5 %.4f  P2 %.4f  P99.5 %.4f  P99.8 %.4f\n", id, b, b2, w, w2);
    CHECK(b <= 0.02f);
    CHECK(w >= 0.94f);
    CHECK(w2 >= 0.95f);
    CHECK(w > sw && b <= sb);
  }
  // the library rule on its own: guards
  std::vector<float> Y;
  std::mt19937 rng(3);
  for (int i = 0; i < 20000; ++i) Y.push_back(0.18f * std::exp2(-6.f + 7.f * float(rng()) / float(rng.max())));
  for (int i = 0; i < 20; ++i) Y.push_back(40.f);  // 0.1 % specular pixels: must not set the white point
  const ModernEnds e = modernEnds(Y.data(), Y.size(), 0.18f);
  std::printf("  guard: 20 specular pixels (0.1 %%) of 40.0: white point %.3f black %.5f\n", e.white, e.black);
  CHECK(e.white < 2.5f);
  CHECK(!e.stretchLimited);
}

// ---- fog -----------------------------------------------------------------------------------------------------------------------
void testFog() {
  const SourceImage src = makeFog(320, 224);
  const Image8 std8 = render(EditParams{}, src);
  auto span = [&](const Image8& im) {
    std::vector<float> v;
    for (int y = 0; y < im.height; ++y)
      for (int x = 0; x < im.width; ++x) v.push_back(encLuma(im, x, y));
    std::sort(v.begin(), v.end());
    return std::pair<float, float>{v[v.size() / 100], v[v.size() * 99 / 100]};
  };
  const auto s0 = span(std8);
  const float w0 = s0.second - s0.first;
  std::printf("  standard: P1 %.3f P99 %.3f span %.3f\n", s0.first, s0.second, w0);
  CHECK(w0 > 0.01f);
  for (const char* id : kIds) {
    const Image8 im = render(withBase(id), src);
    const auto s = span(im);
    const float w = s.second - s.first;
    std::printf("  %-13s P1 %.3f P99 %.3f span %.3f (x%.2f of standard)\n", id, s.first, s.second, w, w / w0);
    // the rule: the stretch is limited (the output does NOT reach black and white), and the contrast of the base map stays below
    // the stated limit: 1 / 0.62 of the encoded span times the family's own contrast
    CHECK(s.first > 0.08f);
    CHECK(s.second < 0.92f);
    CHECK(w <= w0 * (1.f / 0.62f) * modernRecipe(modernKind(id)).contrast * 1.15f);
  }
}

// the 1D tone table of a bake: ratio over log2(Y) (renderer.cpp: octaves -24 .. +6, 256 buckets per octave)
float tableRatio(const std::vector<float>& tone, float Y) {
  constexpr int kBase = (127 - 24) << 8, kN = ((6 + 24) << 8) + 2;
  std::uint32_t bits;
  std::memcpy(&bits, &Y, 4);
  int k = int(bits >> 15) - kBase;
  float fr = float(bits & 0x7FFFu) * (1.f / 32768.f);
  if (k < 0) k = 0, fr = 0.f;
  else if (k > kN - 2) k = kN - 2, fr = 0.f;
  return tone[std::size_t(k)] + fr * (tone[std::size_t(k) + 1] - tone[std::size_t(k)]);
}

// ---- mid-grey ------------------------------------------------------------------------------------------------------------------
void testMidGrey() {
  const SourceImage src = makeScene(768, 512);
  const int mx = int(rampX(0.f) * 767.f), my = 40;
  EditParams a;
  const Image8 ref = render(a, src);
  const float v0 = encLuma(ref, mx, my);
  std::printf("  standard 18 %% patch: %.4f\n", v0);
  // the tone model itself: the 1D table at 18 % (post-gain) against the standard table, every rendering and strength, with user edits
  {
    const Renderer rs(EditParams{}, cam(), nullptr);
    const auto ls = rs.bake(src, 17);
    const float std18 = 0.18f * tableRatio(ls->tone, 0.18f);
    for (const char* id : kIds)
      for (float s : {10.f, 50.f, 100.f}) {
        const Renderer rm(withBase(id, s), cam(), nullptr);
        const auto lm = rm.bake(src, 17);
        const float m18 = 0.18f * tableRatio(lm->tone, 0.18f);
        CHECK(std::fabs(m18 - std18) <= 2e-4f);
        if (s == 100.f) std::printf("  tone table at 0.18: standard %.5f, %s %.5f\n", std18, id, m18);
      }
    EditParams e = withBase("modern_vivid");
    e.contrast = 30.f;
    EditParams e0;
    e0.contrast = 30.f;
    const float a18 = 0.18f * tableRatio(Renderer(e, cam(), nullptr).bake(src, 17)->tone, 0.18f);
    const float b18 = 0.18f * tableRatio(Renderer(e0, cam(), nullptr).bake(src, 17)->tone, 0.18f);
    CHECK(std::fabs(a18 - b18) <= 2e-4f);
  }
  // end to end on a ten-stop scene (the single-image HDR and the local contrast of the family also act here: informational, bound 0.015)
  for (const char* id : kIds)
    for (float s : {25.f, 60.f, 100.f}) {
      const Image8 im = render(withBase(id, s), src);
      // the patch is noisy (1 %): average a 9x9 block
      double a0 = 0, a1 = 0;
      for (int dy = -4; dy <= 4; ++dy)
        for (int dx = -4; dx <= 4; ++dx) a0 += encLuma(ref, mx + dx, my + dy), a1 += encLuma(im, mx + dx, my + dy);
      a0 /= 81, a1 /= 81;
      std::printf("  %-13s strength %3.0f: %.4f (standard %.4f, diff %.4f)\n", id, s, a1, a0, a1 - a0);
      CHECK(std::fabs(a1 - a0) <= 0.015);
    }
}

// ---- hue -----------------------------------------------------------------------------------------------------------------------
void testHue() {
  const SourceImage src = makeScene(768, 512);
  EditParams a;
  const Image8 ref = render(a, src);
  double worstAll = 0;
  for (const char* id : kIds) {
    const Image8 im = render(withBase(id), src);
    double worst = 0, worstStd = 0;
    int clipSig = 0, wb = -1, wx = 0;
    float wL = 0, wC = 0, wHin = 0, wHout = 0;
    for (int b = 0; b < kBands; ++b) {
      const int y = int((0.45f + 0.48f * (float(b) + 0.5f) / kBands) * 512.f);
      float prevC = 1e9f;
      for (int x = 4; x < 764; x += 2) {
        // the input hue is the hue of the scene pixel itself (the sensor clipped it per channel where the colour was brighter than white)
        const float* sp = src.linear.row(y) + 3 * x;
        float inRgb[3], inLab[3];
        proPhotoToSrgb().apply(sp, inRgb);
        linearSrgbToOklab(inRgb, inLab);
        const float h0 = hueDeg(inLab);
        float lab[3], labS[3];
        lab8(im, x, y, lab);
        lab8(ref, x, y, labS);
        const float C = chromaOf(lab);
        const float L = lab[0];
        // hue only where the colour is visible (C > 0.03) and not within the 8-bit quantisation of the near-black
        if (C > 0.04f && L > 0.3f && hueErr(h0, hueDeg(lab)) > worst) {
          worst = hueErr(h0, hueDeg(lab)), wb = b, wx = x, wL = L, wC = C, wHin = h0, wHout = hueDeg(lab);
        }
        if (chromaOf(labS) > 0.04f && labS[0] > 0.25f) worstStd = std::max<double>(worstStd, hueErr(h0, hueDeg(labS)));
        // towards white (L > 0.85) chroma must not grow with lightness: no per-channel clipping signature
        if (L > 0.88f) {
          if (C > prevC + 0.012f) ++clipSig;
          prevC = std::min(prevC, C);
        }
      }
    }
    std::printf("  %-13s worst OKLab hue error %.2f deg (standard %.2f); chroma re-growth steps near white: %d\n", id, worst, worstStd, clipSig);
    std::printf("      worst at band %d x %d: L %.3f C %.3f, hue in %.1f out %.1f\n", wb, wx, wL, wC, wHin, wHout);
    CHECK(worst <= 3.0);
    CHECK(clipSig == 0);
    worstAll = std::max(worstAll, worst);
  }
  std::printf("  bound: hue error <= 3 deg (C > 0.04, L > 0.3: below that 8-bit quantisation decides the angle)\n");
}

// ---- skin ----------------------------------------------------------------------------------------------------------------------
void testSkin() {
  const SourceImage src = makeScene(768, 512);
  const Image8 ref = render(EditParams{}, src);
  const int y = int(0.965f * 512.f);
  for (const char* id : kIds) {
    const Image8 im = render(withBase(id), src);
    for (int k = 0; k < 4; ++k) {
      const int x = int((float(k) + 0.5f) / 4.f * 768.f);
      float l0[3], l1[3];
      lab8(ref, x, y, l0);
      lab8(im, x, y, l1);
      const float dh = hueErr(hueDeg(l0), hueDeg(l1)), cr = chromaOf(l1) / chromaOf(l0);
      std::printf("  %-13s patch %d: hue %.1f -> %.1f (%.2f deg), chroma x%.3f, L %.3f -> %.3f\n", id, k, hueDeg(l0), hueDeg(l1), dh, cr, l0[0], l1[0]);
      CHECK(dh <= 2.5f);
      CHECK(cr >= 0.90f && cr <= 1.15f);
      CHECK(std::fabs(l1[0] - l0[0]) <= 0.04f);
    }
  }
  std::printf("  window: hue shift <= 2.5 deg, chroma x0.90 .. x1.15, lightness within 0.04 of the standard rendering\n");
}

// ---- monotonic -----------------------------------------------------------------------------------------------------------------
void testMonotonic() {
  const SourceImage src = makeScene(640, 432);
  const Image8 ref = render(EditParams{}, src);
  for (const char* id : kIds) {
    float prevB = 2.f, prevW = -1.f, prevC = -1.f;
    std::printf("  %-13s", id);
    for (float s : {0.f, 25.f, 50.f, 75.f, 100.f}) {
      const Image8 im = render(withBase(id, s), src);
      const float b = rampPercentile(im, 0.01f), w = rampPercentile(im, 0.99f);
      double c = 0;
      int n = 0;  // fixed scene positions (the middle of the colour ramps), not selected by the output lightness
      for (int y = int(0.50f * im.height); y < int(0.90f * im.height); y += 3)
        for (int x = int(0.45f * im.width); x < int(0.80f * im.width); x += 3) {
          float lab[3];
          lab8(im, x, y, lab);
          c += chromaOf(lab), ++n;
        }
      c /= std::max(1, n);
      std::printf("  s%3.0f: black %.3f white %.3f chroma %.4f |", s, b, w, c);
      CHECK(b <= prevB + 0.002f);
      CHECK(w >= prevW - 0.002f);
      double dsum = 0;  // mean OKLab distance from the standard rendering of the same pixels: the effect grows with the strength
      int dn = 0;
      for (int y = 0; y < ref.height; y += 4)
        for (int x = 0; x < ref.width; x += 4) {
          float l0[3], l1[3];
          lab8(ref, x, y, l0);
          lab8(im, x, y, l1);
          dsum += std::sqrt(double((l0[0] - l1[0]) * (l0[0] - l1[0]) + (l0[1] - l1[1]) * (l0[1] - l1[1]) + (l0[2] - l1[2]) * (l0[2] - l1[2]))), ++dn;
        }
      const float dist = float(dsum / dn);
      std::printf(" dE %.4f |", dist);
      CHECK(dist >= prevC - 1e-4f);
      prevB = b, prevW = w, prevC = dist;
    }
    std::printf("\n");
  }
}

// mean OKLab chroma and standard deviation of OKLab lightness over a region (fractions of the frame)
void regionStats(const Image8& im, float u0, float u1, float v0, float v1, double& meanC, double& sdL, double& meanL) {
  double sc = 0, sl = 0, sl2 = 0;
  int n = 0;
  for (int y = int(v0 * im.height); y < int(v1 * im.height); y += 2)
    for (int x = int(u0 * im.width); x < int(u1 * im.width); x += 2) {
      float lab[3];
      lab8(im, x, y, lab);
      sc += chromaOf(lab), sl += lab[0], sl2 += double(lab[0]) * lab[0], ++n;
    }
  meanC = sc / n;
  meanL = sl / n;
  sdL = std::sqrt(std::max(0.0, sl2 / n - meanL * meanL));
}

// ---- highkey / lowkey / gap: where the mass of the picture is ---------------------------------------------------------------------
void testHighKey() {
  const SourceImage src = makeHighKey(640, 440);
  const Image8 ref = render(EditParams{}, src);
  double c0, s0, l0;
  regionStats(ref, 0.5f, 1.f, 0.f, 1.f, c0, s0, l0);
  std::printf("  standard background: chroma %.4f, lightness spread %.4f, mean L %.3f\n", c0, s0, l0);
  for (const char* id : kIds) {
    const Image8 im = render(withBase(id), src);
    double c1, s1, l1;
    regionStats(im, 0.5f, 1.f, 0.f, 1.f, c1, s1, l1);
    std::printf("  %-13s background: chroma %.4f (x%.2f), spread %.4f (x%.2f), mean L %.3f\n", id, c1, c1 / c0, s1, s1 / s0, l1);

    CHECK(c1 >= 0.90 * c0);
    CHECK(s1 >= 0.85 * s0);  // lightness separation of the bright area stays within 15 % of the standard (measured: x0.9 .. x1.05)
    CHECK(l1 <= l0 + 0.08);
  }
}

void testLowKey() {
  const SourceImage src = makeLowKey(640, 440);
  const Image8 ref = render(EditParams{}, src);
  double c0, s0, l0;
  regionStats(ref, 0.f, 0.55f, 0.f, 1.f, c0, s0, l0);
  std::printf("  standard dark area: chroma %.4f, lightness spread %.4f, mean L %.3f\n", c0, s0, l0);
  for (const char* id : kIds) {
    const Image8 im = render(withBase(id), src);
    double c1, s1, l1;
    regionStats(im, 0.f, 0.55f, 0.f, 1.f, c1, s1, l1);
    std::printf("  %-13s dark area: chroma %.4f (x%.2f), spread %.4f (x%.2f), mean L %.3f\n", id, c1, c1 / c0, s1, s1 / s0, l1);
    CHECK(s1 >= 0.88 * s0);  // a large dark area is not crushed: its lightness spread stays within 12 % of the standard
    CHECK(c1 >= 0.85 * c0);
  }
}

void testGap() {
  const SourceImage src = makeFogField(480, 320);
  const Image8 ref = render(EditParams{}, src);
  double c0, s0, l0;
  regionStats(ref, 0.f, 1.f, 0.6f, 1.f, c0, s0, l0);
  std::printf("  standard ground: mean L %.3f, spread %.4f\n", l0, s0);
  for (const char* id : kIds) {
    const Image8 im = render(withBase(id), src);
    double c1, s1, l1;
    regionStats(im, 0.f, 1.f, 0.6f, 1.f, c1, s1, l1);
    float p5 = 0;
    {
      std::vector<float> v;
      for (int y = int(0.6f * im.height); y < im.height; ++y)
        for (int x = 0; x < im.width; ++x) v.push_back(encLuma(im, x, y));
      std::sort(v.begin(), v.end());
      p5 = v[v.size() / 20];
    }
    std::printf("  %-13s ground: mean L %.3f (standard %.3f), spread %.4f (x%.2f), P5 luma %.3f\n", id, l1, l0, s1, s1 / s0, p5);
    CHECK(l1 >= 0.55 * l0);  // the ground does not go heavy
    CHECK(s1 >= 0.9 * s0);
  }
}

// ---- tiles ---------------------------------------------------------------------------------------------------------------------
void testTiles() {
  const SourceImage src = makeScene(613, 421);
  EngineSession es;
  for (const char* id : kIds) {
    EditParams p = withBase(id);
    p.crop = "4:5";
    p.straighten = 2.f;
    p.look = "warm";
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
      std::printf("  %-13s tile %3d: %d tiles, max code %d, differing bytes %zu\n", id, tile, st.tiles, maxCode, differing);
      CHECK(st.tiles > 1);
      CHECK(maxCode <= 1);
    }
  }
}

#ifdef DAPPLE_MODERN_GPU
std::shared_ptr<engine::gpu::Context> gpuContext() {
  std::string err;
  engine::gpu::ContextOptions co;
  co.device.allowSoftware = std::getenv("DAPPLE_GPU_SOFTWARE") != nullptr;
  co.device.profile = std::getenv("DAPPLE_GPU_PROFILE") != nullptr;
  auto ctx = engine::gpu::Context::create(co, err);
  if (!ctx) {
    std::printf("no usable GPU context: %s\n", err.c_str());
    std::exit(77);
  }
  std::printf("device: %s\n", ctx->core().dev().caps().text().c_str());
  return ctx;
}

void parityImpl(const std::vector<std::pair<int, int>>& sizes) {
  auto ctx = gpuContext();
  struct Case {
    const char* name;
    std::function<void(EditParams&)> set;
    bool interactive;
  };
  const std::vector<Case> cases = {
      {"clean", [](EditParams& p) { p.filmStock = "modern_clean"; }, false},
      {"vivid", [](EditParams& p) { p.filmStock = "modern_vivid"; }, false},
      {"soft", [](EditParams& p) { p.filmStock = "modern_soft"; }, false},
      {"vivid 55 + exposure, contrast, look", [](EditParams& p) { p.filmStock = "modern_vivid"; p.filmStrength = 55.f; p.exposure = 0.6f; p.contrast = 15.f; p.look = "teal"; }, false},
      {"clean + crop, vignette, NR, sharpen", [](EditParams& p) { p.filmStock = "modern_clean"; p.crop = "4:5"; p.vignette = -20.f; p.nrLuma = 20.f; p.sharpen = 50.f; }, false},
      {"vivid interactive", [](EditParams& p) { p.filmStock = "modern_vivid"; }, true},
  };
  for (const std::pair<int, int>& sz : sizes) {
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
      }
      std::free(gpuPx);
      std::free(res.rgba);
    }
  }
}

void testParity() { parityImpl({{640, 448}, {1536, 1024}}); }
// the three modern looks, strength, look, geometry and detail stages, interactive: GPU against CPU on one small frame (default run)
void testParitySmall() { parityImpl({{320, 224}}); }

double medianGpuMs(dapple::gpu::Rig& rig, const EditParams& p, const SourceImage& src, const FastOptions& fo, int& w, int& h) {
  std::vector<double> v;
  Renderer r(p, cam(), nullptr);
  for (int i = 0; i < 9; ++i) {
    std::string err;
    engine::GpuStats st;
    std::uint8_t* px = rig.render(p, r, src, fo, 33, w, h, &st, err, 0);
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
    SourceImage s = big;
    if (m.srcLong) s.linear = resizeToLongEdge(big.linear, m.srcLong);
    FastOptions fo;
    fo.spatial = m.q;
    fo.noise = false;
    int w = 0, h = 0;
    const double a = medianGpuMs(rig, EditParams{}, s, fo, w, h);
    std::printf("  GPU %-26s %dx%d: standard %.3f ms", m.name, w, h, a);
    for (const char* id : kIds) std::printf(", %s %.3f (+%.3f)", id, medianGpuMs(rig, withBase(id), s, fo, w, h), medianGpuMs(rig, withBase(id), s, fo, w, h) - a);
    std::printf("\n");
  }
}
#endif

void testCpuBench() {
  const SourceImage src = makeScene(1920, 1280);
  EngineSession es;
  for (int pass = 0; pass < 2; ++pass) {
    const char* names[4] = {"standard", kIds[0], kIds[1], kIds[2]};
    for (int i = 0; i < 4; ++i) {
      EditParams p = i ? withBase(kIds[i - 1]) : EditParams{};
      Image8 out;
      const auto t0 = Clock::now();
      exactRender(es, p, src, ExecMode::Whole, 0, out);
      const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
      if (pass) std::printf("  CPU exact 1920x1280 %-13s %.1f ms\n", names[i], ms);
    }
  }
}

// evidence: the synthetic high-key / low-key / fog scenes rendered as PPM for the contact sheets (dapple_modern_tests sheetsrc <dir>)
void writeSheetSources(const char* dir) {
  struct Sc {
    const char* name;
    SourceImage img;
  };
  const Sc scenes[] = {{"synth_highkey", makeHighKey(420, 300)}, {"synth_lowkey", makeLowKey(420, 300)}, {"synth_fogfield", makeFogField(420, 300)}};
  const char* vn[4] = {"std", "clean", "vivid", "soft"};
  for (const Sc& sc : scenes)
    for (int i = 0; i < 4; ++i) {
      const Image8 im = render(i ? withBase(kIds[i - 1]) : EditParams{}, sc.img);
      const std::string f = std::string(dir) + "/" + sc.name + "_" + vn[i] + ".ppm";
      if (FILE* fp = std::fopen(f.c_str(), "wb")) {
        std::fprintf(fp, "P6\n%d %d\n255\n", im.width, im.height);
        std::fwrite(im.data.data(), 1, im.data.size(), fp);
        std::fclose(fp);
      }
    }
}

}  // namespace

int main(int argc, char** argv) {
  const std::string id = argc >= 2 ? argv[1] : "";
  if (id == "sheetsrc" && argc > 2) {
    writeSheetSources(argv[2]);
    return 0;
  }
  struct T {
    const char* name;
    void (*fn)();
  };
  const T tests[] = {{"identity", testIdentity}, {"off", testOff},         {"json", testJson},   {"ends", testEnds},       {"fog", testFog},
                     {"highkey", testHighKey}, {"lowkey", testLowKey}, {"gap", testGap}, {"midgrey", testMidGrey},   {"hue", testHue},         {"skin", testSkin},   {"monotonic", testMonotonic}, {"tiles", testTiles},
                     {"cpubench", testCpuBench},
#ifdef DAPPLE_MODERN_GPU
                     {"parity", testParity}, {"parity_small", testParitySmall},     {"bench", testBench},
#endif
  };
  bool ran = false;
  for (const T& t : tests)
    if (id.empty() || id == t.name) {
      if (id.empty() && (std::string(t.name) == "cpubench" || std::string(t.name) == "off")) continue;
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
