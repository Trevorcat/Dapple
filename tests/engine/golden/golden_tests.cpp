// Golden corpus + hash harness for the engine migration (docs/engine/pipeline-architecture.md 10.1, step a0).
//
//   dapple_golden record <corpus.txt>            records the corpus from the CURRENT code (DAPPLE_ENGINE ignored: off)
//   dapple_golden verify <corpus.txt> [mode...]  mode = off | legacy | tables (default: off legacy tables); every frame
//                                                must hash to the recorded value in every mode (gate B)
//
// Inputs are synthetic (deterministic integer-hash scenes, no private photos). Each scenario is a short sequence of
// frames through dapple_session_render_frame on one session, so cache state (lattice reuse, tone tables, the
// interactive noise rule) is part of what is hashed. Plus core-API renders (RGBA8 direct, float baked, float direct).
// The corpus is tied to the platform it was recorded on (the film kernels pick AVX2 at run time): the file header
// carries a platform tag and a mismatching run exits 77 (skipped) instead of failing.
//
// The C API under test is compiled into this executable (like dapple_film_tests): the registry and the thread pool are
// process singletons.
#include "../../../core/src/c_api.cpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>

#include "dapple/look.h"

namespace {

using namespace dapple;

// ---- hashing ------------------------------------------------------------------------------------------------------------
struct Fnv {
  std::uint64_t h = 1469598103934665603ull;
  void bytes(const void* p, std::size_t n) {
    const auto* b = static_cast<const std::uint8_t*>(p);
    for (std::size_t i = 0; i < n; ++i) {
      h ^= b[i];
      h *= 1099511628211ull;
    }
  }
  void i32(std::int32_t v) { bytes(&v, 4); }
};

std::string hex(std::uint64_t v) {
  char b[24];
  std::snprintf(b, sizeof b, "%016llx", (unsigned long long)v);
  return b;
}

// ---- synthetic scenes (integer hash noise + polynomial gradients: no libm in the input) -----------------------------------
std::uint32_t hash32(std::uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}
float rnd(std::uint32_t a, std::uint32_t b, std::uint32_t c) { return float(hash32(a * 73856093u ^ b * 19349663u ^ c * 83492791u) >> 8) * (1.f / 16777216.f); }

SourceImage makeScene(int w, int h, std::uint32_t seed, bool scene) {
  SourceImage s;
  s.sceneReferred = scene;
  s.linear = ImageF(w, h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const float u = float(x) / float(w - 1), v = float(y) / float(h - 1);
      float* p = s.linear.row(y) + 3 * x;
      // sky-to-ground gradient with colour variation, a bright disc (highlights above 1), a dark band, textured patches
      float r = 0.08f + 0.9f * u * (1.f - 0.4f * v), g = 0.05f + 0.7f * (1.f - u) * v + 0.15f * u, b = 0.04f + 0.8f * (1.f - v) * (1.f - 0.5f * u);
      const float dx = u - 0.72f, dy = v - 0.28f;
      if (dx * dx + dy * dy < 0.012f) { r = 2.4f; g = 2.1f; b = 1.6f; }
      if (v > 0.78f && v < 0.9f) { r *= 0.05f; g *= 0.05f; b *= 0.06f; }
      if (u > 0.1f && u < 0.35f && v > 0.5f && v < 0.7f) {  // saturated patch
        r = 0.9f; g = 0.12f; b = 0.1f;
      }
      const float n = rnd(seed, std::uint32_t(x), std::uint32_t(y));
      const float n2 = rnd(seed + 7, std::uint32_t(x >> 2), std::uint32_t(y >> 2));
      const float tex = 1.f + 0.12f * (n - 0.5f) + 0.2f * (n2 - 0.5f) * ((x / 16 + y / 16) & 1);
      p[0] = std::max(0.f, r * tex);
      p[1] = std::max(0.f, g * tex);
      p[2] = std::max(0.f, b * tex);
      if (!scene) {  // display-referred: clip to 1
        for (int c = 0; c < 3; ++c) p[c] = std::min(p[c], 1.f);
      }
    }
  return s;
}

// ---- frame description ------------------------------------------------------------------------------------------------------
struct Frame {
  std::string label;
  EditParams p;
  int flags = 0;
  int longEdge = 0;
  int bake = 0;
  float region[4] = {0, 0, 0, 0};
  int regionLong = 0;
  std::string lut;
};

struct Scenario {
  std::string name;
  bool sceneReferred = true;
  int w = 640, h = 427;
  std::vector<Frame> frames;
};

enum { I = DAPPLE_FRAME_INTERACTIVE, A = DAPPLE_FRAME_ANALYSIS, X = DAPPLE_FRAME_EXACT };

EditParams base() {
  EditParams p;
  p.exposure = 0.35f;
  p.contrast = 12.f;
  p.highlights = -20.f;
  p.shadows = 18.f;
  p.toneStrength = 0.f;
  return p;
}

std::vector<Scenario> buildScenarios(const std::string& cubePath, bool haveFilm) {
  std::vector<Scenario> out;
  auto one = [&](const std::string& name, const EditParams& p, int flags, int le, int bake = 0, bool scene = true) {
    Scenario s;
    s.name = name;
    s.sceneReferred = scene;
    Frame f;
    f.label = "f0";
    f.p = p;
    f.flags = flags;
    f.longEdge = le;
    f.bake = bake;
    s.frames.push_back(f);
    out.push_back(std::move(s));
  };
  const EditParams b = base();

  // direct / baked / interactive / RGBA8
  one("direct_default", EditParams{}, X | A, 0);
  one("direct_base", b, X | A, 480);
  one("baked_final_default", EditParams{}, A, 0);
  one("baked_final_base", b, A, 480);
  one("baked_interactive_base", b, I | A, 320);
  one("baked_bake33", b, A, 400, 33);
  one("baked_jpeg_source", b, A, 0, 0, false);
  one("direct_jpeg_source", b, X, 0, 0, false);

  // region (zoom detail)
  {
    Scenario s;
    s.name = "region";
    for (int k = 0; k < 3; ++k) {
      Frame f;
      f.label = "r" + std::to_string(k);
      f.p = b;
      f.p.clarity = 20.f;
      f.flags = k == 1 ? (I | A) : A;
      f.longEdge = 0;
      f.region[0] = 0.2f + 0.05f * k;
      f.region[1] = 0.1f;
      f.region[2] = 0.7f;
      f.region[3] = 0.6f + 0.1f * k;
      f.regionLong = 400;
      s.frames.push_back(f);
    }
    out.push_back(std::move(s));
  }

  // geometry
  {
    Scenario s;
    s.name = "geometry";
    const char* crops[] = {"1:1", "4:5", "3:4", "16:9", "9:16", "orig"};
    for (const char* c : crops) {
      Frame f;
      f.label = std::string("crop_") + c;
      f.p = b;
      f.p.crop = c;
      f.p.vignette = -30.f;
      f.flags = A;
      f.longEdge = 420;
      s.frames.push_back(f);
    }
    for (int rot : {90, 180, 270}) {
      Frame f;
      f.label = "rot" + std::to_string(rot);
      f.p = b;
      f.p.rotate = rot;
      f.p.flip = rot == 180;
      f.flags = A;
      f.longEdge = 420;
      s.frames.push_back(f);
    }
    for (float st : {-6.5f, 3.25f, 17.f}) {
      Frame f;
      f.label = "straighten" + std::to_string(int(st * 100));
      f.p = b;
      f.p.straighten = st;
      f.p.crop = "4:5";
      f.p.sharpen = 70.f;
      f.flags = A;
      f.longEdge = 420;
      s.frames.push_back(f);
    }
    Frame f;
    f.label = "exact_geo";
    f.p = b;
    f.p.straighten = 4.f;
    f.p.rotate = 90;
    f.p.flip = true;
    f.flags = X | A;
    f.longEdge = 420;
    s.frames.push_back(f);
    out.push_back(std::move(s));
  }

  // curves
  {
    Scenario s;
    s.name = "curves";
    Frame f;
    f.label = "master";
    f.p = b;
    f.p.toneCurve = {{0, 0}, {64, 52}, {192, 208}, {255, 255}};
    f.flags = A;
    f.longEdge = 400;
    s.frames.push_back(f);
    f.label = "channels";
    f.p.toneCurveRed = {{0, 6}, {128, 140}, {255, 250}};
    f.p.toneCurveBlue = {{0, 0}, {100, 90}, {255, 255}};
    s.frames.push_back(f);
    f.label = "parametric";
    f.p.parametricShadows = 25.f;
    f.p.parametricLights = -30.f;
    f.p.parametricMidtoneSplit = 44.f;
    f.p.channelParametric[1][2] = 20.f;
    s.frames.push_back(f);
    f.label = "interactive";
    f.flags = I | A;
    s.frames.push_back(f);
    f.label = "exact";
    f.flags = X | A;
    s.frames.push_back(f);
    out.push_back(std::move(s));
  }

  // grading + colour
  {
    Scenario s;
    s.name = "grading";
    Frame f;
    f.longEdge = 400;
    f.flags = A;
    f.label = "split";
    f.p = b;
    f.p.gradeShadows = {220.f, 30.f, 5.f};
    f.p.gradeHighlights = {40.f, 25.f, -3.f};
    f.p.gradeBlend = 70.f;
    f.p.gradeBalance = 20.f;
    s.frames.push_back(f);
    f.label = "midglobal";
    f.p.gradeMidtones = {120.f, 20.f, 0.f};
    f.p.gradeGlobal = {300.f, 10.f, 2.f};
    s.frames.push_back(f);
    f.label = "hsl";
    f.p.hsl[kOrange].saturation = 30.f;
    f.p.hsl[kBlue].hue = -20.f;
    f.p.hsl[kRed].luminance = 25.f;
    f.p.vibrance = 30.f;
    f.p.saturation = 12.f;
    s.frames.push_back(f);
    f.label = "palette";
    f.p.hasPalette = true;
    f.p.paletteX = 0.3f;
    f.p.paletteY = 0.7f;
    f.p.look = "warm";
    s.frames.push_back(f);
    f.label = "exact";
    f.flags = X | A;
    s.frames.push_back(f);
    out.push_back(std::move(s));
  }

  // grain
  {
    Scenario s;
    s.name = "grain";
    Frame f;
    f.longEdge = 480;
    f.p = b;
    f.p.grain = 45.f;
    f.label = "final";
    f.flags = A;
    s.frames.push_back(f);
    f.label = "interactive";
    f.flags = I | A;
    s.frames.push_back(f);
    f.label = "region";
    f.flags = A;
    f.region[0] = 0.1f; f.region[1] = 0.1f; f.region[2] = 0.6f; f.region[3] = 0.7f;
    f.regionLong = 320;
    s.frames.push_back(f);
    f.region[0] = f.region[1] = f.region[2] = f.region[3] = 0;
    f.regionLong = 0;
    f.label = "exact";
    f.flags = X;
    s.frames.push_back(f);
    out.push_back(std::move(s));
  }

  // tonal class (fast vs exact analysis, reuse, the move)
  {
    Scenario s;
    s.name = "tonal_class";
    Frame f;
    f.longEdge = 480;
    f.p = b;
    f.p.toneStrength = 60.f;
    f.p.toneTarget = "low-long";
    f.label = "interactive";
    f.flags = I | A;
    s.frames.push_back(f);
    f.label = "final";
    f.flags = A;
    s.frames.push_back(f);
    f.p.toneTarget = "high-short";
    f.label = "interactive_hs";
    f.flags = I | A;
    s.frames.push_back(f);
    f.p.toneTarget = "";
    f.label = "own_class";
    s.frames.push_back(f);
    f.p.exposure += 0.4f;
    f.label = "own_class_exp";
    s.frames.push_back(f);
    f.label = "final_exp";
    f.flags = A;
    s.frames.push_back(f);
    f.label = "exact";
    f.flags = X | A;
    s.frames.push_back(f);
    out.push_back(std::move(s));
  }

  // looks
  {
    Scenario s;
    s.name = "looks";
    for (const auto& l : looks()) {
      Frame f;
      f.label = l.id;
      f.p = b;
      f.p.look = l.id;
      f.p.lookStrength = 80.f;
      f.flags = A;
      f.longEdge = 360;
      s.frames.push_back(f);
    }
    out.push_back(std::move(s));
  }

  // detail stages, one at a time and together
  {
    Scenario s;
    s.name = "detail";
    auto add = [&](const char* label, auto&& edit, int flags) {
      Frame f;
      f.label = label;
      f.p = b;
      f.p.sharpen = 0.f;
      f.p.nrColor = 0.f;
      edit(f.p);
      f.flags = flags;
      f.longEdge = 420;
      s.frames.push_back(f);
    };
    add("sharpen", [](EditParams& p) { p.sharpen = 80.f; p.sharpRadius = 15.f; }, A);
    add("nr_color", [](EditParams& p) { p.nrColor = 50.f; }, A);
    add("nr_luma", [](EditParams& p) { p.nrLuma = 40.f; }, A);
    add("clarity", [](EditParams& p) { p.clarity = 45.f; }, A);
    add("texture", [](EditParams& p) { p.texture = -30.f; }, A);
    add("dehaze", [](EditParams& p) { p.dehaze = 35.f; }, A);
    add("dehaze_neg", [](EditParams& p) { p.dehaze = -30.f; }, A);
    add("all", [](EditParams& p) { p.sharpen = 60.f; p.nrColor = 30.f; p.nrLuma = 25.f; p.clarity = 30.f; p.texture = 20.f; p.dehaze = 15.f; p.grain = 20.f; p.vignette = -25.f; }, A);
    add("all_interactive", [](EditParams& p) { p.sharpen = 60.f; p.nrColor = 30.f; p.nrLuma = 25.f; p.clarity = 30.f; p.texture = 20.f; p.dehaze = 15.f; p.grain = 20.f; }, I | A);
    add("all_exact", [](EditParams& p) { p.sharpen = 60.f; p.nrColor = 30.f; p.nrLuma = 25.f; p.clarity = 30.f; p.texture = 20.f; p.dehaze = 15.f; p.grain = 20.f; }, X);
    out.push_back(std::move(s));
  }

  // LUT
  if (!cubePath.empty()) {
    Scenario s;
    s.name = "lut";
    Frame f;
    f.p = b;
    f.p.lutAmount = 70.f;
    f.lut = cubePath;
    f.flags = A;
    f.longEdge = 400;
    f.label = "final";
    s.frames.push_back(f);
    f.label = "interactive";
    f.flags = I | A;
    s.frames.push_back(f);
    f.label = "exact";
    f.flags = X;
    s.frames.push_back(f);
    out.push_back(std::move(s));
  }

  // film on / off / blend / exact
  if (haveFilm) {
    Scenario s;
    s.name = "film";
    Frame f;
    f.longEdge = 400;
    f.p = b;
    f.p.filmStock = "kodak_portra_400";
    f.p.filmStrength = 100.f;
    f.label = "full_final";
    f.flags = A;
    s.frames.push_back(f);
    f.label = "full_interactive";
    f.flags = I | A;
    s.frames.push_back(f);
    f.p.filmWarmth = 25.f;
    f.p.filmTint = -10.f;
    f.p.filmExposure = 0.5f;
    f.label = "full_warm_interactive";
    s.frames.push_back(f);
    f.p.filmStrength = 50.f;
    f.p.vignette = -20.f;
    f.label = "blend50_interactive";
    s.frames.push_back(f);
    f.label = "blend50_final";
    f.flags = A;
    s.frames.push_back(f);
    f.p.filmStrength = 100.f;
    f.p.clarity = 25.f;
    f.p.grain = 15.f;
    f.label = "full_spatial_final";
    s.frames.push_back(f);
    f.p.crop = "4:5";
    f.p.straighten = 2.f;
    f.label = "full_geometry";
    s.frames.push_back(f);
    f.p.crop = "orig";
    f.p.straighten = 0.f;
    f.label = "full_region";
    f.region[0] = 0.2f; f.region[1] = 0.2f; f.region[2] = 0.7f; f.region[3] = 0.7f;
    f.regionLong = 300;
    s.frames.push_back(f);
    f.region[0] = f.region[1] = f.region[2] = f.region[3] = 0;
    f.regionLong = 0;
    f.p.filmStrength = 0.f;
    f.label = "strength0";
    s.frames.push_back(f);
    f.p.filmStock = "";
    f.label = "off";
    s.frames.push_back(f);
    f.p.filmStock = "kodak_portra_400";
    f.p.filmStrength = 60.f;
    f.label = "exact_blend";
    f.flags = X | A;
    s.frames.push_back(f);
    out.push_back(std::move(s));
  }

  // drag sequences: lattice reuse, tone-only changes, spatial-only changes, revisits, interactive -> final
  {
    auto seq = [&](const char* name, bool scene, auto&& vary, int n, int firstFlags, int restFlags, bool endFinal) {
      Scenario s;
      s.name = name;
      s.sceneReferred = scene;
      for (int k = 0; k < n; ++k) {
        Frame f;
        f.label = "d" + std::to_string(k);
        f.p = base();
        f.p.toneStrength = 40.f;
        vary(f.p, k);
        f.flags = (k == 0 ? firstFlags : restFlags) | A;
        f.longEdge = 480;
        s.frames.push_back(f);
      }
      if (endFinal) {
        Frame f = s.frames.back();
        f.label = "final";
        f.flags = A;
        s.frames.push_back(f);
        f.label = "final_again";
        s.frames.push_back(f);
      }
      out.push_back(std::move(s));
    };
    seq("drag_exposure", true, [](EditParams& p, int k) { p.exposure = 0.1f * float(k); }, 6, I, I, true);
    seq("drag_saturation", true, [](EditParams& p, int k) { p.saturation = 4.f * float(k); }, 6, I, I, true);
    seq("drag_clarity", true, [](EditParams& p, int k) { p.clarity = 7.f * float(k); }, 6, I, I, true);
    seq("drag_contrast_tc", true, [](EditParams& p, int k) { p.contrast = 8.f * float(k); p.toneTarget = "low-long"; }, 5, I, I, true);
    seq("drag_nr", true, [](EditParams& p, int k) { p.nrLuma = 9.f * float(k); }, 5, I, I, true);
    seq("drag_exposure_jpeg", false, [](EditParams& p, int k) { p.exposure = 0.1f * float(k); }, 4, I, I, true);
    // revisit: A B A C A D (retention of the last three bakes)
    seq("revisit", true, [](EditParams& p, int k) {
      static const float ev[] = {0.f, 0.3f, 0.f, 0.6f, 0.f, 0.9f, 0.3f, 0.f};
      p.exposure = ev[k % 8];
      p.saturation = (k == 3 || k == 5) ? 10.f : 0.f;
    }, 8, I, I, false);
    // bake size flows: interactive 57 -> final 65 -> interactive again (reuses the 65 lattice) -> explicit sizes
    {
      Scenario s;
      s.name = "bake_sizes";
      const int sizes[] = {0, 0, 0, 33, 41, 0, 65, 25};
      const int flags[] = {I, 0, I, I, 0, I, 0, I};
      int k = 0;
      for (int n : sizes) {
        Frame f;
        f.label = "b" + std::to_string(k);
        f.p = base();
        f.p.toneStrength = 30.f;
        if (k >= 5) f.p.shadows += 10.f;
        f.flags = flags[k] | A;
        f.longEdge = 420;
        f.bake = n;
        s.frames.push_back(f);
        ++k;
      }
      out.push_back(std::move(s));
    }
  }
  return out;
}

struct GoldenHash {
  std::string name;
  std::string hash;
  char bake = '-';  // B = lattice evaluated, R = lattice reused (tone table rebuilt), H = no bake work (full hit / exact)
};

std::string writeCube() {
  const fs::path p = fs::temp_directory_path() / "dapple_golden.cube";
  std::ofstream f(p);
  f << "TITLE \"golden\"\nLUT_3D_SIZE 4\n";
  for (int b = 0; b < 4; ++b)
    for (int g = 0; g < 4; ++g)
      for (int r = 0; r < 4; ++r) {
        const float rr = float(r) / 3.f, gg = float(g) / 3.f, bb = float(b) / 3.f;
        f << rr * 0.9f + 0.05f * gg << " " << gg * 0.95f + 0.04f * bb << " " << bb * 0.85f + 0.1f * rr << "\n";
      }
  return p.string();
}

// session on a synthetic source
DappleSession* openSynthetic(const Scenario& sc, std::uint32_t seed) {
  auto* s = new DappleSession;
  s->source = makeScene(sc.w, sc.h, seed, sc.sceneReferred);
  s->camera = &cameras().resolve("SONY", "ILCE-7M4");
  return s;
}

void runScenario(const Scenario& sc, std::uint32_t seed, std::vector<GoldenHash>& out) {
  DappleSession* s = openSynthetic(sc, seed);
  for (const Frame& f : sc.frames) {
    DappleFrameRequest rq{};
    rq.size = sizeof rq;
    rq.long_edge = f.longEdge;
    rq.flags = f.flags;
    rq.bake_size = f.bake;
    for (int i = 0; i < 4; ++i) rq.region[i] = f.region[i];
    rq.region_long_edge = f.regionLong;
    DappleImage im{};
    char* an = nullptr;
    const std::string pj = toJson(f.p);
    const auto ev0 = bakeCounters().latticeEvaluated.load(), re0 = bakeCounters().latticeReused.load();
    const int rc = dapple_session_render_frame(s, pj.c_str(), f.lut.empty() ? nullptr : f.lut.c_str(), &rq, &im,
                                               (f.flags & DAPPLE_FRAME_ANALYSIS) ? &an : nullptr);
    GoldenHash r;
    r.name = sc.name + "/" + f.label;
    r.bake = bakeCounters().latticeEvaluated.load() > ev0 ? 'B' : (bakeCounters().latticeReused.load() > re0 ? 'R' : 'H');
    if (rc != DAPPLE_OK) {
      r.hash = "ERROR:" + std::string(dapple_last_error() ? dapple_last_error() : "");
    } else {
      Fnv h;
      h.i32(im.width);
      h.i32(im.height);
      h.bytes(im.rgba, std::size_t(im.width) * im.height * 4);
      Fnv a;
      if (an) a.bytes(an, std::strlen(an));
      r.hash = hex(h.h) + (an ? ":" + hex(a.h) : "");
    }
    dapple_free_image(&im);
    dapple_free_string(an);
    out.push_back(std::move(r));
  }
  delete s;
}

// core API: RGBA8 direct render, float baked render, float direct render
void runCoreApi(std::vector<GoldenHash>& out) {
  const CameraProfile& cam = cameras().resolve("SONY", "ILCE-7M4");
  SourceImage src = makeScene(320, 214, 11, true);
  SourceImage proxy;
  proxy.sceneReferred = true;
  proxy.linear = resizeToLongEdge(src.linear, 256);
  EditParams p = base();
  p.clarity = 20.f;
  p.nrLuma = 15.f;
  p.grain = 10.f;
  p.toneStrength = 50.f;
  auto hashFloat = [](const ImageF& im) {
    Fnv h;
    h.i32(im.width);
    h.i32(im.height);
    h.bytes(im.data.data(), im.data.size() * 4);
    return hex(h.h);
  };
  {
    Renderer r(p, cam, nullptr);
    r.setToneProxy(&proxy);
    const Image8 i8 = r.render(src);
    Fnv h;
    h.i32(i8.width);
    h.i32(i8.height);
    h.bytes(i8.data.data(), i8.data.size());
    out.push_back({"core/render_rgb8", hex(h.h)});
    out.push_back({"core/render_float", hashFloat(r.renderFloat(src))});
    out.push_back({"core/render_float_nospatial", hashFloat(r.renderFloat(src, false))});
    for (int n : {25, 41}) {
      const auto lut = r.bake(src, n);
      FastOptions fo;
      out.push_back({"core/baked_float_n" + std::to_string(n), hashFloat(r.renderBakedFloat(src, *lut, fo))});
      int w, h2;
      std::uint8_t* px = r.renderBakedRgba(src, *lut, fo, w, h2);
      Fnv hh;
      hh.bytes(px, std::size_t(w) * h2 * 4);
      std::free(px);
      out.push_back({"core/baked_rgba_n" + std::to_string(n), hex(hh.h)});
      Fnv tl;
      tl.bytes(lut->tone.data(), lut->tone.size() * 4);
      tl.bytes(lut->rgb.data(), lut->rgb.size() * 4);
      out.push_back({"core/bake_tables_n" + std::to_string(n), hex(tl.h)});
    }
    const auto fast = r.bake(src, 33, nullptr, true);
    Fnv tl;
    tl.bytes(fast->tone.data(), fast->tone.size() * 4);
    tl.bytes(fast->analysis.histogram.data(), fast->analysis.histogram.size() * 4);
    out.push_back({"core/bake_fast_analysis", hex(tl.h)});
  }
}

std::string platformTag() {
  std::string t = "arch=";
#if defined(__x86_64__) || defined(_M_X64)
  t += "x86_64";
  t += __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma") ? " avx2=1" : " avx2=0";
#elif defined(__aarch64__)
  t += "arm64";
#else
  t += "other";
#endif
  return t;
}

std::vector<GoldenHash> runAll(bool haveFilm) {
  std::vector<GoldenHash> all;
  const std::string cube = writeCube();
  const auto scenarios = buildScenarios(cube, haveFilm);
  std::uint32_t seed = 100;
  for (const Scenario& sc : scenarios) runScenario(sc, seed++, all);
  runCoreApi(all);
  return all;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fputs("usage: dapple_golden record|verify <corpus.txt> [off|legacy|tables ...]\n", stderr);
    return 2;
  }
  const std::string cmd = argv[1], file = argv[2];
  const fs::path pack = fs::path(DAPPLE_SOURCE_DIR) / "core" / "data" / "film" / "dapple.dfp";
  bool haveFilm = false;
  if (fs::exists(pack)) {
    const std::string ps = pack.string();
    haveFilm = dapple_film_load_pack(ps.c_str()) == DAPPLE_OK;
  }
  if (!haveFilm) std::fputs("warning: film pack not loaded, film scenarios skipped\n", stderr);

  if (cmd == "record") {
    dapple::engine::setModeOverride(0);
    const auto r1 = runAll(haveFilm);
    const auto r2 = runAll(haveFilm);  // a second run in a fresh state must agree (determinism of the corpus itself)
    int bad = 0;
    for (std::size_t i = 0; i < r1.size(); ++i)
      if (r1[i].hash != r2[i].hash) {
        std::printf("NONDETERMINISTIC %s: %s vs %s\n", r1[i].name.c_str(), r1[i].hash.c_str(), r2[i].hash.c_str());
        ++bad;
      }
    if (bad) return 1;
    std::ofstream f(file);
    f << "# dapple golden corpus (docs/engine/pipeline-architecture.md 10.1). Recorded from the unchanged renderer.\n";
    f << "# platform " << platformTag() << (haveFilm ? " film=1" : " film=0") << "\n";
    for (const GoldenHash& r : r1) f << r.name << " " << r.hash << "\n";
    std::printf("recorded %zu hashes to %s (%s)\n", r1.size(), file.c_str(), platformTag().c_str());
    return 0;
  }
  if (cmd == "verify") {
    std::map<std::string, std::string> want;
    std::string platform;
    {
      std::ifstream f(file);
      std::string line;
      while (std::getline(f, line)) {
        if (line.rfind("# platform ", 0) == 0) platform = line.substr(11);
        if (line.empty() || line[0] == '#') continue;
        const auto sp = line.find(' ');
        want[line.substr(0, sp)] = line.substr(sp + 1);
      }
    }
    if (want.empty()) {
      std::fprintf(stderr, "corpus %s is empty or missing\n", file.c_str());
      return 1;
    }
    const std::string have = platformTag() + (haveFilm ? " film=1" : " film=0");
    if (platform != have) {
      std::printf("corpus recorded on '%s', this machine is '%s': skipped\n", platform.c_str(), have.c_str());
      return 77;
    }
    std::vector<std::string> modes;
    for (int i = 3; i < argc; ++i) modes.push_back(argv[i]);
    if (modes.empty()) modes = {"off", "legacy", "tables"};
    int failures = 0;
    for (const std::string& m : modes) {
      dapple::engine::setModeOverride(m == "off" ? 0 : (m == "legacy" ? 1 : 2));
      const auto r = runAll(haveFilm);
      int bad = 0;
      for (const GoldenHash& x : r) {
        const auto it = want.find(x.name);
        if (it == want.end()) {
          std::printf("[%s] NEW (not in corpus) %s\n", m.c_str(), x.name.c_str());
          ++bad;
        } else if (it->second != x.hash) {
          std::printf("[%s] MISMATCH %s: got %s want %s\n", m.c_str(), x.name.c_str(), x.hash.c_str(), it->second.c_str());
          ++bad;
        }
      }
      if (r.size() != want.size()) {
        std::printf("[%s] frame count %zu vs corpus %zu\n", m.c_str(), r.size(), want.size());
        ++bad;
      }
      std::printf("[%s] %zu frames, %d mismatches  %s\n", m.c_str(), r.size(), bad, dapple_engine_info_json());
      if (std::getenv("DAPPLE_GOLDEN_TRACE")) {  // per-frame bake classification, to diff the modes against each other
        std::string seq;
        for (const GoldenHash& x : r) seq += x.name + " " + x.bake + "\n";
        std::ofstream("golden_trace_" + m + ".txt") << seq;
      }
      failures += bad;
    }
    return failures ? 1 : 0;
  }
  return 2;
}
