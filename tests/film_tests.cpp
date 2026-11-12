// Unit tests for the spectral film engine (core/include/dapple/film.h). Run with ctest.

#include <algorithm>
#include <atomic>
#include <limits>
#include <stdexcept>
#include <thread>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

#include "colour_metrics.h"
#include "dapple/camera.h"
#include "dapple/dapple_c.h"
#include "dapple/edit_params.h"
#include "dapple/film.h"
#include "dapple/film_match.h"
#include "dapple/illuminant.h"
#include "dapple/renderer.h"
#include "dapple/tone.h"
#include "film_internal.h"
#include "film_kernels.h"
#include "film_spatial.h"
#include "parallel.h"

namespace fs = std::filesystem;
using namespace dapple;
using namespace dapple::film;

namespace {

int g_checks = 0, g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    ++g_checks;                                                              \
    if (!(cond)) {                                                           \
      ++g_failures;                                                          \
      std::fprintf(stderr, "  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    }                                                                        \
  } while (0)

#define CHECK_NEAR(a, b, tol)                                                                              \
  do {                                                                                                     \
    ++g_checks;                                                                                            \
    const double va_ = (a), vb_ = (b);                                                                     \
    if (!(std::fabs(va_ - vb_) <= (tol))) {                                                                \
      ++g_failures;                                                                                        \
      std::fprintf(stderr, "  FAIL %s:%d  %s = %.6g, expected %.6g +- %.3g\n", __FILE__, __LINE__, #a, va_, vb_, \
                   double(tol));                                                                           \
    }                                                                                                      \
  } while (0)

#ifndef DAPPLE_SOURCE_DIR
#define DAPPLE_SOURCE_DIR "."
#endif
const fs::path kData = fs::path(DAPPLE_SOURCE_DIR) / "core" / "data" / "film";

std::shared_ptr<FilmLibrary> g_lib;  // pack if present, else generated from JSON (slower)

std::shared_ptr<FilmLibrary> library() {
  if (!g_lib) {
    std::string err;
    const char* envPack = std::getenv("DAPPLE_TEST_PACK");
    const fs::path pack = envPack ? fs::path(envPack) : kData / "dapple.dfp";
    if (fs::exists(pack)) g_lib = FilmLibrary::loadPack(pack, &err);
    if (!g_lib) g_lib = FilmLibrary::loadJsonDir(kData / "profiles", &err);
    if (!g_lib) std::fprintf(stderr, "cannot load film library: %s\n", err.c_str());
  }
  return g_lib;
}

FilmParams noSpatial(const std::string& film = "kodak_portra_400", const std::string& paper = "kodak_portra_endura") {
  FilmParams p;
  p.film = film;
  p.paper = paper;
  p.camera.autoExposure = false;
  p.disableSpatialAndStochastic();
  return p;
}

metrics::Lab lab(const float rgb[3]) { return metrics::srgbToLab(rgb); }

void testProfiles() {
  std::string err;
  auto lib = library();
  CHECK(lib != nullptr);
  if (!lib) return;
  CHECK(lib->names().size() == 28);
  const FilmStock* p4 = lib->find("kodak_portra_400");
  CHECK(p4 != nullptr);
  if (!p4) return;
  CHECK(p4->type == "negative" && p4->support == "film" && p4->stage == "filming");
  CHECK(p4->referenceIlluminant == "D55" && p4->viewingIlluminant == "D50");
  // each stock names the paper it is made to be printed on (info.target_print), and the default paper follows it
  CHECK(p4->targetPrint == "kodak_portra_endura");
  CHECK(film::defaultPaperFor(*lib, "kodak_portra_400") == "kodak_portra_endura");
  CHECK(film::defaultPaperFor(*lib, "fujifilm_pro_400h") == "fujifilm_crystal_archive_typeii");
  CHECK(film::defaultPaperFor(*lib, "fujifilm_xtra_400") == "fujifilm_crystal_archive_typeii");
  CHECK(film::defaultPaperFor(*lib, "kodak_vision3_500t") == "kodak_2383");
  CHECK(film::defaultPaperFor(*lib, "kodak_kodachrome_64") == "none");
  CHECK(p4->logSensitivity.size() == 81 * 3 && p4->channelDensity.size() == 81 * 3 && p4->baseDensity.size() == 81);
  CHECK(p4->logExposure.size() == 256 && p4->densityCurves.size() == 256 * 3 && p4->densityCurvesLayers.size() == 256 * 9);
  CHECK(p4->windowParams.size() == 4 && p4->surfaceParams.size() == 45);
  CHECK_NEAR(p4->windowParams[0], 433.4923489, 1e-3);
  // spec 02 s2.1: Portra 400 channel_density NaN at 380 and 750..780; base NaN at 380..395 and 705..780
  CHECK(std::isnan(p4->channelDensity[0]) && std::isnan(p4->channelDensity[80 * 3]) && !std::isnan(p4->channelDensity[20 * 3]));
  CHECK(std::isnan(p4->baseDensity[0]) && std::isnan(p4->baseDensity[3]) && !std::isnan(p4->baseDensity[4]));
  const FilmStock* e = lib->find("kodak_portra_endura");
  CHECK(e && e->isPaper() && e->isPrintMedium() && e->modelCenters.size() == 9);
  CHECK(!e->copyright.empty() && e->license.find("CC BY-SA 4.0") != std::string::npos);
  CHECK(lib->find("kodak_ektachrome_100")->positive());
  CHECK(lib->attribution().find("Andrea Volpato") != std::string::npos);

  // JSON path agrees with the pack
  FilmStock js;
  CHECK(FilmLibrary::loadProfileJson(kData / "profiles" / "kodak_portra_400.json", js, &err));
  CHECK(js.stock == p4->stock && js.densityCurves == p4->densityCurves && js.channelDensity.size() == p4->channelDensity.size());
  bool sameNan = true;
  for (std::size_t i = 0; i < js.channelDensity.size(); ++i)
    sameNan = sameNan && (std::isnan(js.channelDensity[i]) == std::isnan(p4->channelDensity[i]));
  CHECK(sameNan);
  double worstLayers = 0;
  for (std::size_t i = 0; i < js.densityCurvesLayers.size(); ++i)
    worstLayers = std::max(worstLayers, double(std::fabs(js.densityCurvesLayers[i] - p4->densityCurvesLayers[i])));
  CHECK(worstLayers == 0.0);  // grain sub-layers are stored as float32: pack and JSON libraries are identical

  // pack round trip through memory
  const fs::path tmp = fs::temp_directory_path() / "dapple_film_test.dfp";
  CHECK(lib->writePack(tmp, &err));
  auto back = FilmLibrary::loadPack(tmp, &err);
  CHECK(back && back->names() == lib->names() && back->upsample().coef == lib->upsample().coef &&
        back->cmax().data == lib->cmax().data);
  if (back)
    for (const std::string& n : lib->names()) CHECK(back->find(n) && back->find(n)->targetPrint == lib->find(n)->targetPrint);  // PAIR section
  std::error_code ec;
  fs::remove(tmp, ec);
  CHECK(FilmLibrary::loadPackMemory("garbage", 7, &err) == nullptr);

  // every profile: curves monotone in the process direction, within data tolerance
  for (const std::string& n : lib->names()) {
    const FilmStock* s = lib->find(n);
    for (int m = 0; m < 3; ++m) {
      double worst = 0;
      for (int k = 1; k < 256; ++k) {
        const double d = double(s->densityCurves[k * 3 + m]) - s->densityCurves[(k - 1) * 3 + m];
        worst = std::max(worst, s->positive() ? d : -d);  // violation magnitude
      }
      if (!(worst < 2e-3)) std::fprintf(stderr, "  non-monotone curve %s ch %d: %.4g\n", n.c_str(), m, worst);
      CHECK(worst < 2e-3);
    }
  }
}

void testColorimetry() {
  using namespace dapple::film::detail;
  struct Ill {
    const char* n;
    double x, y;
  };
  for (const Ill& i : {Ill{"D55", 0.3324316389, 0.3474443989}, Ill{"T", 0.44757, 0.40745},
                       Ill{"TH-KG3", 0.4017433451, 0.3982017988}, Ill{"D50", 0.345674591, 0.3585103882},
                       Ill{"D65", 0.3127205252, 0.329030685}, Ill{"K75P", 0.3152468589, 0.3328584711}}) {
    Spec s;
    CHECK(illuminant(i.n, s));
    double x, y;
    xyOfXyz(illuminantXyz(s), x, y);
    CHECK_NEAR(x, i.x, 2e-4);
    CHECK_NEAR(y, i.y, 2e-4);
  }
  // TH-KG3 computed from Planck(3400 K) x KG3 agrees with the tabulated column (spec appendix) at a few samples
  Spec th;
  illuminant("TH-KG3", th);
  CHECK_NEAR(th[0], 0.271155, 2e-3);
  CHECK_NEAR(th[32], 1.23034, 4e-3);
  CHECK_NEAR(th[80], 0.414553, 2e-3);
  // CAT16 composite ProPhoto -> D55 (spec 01 s2.3)
  Spec d55;
  illuminant("D55", d55);
  double wx, wy;
  xyOfXyz(illuminantXyz(d55), wx, wy);
  const Mat3 m = mul(adaptationMatrix(cat16(), kD50x, kD50y, wx, wy), prophotoToXyz());
  const double ref[9] = {0.7894029154, 0.1228212235, 0.0444682067, 0.2869884109, 0.7134785957,
                         -0.0004666708, 0.0014710101, 0.0038855140, 0.9157837998};
  for (int i = 0; i < 9; ++i) CHECK_NEAR(m[i], ref[i], 2e-4);
  // CAM16-UCS (spec 02 s4.5.2): Y=0.18 grey -> J' 52.28; white -> 100
  Cam16 cam;
  double J, a, b;
  cam.forward(mul(srgbToXyz(), Vec3{0.18, 0.18, 0.18}), J, a, b);
  CHECK_NEAR(J, 52.28, 0.05);
  cam.forward(mul(srgbToXyz(), Vec3{1, 1, 1}), J, a, b);
  CHECK_NEAR(J, 100.0, 0.01);
  // inverse is the exact inverse
  const Vec3 xyz{0.3, 0.25, 0.1};
  cam.forward(xyz, J, a, b);
  const Vec3 back = cam.inverse(J, a, b);
  for (int i = 0; i < 3; ++i) CHECK_NEAR(back[i], xyz[i], 1e-9);
  // output gamut compression reference samples (spec 02 s4.5.3)
  const CmaxTable& tab = library()->cmax();
  const Vec3 w = compressOutput(cam, tab, Vec3{1, 1, 1});
  CHECK_NEAR(w[0], 0.771826, 4e-3);
  const Vec3 g = compressOutput(cam, tab, Vec3{0.18, 0.18, 0.18});
  CHECK_NEAR(g[1], 0.180010, 1e-3);
  const Vec3 c = compressOutput(cam, tab, Vec3{0.9, 0.3, 0.1});
  CHECK_NEAR(c[0], 0.866123, 6e-3);
  CHECK_NEAR(c[1], 0.309449, 6e-3);
  CHECK_NEAR(c[2], 0.116713, 6e-3);
}

void testUpsampleTable() {
  using namespace dapple::film::detail;
  const UpsampleTable& up = library()->upsample();
  CHECK(up.n == 192);
  // interior nodes reproduce their chromaticity; spectra stay positive and bounded
  int bad = 0, checked = 0;
  for (int j = 4; j < up.n; j += 5)
    for (int i = 4; i < up.n; i += 5) {
      double x, y;
      tcToXy(double(i) / (up.n - 1), double(j) / (up.n - 1), x, y);
      if (std::hypot(x - 1.0 / 3.0, y - 1.0 / 3.0) > 0.2) continue;  // well inside the locus (attainable range)
      const float* c = &up.coef[(std::size_t(j) * up.n + i) * 4];
      const double a[3] = {c[0], c[1], c[2]};
      Spec s;
      spectrumFromCoeffs(a, s);
      double X = 0, Y = 0, Z = 0;
      for (int l = 0; l < kN; ++l) {
        X += s[l] * cmf(0)[l];
        Y += s[l] * cmf(1)[l];
        Z += s[l] * cmf(2)[l];
        if (!(s[l] > 0 && s[l] < 1)) {
          ++bad;
          std::fprintf(stderr, "  s out of range at node %d,%d l=%d: %g\n", i, j, l, s[l]);
        }
      }
      const double sum = X + Y + Z;
      const double ex = std::hypot(X / sum - x, Y / sum - y);
      ++checked;
      if (ex > 1e-3) {
        ++bad;
        std::fprintf(stderr, "  chromaticity error %.4g at node %d,%d xy %.3f %.3f\n", ex, i, j, x, y);
      }
      CHECK_NEAR(c[3] * sum, 1.0, 1e-4);
    }
  CHECK(checked > 100);
  CHECK(bad == 0);
}

void testGreyNeutral() {
  FilmSimulator sim(library());
  std::string err;
  CHECK(sim.setParams(noSpatial(), &err));
  const FilmParams& e = sim.effectiveParams();
  // our own solver lands near the reference's database values (spec appendix B: 51.57 / 52.53)
  CHECK_NEAR(e.enlarger.mFilterNeutral, 51.57, 4.0);
  CHECK_NEAR(e.enlarger.yFilterNeutral, 52.53, 4.0);
  double worstSpread = 0;
  float prevL = -1;
  bool mono = true;
  for (int i = 0; i < 40; ++i) {
    const double v = 0.002 * std::pow(1000.0, i / 39.0);  // 0.002 .. 2
    const double rgb[3] = {v, v, v};
    double o[3];
    sim.renderPixelDirect(rgb, o);
    worstSpread = std::max(worstSpread, std::max(std::fabs(o[0] - o[1]), std::fabs(o[1] - o[2])));
    const float f[3] = {float(o[0]), float(o[1]), float(o[2])};
    const float L = float(lab(f).L);
    if (L < prevL - 1e-4f) mono = false;
    prevL = L;
  }
  std::printf("  grey ramp max channel spread %.4f (encoded)\n", worstSpread);
  CHECK(worstSpread < 0.02);
  CHECK(mono);
  // 18.4 % grey prints at ~18 % linear = 0.46 encoded
  const double g[3] = {0.184, 0.184, 0.184};
  double o[3];
  sim.renderPixelDirect(g, o);
  CHECK_NEAR(o[0], 0.4605, 0.01);
  CHECK_NEAR(o[1], 0.4605, 0.01);
  CHECK_NEAR(o[2], 0.4605, 0.01);
  // neutral stays neutral in Lab chroma over the ramp
  for (double v : {0.01, 0.05, 0.184, 0.5}) {
    const double rgb[3] = {v, v, v};
    sim.renderPixelDirect(rgb, o);
    const float f[3] = {float(o[0]), float(o[1]), float(o[2])};
    CHECK(lab(f).C() < 3.0);
  }
}

void testSpecVectors() {
  // Informational comparison with the reference values of spec 01 s2.10 / 02 s8 (our own spectral
  // upsampling table differs from the reference's, so we only assert loose agreement).
  FilmSimulator sim(library());
  FilmParams p = noSpatial();
  p.io.outputGamutCompress = false;
  p.enlarger.mFilterNeutral = 51.568015;  // spec uses the database neutral filtration
  p.enlarger.yFilterNeutral = 52.534004;
  p.settings.neutralPrintFilters = false;
  CHECK(sim.setParams(p));
  struct V {
    double rgb[3], logE[3], d[3];
  };
  const V vs[] = {{{0.184, 0.184, 0.184}, {0.005433, -0.004599, -0.037299}, {0.692898, 0.704642, 0.832530}},
                  {{0.0184, 0.0184, 0.0184}, {-0.994567, -1.004599, -1.037299}, {0.163793, 0.202774, 0.255060}},
                  {{0.736, 0.736, 0.736}, {0.607493, 0.597461, 0.564761}, {1.017897, 1.006003, 1.185955}},
                  {{0.5, 0.1, 0.1}, {0.634810, -0.451096, -0.130811}, {1.170164, 0.237869, 0.724925}},
                  {{0.1, 0.4, 0.1}, {-0.389908, 0.357974, -0.424760}, {0.444005, 1.122071, 0.474925}},
                  {{0.1, 0.1, 0.5}, {-0.684340, -0.060549, 0.414337}, {0.191806, 0.723574, 1.423281}},
                  {{0.6, 0.4, 0.2}, {0.603657, 0.261611, -0.007414}, {1.092154, 0.801189, 0.717192}},
                  {{1, 1, 1}, {0.740615, 0.730583, 0.697883}, {1.094575, 1.074399, 1.264672}}};
  double sumLog = 0, sumD = 0;
  for (const V& v : vs) {
    double le[3], d[3];
    sim.directIntermediates(v.rgb, le, d);
    std::printf("  rgb %.3f %.3f %.3f  logE ours (%+.4f %+.4f %+.4f) ref (%+.4f %+.4f %+.4f)  density ours (%.4f %.4f %.4f) ref (%.4f %.4f %.4f)\n",
                v.rgb[0], v.rgb[1], v.rgb[2], le[0], le[1], le[2], v.logE[0], v.logE[1], v.logE[2], d[0], d[1], d[2],
                v.d[0], v.d[1], v.d[2]);
    for (int c = 0; c < 3; ++c) {
      sumLog += std::fabs(le[c] - v.logE[c]);
      sumD += std::fabs(d[c] - v.d[c]);
    }
  }
  std::printf("  mean |dlogE| %.4f, mean |ddensity| %.4f over the spec vectors\n", sumLog / 24, sumD / 24);
  // grey exposure: window-on values of the spec (1.0126, 0.9895, 0.9177 -> log10 0.0054, -0.0046, -0.0373)
  double le[3], d[3];
  sim.directIntermediates(vs[0].rgb, le, d);
  for (int c = 0; c < 3; ++c) CHECK_NEAR(le[c], vs[0].logE[c], 0.03);
  CHECK(sumLog / 24 < 0.12);
  // section 8: print + scan of the film densities (compression off), linear output
  struct S8 {
    double rgb[3], out[3];
  };
  const S8 s8[] = {{{0.184, 0.184, 0.184}, {0.179458, 0.179308, 0.179180}},
                   {{0.7, 0.7, 0.7}, {0.587708, 0.599399, 0.604585}},
                   {{0.5, 0.1, 0.1}, {0.478172, 0.010270, 0.066269}},
                   {{2, 1, 0.3}, {0.861840, 0.475074, 0.124249}}};
  for (const S8& s : s8) {
    double o[3];
    sim.renderPixelDirect(s.rgb, o);
    const double lin[3] = {metrics::srgbToLinear(o[0]), metrics::srgbToLinear(o[1]), metrics::srgbToLinear(o[2])};
    std::printf("  print+scan rgb %.3f %.3f %.3f  linear out ours (%.4f %.4f %.4f) ref (%.4f %.4f %.4f)\n", s.rgb[0], s.rgb[1],
                s.rgb[2], lin[0], lin[1], lin[2], s.out[0], s.out[1], s.out[2]);
  }
  double o[3];
  sim.renderPixelDirect(vs[0].rgb, o);
  CHECK_NEAR(metrics::srgbToLinear(o[1]), 0.179308, 0.012);
}

void testBakedVsDirect() {
  struct Case {
    const char* film;
    const char* paper;
    int lut;
  };
  std::mt19937 rng(7);
  std::uniform_real_distribution<double> u(0, 1);
  for (const Case& c : {Case{"kodak_portra_400", "kodak_portra_endura", 33}, Case{"kodak_ektar_100", "kodak_supra_endura", 33},
                        Case{"fujifilm_pro_400h", "fujifilm_crystal_archive_typeii", 33}, Case{"kodak_portra_400", "kodak_portra_endura", 17},
                        Case{"kodak_portra_400", "kodak_portra_endura", 65}, Case{"kodak_ektachrome_100", "none", 33},
                        Case{"kodak_vision3_500t", "kodak_2383", 33}}) {
    FilmSimulator sim(library());
    FilmParams p = noSpatial(c.film, c.paper);
    p.settings.lutResolution = c.lut;
    std::string err;
    CHECK(sim.setParams(p, &err));
    std::vector<double> des;
    double mean = 0, worst = 0;
    for (int i = 0; i < 3000; ++i) {
      double rgb[3];
      if (i < 40) {
        const double v = 0.002 * std::pow(1000.0, i / 39.0);
        rgb[0] = rgb[1] = rgb[2] = v;
      } else {
        for (double& v : rgb) v = u(rng) * u(rng) * 1.6;
      }
      double d[3];
      sim.renderPixelDirect(rgb, d);
      const float f[3] = {float(rgb[0]), float(rgb[1]), float(rgb[2])};
      float o[3];
      sim.renderPixelBaked(f, o);
      const float df[3] = {float(d[0]), float(d[1]), float(d[2])};
      const double de = metrics::deltaE2000(lab(df), lab(o));
      mean += de;
      worst = std::max(worst, de);
      des.push_back(de);
    }
    mean /= des.size();
    std::sort(des.begin(), des.end());
    std::printf("  baked vs direct %-20s + %-32s N=%3d: mean dE2000 %.4f  p99 %.3f  max %.3f\n", c.film, c.paper, c.lut, mean,
                des[size_t(des.size() * 0.99)], worst);
    CHECK(mean < 0.5);
    if (c.lut >= 33) CHECK(mean < 0.1);
  }
}

void testDirAndParams() {
  FilmSimulator sim(library());
  FilmParams p = noSpatial();
  CHECK(sim.setParams(p));
  const double grey[3] = {0.184, 0.184, 0.184};
  double le[3], dOn[3];
  sim.directIntermediates(grey, le, dOn);
  p.filmRender.dirCouplers.active = false;
  CHECK(sim.setParams(p));
  double dOff[3];
  sim.directIntermediates(grey, le, dOff);
  for (int c = 0; c < 3; ++c) CHECK_NEAR(dOn[c], dOff[c], 0.02);  // grey ramp reproduces the stock curve
  // saturated colour: DIR raises colour density differences
  const double red[3] = {0.5, 0.1, 0.1};
  double dRedOff[3];
  sim.directIntermediates(red, le, dRedOff);
  p.filmRender.dirCouplers.active = true;
  CHECK(sim.setParams(p));
  double dRedOn[3];
  sim.directIntermediates(red, le, dRedOn);
  CHECK(dRedOn[1] < dRedOff[1] - 0.05);

  // params JSON: harness dump parses, unknown keys are rejected, round trip is stable
  FilmParams q;
  std::string err;
  CHECK(q.applyJson("{\"enlarger\":{\"y_filter_shift\":5},\"film_render\":{\"grain\":{\"particle_area_um2\":0.4}}}", &err));
  CHECK_NEAR(q.enlarger.yFilterShift, 5, 0);
  CHECK_NEAR(q.filmRender.grain.particleAreaUm2, 0.4, 0);
  CHECK(!q.applyJson("{\"enlarger\":{\"y_filter_shiftt\":5}}", &err));
  CHECK(err.find("unknown key") != std::string::npos);
  const fs::path dump = fs::path(DAPPLE_SOURCE_DIR) / "build" / "film" / "ref" / "params_nospatial.json";
  if (fs::exists(dump)) {
    std::ifstream f(dump);
    std::stringstream ss;
    ss << f.rdbuf();
    FilmParams r;
    CHECK(r.applyJson(ss.str(), &err));
    if (!err.empty()) std::fprintf(stderr, "  %s\n", err.c_str());
    CHECK(r.debug.deactivateSpatialEffects && !r.filmRender.glare.active);
  }
  FilmParams rt;
  CHECK(rt.applyJson(p.toJson(), &err));
  CHECK(rt.toJson() == p.toJson());
  // print exposure: 2.0 darkens
  FilmParams pe = noSpatial();
  FilmSimulator s2(library());
  CHECK(s2.setParams(pe));
  float a[3], b[3];
  const float rgbf[3] = {0.184f, 0.184f, 0.184f};
  s2.renderPixelBaked(rgbf, a);
  pe.enlarger.printExposure = 2.0;
  FilmTimings t;
  CHECK(s2.setParams(pe, &err, &t));
  s2.renderPixelBaked(rgbf, b);
  CHECK(b[1] < a[1] - 0.1);
  // changing only a spatial parameter must not rebake
  pe.filmRender.grain.particleAreaUm2 = 0.5;
  CHECK(s2.setParams(pe, &err, &t));
  CHECK(t.bakeMs == 0);
  // unknown stock
  FilmParams bad = noSpatial("no_such_film");
  CHECK(!s2.setParams(bad, &err));
  CHECK(!s2.setParams(noSpatial("kodak_portra_400", "kodak_portra_400"), &err));
}

void testRenderPaths() {
  FilmSimulator sim(library());
  CHECK(sim.setParams(noSpatial()));
  ImageF in(97, 61);
  std::mt19937 rng(3);
  std::uniform_real_distribution<float> u(0.f, 1.f);
  for (float& v : in.data) v = u(rng) * u(rng) * 1.3f;
  ImageF fused, staged, hooked;
  CHECK(sim.render(in, fused));
  CHECK(sim.renderStaged(in, staged));
  double worst = 0;
  for (std::size_t i = 0; i < fused.data.size(); ++i) worst = std::max(worst, double(std::fabs(fused.data[i] - staged.data[i])));
  CHECK(worst < 1e-5);
  // hooks
  struct Counts {
    int e = 0, q = 0, d = 0, s = 0;
  } counts;
  FilmSpatialHooks h;
  h.user = &counts;
  h.exposure = [](void* u, float*, const FilmSpatialContext& c) {
    ++static_cast<Counts*>(u)->e;
    (void)c;
  };
  h.inhibitor = [](void* u, float*, const FilmSpatialContext&) { ++static_cast<Counts*>(u)->q; };
  h.density = [](void* u, float*, const FilmSpatialContext&) { ++static_cast<Counts*>(u)->d; };
  h.scan = [](void* u, float*, const FilmSpatialContext& c) {
    ++static_cast<Counts*>(u)->s;
    CHECK(c.width == 97 && c.height == 61 && c.pixelSizeUm > 0);
  };
  sim.setHooks(h);
  CHECK(sim.render(in, hooked));
  CHECK(counts.e == 1 && counts.q == 1 && counts.d == 1 && counts.s == 1);
  worst = 0;
  for (std::size_t i = 0; i < fused.data.size(); ++i) worst = std::max(worst, double(std::fabs(fused.data[i] - hooked.data[i])));
  CHECK(worst < 1e-5);
  sim.setHooks({});
  // SIMD variants agree
  const auto& sel = detail::selectKernels();
  if (&sel != &detail::kBase) {
    FilmSimulator s2(library());
    s2.setParams(noSpatial());
    ImageF viaBase(in.width, in.height);
    // render with the base set through the public single-pixel entry is not exposed: compare pixel sets
    float worstDiff = 0;
    for (int i = 0; i < in.width * in.height; i += 7) {
      float o[3];
      s2.renderPixelBaked(&in.data[i * 3], o);
      for (int c = 0; c < 3; ++c) worstDiff = std::max(worstDiff, std::fabs(o[c] - fused.data[i * 3 + c]));
    }
    CHECK(worstDiff < 1e-5f);
  }
  // auto exposure
  ImageF flat(64, 64);
  for (float& v : flat.data) v = 0.092f;
  CHECK_NEAR(FilmSimulator::meterEv(flat, "center_weighted"), 1.0, 0.01);
  for (float& v : flat.data) v = 0.184f;
  for (const char* m : {"average", "median", "center_weighted", "partial", "matrix", "multi_zone", "highlight_weighted"})
    CHECK_NEAR(FilmSimulator::meterEv(flat, m), 0.0, 0.01);
  CHECK_NEAR(FilmSimulator::meterEv(flat, "nonsense"), 0.0, 1e-6);
}

void testSmokeAllStocks() {
  auto lib = library();
  std::mt19937 rng(11);
  std::uniform_real_distribution<double> u(0, 1);
  int count = 0;
  for (const std::string& n : lib->names()) {
    const FilmStock* s = lib->find(n);
    if (s->isPrintMedium()) continue;
    for (const char* paper : {"kodak_portra_endura", "none"}) {
      FilmSimulator sim(lib);
      FilmParams p = noSpatial(n, paper);
      std::string err;
      if (!sim.setParams(p, &err)) {
        std::fprintf(stderr, "  %s + %s: %s\n", n.c_str(), paper, err.c_str());
        CHECK(false);
        continue;
      }
      bool finite = true;
      float lo = 1e9f, hi = -1e9f;
      for (int i = 0; i < 200; ++i) {
        const float rgb[3] = {float(u(rng) * u(rng)), float(u(rng) * u(rng)), float(u(rng) * u(rng))};
        float o[3];
        sim.renderPixelBaked(rgb, o);
        for (float v : o) {
          finite = finite && std::isfinite(v);
          lo = std::min(lo, v);
          hi = std::max(hi, v);
        }
      }
      CHECK(finite);
      CHECK(lo > -0.15f && hi < 1.15f);
      ++count;
    }
  }
  CHECK(count == 40);
}


// ---- spatial effects ------------------------------------------------------------------------------------------------

using namespace dapple::film::detail;

// Exact reflect-boundary Gaussian (double precision, 6 sigma kernel) for one plane.
std::vector<double> bruteGauss(const std::vector<float>& img, int W, int H, double sigma) {
  const int r = int(std::ceil(6 * sigma));
  std::vector<double> k(r + 1), tmp(img.size()), out(img.size());
  double sum = 0;
  for (int i = 0; i <= r; ++i) {
    k[i] = std::exp(-double(i) * i / (2 * sigma * sigma));
    sum += i ? 2 * k[i] : k[i];
  }
  for (double& v : k) v /= sum;
  auto refl = [](int i, int n) {
    const int p = 2 * n;
    i %= p;
    if (i < 0) i += p;
    return i < n ? i : p - 1 - i;
  };
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      double a = 0;
      for (int d = -r; d <= r; ++d) a += k[std::abs(d)] * img[std::size_t(y) * W + refl(x + d, W)];
      tmp[std::size_t(y) * W + x] = a;
    }
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      double a = 0;
      for (int d = -r; d <= r; ++d) a += k[std::abs(d)] * tmp[std::size_t(refl(y + d, H)) * W + x];
      out[std::size_t(y) * W + x] = a;
    }
  return out;
}

std::vector<float> testImage(int W, int H, unsigned seed) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> u(0.f, 1.f);
  std::vector<float> v(std::size_t(W) * H);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      float s = 0.2f + 0.3f * std::sin(x * 0.05f) * std::cos(y * 0.07f);
      if (x > W / 3 && x < W / 2 && y > H / 4 && y < H / 2) s += 2.0f;  // bright block (hard edges)
      if (std::hypot(x - 0.7 * W, y - 0.7 * H) < 3) s += 8.0f;           // point-like highlight
      v[std::size_t(y) * W + x] = s + 0.02f * u(rng);
    }
  return v;
}

void testGaussianMixtureEngine() {
  const SpatialKernels& K = selectSpatialKernels();
  Arena arena;
  const int W = 211, H = 157;
  const auto img = testImage(W, H, 5);
  Plane src{const_cast<float*>(img.data()), W, H, W};
  std::vector<float> dstv(img.size());
  Plane dst{dstv.data(), W, H, W};
  const double range = 8.5;
  for (double sigma : {0.4, 0.9, 2.0, 3.0, 3.5, 5.0, 7.5, 12.0, 31.0, 90.0}) {
    arena.reset();
    Mixture m = mixGauss(sigma);
    applyMixture(K, arena, src, dst, m);
    const auto ref = bruteGauss(img, W, H, sigma);
    double worst = 0, mean = 0;
    for (std::size_t i = 0; i < img.size(); ++i) {
      worst = std::max(worst, std::fabs(ref[i] - dstv[i]));
      mean += std::fabs(ref[i] - dstv[i]);
    }
    std::printf("  gauss sigma %5.1f: max err %.2e (%.2e of range), mean err %.2e\n", sigma, worst, worst / range, mean / img.size());
    // sigma < 3 uses the reference's truncated FIR (3 sigma): looser against the exact Gaussian
    CHECK(worst / range < (sigma < 3 ? 2e-2 : 4e-3));
  }
  // energy: a mixture whose weights sum to one keeps a constant image constant and the mean of any image
  Mixture m;
  m.w0 = 0.3;
  m.comps = {{0.5, 0.1}, {3.5, 0.2}, {12.0, 0.2}, {60.0, 0.2}};
  std::vector<float> ones(img.size(), 1.f);
  Plane po{ones.data(), W, H, W};
  arena.reset();
  applyMixture(K, arena, po, dst, m);
  double dev = 0;
  for (float v : dstv) dev = std::max(dev, double(std::fabs(v - 1.f)));
  CHECK(dev < 2e-4);
  arena.reset();
  applyMixture(K, arena, src, dst, m);
  double a = 0, b = 0;
  for (std::size_t i = 0; i < img.size(); ++i) {
    a += img[i];
    b += dstv[i];
  }
  std::printf("  mixture energy: in %.3f out %.3f (rel %.2e)\n", a, b, (b - a) / a);
  CHECK(std::fabs(b - a) / a < 2e-3);
  // odd sizes and tiny images do not crash and stay normalised
  for (auto wh : {std::pair<int, int>{3, 2}, {1, 9}, {17, 1}, {33, 65}}) {
    std::vector<float> s2(std::size_t(wh.first) * wh.second, 0.5f), d2(s2.size());
    Plane a2{s2.data(), wh.first, wh.second, wh.first}, b2{d2.data(), wh.first, wh.second, wh.first};
    arena.reset();
    applyMixture(K, arena, a2, b2, m);
    double dv = 0;
    for (float v : d2) dv = std::max(dv, double(std::fabs(v - 0.5f)));
    CHECK(dv < 2e-4);
  }
}

FilmParams spatialParams() {
  FilmParams p;
  p.camera.autoExposure = false;
  return p;  // everything on, reference defaults
}

double sumWeights(const Mixture& m) {
  double s = m.w0;
  for (const auto& c : m.comps) s += c.w;
  return s;
}

void testSpatialPlans() {
  auto lib = library();
  FilmSimulator sim(lib);
  FilmParams p = spatialParams();
  CHECK(sim.setParams(p));
  const FilmParams& eff = sim.effectiveParams();
  const FilmStock* film = lib->find("kodak_portra_400");
  FilmTables dummy;
  SpatialPlan pl = makeSpatialPlan(*film, eff, 6000, 4000, true, dummy, nullptr);
  CHECK_NEAR(pl.pixelUm, 35000.0 / 6000.0, 1e-9);
  CHECK(pl.haloMix && pl.dirDiffuse && pl.grain.active && pl.glare && !pl.preMix && !pl.boost && !pl.enlargerDiffusion);
  // scatter + halation: energy conserving (renormalised halation), unit total weight per channel
  for (int c = 0; c < 3; ++c) CHECK_NEAR(sumWeights(pl.halo[c]), 1.0, 1e-9);
  for (int c = 0; c < 3; ++c) CHECK_NEAR(sumWeights(pl.dir[c]), 1.0, 1e-9);
  CHECK(pl.dir[0].w0 == 0.0);  // DIR diffusion has no identity part
  // halation: first sigma 65 um / 5.833 um/px = 11.1 px for a strong-antihalation still film; red strength 0.015
  {
    double wBig = 0;
    for (const auto& g : pl.halo[0].comps)
      if (g.sigma > 8.0) wBig += g.w;
    CHECK_NEAR(wBig, 0.015 / 1.015, 2e-3);
  }
  // without renormalisation the pass adds a_tot of the image energy
  FilmParams q = p;
  q.filmRender.halation.halationRenormalize = false;
  q.filmRender.halation.scatterAmount = 0;
  FilmSimulator s2(lib);
  CHECK(s2.setParams(q));
  SpatialPlan p2 = makeSpatialPlan(*film, s2.effectiveParams(), 6000, 4000, true, dummy, nullptr);
  CHECK_NEAR(sumWeights(p2.halo[0]), 1.0 + 0.015, 1e-9);  // digest: strong antihalation still film
  // Diffusion filter: scatter fraction from the strength table, energy conserving, warm halo shifts red outward
  DiffusionFilterParams f;
  f.active = true;
  Mixture m[3];
  CHECK(buildDiffusionMixture(f, 5.833, m));
  for (int c = 0; c < 3; ++c) CHECK_NEAR(sumWeights(m[c]), 1.0, 1e-9);
  CHECK_NEAR(m[0].w0, 1.0 - 0.35 * 0.75, 1e-9);  // strength 0.5 -> 0.35, black pro-mist gain 0.75
  auto meanSigma2 = [](const Mixture& mm) {
    double s = 0, w = 0;
    for (const auto& g : mm.comps) {
      s += g.w * g.sigma * g.sigma;
      w += g.w;
    }
    return s / w;
  };
  CHECK(meanSigma2(m[0]) > meanSigma2(m[2]));  // red scatters wider than blue for the warm halo
  f.strength = 4.0;
  CHECK(buildDiffusionMixture(f, 5.833, m));
  CHECK_NEAR(m[1].w0, 1.0 - 0.75 * 0.75, 1e-9);  // clamps at 2 stops
  f.active = false;
  CHECK(!buildDiffusionMixture(f, 5.833, m));
  // highlight boost: continuous with zero slope at x0, brightest sample boosted by exactly M stops
  {
    const double M = 1.5, A = 0.3, P = 4.0, X = 6.0;
    double x0, a, k;
    CHECK(boostConstants(M, A, P, X, x0, a, k));
    auto f2 = [&](double v) { return v <= x0 ? v : v + X * k * (std::exp(a * (v - x0) / X) - a * (v - x0) / X - 1.0); };
    CHECK_NEAR(f2(X), X * std::exp2(M), 1e-9);
    CHECK_NEAR(f2(x0 + 1e-4) - (x0 + 1e-4), 0.0, 1e-6);
    CHECK(!boostConstants(-1, A, P, X, x0, a, k));
    CHECK(!boostConstants(M, 1.5, P, X, x0, a, k));
  }
  // invalid spatial parameters are rejected
  FilmParams bad = p;
  bad.filmRender.halation.boostEv = -1;
  FilmSimulator s3(lib);
  std::string err;
  CHECK(!s3.setParams(bad, &err));
}

void testGrainStatistics() {
  auto lib = library();
  FilmSimulator sim(lib);
  CHECK(sim.setParams(spatialParams()));
  const FilmStock* film = lib->find("kodak_portra_400");
  FilmTables dummy;
  const SpatialKernels& K = selectSpatialKernels();
  const int N = 1 << 18;
  std::vector<float> d(N), g0(N), g1(N), g2(N);
  for (int px : {4000, 1000}) {  // pixel size 8.75 um and 35 um: particles per pixel differ 16x
    SpatialPlan pl = makeSpatialPlan(*film, sim.effectiveParams(), px, px * 2 / 3, true, dummy, nullptr);
    for (int ch = 0; ch < 3; ++ch) {
      const GrainChannelPlan& gc = pl.grain.ch[ch];
      for (float dens : {0.15f, 0.5f, 0.9f, 1.3f}) {
        std::fill(d.begin(), d.end(), dens);
        const std::uint32_t stream = 0x1234u + std::uint32_t(ch);
        for (int r = 0; r < N / 4096; ++r)
          K.grainRow(gc, d.data() + r * 4096, g0.data() + r * 4096, g1.data(), g2.data(), 4096, 0, r, stream, 1);
        // expectation from the model (spec 03 s5.2): E[G] = sum_l (D_l + dmin_l), Var = sum_l Dmax_l D_l s_l / n_l
        const float v = std::min(dens * gc.invScale, float(gc.tableN));
        const int i = int(v);
        const float fr = v - float(i);
        double mean = 0, var = 0;
        for (int l = 0; l < 3; ++l) {
          const double Dl = gc.inv[i * 4 + l] + fr * (gc.inv[(i + 1) * 4 + l] - gc.inv[i * 4 + l]) + gc.dminL[l];
          const double Dmax = 1.0 / gc.invDmaxL[l];
          const double p = std::clamp(Dl / Dmax, 1e-6, 1.0 - 1e-6);
          const double s = 1.0 - p * gc.u * (1.0 - 1e-6);
          mean += Dl;
          var += Dmax * Dl * s / gc.nL[l];
        }
        double m = 0, m2 = 0;
        for (int k = 0; k < N; ++k) m += g0[k];
        m /= N;
        for (int k = 0; k < N; ++k) m2 += (g0[k] - m) * (g0[k] - m);
        m2 /= N;
        const double se = std::sqrt(var / N);
        if (!(std::fabs(m - mean) < 5 * se + 1e-5) || !(std::fabs(m2 / var - 1.0) < 0.04))
          std::fprintf(stderr, "  grain px %d ch %d D %.2f: mean %.5f (exp %.5f, se %.2g)  var %.4g (exp %.4g)\n", px, ch, dens, m,
                       mean, se, m2, var);
        CHECK(std::fabs(m - mean) < 5 * se + 1e-5);  // mean preserving: no renormalisation needed
        CHECK(std::fabs(m2 / var - 1.0) < 0.04);
      }
    }
  }
  // stateless: same (seed, x, y, layer) -> same value, whatever the block alignment; other row -> different
  const GrainChannelPlan& gc = makeSpatialPlan(*film, sim.effectiveParams(), 4000, 2667, true, dummy, nullptr).grain.ch[1];
  std::vector<float> a(64, 0.6f), b(64), c(64), e(64);
  K.grainRow(gc, a.data(), b.data(), g1.data(), g2.data(), 64, 100, 7, 99, 1);
  K.grainRow(gc, a.data(), c.data(), g1.data(), g2.data(), 64, 100, 7, 99, 1);
  K.grainRow(gc, a.data() + 3, e.data(), g1.data(), g2.data(), 61, 103, 7, 99, 1);  // different alignment, same pixels
  CHECK(b == c);
  bool same = true;
  for (int i = 0; i < 61; ++i) same = same && (e[i] == b[i + 3]);
  CHECK(same);
  K.grainRow(gc, a.data(), c.data(), g1.data(), g2.data(), 64, 100, 8, 99, 1);
  CHECK(b != c);
}

ImageF gradientImage(int W, int H, unsigned seed) {
  ImageF img(W, H);
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> u(0.f, 1.f);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      float* p = img.row(y) + 3 * x;
      const float s = 0.02f + 0.5f * float(x) / W * (1.f + 0.5f * std::sin(y * 0.1f));
      p[0] = s * (1.0f + 0.1f * u(rng));
      p[1] = s * 0.9f;
      p[2] = s * 0.7f + ((x / 40 + y / 40) % 7 == 0 ? 0.6f : 0.f);
    }
  return img;
}

double maxAbsDiff(const ImageF& a, const ImageF& b) {
  double w = 0;
  for (std::size_t i = 0; i < a.data.size(); ++i) w = std::max(w, double(std::fabs(a.data[i] - b.data[i])));
  return w;
}

void testSpatialRender() {
  auto lib = library();
  const ImageF in = gradientImage(300, 211, 4);
  ImageF base, full, again, other, tiled, tiled2;
  FilmSimulator ns(lib);
  CHECK(ns.setParams(noSpatial()));
  CHECK(ns.render(in, base));
  FilmSimulator sim(lib);
  FilmParams p = spatialParams();
  CHECK(sim.setParams(p));
  CHECK(sim.render(in, full));
  CHECK(sim.render(in, again));
  CHECK(maxAbsDiff(full, again) == 0.0);  // deterministic: a re-render is bit-identical
  // another seed changes the pattern, not the look
  FilmParams p1 = p;
  p1.settings.seed = 7;
  FilmSimulator s7(lib);
  CHECK(s7.setParams(p1));
  CHECK(s7.render(in, other));
  CHECK(maxAbsDiff(full, other) > 1e-3);
  double dm = 0;
  for (std::size_t i = 0; i < full.data.size(); ++i) dm += full.data[i] - other.data[i];
  CHECK(std::fabs(dm / full.data.size()) < 2e-3);
  // tile independence: the result does not depend on the tile decomposition
  FilmParams pt = p;
  pt.settings.tileWidth = 64;
  pt.settings.tileHeight = 32;
  CHECK(sim.setParams(pt));
  CHECK(sim.render(in, tiled));
  pt.settings.tileWidth = 100;
  pt.settings.tileHeight = 50;
  CHECK(sim.setParams(pt));
  CHECK(sim.render(in, tiled2));
  CHECK(sim.setParams(p));
  std::printf("  tile independence: max diff %.2e / %.2e\n", maxAbsDiff(full, tiled), maxAbsDiff(full, tiled2));
  CHECK(maxAbsDiff(full, tiled) == 0.0);  // bit-identical (halo recompute, no cross-tile state)
  CHECK(maxAbsDiff(full, tiled2) == 0.0);
  // --no-spatial gating: disabled effects give exactly the fast path
  FilmParams pn = p;
  pn.disableSpatialAndStochastic();
  FilmSimulator sn(lib);
  CHECK(sn.setParams(pn));
  ImageF off;
  CHECK(sn.render(in, off));
  CHECK(maxAbsDiff(off, base) == 0.0);
  // preview mode renders without grain (smooth) and is deterministic
  FilmParams pv = p;
  pv.settings.previewMode = true;
  FilmSimulator spv(lib);
  CHECK(spv.setParams(pv));
  ImageF prev;
  CHECK(spv.render(in, prev));
  CHECK(maxAbsDiff(prev, full) > 1e-3);
  // tile pass plumbing (decode / encode tables, halos) is neutral: glare with zero roughness and blur is the baked mean
  FilmParams pg = noSpatial();
  pg.printRender.glare.active = true;
  pg.printRender.glare.roughness = 0;
  pg.printRender.glare.blur = 0;
  pg.debug.deactivateStochasticEffects = false;
  pg.filmRender.grain.active = false;
  pg.scanner.unsharpMask = {0.0, 0.0};
  FilmSimulator sg(lib);
  CHECK(sg.setParams(pg));
  ImageF viaTiles;
  CHECK(sg.render(in, viaTiles));
  double worst = 0;
  for (int i = 0; i < 300 * 211; i += 11) {
    float o[3];
    sg.renderPixelBaked(&in.data[i * 3], o);
    for (int c = 0; c < 3; ++c) worst = std::max(worst, double(std::fabs(o[c] - viaTiles.data[i * 3 + c])));
  }
  std::printf("  tile pass vs fused kernel (decode/encode round trip): max diff %.2e\n", worst);
  CHECK(worst < 3e-4);
  // unsharp mask (linear light): flat fields unchanged, overshoot on both sides of an edge
  ImageF edge(96, 40);
  for (int y = 0; y < 40; ++y)
    for (int x = 0; x < 96; ++x)
      for (int c = 0; c < 3; ++c) edge.row(y)[3 * x + c] = x < 48 ? 0.05f : 0.5f;
  FilmParams pu = noSpatial();
  FilmSimulator su(lib), su0(lib);
  CHECK(su0.setParams(pu));
  pu.scanner.unsharpMask = {0.7, 0.7};
  pu.debug.deactivateSpatialEffects = false;
  CHECK(su.setParams(pu));
  ImageF eu, e0;
  CHECK(su.render(edge, eu));
  CHECK(su0.render(edge, e0));
  CHECK(std::fabs(eu.row(20)[3 * 10 + 1] - e0.row(20)[3 * 10 + 1]) < 3e-4);
  CHECK(eu.row(20)[3 * 47 + 1] < e0.row(20)[3 * 47 + 1] - 0.005f);  // undershoot on the dark side
  CHECK(eu.row(20)[3 * 48 + 1] > e0.row(20)[3 * 48 + 1] + 0.005f);  // overshoot on the bright side
}

void testGlareAndEnlargerDiffusion() {
  auto lib = library();
  // stochastic glare: the mean of the added stray light equals percent/100 of white (spec 03 s9.3)
  ImageF black(256, 256);  // exposure 0 -> deepest shadow: the glare dominates the print
  FilmParams pg = noSpatial();
  pg.printRender.glare.active = true;
  pg.debug.deactivateStochasticEffects = false;
  pg.debug.deactivateSpatialEffects = false;  // keep the glare blur
  pg.filmRender.grain.active = false;
  pg.filmRender.halation.active = false;
  pg.filmRender.dirCouplers.diffusionSizeUm = 0;
  pg.printRender.glare.blur = 0.5;
  pg.scanner.unsharpMask = {0.0, 0.0};
  FilmSimulator sg(lib);
  CHECK(sg.setParams(pg));
  ImageF og, obase;
  CHECK(sg.render(black, og));
  FilmParams pb = pg;
  pb.printRender.glare.roughness = 0;  // constant field = the baked mean
  pb.printRender.glare.blur = 0;
  FilmSimulator sb(lib);
  CHECK(sb.setParams(pb));
  CHECK(sb.render(black, obase));
  auto lin = [](float e) { return e <= 0.04045f ? e / 12.92f : std::pow((e + 0.055f) / 1.055f, 2.4f); };
  double mg = 0, mb = 0, vg = 0;
  for (std::size_t i = 1; i < og.data.size(); i += 3) {
    mg += lin(og.data[i]);
    mb += lin(obase.data[i]);
  }
  mg /= 256 * 256;
  mb /= 256 * 256;
  for (std::size_t i = 1; i < og.data.size(); i += 3) vg += (lin(og.data[i]) - mg) * (lin(og.data[i]) - mg);
  vg = std::sqrt(vg / (256 * 256));
  std::printf("  glare: mean linear %.3e vs constant field %.3e, std %.3e (roughness 0.7 field, 0.5 px blur)\n", mg, mb, vg);
  CHECK(std::fabs(mg - mb) / mb < 0.03);
  CHECK(vg > 0.8e-4 && vg < 1.8e-4);  // 0.7 * 3e-4 = 2.1e-4 before the 0.5 px blur (area factor ~0.56)
  // enlarger diffusion: constant exposure is unchanged by an energy-conserving filter, through the split print path
  ImageF flat(64, 48);
  for (float& v : flat.data) v = 0.184f;
  FilmParams pe = noSpatial();
  FilmSimulator s0(lib), s1(lib);
  CHECK(s0.setParams(pe));
  pe.camera.filmFormatMm = 4;  // 31 um per pixel: the filter halos span several pixels
  pe.enlarger.diffusionFilter.active = true;
  pe.debug.deactivateSpatialEffects = false;
  CHECK(s1.setParams(pe));
  ImageF a, b;
  CHECK(s0.render(flat, a));
  CHECK(s1.render(flat, b));
  std::printf("  enlarger diffusion on a flat field (split LUT path vs fused LUT): max diff %.2e\n", maxAbsDiff(a, b));
  CHECK(maxAbsDiff(a, b) < 2e-3);
  // an edge gets a halo: the dark side next to a bright area lightens
  ImageF edge(128, 64);
  for (int y = 0; y < 64; ++y)
    for (int x = 0; x < 128; ++x)
      for (int c = 0; c < 3; ++c) edge.row(y)[3 * x + c] = x < 64 ? 0.02f : 0.9f;
  ImageF e0, e1;
  CHECK(s0.render(edge, e0));
  CHECK(s1.render(edge, e1));
  CHECK(e1.row(30)[3 * 60 + 1] > e0.row(30)[3 * 60 + 1] + 0.003f);
  // camera diffusion filter + lens blur through the exposure stage
  FilmParams pc = noSpatial();
  pc.debug.deactivateSpatialEffects = false;
  pc.camera.diffusionFilter.active = true;
  pc.camera.lensBlurUm = 20;
  pc.camera.filmFormatMm = 4;
  FilmSimulator sc(lib);
  CHECK(sc.setParams(pc));
  ImageF c1;
  CHECK(sc.render(edge, c1));
  CHECK(c1.row(30)[3 * 60 + 1] > e0.row(30)[3 * 60 + 1] + 0.003f);
  CHECK(std::isfinite(c1.row(30)[0]));
}

void testSpatialExtras() {
  auto lib = library();
  // half-resolution mixture path (DIR inhibitor field): smooth field, widths >= 3.2 px
  {
    const SpatialKernels& K = selectSpatialKernels();
    Arena arena;
    const int W = 301, H = 233;
    std::vector<float> img(std::size_t(W) * H);
    {
      const auto rough = testImage(W, H, 11);
      const auto smooth = bruteGauss(rough, W, H, 3.0);  // photographic: no content above the half-resolution Nyquist
      for (std::size_t i = 0; i < img.size(); ++i) img[i] = float(smooth[i]);
    }
    const int hw = (W + 1) / 2, hh = (H + 1) / 2;
    std::vector<float> half(std::size_t(hw) * hh), out(img.size());
    for (int j = 0; j < hh; ++j)
      for (int i = 0; i < hw; ++i) {
        auto at = [&](int x, int y) { return img[std::size_t(std::min(y, H - 1)) * W + std::min(x, W - 1)]; };
        half[std::size_t(j) * hw + i] = 0.25f * (at(2 * i, 2 * j) + at(2 * i + 1, 2 * j) + at(2 * i, 2 * j + 1) + at(2 * i + 1, 2 * j + 1));
      }
    Mixture m;
    m.w0 = 0;
    m.comps = {{4.5, 0.3}, {14.0, 0.5}, {40.0, 0.2}};
    CHECK(mixtureFitsHalf(m));
    Plane ph{half.data(), hw, hh, hw}, po{out.data(), W, H, W};
    applyMixtureHalf(K, arena, ph, po, m);
    std::vector<double> ref(img.size(), 0.0);
    for (const auto& c : m.comps) {
      const auto g = bruteGauss(img, W, H, c.sigma);
      for (std::size_t i = 0; i < ref.size(); ++i) ref[i] += c.w * g[i];
    }
    double worst = 0, range = 0, mean = 0;
    for (std::size_t i = 0; i < ref.size(); ++i) {
      worst = std::max(worst, std::fabs(ref[i] - out[i]));
      mean += std::fabs(ref[i] - out[i]);
      range = std::max(range, ref[i]);
    }
    std::printf("  half-resolution mixture: max err %.2e, mean err %.2e (field max %.2f)\n", worst, mean / ref.size(), range);
    CHECK(worst / range < 6e-3);
    Mixture bad = m;
    bad.comps.push_back({1.0, 0.1});
    CHECK(!mixtureFitsHalf(bad));
  }
  // highlight boost: not a spatial filter (kept by the no-spatial switches); it feeds brighter highlights into the halation
  {
    ImageF img(64, 48);
    for (float& v : img.data) v = 0.03f;
    for (int y = 19; y < 22; ++y)
      for (int x = 29; x < 32; ++x)
        for (int c = 0; c < 3; ++c) img.row(y)[3 * x + c] = 40.0f;
    FilmParams p0 = noSpatial("kodak_gold_200"), p1 = noSpatial("kodak_gold_200");  // weak antihalation layer: visible halo
    for (FilmParams* p : {&p0, &p1}) {
      p->camera.filmFormatMm = 2.0;  // 31 um per pixel
      p->debug.deactivateSpatialEffects = false;
      p->filmRender.dirCouplers.diffusionSizeUm = 0;
      p->scanner.unsharpMask = {0, 0};
    }
    p1.filmRender.halation.boostEv = 2.0;
    FilmSimulator s0(lib), s1(lib);
    CHECK(s0.setParams(p0));
    CHECK(s1.setParams(p1));
    ImageF a, b;
    CHECK(s0.render(img, a));
    CHECK(s1.render(img, b));
    std::printf("  boost: halo pixel 4 px from a highlight %.4f -> %.4f with +2 EV\n", a.row(20)[3 * 35], b.row(20)[3 * 35]);
    CHECK(b.row(20)[3 * 35] > a.row(20)[3 * 35] + 0.003f);
    CHECK(std::fabs(b.row(2)[3 * 5 + 1] - a.row(2)[3 * 5 + 1]) < 1e-3f);  // far away: untouched
  }
  // single-layer grain path (sublayers_active = false) and grain on a positive (reversal) stock
  {
    ImageF img(160, 120);
    for (float& v : img.data) v = 0.184f;
    for (const char* film : {"kodak_portra_400", "kodak_ektachrome_100"}) {
      for (bool sub : {false, true}) {
        FilmParams pg = noSpatial(film, film == std::string("kodak_ektachrome_100") ? "none" : "kodak_portra_endura");
        pg.camera.filmFormatMm = 1.0;  // 6.25 um per pixel
        pg.debug.deactivateStochasticEffects = false;
        pg.debug.deactivateSpatialEffects = false;
        pg.filmRender.halation.active = false;
        pg.filmRender.dirCouplers.diffusionSizeUm = 0;
        pg.scanner.unsharpMask = {0, 0};
        pg.filmRender.glare.active = false;
        pg.printRender.glare.active = false;
        pg.filmRender.grain.sublayersActive = sub;
        FilmParams pb = pg;
        pb.filmRender.grain.active = false;
        FilmSimulator sg(lib), sb(lib);
        CHECK(sg.setParams(pg));
        CHECK(sb.setParams(pb));
        ImageF g, b;
        CHECK(sg.render(img, g));
        CHECK(sb.render(img, b));
        double mg = 0, mb = 0, var = 0;
        const std::size_t n = std::size_t(160) * 120;
        for (std::size_t i = 0; i < n; ++i) {
          mg += g.data[3 * i + 1];
          mb += b.data[3 * i + 1];
        }
        mg /= n;
        mb /= n;
        for (std::size_t i = 0; i < n; ++i) var += (g.data[3 * i + 1] - mg) * (g.data[3 * i + 1] - mg);
        var = std::sqrt(var / n);
        std::printf("  grain %-22s sublayers %d: mean %.4f vs %.4f (no grain), rms %.4f\n", film, int(sub), mg, mb, var);
        CHECK(std::fabs(mg - mb) < 0.01);
        CHECK(var > 0.003 && var < 0.06);
      }
    }
  }
  // micro-structure only exists at sub-0.6 um pixels (heavy upscaling); the plan switches it on there and not at normal sizes
  {
    FilmSimulator sim(lib);
    CHECK(sim.setParams(spatialParams()));
    FilmTables dummy;
    const FilmStock* film = lib->find("kodak_portra_400");
    CHECK(!makeSpatialPlan(*film, sim.effectiveParams(), 6000, 4000, true, dummy, nullptr).grain.microActive);
    SpatialPlan big = makeSpatialPlan(*film, sim.effectiveParams(), 70000, 46667, true, dummy, nullptr);
    CHECK(big.grain.microActive);
    CHECK_NEAR(big.grain.microStd, 0.03 / (35000.0 / 70000.0), 1e-6);
  }
  // seeds do not change statistics: the mean of two seeds' renders of a flat field agrees within sampling error
  {
    ImageF img(128, 128);
    for (float& v : img.data) v = 0.05f;
    FilmParams p = spatialParams();
    p.camera.filmFormatMm = 4;
    double means[2];
    for (int s = 0; s < 2; ++s) {
      p.settings.seed = std::uint32_t(s * 1000 + 3);
      FilmSimulator sim(lib);
      CHECK(sim.setParams(p));
      ImageF o;
      CHECK(sim.render(img, o));
      double m = 0;
      for (std::size_t i = 1; i < o.data.size(); i += 3) m += o.data[i];
      means[s] = m / (128 * 128);
    }
    CHECK_NEAR(means[0], means[1], 3e-3);
  }
}

// ---- integration into the develop pipeline (EditParams film keys, Renderer, C API) ----------------------------------

SourceImage devSource(int w, int h, bool withDisc = false) {
  SourceImage s;
  s.sceneReferred = true;
  s.linear = ImageF(w, h);
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> n(-0.01f, 0.01f);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const float u = float(x) / w, v = float(y) / h;
      float* p = s.linear.row(y) + 3 * x;
      p[0] = 0.02f + 0.9f * u * u + n(rng);
      p[1] = 0.02f + 0.5f * v + n(rng);
      p[2] = 0.02f + 0.4f * (1 - u) * v + n(rng);
      if (withDisc) {
        const float dx = x - w * 0.5f, dy = y - h * 0.5f;
        const float r = std::sqrt(dx * dx + dy * dy);
        p[0] = p[1] = p[2] = r < w * 0.02f ? 40.f : 0.04f;
      }
    }
  return s;
}

bool sameBits(const ImageF& a, const ImageF& b) {
  return a.width == b.width && a.height == b.height &&
         std::memcmp(a.data.data(), b.data.data(), a.data.size() * sizeof(float)) == 0;
}

double meanAbsDiff(const ImageF& a, const ImageF& b) {
  double s = 0;
  for (std::size_t i = 0; i < a.data.size(); ++i) s += std::fabs(a.data[i] - b.data[i]);
  return s / double(a.data.size());
}

double channelMean(const ImageF& a, int c) {
  double s = 0;
  for (std::size_t i = std::size_t(c); i < a.data.size(); i += 3) s += a.data[i];
  return s / double(a.data.size() / 3);
}

EditParams filmEdit(const char* stock = "kodak_portra_400") {
  EditParams p;
  p.filmStock = stock;
  p.sharpen = 0.f;  // the detail defaults would add their own spatial work to every comparison
  p.nrColor = 0.f;
  return p;
}

void testEditParamsFilmJson() {
  // defaults: film off, nothing about film in the diff JSON
  EditParams def;
  CHECK(def.filmStock.empty() && def.filmPaper.empty() && def.filmStrength == 100.f && def.filmGrain == 100.f);
  CHECK(toJsonDiff(def).find("film") == std::string::npos);

  // round trip of every film key
  EditParams p;
  p.filmStock = "fujifilm_pro_400h";
  p.filmPaper = "none";
  p.filmStrength = 62.f;
  p.filmExposure = -1.5f;
  p.filmWarmth = 35.f;
  p.filmTint = -20.f;
  p.filmGrain = 40.f;
  p.filmHalation = 75.f;
  for (const std::string& json : {toJson(p), toJsonDiff(p)}) {
    auto q = fromJson(json);
    CHECK(q.has_value());
    if (!q) continue;
    CHECK(q->filmStock == p.filmStock && q->filmPaper == p.filmPaper);
    CHECK_NEAR(q->filmStrength, 62.0, 1e-4);
    CHECK_NEAR(q->filmExposure, -1.5, 1e-4);
    CHECK_NEAR(q->filmWarmth, 35.0, 1e-4);
    CHECK_NEAR(q->filmTint, -20.0, 1e-4);
    CHECK_NEAR(q->filmGrain, 40.0, 1e-4);
    CHECK_NEAR(q->filmHalation, 75.0, 1e-4);
    CHECK(toJson(*q) == toJson(p));
  }
  // contract units: film_exp = EV * 20
  auto e = fromJson(R"({"film":"kodak_gold_200","film_exp":40,"film_paper":"kodak_supra_endura"})");
  CHECK(e.has_value() && e->filmExposure == 2.f && e->filmPaper == "kodak_supra_endura");
  // clamping, junk, "none" as a stock means off
  auto c = fromJson(R"({"film":"x","film_strength":500,"film_exp":-999,"film_warmth":1e9,"film_grain":-5})");
  CHECK(c.has_value());
  if (c) {
    EditParams cc = *c;
    cc.clamp();
    CHECK(cc.filmStrength == 100.f && cc.filmExposure == -3.f && cc.filmWarmth == 100.f && cc.filmGrain == 0.f);
  }
  auto none = fromJson(R"({"film":"none"})");
  CHECK(none.has_value() && none->filmStock.empty());
  // combine rules: ids from the delta win when set, sliders add (exposure, warmth, tint) or replace (strength, grain, halation)
  EditParams base, delta;
  base.filmStock = "kodak_portra_400";
  base.filmWarmth = 10.f;
  base.filmExposure = 0.5f;
  delta.filmWarmth = 15.f;
  delta.filmExposure = 0.25f;
  delta.filmStrength = 70.f;
  base += delta;
  CHECK(base.filmStock == "kodak_portra_400");  // empty delta id keeps it
  CHECK_NEAR(base.filmWarmth, 25.0, 1e-5);
  CHECK_NEAR(base.filmExposure, 0.75, 1e-5);
  CHECK_NEAR(base.filmStrength, 70.0, 1e-5);
  EditParams d2;
  d2.filmStock = "kodak_ektar_100";
  d2.filmPaper = "none";
  base += d2;
  CHECK(base.filmStock == "kodak_ektar_100" && base.filmPaper == "none" && base.filmStrength == 70.f);
  // schema lists the new keys
  const std::string schema = paramsSchemaJson();
  for (const char* k : {"film_strength", "film_exp", "film_warmth", "film_tint", "film_grain", "film_halation"})
    CHECK(schema.find(std::string("\"") + k + "\"") != std::string::npos);
}

void testFilmMatch() {
  auto lib = library();
  CHECK(lib != nullptr);
  if (!lib) return;
  const CameraRegistry cams = CameraRegistry::withBuiltins();
  const CameraProfile& cam = cams.resolve("Sony", "ILCE-7RM5");
  const SourceImage src = devSource(192, 128);
  // a dim frame (the develop render of src is bright, 85 L*; the paper white of the print is ~87 L*, so a brighter target is out of reach)
  film::setLibrary(lib);
  SourceImage bright = src;
  for (float& v : bright.linear.data) v *= 0.3f;
  EditParams p;
  p.filmStock = "kodak_portra_400";
  EditParams plain = p;
  plain.filmStock.clear();
  CHECK(filmMatchKey(plain).empty());
  CHECK(!filmMatchKey(p).empty());
  EditParams p2 = p;  // the user's offset, strength, grain and halation are not part of the key
  p2.filmExposure = 1.f;
  p2.filmStrength = 40.f;
  p2.filmGrain = 10.f;
  p2.filmHalation = 0.f;
  CHECK(filmMatchKey(p) == filmMatchKey(p2));
  EditParams p3 = p;
  p3.contrast = 20.f;
  CHECK(filmMatchKey(p) != filmMatchKey(p3));
  FilmMatchOptions full;  // v2: 100 % of the match on the film EV, no latitude window, no print exposure
  full.mode = FilmMatchMode::Full;
  full.pull = 1.f;
  full.latitude = false;
  FilmMatchOptions pull;  // v3: 70 % pull + latitude window
  pull.mode = FilmMatchMode::Pull;
  for (const SourceImage* s : {&src, static_cast<const SourceImage*>(&bright)}) {
    FilmMatchInfo mi;
    const FilmExposure fe = matchFilmExposure(p, cam, nullptr, *s, &mi, &full);
    const float ev = fe.filmEv;
    const float normal = medianLightness(Renderer(plain, cam, nullptr).renderFloat(*s, false));
    EditParams m = p;
    m.filmKeyEv = ev;
    const float film = medianLightness(Renderer(m, cam, nullptr).renderFloat(*s, false));
    const float film0 = medianLightness(Renderer(p, cam, nullptr).renderFloat(*s, false));
    std::printf("  film match (full, v2): ev %+.2f  L* develop %.1f  film unmatched %.1f  matched %.1f  (%d evals)\n", ev, normal, film0, film, mi.evals);
    CHECK(std::fabs(film - normal) < 2.0f);
    CHECK(std::fabs(film - normal) <= std::fabs(film0 - normal) + 0.01f);
    CHECK(std::fabs(ev) <= 4.0f && fe.printEv == 0.f);
    CHECK(mi.ev == ev && mi.evMatch == ev && mi.bound == FilmBound::None);

    FilmMatchInfo md;
    const FilmExposure fd = matchFilmExposure(p, cam, nullptr, *s, &md, &pull);
    CHECK(std::fabs(md.evMatch - ev) < 1e-5f && fd.printEv == 0.f);
    CHECK_NEAR(md.evKey, kFilmKeyPull * md.evMatch, 1e-5);
    CHECK(md.latitudeOk);
    if (md.bound == FilmBound::None) CHECK(fd.filmEv == md.evKey);
    CHECK(fd.filmEv >= std::min(md.evLo, md.evHi) - 1e-5f && fd.filmEv <= std::max(md.evLo, md.evHi) + 1e-5f);
  }

  // ---- split model (default): A film exposure from the latitude, B print exposure to the key, C paper to screen ----
  FilmMatchOptions split;  // v4: A (latitude window) and B (print solved to the key) per image and stock
  split.mode = FilmMatchMode::Window;
  CHECK(filmPaperMappingEnabled());
  {
    // A does not look at the digital render's brightness: the same scene with a different develop tone gives the same film EV
    EditParams q = p;
    q.contrast = 45.f;
    FilmMatchInfo m0, m1;
    const FilmExposure e0 = matchFilmExposure(p, cam, nullptr, src, &m0, &split);
    const FilmExposure e1 = matchFilmExposure(q, cam, nullptr, src, &m1, &split);
    std::printf("  split A: film EV %+.3f / %+.3f (develop L* %.1f / %.1f)  print EV %+.2f / %+.2f\n", e0.filmEv, e1.filmEv, m0.targetL, m1.targetL, e0.printEv, e1.printEv);
    CHECK(std::fabs(m0.targetL - m1.targetL) > 1.0f);  // the digital key does differ
    CHECK(e0.filmEv == e1.filmEv);
    // it follows the shadows: the dim scene is exposed more, within the stock's window
    FilmMatchInfo md;
    const FilmExposure ed = matchFilmExposure(p, cam, nullptr, bright, &md, &split);
    CHECK(ed.filmEv > e0.filmEv + 0.5f);
    CHECK(md.latitudeOk && ed.filmEv >= md.evLo - 1e-5f);
    CHECK_NEAR(ed.filmEv, md.evLo + kFilmNegativeBiasEv, 1e-4);
    // B hits the scene's key within the tolerance when it is inside the paper's range, and the render shows it
    for (const auto& pr : {std::make_pair(&m0, 0), std::make_pair(&md, 1)}) {
      const FilmMatchInfo& mi = *pr.first;
      CHECK(mi.printBound == PrintBound::None);
      CHECK(mi.targetL >= mi.paperLoL && mi.targetL <= mi.paperHiL);
      CHECK(std::fabs(mi.printL - mi.keyL) < 1.0f);
    }
    {
      EditParams m = p;
      m.filmKeyEv = ed.filmEv;
      m.filmPrintEv = ed.printEv;
      const float L = medianLightness(Renderer(m, cam, nullptr).renderFloat(bright, false));
      std::printf("  dim scene, portra 400: film EV %+.2f print EV %+.2f -> median L* %.1f (develop %.1f)\n", ed.filmEv, ed.printEv, L, md.targetL);
      CHECK(std::fabs(L - md.targetL) < 1.5f);
      EditParams u = p;  // brighter than the unexposed film
      CHECK(L > medianLightness(Renderer(u, cam, nullptr).renderFloat(bright, false)) + 1.f);
    }
  }
  // B on a synthetic print response: inside the range it hits the target, outside it clamps at the paper bounds
  {
    FilmPrintRange rg;
    rg.ok = true;
    rg.loL = 10.f;
    rg.hiL = 90.f;
    auto resp = [](float ev) { return 50.f + 40.f * std::tanh(0.6f * ev); };  // a toe / shoulder like paper response
    FilmMatchInfo mi;
    float ev = solvePrintEv(resp, 70.f, rg, &mi);
    CHECK(std::fabs(resp(ev) - 70.f) < 0.5f && mi.printBound == PrintBound::None && mi.keyL == 70.f);
    ev = solvePrintEv(resp, 120.f, rg, &mi);  // brighter than the paper white: the key is lowered to the white limit
    CHECK(mi.printBound == PrintBound::PaperWhite && mi.keyL == 90.f && std::fabs(resp(ev) - 90.f) < 1.0f);
    ev = solvePrintEv(resp, -5.f, rg, &mi);
    CHECK(mi.printBound == PrintBound::PaperBlack && mi.keyL == 10.f && std::fabs(resp(ev) - 10.f) < 1.0f);
    rg.maxPrintEv = 0.3f;  // no print: the lamp may not lift the clear film above display white
    ev = solvePrintEv(resp, 80.f, rg, &mi);
    CHECK(ev <= 0.3f + 1e-4f && mi.printBound == PrintBound::PaperWhite);
  }
  // reversal film: the film EV puts the unclipped highlight at the shoulder margin
  for (const char* id : {"kodak_kodachrome_64", "fujifilm_velvia_100"}) {
    const film::FilmStock* st = lib->find(id);
    CHECK(st != nullptr);
    if (!st) continue;
    const FilmLatitude lat = filmLatitude(*st);
    CHECK(lat.ok && lat.reversal);
    EditParams rp;
    rp.filmStock = id;
    FilmMatchInfo mi;
    const FilmExposure fe = matchFilmExposure(rp, cam, nullptr, src, &mi, &split);
    const float logH = std::log10(mi.p995 / 0.184f) + fe.filmEv * 0.30103f;
    std::printf("  %s: film EV %+.2f print EV %+.2f  window [%+.2f, %+.2f]  p99.5 logH %+.3f  shoulder %+.3f  key L* %.1f -> %.1f\n", id, fe.filmEv, fe.printEv, mi.evLo, mi.evHi, logH, lat.shoulder, mi.keyL, mi.printL);
    CHECK(logH <= lat.shoulder + 1e-3f);
    CHECK_NEAR(logH, lat.shoulder - kFilmShoulderMarginReversalLogH, 1e-3);
    CHECK(fe.filmEv == mi.evHi);
    CHECK(fe.printEv <= 0.3f);  // the lamp does not lift the clear film above display white
  }
  // C: paper to screen. White and black of every print paper and of a slide, as displayed.
  {
    std::printf("  paper | white L* | black L* | white Y | black Y | contrast | usable L* | sRGB range covered\n");
    for (const std::string& name : lib->names()) {
      const film::FilmStock* pp = lib->find(name);
      if (!pp || !pp->isPrintMedium() || pp->isPaper() == false) continue;
      const FilmPrintRange r = filmPrintRange(lib, "kodak_portra_400", name);
      CHECK(r.ok);
      std::printf("  %-34s %6.1f %6.1f %7.4f %8.5f %8.0f:1 %5.1f..%5.1f %5.1f %%\n", name.c_str(), r.lWhite, r.lBlack, r.yWhite, r.yBlack,
                  r.yBlack > 0 ? r.yWhite / r.yBlack : 0.f, r.loL, r.hiL, r.lWhite - r.lBlack);
      CHECK(r.lWhite > 96.f && r.lBlack < 8.f && r.loL < r.hiL);  // white maps to display white, the black keeps its true luminance
    }
    for (const char* id : {"kodak_kodachrome_64", "fujifilm_velvia_100"}) {
      const FilmPrintRange r = filmPrintRange(lib, id, "none");
      CHECK(r.ok && r.lWhite > 96.f && r.lBlack < 8.f);
      CHECK(r.maxPrintEv > -0.5f && r.maxPrintEv < 0.5f);
    }
    // the mapping is the old stage when switched off
    const FilmPrintRange on = filmPrintRange(lib, "kodak_portra_400", "kodak_portra_endura");
#ifdef _WIN32
    _putenv_s("DAPPLE_FILM_PAPER_MAP_OFF", "1");
#else
    setenv("DAPPLE_FILM_PAPER_MAP_OFF", "1", 1);
#endif
    CHECK(!filmPaperMappingEnabled());
    const FilmPrintRange off = filmPrintRange(lib, "kodak_portra_400", "kodak_portra_endura");
    std::printf("  portra endura, mapping off: white L* %.1f black L* %.1f\n", off.lWhite, off.lBlack);
    CHECK(off.lWhite < on.lWhite - 4.f && off.lWhite > 80.f);
#ifdef _WIN32
    _putenv_s("DAPPLE_FILM_PAPER_MAP_OFF", "");
#else
    unsetenv("DAPPLE_FILM_PAPER_MAP_OFF");
#endif
    CHECK(filmPaperMappingEnabled());
  }
  // ---- v5 (default): one exposure decision per image shared by all stocks, a per stock + paper print constant, the paper mapping ----
  {
    struct Stat {
      float median = 0, black = 0, white = 0;
    };
    auto srgbToLinear = [](float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
    auto lstats = [&](const EditParams& e, const SourceImage& s) {
      const ImageF img = Renderer(e, cam, nullptr).renderFloat(s, false);
      std::vector<float> ls;
      ls.reserve(std::size_t(img.width) * img.height);
      std::size_t nb = 0, nw = 0;
      for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x) {
          const float* r = img.row(y) + 3 * x;
          const float Y = std::clamp(0.2126f * srgbToLinear(r[0]) + 0.7152f * srgbToLinear(r[1]) + 0.0722f * srgbToLinear(r[2]), 0.f, 1.f);
          const float L = Y > 0.008856f ? 116.f * std::cbrt(Y) - 16.f : 903.3f * Y;
          ls.push_back(L);
          nb += L < 3.f;
          nw += L > 97.f;
        }
      std::nth_element(ls.begin(), ls.begin() + ls.size() / 2, ls.end());
      Stat st;
      st.median = ls[ls.size() / 2];
      st.black = float(nb) / float(ls.size());
      st.white = float(nw) / float(ls.size());
      return st;
    };
    auto withExposure = [&](EditParams e, const FilmExposure& fe) {
      e.filmKeyEv = fe.filmEv;
      e.filmPrintEv = fe.printEv;
      return e;
    };
    // stage 1 and the convention are identical for all stocks of one image; the print constant does not depend on the image
    FilmMatchInfo mn1, mn2, mr1, mr2, dim1;
    EditParams pn = p, pr = p;
    pr.filmStock = "kodak_kodachrome_64";
    const FilmExposure en1 = matchFilmExposure(pn, cam, nullptr, src, &mn1);
    pn.filmStock = "kodak_ektar_100";
    const FilmExposure en2 = matchFilmExposure(pn, cam, nullptr, src, &mn2);
    const FilmExposure er1 = matchFilmExposure(pr, cam, nullptr, src, &mr1);
    pr.filmStock = "fujifilm_velvia_100";
    const FilmExposure er2 = matchFilmExposure(pr, cam, nullptr, src, &mr2);
    std::printf("  v5 scene EV %+.3f  meter %+.3f  film EV shared: N400C %+.2f N100 %+.2f R64 %+.2f R100B %+.2f\n", mn1.evScene, mn1.evMeter, mn1.sharedEv, mn2.sharedEv, mr1.sharedEv, mr2.sharedEv);
    CHECK(mn1.evScene == mn2.evScene && mn1.evScene == mr1.evScene && mn1.evScene == mr2.evScene);
    CHECK(mn1.evMeter == mr1.evMeter && mn1.evMeter == mr2.evMeter);
    CHECK(mn1.sharedEv == mn2.sharedEv);  // same type convention
    CHECK_NEAR(mn1.sharedEv - mr1.sharedEv, kFilmNegativeBiasEv - kFilmReversalBiasEv, 1e-5);
    const FilmExposure ed1 = matchFilmExposure(p, cam, nullptr, bright, &dim1);
    FilmMatchInfo srcP;
    const FilmExposure es = matchFilmExposure(p, cam, nullptr, src, &srcP);
    std::printf("  v5 portra 400 print const: bright scene %+.3f (delta %+.2f), dim scene %+.3f (delta %+.2f)\n", srcP.printConst, srcP.printDelta, dim1.printConst, dim1.printDelta);
    if (srcP.printDelta == 0.f && dim1.printDelta == 0.f) CHECK(srcP.printConst == dim1.printConst);
    CHECK(srcP.printConst == filmPrintConstantAt(lib, "kodak_portra_400", film::defaultPaperFor(*lib, "kodak_portra_400"), srcP.printDelta));
    CHECK(dim1.evScene < srcP.evScene - 0.3f);  // the dim scene gets less compensation than the bright one
    (void)en1; (void)en2; (void)er1; (void)er2; (void)ed1; (void)es;
    // EV_scene formula: 50 -> 0, clamps
    CHECK_NEAR(sceneKeyEv(50.f), 0.0, 0.02);
    CHECK(sceneKeyEv(2.f) == kFilmSceneEvMin && sceneKeyEv(100.f) <= kFilmSceneEvMax);
    CHECK(sceneKeyEv(70.f) > sceneKeyEv(60.f));
    // the print constant puts the grey card (box speed + convention) at L* 50 relative to the paper white
    for (const char* id : {"kodak_portra_400", "kodak_vision3_250d"}) {
      const std::string paper = film::defaultPaperFor(*lib, id);
      const float pc = filmPrintConstant(lib, id, paper);
      FilmParams fpp;
      fpp.film = id;
      fpp.paper = paper;
      fpp.camera.autoExposure = false;
      applyFilmPaperMapping(fpp);
      fpp.disableSpatialAndStochastic();
      fpp.enlarger.printExposure = std::exp2(-double(pc));
      FilmSimulator sim(lib);
      CHECK(sim.setParams(fpp));
      ImageF in(8, 8), out;
      for (float& x : in.data) x = 0.184f;
      const float g = std::exp2(lib->find(id)->positive() ? kFilmReversalBiasEv : kFilmNegativeBiasEv);
      const float g3[3] = {g, g, g};
      CHECK(sim.render(in, out, nullptr, g3));
      const FilmPrintRange rg = filmPrintRange(lib, id, paper);
      const float want = 116.f * std::cbrt(0.184f * rg.yWhite) - 16.f;
      std::printf("  grey card on %s: print constant %+.3f, L* %.2f (wanted %.2f)\n", id, pc, medianLightness(out), want);
      CHECK_NEAR(medianLightness(out), want, 0.6);
    }
    // per-stock rescue: a range that fits keeps the shared exposure, a narrow stock is moved just enough
    {
      FilmMatchInfo mw;
      EditParams rw = p;
      rw.filmStock = "kodak_kodachrome_64";
      SourceImage wide = src;  // 14 stops
      for (int y = 0; y < wide.linear.height; ++y)
        for (int x = 0; x < wide.linear.width; ++x) {
          float* q = wide.linear.row(y) + 3 * x;
          q[0] = q[1] = q[2] = 0.184f * std::exp2(-7.f + 14.f * float(x) / float(wide.linear.width - 1));
        }
      const FilmExposure ew = matchFilmExposure(rw, cam, nullptr, wide, &mw);
      std::printf("  R64 on a 14 stop ramp: shared EV %+.2f final %+.2f  in-range %.0f%% -> %.0f%% (below %.0f%% above %.0f%%)\n", mw.sharedEv, ew.filmEv, 100 * mw.fracShared, 100 * mw.fracFinal, 100 * mw.fracBelowToe, 100 * mw.fracAboveShoulder);
      CHECK(mw.fracFinal >= mw.fracShared - 1e-6f);
      CHECK(mw.fracFinal < kFilmRangeFraction);  // a slide cannot hold 14 stops: it loses both ends
      CHECK(mw.fracBelowToe + mw.fracAboveShoulder > 0.3f);
      CHECK(ew.printEv == mw.printConst);  // reversal: no print compensation
      FilmMatchInfo mf;
      EditParams rn = p;
      const FilmExposure efit = matchFilmExposure(rn, cam, nullptr, src, &mf);
      CHECK(mf.fracShared >= kFilmRangeFraction ? efit.filmEv == mf.sharedEv : mf.fracFinal > mf.fracShared);
      // the same ramp: the slide loses shadows and highlights on screen, the negative keeps (almost all of) them
      EditParams nw = p;
      FilmMatchInfo mnw;
      const FilmExposure enw = matchFilmExposure(nw, cam, nullptr, wide, &mnw);
      const Stat sr = lstats(withExposure(rw, ew), wide), sn = lstats(withExposure(nw, enw), wide);
      std::printf("  14 stop ramp on screen: R64 black %.1f%% white %.1f%% (median %.1f)   N400C black %.1f%% white %.1f%% (median %.1f)\n", 100 * sr.black, 100 * sr.white, sr.median, 100 * sn.black, 100 * sn.white, sn.median);
      CHECK(sr.black > 0.03f && sr.white > 0.03f);
      CHECK(sn.black < sr.black && sn.black + sn.white < sr.black + sr.white);
    }
    // overrides: film_shoot, film_print, film_black, film_latitude
    {
      auto at = [&](const char* id, float shoot, float print, float black, float lat, const SourceImage& sc, FilmMatchInfo* inf = nullptr) {
        EditParams e = p;
        e.filmStock = id;
        e.filmShoot = shoot;
        e.filmPrint = print;
        e.filmBlack = black;
        e.filmLatitude = lat;
        const FilmExposure fe = matchFilmExposure(e, cam, nullptr, sc, inf);
        return std::make_pair(e, fe);
      };
      for (const char* id : {"kodak_portra_400", "kodak_kodachrome_64"}) {
        FilmMatchInfo m0, ml, mh;
        const auto a0 = at(id, 0, 0, 0, 0, src, &m0), alo = at(id, -1, 0, 0, 0, src, &ml), ahi = at(id, 1, 0, 0, 0, src, &mh);
        std::printf("  %s film_shoot -1/0/+1: EV %+.2f %+.2f %+.2f (in-range %.0f%% %.0f%% %.0f%%)  print %+.2f %+.2f %+.2f\n", id, alo.second.filmEv, a0.second.filmEv, ahi.second.filmEv, 100 * ml.fracFinal, 100 * m0.fracFinal, 100 * mh.fracFinal, alo.second.printEv, a0.second.printEv, ahi.second.printEv);
        CHECK(alo.second.filmEv < a0.second.filmEv && a0.second.filmEv < ahi.second.filmEv);
        CHECK(alo.second.filmEv >= a0.second.filmEv - kFilmShootMaxEv - 1e-4f && ahi.second.filmEv <= a0.second.filmEv + kFilmShootMaxEv + 1e-4f);
        if (std::string(id).rfind("kodak_kodachrome", 0) != 0) {  // a negative is printed down / up by the shift: brightness stays near
          const float la = lstats(withExposure(a0.first, a0.second), src).median, lh = lstats(withExposure(ahi.first, ahi.second), src).median;
          std::printf("    negative median L*: auto %.1f, shoot +1 %.1f\n", la, lh);
          CHECK(std::fabs(la - lh) < 12.f);
        }
        // film_print: -1 darker, +1 lighter, within the paper's range
        const auto plo = at(id, 0, -1, 0, 0, src), phi = at(id, 0, 1, 0, 0, src);
        const float lm = lstats(withExposure(a0.first, a0.second), src).median, ll = lstats(withExposure(plo.first, plo.second), src).median,
                    lhh = lstats(withExposure(phi.first, phi.second), src).median;
        std::printf("  %s film_print -1/0/+1: median L* %.1f %.1f %.1f (print EV %+.2f %+.2f %+.2f)\n", id, ll, lm, lhh, plo.second.printEv, a0.second.printEv, phi.second.printEv);
        CHECK(ll < lm - 2.f || lm < 40.f);
        CHECK(lhh > lm + 2.f || lm > 75.f || std::string(id).rfind("kodak_kodachrome", 0) == 0);  // a slide's lamp cannot brighten
        CHECK(lhh >= lm - 0.5f);
        // film_latitude: the rescue target follows
        FilmMatchInfo mlt, mht;
        at(id, 0, 0, 0, -1, src, &mlt);
        at(id, 0, 0, 0, 1, src, &mht);
        CHECK(mht.fracFinal + 1e-4f >= mlt.fracFinal);
      }
      CHECK_NEAR(rangeFractionFor(-1.f), kFilmRangeFractionMin, 1e-6);
      CHECK_NEAR(rangeFractionFor(0.f), kFilmRangeFraction, 1e-6);
      CHECK_NEAR(rangeFractionFor(1.f), kFilmRangeFractionMax, 1e-6);
      // film_black: matte > true > display black on a deep shadow patch
      SourceImage dark = src;
      for (float& v : dark.linear.data) v = 1e-5f;
      float bl[3];
      for (int k = 0; k < 3; ++k) {
        EditParams e = p;
        e.filmBlack = float(k - 1);
        const FilmExposure fe = matchFilmExposure(e, cam, nullptr, src);
        bl[k] = lstats(withExposure(e, fe), dark).median;
      }
      std::printf("  film_black -1/0/+1: black L* %.2f %.2f %.2f\n", bl[0], bl[1], bl[2]);
      CHECK(bl[0] > bl[1] + 5.f && bl[1] > bl[2] + 0.3f);
      // film_wb: the stock's own balance sees the scene's real light
      {
        auto sceneXy = [](double cct, float& x, float& y) {
          double xx, yy;
          cctToXy(cct, xx, yy);
          x = float(xx), y = float(yy);
        };
        // L*a*b* chroma and hue (deg) of the centre of a render of a uniform neutral grey
        auto greyCast = [&](const EditParams& e, double* hue) {
          SourceImage grey = src;
          for (float& v : grey.linear.data) v = 0.184f;
          const ImageF out = Renderer(e, cam, nullptr).renderFloat(grey, false);
          const float* r = out.row(out.height / 2) + 3 * (out.width / 2);
          auto dec = [](float v) { return double(v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f)); };
          const double R = dec(r[0]), G = dec(r[1]), B = dec(r[2]);
          const double X = 0.4124564 * R + 0.3575761 * G + 0.1804375 * B, Y = 0.2126729 * R + 0.7151522 * G + 0.0721750 * B,
                       Z = 0.0193339 * R + 0.1191920 * G + 0.9503041 * B;
          auto f = [](double t) { return t > 0.008856 ? std::cbrt(t) : 7.787 * t + 16.0 / 116.0; };
          const double a = 500.0 * (f(X / 0.95047) - f(Y)), b = 200.0 * (f(Y) - f(Z / 1.08883));
          if (hue) *hue = std::fmod(std::atan2(b, a) * 180.0 / 3.14159265358979 + 360.0, 360.0);
          return std::hypot(a, b);
        };
        auto edit = [&](const char* id, double cct, float wbv) {
          EditParams e = p;
          e.filmStock = id;
          e.filmWb = wbv;
          if (cct > 0) sceneXy(cct, e.filmSceneX, e.filmSceneY);
          return e;
        };
        // film_wb = 0 is the digital white balance: bit-identical to a render that knows nothing of the scene light, for any WB setting
        for (float temp : {0.f, 35.f, -50.f}) {
          EditParams a = edit("kodak_portra_400", 3200, 0.f), b = edit("kodak_portra_400", 0, 0.f);
          a.temperature = b.temperature = temp;
          a.tint = b.tint = temp * 0.3f;
          CHECK(sameBits(Renderer(a, cam, nullptr).renderFloat(src, false), Renderer(b, cam, nullptr).renderFloat(src, false)));
        }
        // an unknown scene light has no effect at any film_wb
        {
          EditParams a = edit("kodak_portra_400", 0, 1.f), b = edit("kodak_portra_400", 0, 0.f);
          CHECK(sameBits(Renderer(a, cam, nullptr).renderFloat(src, false), Renderer(b, cam, nullptr).renderFloat(src, false)));
        }
        double hue = 0;
        const double cDay3200 = greyCast(edit("kodak_portra_400", 3200, 1.f), &hue);
        std::printf("  film_wb 1, daylight stock (Portra 400, D55) under 3200 K: grey C* %.1f hue %.0f\n", cDay3200, hue);
        CHECK(cDay3200 > 15.0 && hue > 40.0 && hue < 95.0);  // orange
        const double cDay7500 = greyCast(edit("kodak_portra_400", 7500, 1.f), &hue);
        std::printf("  ... under 7500 K: C* %.1f hue %.0f\n", cDay7500, hue);
        CHECK(cDay7500 > 5.0 && hue > 235.0 && hue < 290.0);  // blue
        CHECK(greyCast(edit("kodak_portra_400", 5500, 1.f), nullptr) < 3.0);  // the stock's own light: nothing to see
        const double cTung5500 = greyCast(edit("kodak_vision3_500t", 5500, 1.f), &hue);
        std::printf("  film_wb 1, tungsten stock (Vision3 500T) under 5500 K: C* %.1f hue %.0f; under 3200 K C* %.1f\n", cTung5500, hue, greyCast(edit("kodak_vision3_500t", 3200, 1.f), nullptr));
        CHECK(cTung5500 > 15.0 && hue > 235.0 && hue < 290.0);  // blue
        CHECK(greyCast(edit("kodak_vision3_500t", 3200, 1.f), nullptr) < 10.0);
        // the cast grows with film_wb
        double prev = -1;
        for (float w : {0.f, 0.25f, 0.5f, 0.75f, 1.f}) {
          const double cc = greyCast(edit("kodak_portra_400", 3200, w), nullptr);
          CHECK(cc > prev);
          prev = cc;
        }
        // the user's white balance edit re-declares the scene light: at film_wb 1 it changes the render, at 0 it is the plain digital white balance
        {
          EditParams a = edit("kodak_portra_400", 3200, 1.f), b = a;
          b.temperature = 40.f;
          CHECK(!sameBits(Renderer(a, cam, nullptr).renderFloat(src, false), Renderer(b, cam, nullptr).renderFloat(src, false)));
          double x0, y0, d0, x1, y1, d1;
          filmInputAdaptation(a, &x0, &y0, &d0);
          filmInputAdaptation(b, &x1, &y1, &d1);
          CHECK(d0 == 0.0 && d1 == 0.0 && std::fabs(x0 - a.filmSceneX) < 1e-4 && std::fabs(x1 - x0) > 1e-3);  // no edit: the as-shot light itself
        }
      }
      // the corners and edge midpoints of the (film_shoot, film_print) pad are never broken
      const SourceImage* scenes[2] = {&src, &bright};
      int bad = 0, total = 0;
      for (const char* id : {"kodak_portra_400", "kodak_vision3_250d", "fujifilm_velvia_100", "kodak_kodachrome_64"})
        for (const SourceImage* sc : scenes)
          for (int i = 0; i < 9; ++i) {
            const float sh = float(i % 3 - 1), pr2 = float(i / 3 - 1);
            const auto r = at(id, sh, pr2, 0, 0, *sc);
            const Stat st = lstats(withExposure(r.first, r.second), *sc);
            const bool ok = st.median >= 25.f && st.median <= 90.f && st.black < 0.35f && st.white < 0.35f;
            ++total;
            if (!ok) {
              ++bad;
              std::printf("    pad %s scene %d shoot %+.0f print %+.0f: median %.1f black %.1f%% white %.1f%%\n", id, sc == &src ? 0 : 1, sh, pr2, st.median, 100 * st.black, 100 * st.white);
            }
          }
      std::printf("  pad corner / edge check: %d of %d positions broken\n", bad, total);
      CHECK(bad == 0);
      // film_wb pad labels: the scene and the stock's reference colour temperature, and whether the axis does anything
      for (const char* id : {"kodak_portra_400", "kodak_ektar_100", "fujifilm_pro_400h", "kodak_vision3_500t", "kodak_kodachrome_64"}) {
        EditParams e = p;
        e.filmStock = id;
        { double xx, yy; cctToXy(3200.0, xx, yy); e.filmSceneX = float(xx), e.filmSceneY = float(yy); }
        const FilmPadInfo pad = filmPadFor(e, cam, nullptr, src);
        CHECK(pad.ok);
        std::printf("  %s pad: scene %.0f K reference %.0f K, effective lo %d%d%d%d hi %d%d%d%d\n", id, pad.sceneCct, pad.referenceCct,
                    pad.effectiveLo[0], pad.effectiveLo[1], pad.effectiveLo[2], pad.effectiveLo[3], pad.effectiveHi[0], pad.effectiveHi[1], pad.effectiveHi[2], pad.effectiveHi[3]);
        CHECK(std::fabs(pad.sceneCct - 3200.f) < 150.f && pad.referenceCct > 2800.f);
        CHECK(pad.effectiveLo[2] && pad.effectiveHi[2]);  // film_black: the paper mapping is on
        CHECK(!pad.effectiveLo[3]);                       // film_wb has no negative end
        const bool tungsten = std::string(id) == "kodak_vision3_500t", slide = std::string(id) == "kodak_kodachrome_64";
        CHECK(pad.effectiveHi[3]);  // 3200 K against the stock's reference light
        if (tungsten) {             // its own light: nothing to do
          double xx, yy;
          cctToXy(2857.0, xx, yy);
          e.filmSceneX = float(xx), e.filmSceneY = float(yy);
          CHECK(!filmPadFor(e, cam, nullptr, src).effectiveHi[3]);
        }
        if (!tungsten) CHECK(std::fabs(pad.referenceCct - 5500.f) < 200.f);
        if (slide) CHECK(!pad.effectiveHi[1]);  // the lamp cannot brighten
        CHECK(pad.effectiveLo[0] && pad.effectiveHi[0] ? true : slide);
        e.filmSceneX = e.filmSceneY = 0.f;  // unknown scene light: the axis cannot be used
        CHECK(!filmPadFor(e, cam, nullptr, src).effectiveHi[3]);
      }
    }
  }
  // latitude of every shipped camera film is sane; slides are narrower than negatives
  for (const std::string& name : lib->names()) {
    const film::FilmStock* st = lib->find(name);
    if (!st || st->isPrintMedium()) continue;
    const FilmLatitude lat = filmLatitude(*st);
    CHECK(lat.ok);
    CHECK(lat.toe < 0.f && lat.shoulder > 0.f && lat.midGamma > 0.1f);
    CHECK(st->positive() ? lat.stops() < 11.f : lat.stops() > 10.f);
  }
  // the window is ordered for a moderate scene and a fixed width: hi - lo = latitude - margins - scene range (stops)
  {
    const SceneStats mid{0.03f, 0.18f, 0.5f};
    const SceneStats wide{0.002f, 0.2f, 4.f};
    for (const char* id : {"kodak_portra_400", "kodak_kodachrome_64", "fujifilm_velvia_100", "kodak_vision3_250d"}) {
      const film::FilmStock* st = lib->find(id);
      if (!st) { CHECK(false); continue; }
      const FilmLatitude lat = filmLatitude(*st);
      float lo = 0.f, hi = 0.f;
      CHECK(filmExposureWindow(lat, mid, lo, hi));
      CHECK(lo < hi);
      const float range = std::log10(mid.p995 / std::max(mid.p2, 0.002f));
      const float shMargin = lat.reversal ? kFilmShoulderMarginReversalLogH : kFilmShoulderMarginLogH;
      CHECK_NEAR(hi - lo, (lat.shoulder - lat.toe - shMargin - kFilmToeMarginLogH - range) / 0.30103, 1e-4);
      // the pull is applied: a match inside the window is scaled by kFilmKeyPull
      FilmMatchInfo mi;
      const float a = resolveFilmKeyEv(st, mid, 0.4f, FilmMatchOptions{}, &mi);
      if (mi.bound == FilmBound::None) CHECK_NEAR(a, 0.7f * 0.4f, 1e-6);
      CHECK_NEAR(mi.evKey, kFilmKeyPull * 0.4f, 1e-6);
      FilmMatchOptions fo;
      fo.pull = 1.f;
      fo.latitude = false;
      CHECK(resolveFilmKeyEv(st, mid, 0.4f, fo) == 0.4f);
      // a scene wider than the latitude: slides protect the highlights, negatives keep the shadows
      FilmMatchInfo mw;
      const float w = resolveFilmKeyEv(st, wide, 0.f, FilmMatchOptions{}, &mw);
      CHECK(mw.conflict == (mw.evLo > mw.evHi));
      if (mw.conflict) {
        CHECK(w == std::clamp(lat.reversal ? mw.evHi : mw.evLo, -4.f, 4.f));
        CHECK(mw.bound == (lat.reversal ? FilmBound::Shoulder : FilmBound::Toe));
      }
      if (lat.reversal) CHECK(mw.conflict);
    }
  }
  // clipped pixels do not set the highlight statistic; a mostly clipped frame falls back to p90 of everything
  {
    const float g1[3] = {1.f, 1.f, 1.f};
    ImageF im(100, 100);
    for (int y = 0; y < 100; ++y)
      for (int x = 0; x < 100; ++x) {
        float* q = im.row(y) + 3 * x;
        const float v = x < 10 ? 1.0f : 0.1f + 0.4f * float(x) / 100.f;  // 10 % clipped, the rest 0.14 .. 0.5
        q[0] = q[1] = q[2] = v;
      }
    const SceneStats st = sceneLumaStats(im, g1);
    CHECK_NEAR(st.clipFrac, 0.10, 1e-4);
    CHECK(st.p995 < 0.55f && st.p995 > 0.45f);  // not 1.0
    for (int y = 0; y < 100; ++y)
      for (int x = 0; x < 100; ++x) {
        float* q = im.row(y) + 3 * x;
        q[0] = q[1] = q[2] = x < 40 ? 1.0f : 0.3f;  // 40 % clipped: p90 of all pixels
      }
    const SceneStats s2 = sceneLumaStats(im, g1);
    CHECK_NEAR(s2.clipFrac, 0.40, 1e-4);
    CHECK_NEAR(s2.p995, 1.0, 1e-3);
  }
  // a stock without usable curves: plain +-2 EV limit
  {
    film::FilmStock bad = *lib->find("kodak_portra_400");
    bad.densityCurves.clear();
    FilmMatchInfo mi;
    CHECK(resolveFilmKeyEv(&bad, SceneStats{0.03f, 0.18f, 0.5f}, 4.f, FilmMatchOptions{}, &mi) == kFilmFallbackMaxEv);
    CHECK(mi.bound == FilmBound::Fallback && !mi.latitudeOk);
    CHECK(resolveFilmKeyEv(nullptr, SceneStats{0.03f, 0.18f, 0.5f}, -4.f, FilmMatchOptions{}) == -kFilmFallbackMaxEv);
    CHECK(chooseFilmEv(&bad, SceneStats{0.03f, 0.18f, 0.5f}, FilmMatchOptions{}, &mi) == 0.f && mi.bound == FilmBound::Fallback);
  }
  // DAPPLE_FILM_MATCH_FULL=1 restores the old behaviour (film EV only, no print exposure, no paper mapping)
  {
#ifdef _WIN32
    _putenv_s("DAPPLE_FILM_MATCH_FULL", "1");
#else
    setenv("DAPPLE_FILM_MATCH_FULL", "1", 1);
#endif
    const FilmMatchOptions o = FilmMatchOptions::fromEnv();
    CHECK(o.mode == FilmMatchMode::Full && o.pull == 1.f && !o.latitude);
    CHECK(!filmPaperMappingEnabled());
    FilmMatchInfo a, b;
    const FilmExposure evFull = matchFilmExposure(p, cam, nullptr, src, &a);
    CHECK(evFull.filmEv == a.evMatch && evFull.printEv == 0.f && a.bound == FilmBound::None);
#ifdef _WIN32
    _putenv_s("DAPPLE_FILM_MATCH_FULL", "");
#else
    unsetenv("DAPPLE_FILM_MATCH_FULL");
#endif
    CHECK(FilmMatchOptions::fromEnv().mode == FilmMatchMode::Meter && filmPaperMappingEnabled());
    const FilmExposure sp = matchFilmExposure(p, cam, nullptr, src, &b);
    CHECK(sp.printEv != 0.f && b.printConst == sp.printEv);
  }
  // no film: nothing to match
  CHECK(matchFilmExposure(plain, cam, nullptr, src).filmEv == 0.f);
  // the user's film exposure stays an offset on the film exposure, the print exposure is separate
  EditParams a = p, b = p;
  a.filmKeyEv = 1.f;
  b.filmKeyEv = 0.f;
  b.filmExposure = 1.f;
  CHECK(meanAbsDiff(Renderer(a, cam, nullptr).renderFloat(src, false), Renderer(b, cam, nullptr).renderFloat(src, false)) < 1e-6);
  EditParams c = p;
  c.filmPrintEv = 0.5f;
  CHECK(meanAbsDiff(Renderer(c, cam, nullptr).renderFloat(src, false), Renderer(p, cam, nullptr).renderFloat(src, false)) > 1e-3);
}

void testDevelopFilmIntegration() {
  auto lib = library();
  CHECK(lib != nullptr);
  if (!lib) return;
  const CameraRegistry cams = CameraRegistry::withBuiltins();
  const CameraProfile& cam = cams.resolve("Sony", "ILCE-7RM5");
  const SourceImage src = devSource(192, 128);

  // ---- no film: output does not depend on the film library or on a stock that cannot be resolved ----
  film::setLibrary(nullptr);
  const EditParams off;
  EditParams dangling = off;
  dangling.filmStock = "kodak_portra_400";  // no library installed: ignored
  const ImageF refOff = Renderer(off, cam, nullptr).renderFloat(src);
  CHECK(sameBits(refOff, Renderer(dangling, cam, nullptr).renderFloat(src)));
  const auto bakeOff = Renderer(off, cam, nullptr).bake(src, 33);
  FastOptions fo;
  const ImageF bakedOff = Renderer(off, cam, nullptr).renderBakedFloat(src, *bakeOff, fo);
  const auto bakeDangling = Renderer(dangling, cam, nullptr).bake(src, 33);
  CHECK(sameBits(bakedOff, Renderer(dangling, cam, nullptr).renderBakedFloat(src, *bakeDangling, fo)));
  CHECK(!Renderer(dangling, cam, nullptr).filmActive());

  film::setLibrary(lib);
  CHECK(Renderer(filmEdit(), cam, nullptr).filmActive());
  CHECK(!Renderer(off, cam, nullptr).filmActive());
  EditParams bogus = off;
  bogus.filmStock = "no_such_stock";
  CHECK(!Renderer(bogus, cam, nullptr).filmActive());
  EditParams isPaper = off;
  isPaper.filmStock = "kodak_portra_endura";  // a paper is not a camera film
  CHECK(!Renderer(isPaper, cam, nullptr).filmActive());
  // installed library, film off or strength 0: identical to the render without any film code involved
  CHECK(sameBits(refOff, Renderer(off, cam, nullptr).renderFloat(src)));
  EditParams zero = filmEdit();
  zero.filmStrength = 0.f;
  {
    EditParams offLike = zero;
    offLike.filmStock.clear();
    CHECK(sameBits(Renderer(offLike, cam, nullptr).renderFloat(src), Renderer(zero, cam, nullptr).renderFloat(src)));
  }

  // ---- film on: differs from the normal render, is deterministic, direct == baked ----
  const EditParams fp = filmEdit();
  const ImageF f1 = Renderer(fp, cam, nullptr).renderFloat(src);
  const ImageF f2 = Renderer(fp, cam, nullptr).renderFloat(src);
  CHECK(sameBits(f1, f2));
  CHECK(meanAbsDiff(f1, Renderer(filmEdit(), cam, nullptr).renderFloat(src)) == 0.0);
  EditParams plainP = fp;
  plainP.filmStock.clear();
  const ImageF plain = Renderer(plainP, cam, nullptr).renderFloat(src);
  CHECK(meanAbsDiff(f1, plain) > 0.02);
  const auto bk = Renderer(fp, cam, nullptr).bake(src, 33);
  const ImageF b1 = Renderer(fp, cam, nullptr).renderBakedFloat(src, *bk, fo);
  const ImageF b2 = Renderer(fp, cam, nullptr).renderBakedFloat(src, *bk, fo);
  CHECK(sameBits(b1, b2));
  CHECK(meanAbsDiff(f1, b1) < 1e-6);  // both paths run the same simulator on the same input
  // the streaming 8-bit path (no float frame) equals the float path quantised: plain, with curves + vignette, zoomed
  {
    auto sameAs8 = [&](const EditParams& ep, const FastOptions& o) {
      const Renderer r(ep, cam, nullptr);
      const auto bl = r.bake(src, 33);
      int w = 0, h = 0;
      std::uint8_t* px = r.renderBakedRgba(src, *bl, o, w, h);
      const ImageF fl = r.renderBakedFloat(src, *bl, o);
      bool ok = px && w == fl.width && h == fl.height;
      for (int i = 0; ok && i < w * h; ++i)
        for (int c = 0; c < 3; ++c) {
          const int ref = int(std::clamp(fl.data[std::size_t(i) * 3 + c], 0.f, 1.f) * 255.f + 0.5f);
          ok = ok && ref == px[i * 4 + c];
        }
      std::free(px);
      return ok;
    };
    FastOptions o8;
    o8.spatial = SpatialQuality::Off;
    CHECK(sameAs8(fp, o8));
    EditParams cv = fp;
    cv.vignette = -30.f;
    cv.parametricHighlights = 25.f;
    CHECK(sameAs8(cv, o8));
    FastOptions oz = o8;
    oz.region.x0 = 0.1f, oz.region.y0 = 0.2f, oz.region.x1 = 0.7f, oz.region.y1 = 0.8f, oz.region.outLongEdge = 96;
    CHECK(sameAs8(fp, oz));
    EditParams rot = fp;
    rot.straighten = 4.f;
    CHECK(sameAs8(rot, o8));
  }
  // the pooled simulator (cached bake) renders the same as after other films were used in between
  {
    const ImageF other = Renderer(filmEdit("kodak_ektar_100"), cam, nullptr).renderFloat(src);
    CHECK(meanAbsDiff(f1, other) > 0.01);
    CHECK(sameBits(f1, Renderer(fp, cam, nullptr).renderFloat(src)));
  }
  // threads do not matter: a second render on a differently chunked pool is identical (stateless grain)
  {
    SpatialQuality q = SpatialQuality::Full;
    FastOptions o2;
    o2.spatial = q;
    EditParams withGrain = fp;
    const ImageF g1 = Renderer(withGrain, cam, nullptr).renderBakedFloat(src, *bk, o2);
    const ImageF g2 = Renderer(withGrain, cam, nullptr).renderBakedFloat(src, *bk, o2);
    CHECK(sameBits(g1, g2));
  }
  // the lattice of a full-strength film is not baked (only the blend partner needs it)
  CHECK(bk->rgb.size() < bakeOff->rgb.size());
  // key: film parameters that are not spatial change it, spatial ones do not
  {
    Renderer a(fp, cam, nullptr);
    EditParams w = fp;
    w.filmWarmth = 20.f;
    EditParams g = fp;
    g.filmGrain = 10.f;
    g.filmHalation = 10.f;
    EditParams x = fp;
    x.filmExposure = 1.5f;
    x.filmTint = -30.f;
    CHECK(a.pointKey() == Renderer(w, cam, nullptr).pointKey());  // film warmth / tint / exposure: inside the simulator
    CHECK(a.pointKey() == Renderer(x, cam, nullptr).pointKey());
    CHECK(a.pointKey() == Renderer(g, cam, nullptr).pointKey());
    CHECK(a.pointKey() != Renderer(off, cam, nullptr).pointKey());
    EditParams other = fp;
    other.filmStock = "kodak_ektar_100";
    CHECK(a.pointKey() != Renderer(other, cam, nullptr).pointKey());
    // a lattice baked at one film exposure renders every other exposure exactly like a fresh bake
    EditParams ex = fp;
    ex.filmExposure = 1.5f;
    ex.filmWarmth = 25.f;
    const auto bkEx = Renderer(ex, cam, nullptr).bake(src, 33);
    CHECK(sameBits(Renderer(ex, cam, nullptr).renderBakedFloat(src, *bk, fo),
                   Renderer(ex, cam, nullptr).renderBakedFloat(src, *bkEx, fo)));
    CHECK(!sameBits(Renderer(ex, cam, nullptr).renderBakedFloat(src, *bk, fo), b1));  // and it does move the picture
    // tonal class: that of the develop render, never of the film look; the histogram follows the film (and its exposure)
    ToneAnalysis t0 = analyzeTone(src, cam, fp), t1 = analyzeTone(src, cam, ex), tOff = analyzeTone(src, cam, off);
    CHECK(t0.nearest == t1.nearest && t0.nearest == tOff.nearest && t0.histogram == tOff.histogram);
    ToneAnalysis h0 = t0, h1 = t1, hOff = tOff;
    applyFilmHistogram(h0, src, cam, fp);
    applyFilmHistogram(h1, src, cam, ex);
    applyFilmHistogram(hOff, src, cam, off);
    CHECK(h0.histogram != h1.histogram);
    CHECK(h0.histogram != t0.histogram);
    CHECK(hOff.histogram == tOff.histogram);
    CHECK(h0.nearest == t0.nearest && h1.nearest == t0.nearest);
    // the frame variant (interactive film frames): replaces histogram / percentiles from RGBA pixels, keeps the class
    {
      std::vector<std::uint8_t> px(40 * 30 * 4, 255);
      for (int i = 0; i < 40 * 30; ++i) px[std::size_t(i) * 4] = px[std::size_t(i) * 4 + 1] = px[std::size_t(i) * 4 + 2] = 128;
      ToneAnalysis f = t0;
      applyFrameHistogram(f, px.data(), 40, 30);
      float sum = 0, peak = 0;
      for (float v : f.histogram) sum += v, peak = std::max(peak, v);
      CHECK_NEAR(sum, 1.0, 1e-4);
      CHECK(peak > 0.99f);
      CHECK_NEAR(f.p50, 0.5, 0.03);
      CHECK(f.nearest == t0.nearest);
      ToneAnalysis same = t0;
      applyFrameHistogram(same, nullptr, 0, 0);  // nothing to read: untouched
      CHECK(same.histogram == t0.histogram);
    }
  }

  // ---- strength blends: 50 % lies between 0 and 100 % ----
  {
    EditParams half = fp;
    half.filmStrength = 50.f;
    const ImageF h = Renderer(half, cam, nullptr).renderFloat(src);
    const double d0 = meanAbsDiff(h, plain), d1 = meanAbsDiff(h, f1);
    CHECK(d0 > 0.005 && d1 > 0.005);
    const double full = meanAbsDiff(plain, f1);
    CHECK(d0 + d1 < full * 1.2);  // roughly on the segment between the two
    const auto bh = Renderer(half, cam, nullptr).bake(src, 33);
    CHECK(meanAbsDiff(h, Renderer(half, cam, nullptr).renderBakedFloat(src, *bh, fo)) < 0.01);
  }

  // ---- knobs ----
  {
    SourceImage dim;  // the test frame is mostly bright; exposure offsets are judged on a dim one
    dim.sceneReferred = true;
    dim.linear = ImageF(64, 64);
    for (float& v : dim.linear.data) v = 0.015f;
    const ImageF dimBase = Renderer(fp, cam, nullptr).renderFloat(dim);
    EditParams bright = fp;
    bright.filmExposure = 1.f;
    const ImageF brightImg = Renderer(bright, cam, nullptr).renderFloat(dim);
    std::printf("  film exposure +1 EV on a dim frame: mean green %.4f -> %.4f\n", channelMean(dimBase, 1), channelMean(brightImg, 1));
    CHECK(channelMean(brightImg, 1) > channelMean(dimBase, 1) + 0.03);
    EditParams ev = fp;
    ev.exposure = 1.f;  // the develop exposure slider acts before the film, like the film exposure offset
    CHECK(meanAbsDiff(Renderer(ev, cam, nullptr).renderFloat(dim), brightImg) < 0.005);
    EditParams warm = fp;
    warm.filmWarmth = 100.f;
    EditParams cool = fp;
    cool.filmWarmth = -100.f;
    const ImageF w = Renderer(warm, cam, nullptr).renderFloat(src), c = Renderer(cool, cam, nullptr).renderFloat(src);
    CHECK(channelMean(w, 0) / channelMean(w, 2) > channelMean(c, 0) / channelMean(c, 2) * 1.05);
    EditParams mag = fp;
    mag.filmTint = 100.f;
    EditParams grn = fp;
    grn.filmTint = -100.f;
    const ImageF m = Renderer(mag, cam, nullptr).renderFloat(src), g = Renderer(grn, cam, nullptr).renderFloat(src);
    CHECK(channelMean(m, 1) / channelMean(m, 0) < channelMean(g, 1) / channelMean(g, 0) * 0.97);  // magenta = less green
    // white balance slider still works before the film
    EditParams wb = fp;
    wb.temperature = 60.f;
    const ImageF t = Renderer(wb, cam, nullptr).renderFloat(src);
    CHECK(channelMean(t, 0) / channelMean(t, 2) > channelMean(f1, 0) / channelMean(f1, 2) * 1.03);
    // curves still apply after the film
    EditParams cv = fp;
    cv.parametricLights = 60.f;
    CHECK(meanAbsDiff(Renderer(cv, cam, nullptr).renderFloat(src), f1) > 0.003);
    // vignette follows
    EditParams vg = fp;
    vg.vignette = -80.f;
    const ImageF v = Renderer(vg, cam, nullptr).renderFloat(src);
    double sv = 0, sf = 0;
    for (int i = 0; i < 48; ++i) sv += v.data[i], sf += f1.data[i];
    CHECK(sv < sf * 0.9 + 1e-6);  // corner darkened (a black corner stays black)
  }

  // ---- grain amount: flat grey, std dev grows with the knob, 0 = none ----
  {
    SourceImage flat;
    flat.sceneReferred = true;
    flat.linear = ImageF(256, 256);
    for (float& v : flat.linear.data) v = 0.12f;
    auto stdDev = [&](float grain) {
      EditParams p = fp;
      p.filmGrain = grain;
      p.filmHalation = 0.f;
      const ImageF o = Renderer(p, cam, nullptr).renderFloat(flat);
      double m = 0, q = 0;
      int n = 0;
      for (int y = 64; y < 192; ++y)
        for (int x = 64; x < 192; ++x) {
          const double v = o.row(y)[3 * x + 1];
          m += v;
          q += v * v;
          ++n;
        }
      m /= n;
      return std::sqrt(std::max(0.0, q / n - m * m));
    };
    const double s0 = stdDev(0), s50 = stdDev(50), s100 = stdDev(100);
    std::printf("  grain knob: std dev 0 -> %.5f, 50 -> %.5f, 100 -> %.5f (encoded, flat patch)\n", s0, s50, s100);
    CHECK(s0 < 3e-4);  // what remains is the stochastic glare of the print, not the grain
    CHECK(s50 > s0 * 2 && s100 > s50 * 1.15);
  }

  // ---- halation amount and resolution scaling: a bright disc on a dark frame, spread measured in image widths ----
  {
    const SourceImage disc = devSource(768, 768, true);
    auto spread = [&](int longEdge, float halation) {
      EditParams p = fp;
      p.filmGrain = 0.f;
      p.filmHalation = halation;
      Renderer r(p, cam, nullptr);
      const auto b = r.bake(disc, 33);
      FastOptions o;
      o.region.outLongEdge = longEdge;
      return r.renderBakedFloat(disc, *b, o);
    };
    auto profile = [&](const ImageF& with, const ImageF& without) {
      // second moment of the red excess along the centre row, in image widths
      const int y = with.height / 2;
      double m0 = 0, m2 = 0;
      for (int x = 0; x < with.width; ++x) {
        const double d = std::max(0.0, double(with.row(y)[3 * x]) - without.row(y)[3 * x]);
        const double u = (x - with.width * 0.5) / with.width;
        m0 += d;
        m2 += d * u * u;
      }
      return std::pair<double, double>(m0, m0 > 0 ? std::sqrt(m2 / m0) : 0.0);
    };
    const auto big = profile(spread(768, 100.f), spread(768, 0.f));
    const auto small = profile(spread(384, 100.f), spread(384, 0.f));
    std::printf("  halation (red excess along the centre row): 768 px rms radius %.4f widths, 384 px %.4f widths\n", big.second, small.second);
    CHECK(big.first > 1.0 && small.first > 0.5);  // halation adds light around the disc
    CHECK(big.second > 0.01);
    CHECK(std::fabs(big.second - small.second) < 0.3 * big.second);  // same physical spread at both resolutions
  }

  // ---- accuracy against the slow double-precision oracle through the whole develop path ----
  {
    EditParams p = filmEdit();
    SourceImage patches;
    patches.sceneReferred = true;
    constexpr int N = 200;
    patches.linear = ImageF(N, 1);
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> u(0.01f, 1.2f);
    for (float& v : patches.linear.data) v = u(rng) * u(rng);
    FilmSimulator oracle(lib);
    FilmParams fpar;
    fpar.film = p.filmStock;
    fpar.paper = film::defaultPaperFor(*lib, p.filmStock);
    fpar.camera.autoExposure = false;
    applyFilmPaperMapping(fpar);  // the develop pipeline's paper-to-screen mapping
    fpar.disableSpatialAndStochastic();
    CHECK(oracle.setParams(fpar));
    const float gain = std::exp2(cam.baselineExposureEv);
    FastOptions o;
    o.spatial = SpatialQuality::Off;
    Renderer r(p, cam, nullptr);
    const auto b = r.bake(patches, 33);
    const ImageF got = r.renderBakedFloat(patches, *b, o);
    double sum = 0, worst = 0;
    for (int i = 0; i < N; ++i) {
      const double in[3] = {patches.linear.data[3 * i] * gain, patches.linear.data[3 * i + 1] * gain,
                            patches.linear.data[3 * i + 2] * gain};
      double ref[3];
      oracle.renderPixelDirect(in, ref);
      const float rf[3] = {float(ref[0]), float(ref[1]), float(ref[2])};
      const double de = metrics::deltaE2000(metrics::srgbToLab(rf), metrics::srgbToLab(&got.data[3 * i]));
      sum += de;
      worst = std::max(worst, de);
    }
    std::printf("  develop path vs direct double-precision oracle: mean dE2000 %.3f, max %.3f (%d colours)\n", sum / N, worst, N);
    CHECK(sum / N < 0.5);
  }
  film::setLibrary(nullptr);
}

void testFilmCApi() {
  auto lib = library();
  if (!lib) return;
  film::setLibrary(nullptr);
  {
    char* none = dapple_film_list_json();
    CHECK(none && std::string(none).find("\"loaded\":false") != std::string::npos);
    dapple_free_string(none);
  }
  const fs::path pack = kData / "dapple.dfp";
  if (!fs::exists(pack)) return;
  const std::string path = pack.string();
  CHECK(dapple_film_load_pack("definitely/not/there.dfp") == DAPPLE_ERROR);
  CHECK(dapple_film_load_pack(path.c_str()) == DAPPLE_OK);
  char* json = dapple_film_list_json();
  const std::string j = json ? json : "";
  dapple_free_string(json);
  CHECK(j.find("\"loaded\":true") != std::string::npos);
  CHECK(j.find("{\"id\":\"kodak_portra_400\",\"name\":\"") != std::string::npos);
  CHECK(j.find("\"type\":\"negative\"") != std::string::npos);
  CHECK(j.find("\"type\":\"slide\"") != std::string::npos);
  CHECK(j.find("\"type\":\"paper\"") != std::string::npos);
  CHECK(j.find("{\"id\":\"kodak_portra_endura\"") != std::string::npos);
  auto typeOf = [&](const char* id) {
    const std::size_t p = j.find(std::string("\"id\":\"") + id + "\"");
    if (p == std::string::npos) return std::string();
    const std::size_t t = j.find("\"type\":\"", p) + 8;
    return j.substr(t, j.find('"', t) - t);
  };
  CHECK(typeOf("kodak_portra_400") == "negative" && typeOf("fujifilm_velvia_100") == "slide" &&
        typeOf("kodak_portra_endura") == "paper" && typeOf("kodak_2383") == "paper");
  {
    char* sw = dapple_film_swatches_json();
    const std::string w = sw ? sw : "";
    dapple_free_string(sw);
    std::printf("  swatches: %s", w.substr(0, 200).c_str());
    std::putchar(10);
    int stocks = 0;
    for (std::size_t p = j.find("\"type\":\"negative\""); p != std::string::npos; p = j.find("\"type\":\"negative\"", p + 1)) ++stocks;
    for (std::size_t p = j.find("\"type\":\"slide\""); p != std::string::npos; p = j.find("\"type\":\"slide\"", p + 1)) ++stocks;
    int entries = 0;
    for (std::size_t p = w.find("\":[\""); p != std::string::npos; p = w.find("\":[\"", p + 1)) ++entries;
    CHECK(stocks > 10 && entries == stocks);                                 // every camera stock, no paper
    CHECK(w.find("kodak_portra_endura") == std::string::npos);
    CHECK(w.find("\"kodak_portra_400\":[\"") != std::string::npos);
    char* sw2 = dapple_film_swatches_json();
    CHECK(sw2 && w == sw2);  // deterministic
    dapple_free_string(sw2);
    // the patches differ between stocks (a swatch is not one colour for all)
    auto first = [&](const char* id) {
      const std::size_t q = w.find(std::string("\"") + id + "\":[");
      return q == std::string::npos ? std::string() : w.substr(q, 40);
    };
    CHECK(first("kodak_portra_400").substr(18) != first("fujifilm_velvia_100").substr(21));
    // band swatches (skin, sky, deep shadow): one entry per camera stock, deterministic, and the bands look like what they are
    char* sb = dapple_film_swatch_bands_json();
    const std::string bw = sb ? sb : "";
    dapple_free_string(sb);
    int bentries = 0;
    for (std::size_t p = bw.find("\":[\""); p != std::string::npos; p = bw.find("\":[\"", p + 1)) ++bentries;
    CHECK(bentries == stocks);
    char* sb2 = dapple_film_swatch_bands_json();
    CHECK(sb2 && bw == sb2);
    dapple_free_string(sb2);
    {
      const std::size_t q = bw.find("\"kodak_portra_400\":[\"");
      CHECK(q != std::string::npos);
      if (q != std::string::npos) {
        int v[3][3] = {{0}};
        const std::size_t s0 = bw.find('[', q);
        for (int k = 0; k < 3; ++k) {
          const std::string h = bw.substr(s0 + 2 + k * 9, 6);
          for (int c = 0; c < 3; ++c) v[k][c] = std::stoi(h.substr(c * 2, 2), nullptr, 16);
        }
        std::printf("  portra_400 bands: skin %d,%d,%d sky %d,%d,%d shadow %d,%d,%d\n", v[0][0], v[0][1], v[0][2], v[1][0], v[1][1], v[1][2],
                    v[2][0], v[2][1], v[2][2]);
        CHECK(v[0][0] > v[0][2] + 20);            // skin is warm
        CHECK(v[1][2] > v[1][0] + 20);            // sky is blue
        CHECK(v[2][0] < 70 && v[2][1] < 70 && v[2][2] < 70);  // shadow is dark
      }
    }
  }
  char* attr = dapple_film_attribution();
  CHECK(attr && std::string(attr).find("Volpato") != std::string::npos && std::string(attr).find("CC BY-SA 4.0") != std::string::npos);
  dapple_free_string(attr);
  // memory loading
  std::ifstream f(pack, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  const std::string bytes = ss.str();
  // (the C API lives in the shared library, which has its own copy of the process-wide registry)
  auto loaded = [] {
    char* s = dapple_film_list_json();
    const bool ok = s && std::string(s).find("\"loaded\":true") != std::string::npos;
    dapple_free_string(s);
    return ok;
  };
  dapple_film_set_pack_path("definitely/not/there.dfp");  // lazy: resolves to nothing
  CHECK(!loaded());
  CHECK(dapple_film_load_pack_memory(bytes.data(), bytes.size()) == DAPPLE_OK);
  CHECK(loaded());
  CHECK(dapple_film_load_pack_memory("garbage", 7) == DAPPLE_ERROR);
  CHECK(loaded());  // a failed load keeps the installed library
  dapple_film_set_pack_path(path.c_str());  // lazy path: nothing is read until first use
  CHECK(loaded());
  // default paper
  CHECK(film::defaultPaperFor(*lib, "kodak_portra_400") == "kodak_portra_endura");
  CHECK(film::defaultPaperFor(*lib, "fujifilm_velvia_100") == "none");
  CHECK(film::defaultPaperFor(*lib, "kodak_vision3_500t") == "kodak_2383");
}

// ---- robustness / API hygiene tests -------------------------------------------------------------------------

bool allFinite(const ImageF& a) {
  for (float v : a.data)
    if (!std::isfinite(v)) return false;
  return true;
}

void testParamValidation() {
  auto lib = library();
  // every value that used to crash the process (bad_alloc / length_error) or render garbage is now an error
  struct Bad {
    const char* json;
  };
  const Bad bad[] = {{R"({"halation":{"scatter_tail_um":[1e9,1e9,1e9]}})"},
                     {R"({"film_render":{"halation":{"scatter_tail_um":[1e9,1e9,1e9]}}})"},
                     {R"({"print_render":{"glare":{"blur":1e5}}})"},
                     {R"({"scanner":{"unsharp_mask":[1e4,1]}})"},
                     {R"({"film_render":{"dir_couplers":{"diffusion_size_um":1e9}}})"},
                     {R"({"camera":{"lens_blur_um":1e9}})"},
                     {R"({"film_render":{"grain":{"blur_dye_clouds_um":1e9}}})"},
                     {R"({"camera":{"film_format_mm":0}})"},
                     {R"({"camera":{"film_format_mm":1e-9}})"},
                     {R"({"camera":{"film_format_mm":-35}})"},
                     {R"({"camera":{"exposure_compensation_ev":100}})"},
                     {R"({"camera":{"exposure_compensation_ev":-10000}})"},
                     {R"({"enlarger":{"print_exposure":0}})"},
                     {R"({"enlarger":{"print_exposure":-1}})"},
                     {R"({"film_render":{"density_curve_gamma":0}})"},
                     {R"({"film_render":{"density_curve_gamma":-1}})"},
                     {R"({"enlarger":{"lens_blur":3}})"},                                  // dead option: unsupported
                     {R"({"print_render":{"density_curves_morph":{"developer_exhaustion":0.5}}})"},
                     {R"({"io":{"output_gamut_compress":{"knee":[0.1,1,6]}}})"},
                     {R"({"io":{"output_gamut_compress":{"lightness_compression":[0.5,1,2.2]}}})"}};
  for (const Bad& b : bad) {
    FilmParams p;
    std::string err;
    const bool ok = p.applyJson(b.json, &err);
    if (ok) std::fprintf(stderr, "  accepted: %s\n", b.json);
    CHECK(!ok);
    CHECK(!err.empty());
  }
  // neutral values of the unsupported options stay accepted (reference parameter files carry them)
  {
    FilmParams p;
    std::string err;
    CHECK(p.applyJson(R"({"enlarger":{"lens_blur":0},"print_render":{"density_curves_morph":{"developer_exhaustion":0}}})", &err));
  }
  // NaN / Inf set directly on the struct (no JSON in between) are caught by setParams
  FilmSimulator sim(lib);
  auto setBad = [&](auto mutate) {
    FilmParams p = noSpatial();
    mutate(p);
    std::string err;
    return !sim.setParams(p, &err) && !err.empty();
  };
  CHECK(setBad([](FilmParams& p) { p.camera.exposureCompensationEv = std::nan(""); }));
  CHECK(setBad([](FilmParams& p) { p.camera.exposureCompensationEv = 1e4; }));
  CHECK(setBad([](FilmParams& p) { p.enlarger.printExposure = std::numeric_limits<double>::infinity(); }));
  CHECK(setBad([](FilmParams& p) { p.filmRender.glare.percent = std::nan(""); }));
  CHECK(setBad([](FilmParams& p) { p.settings.lutResolution = 4; }));
  CHECK(setBad([](FilmParams& p) { p.filmRender.halation.boostEv = -1; }));
  // the engine itself, with every extreme at its limit, renders without throwing and stays finite
  FilmParams ext;
  ext.camera.autoExposure = false;
  ext.camera.lensBlurUm = 1e5;
  ext.camera.filmFormatMm = 1000;
  ext.filmRender.halation.scatterTailUm = {1e5, 1e5, 1e5};
  ext.filmRender.dirCouplers.diffusionSizeUm = 1e5;
  ext.filmRender.dirCouplers.diffusionTailUm = 1e5;
  ext.filmRender.grain.blurDyeCloudsUm = 1e5;
  ext.printRender.glare.blur = 64;
  ext.scanner.unsharpMask = {64, 1000};
  ext.scanner.lensBlur = 64;
  CHECK(ext.validate());
  const ImageF small = gradientImage(97, 53, 2);
  FilmSimulator se(lib);
  CHECK(se.setParams(ext));
  ImageF o;
  CHECK(se.render(small, o));
  CHECK(allFinite(o));
  const ImageF one = gradientImage(1, 1, 3);
  CHECK(se.render(one, o) && allFinite(o));
}

void testSetParamsStrongGuarantee() {
  auto lib = library();
  FilmSimulator sim(lib);
  FilmParams good = noSpatial();
  CHECK(sim.setParams(good));
  const ImageF in = gradientImage(64, 48, 5);
  ImageF before, after;
  CHECK(sim.render(in, before));
  // failures after the point where the old code had already committed: unknown paper, print stock as film, bad value
  FilmParams p = good;
  p.paper = "no_such_paper";
  std::string err;
  CHECK(!sim.setParams(p, &err));
  p = good;
  p.film = "kodak_portra_endura";
  CHECK(!sim.setParams(p, &err));
  p = good;
  p.enlarger.illuminant = "NOT-AN-ILLUMINANT";
  p.camera.exposureCompensationEv = 3;  // would change the bake if it were half-applied
  CHECK(!sim.setParams(p, &err));
  CHECK(sim.params().camera.exposureCompensationEv == good.camera.exposureCompensationEv);
  CHECK(sim.params().enlarger.illuminant == good.enlarger.illuminant);
  ImageF mid;
  CHECK(sim.render(in, mid));  // still the old, consistent state
  CHECK(maxAbsDiff(before, mid) == 0.0);
  p = good;
  p.camera.filmFormatMm = 0;
  CHECK(!sim.setParams(p, &err));
  // re-apply the good state: the render is bit-identical to the first one
  CHECK(sim.setParams(good));
  CHECK(sim.render(in, after));
  CHECK(maxAbsDiff(before, after) == 0.0);
  // a failed call right after a good one leaves state and output untouched
  FilmParams bad = good;
  bad.paper = "no_such_paper";
  CHECK(!sim.setParams(bad));
  CHECK(sim.params().paper == good.paper);
  ImageF again;
  CHECK(sim.render(in, again));
  CHECK(maxAbsDiff(before, again) == 0.0);
}

void testRobustRender() {
  auto lib = library();
  FilmSimulator sim(lib);
  FilmParams p = spatialParams();  // all effects on
  CHECK(sim.setParams(p));
  // NaN / Inf / negative / huge / tiny input never produces non-finite output
  const float poison[] = {std::nanf(""), std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                          -3.f,          1e8f,                                   1e-30f,
                          0.f,           -0.f};
  ImageF in = gradientImage(61, 37, 9);
  for (std::size_t i = 0; i < in.data.size(); i += 7) in.data[i] = poison[(i / 7) % 8];
  ImageF out;
  CHECK(sim.render(in, out));
  CHECK(allFinite(out));
  // degenerate sizes and non-multiple tile sizes: bit-identical for every tile decomposition, all effects on
  const int sizes[][2] = {{1, 1}, {3, 2}, {7, 5}, {255, 257}, {257, 255}, {513, 129}, {2, 1000}, {1000, 2}};
  const int tiles[][2] = {{0, 0}, {64, 32}, {97, 41}, {1024, 1024}};
  for (const auto& sz : sizes) {
    const ImageF img = gradientImage(sz[0], sz[1], 11u + unsigned(sz[0]));
    ImageF ref;
    for (const auto& t : tiles) {
      FilmParams pt = p;
      pt.settings.tileWidth = t[0];
      pt.settings.tileHeight = t[1];
      CHECK(sim.setParams(pt));
      ImageF o;
      CHECK(sim.render(img, o));
      CHECK(allFinite(o));
      if (ref.empty()) ref = o;
      else if (maxAbsDiff(ref, o) != 0.0) {
        std::fprintf(stderr, "  tile mismatch %dx%d tile %dx%d: %.3g\n", sz[0], sz[1], t[0], t[1], maxAbsDiff(ref, o));
        CHECK(false);
      }
    }
  }
  // empty input is an error, not a crash
  ImageF e, o2;
  CHECK(!sim.render(e, o2));
}

struct RowCollect {
  ImageF* img;
};
void rowSink(void* user, int y, float* rgb, int width) {
  auto* c = static_cast<RowCollect*>(user);
  std::memcpy(c->img->row(y), rgb, std::size_t(width) * 12);
}

void testRenderRows() {
  auto lib = library();
  FilmSimulator sim(lib);
  CHECK(sim.setParams(noSpatial()));
  const float gain[3] = {1.1f, 1.f, 0.9f};
  for (const auto& sz : {std::pair<int, int>{1, 1}, {5, 3}, {301, 97}}) {
    const ImageF in = gradientImage(sz.first, sz.second, 21);
    ImageF a, b(sz.first, sz.second);
    CHECK(sim.render(in, a, nullptr, gain));
    RowCollect rc{&b};
    CHECK(sim.renderRows(in, 0, 0, gain, nullptr, nullptr, rowSink, &rc));
    CHECK(maxAbsDiff(a, b) == 0.0);
  }
  // with spatial effects the streaming path declines (the caller falls back to render())
  FilmSimulator sp(lib);
  CHECK(sp.setParams(spatialParams()));
  const ImageF in = gradientImage(40, 30, 1);
  ImageF b(40, 30);
  RowCollect rc{&b};
  CHECK(!sp.renderRows(in, 0, 0, nullptr, nullptr, nullptr, rowSink, &rc));
  // trimMemory() only releases scratch: results are unchanged
  ImageF r1, r2;
  CHECK(sp.render(in, r1));
  film::trimMemory();
  CHECK(sp.render(in, r2));
  CHECK(maxAbsDiff(r1, r2) == 0.0);
}

// ---- pack format ----------------------------------------------------------------------------------------

std::uint32_t testCrc32(const unsigned char* p, std::size_t n) {
  std::uint32_t c = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < n; ++i) {
    c ^= p[i];
    for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
  }
  return c ^ 0xFFFFFFFFu;
}
void fixCrc(std::string& f) {
  const std::uint32_t c = testCrc32(reinterpret_cast<const unsigned char*>(f.data()) + 16, f.size() - 16);
  for (int i = 0; i < 4; ++i) f[8 + i] = char((c >> (8 * i)) & 0xFF);
}

void testPackFormat() {
  auto lib = library();
  const fs::path tmp = fs::temp_directory_path() / "dapple_film_test2.dfp";
  std::string err;
  CHECK(lib->writePack(tmp, &err));
  std::string file;
  {
    std::ifstream f(tmp, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    file = ss.str();
  }
  std::error_code ec;
  fs::remove(tmp, ec);
  CHECK(file.size() > 1000);
  // little-endian header: magic carries the version, size field equals the file size
  CHECK(file.compare(0, 4, "DFP2") == 0);
  const auto le32 = [&](std::size_t at) {
    return std::uint32_t(std::uint8_t(file[at])) | std::uint32_t(std::uint8_t(file[at + 1])) << 8 |
           std::uint32_t(std::uint8_t(file[at + 2])) << 16 | std::uint32_t(std::uint8_t(file[at + 3])) << 24;
  };
  CHECK(le32(4) == file.size());
  CHECK(le32(8) == testCrc32(reinterpret_cast<const unsigned char*>(file.data()) + 16, file.size() - 16));
  CHECK(FilmLibrary::loadPackMemory(file.data(), file.size(), &err) != nullptr);
  // JSON and pack libraries are bit-identical (grain layers are stored as float32, not half)
  FilmStock js;
  CHECK(FilmLibrary::loadProfileJson(kData / "profiles" / "kodak_portra_400.json", js, &err));
  CHECK(js.densityCurvesLayers == lib->find("kodak_portra_400")->densityCurvesLayers);
  // truncation at every interesting length, trailing bytes, any single flipped byte: rejected cleanly
  for (std::size_t n : {std::size_t(0), std::size_t(3), std::size_t(15), std::size_t(16), std::size_t(100), file.size() / 2,
                        file.size() - 1}) {
    CHECK(FilmLibrary::loadPackMemory(file.data(), n, &err) == nullptr);
  }
  {
    std::string extra = file + "xyz";
    CHECK(FilmLibrary::loadPackMemory(extra.data(), extra.size(), &err) == nullptr);
  }
  std::mt19937 rng(1234);
  for (int i = 0; i < 100; ++i) {
    std::string m = file;
    m[16 + rng() % (m.size() - 16)] ^= char(1u << (rng() % 8));  // bit flip, CRC not fixed
    CHECK(FilmLibrary::loadPackMemory(m.data(), m.size(), &err) == nullptr);
  }
  // fuzz with a valid CRC: structural validation alone must never crash / over-read (ASan-clean by construction),
  // and the section table, sizes and counts it accepts must still describe a coherent library
  int accepted = 0;
  for (int i = 0; i < 120; ++i) {
    std::string m = file;
    const int flips = 1 + int(rng() % 4);
    for (int k = 0; k < flips; ++k) {
      // bias towards the header, the section table and the first stock (counts, offsets, array sizes)
      const std::size_t at = (rng() % 3 == 0) ? 16 + rng() % 400 : 16 + rng() % (m.size() - 16);
      m[at] = char(rng());
    }
    fixCrc(m);
    auto l = FilmLibrary::loadPackMemory(m.data(), m.size(), &err);
    if (l) {
      ++accepted;
      for (const std::string& n : l->names()) CHECK(validateStock(*l->find(n)));
    }
  }
  std::printf("  pack fuzz: %d / 120 mutated packs still valid (data-only changes)\n", accepted);
  // an unknown section is skipped (forward compatibility): append one, patch table, size and CRC
  {
    std::string m = file;
    const std::uint32_t nSec = le32(12);
    // new section data placed after the existing payload, table entry appended in-place is not possible without a
    // rewrite, so extend the table by rebuilding the file: header + (table + one entry) + shifted payload
    std::string out = m.substr(0, 16);
    const std::size_t oldTable = 16 + std::size_t(nSec) * 12;
    std::string table = m.substr(16, std::size_t(nSec) * 12);
    std::string payload = m.substr(oldTable);
    auto put = [](std::string& s, std::uint32_t v) {
      for (int i = 0; i < 4; ++i) s.push_back(char((v >> (8 * i)) & 0xFF));
    };
    // shift all offsets by 12
    for (std::uint32_t i = 0; i < nSec; ++i) {
      std::uint32_t off = std::uint32_t(std::uint8_t(table[i * 12 + 4])) | std::uint32_t(std::uint8_t(table[i * 12 + 5])) << 8 |
                          std::uint32_t(std::uint8_t(table[i * 12 + 6])) << 16 | std::uint32_t(std::uint8_t(table[i * 12 + 7])) << 24;
      off += 12;
      for (int k = 0; k < 4; ++k) table[i * 12 + 4 + k] = char((off >> (8 * k)) & 0xFF);
    }
    put(table, 0x58585858u);  // "XXXX"
    put(table, std::uint32_t(oldTable + 12 + payload.size()));
    put(table, 8);
    out += table;
    out += payload;
    out += std::string(8, 'z');
    const std::uint32_t total = std::uint32_t(out.size());
    for (int i = 0; i < 4; ++i) out[4 + i] = char((total >> (8 * i)) & 0xFF);
    const std::uint32_t ns = nSec + 1;
    for (int i = 0; i < 4; ++i) out[12 + i] = char((ns >> (8 * i)) & 0xFF);
    fixCrc(out);
    auto l = FilmLibrary::loadPackMemory(out.data(), out.size(), &err);
    CHECK(l != nullptr);
    if (l) CHECK(l->names() == lib->names());
  }
  // validateStock: the sizes the old loader forgot to check
  FilmStock s = *lib->find("kodak_portra_endura");
  CHECK(validateStock(s));
  {
    FilmStock t = s;
    t.modelSigmas.resize(5);
    CHECK(!validateStock(t, &err));
    t = s;
    t.modelAmplitudes.clear();
    CHECK(!validateStock(t, &err));
    t = s;
    t.windowParams.assign(3, 0.f);
    CHECK(!validateStock(t, &err));
    t = s;
    t.surfaceParams.assign(44, 0.f);
    CHECK(!validateStock(t, &err));
    t = s;
    t.densityCurvesLayers.assign(10, 0.f);
    CHECK(!validateStock(t, &err));
    t = s;
    t.logExposure[3] = std::numeric_limits<float>::infinity();
    CHECK(!validateStock(t, &err));
  }
}

// ---- cache keys -----------------------------------------------------------------------------------------

void testCacheKeys() {
  auto lib = library();
  FilmSimulator sim(lib);
  FilmParams base = spatialParams();
  FilmTimings t;
  CHECK(sim.setParams(base, nullptr, &t));
  CHECK(t.bakeMs > 0);
  // spatial-only changes never re-bake
  auto spatialOnly = [&](auto mutate, const char* what) {
    FilmParams p = base;
    mutate(p);
    FilmTimings tt;
    CHECK(sim.setParams(p, nullptr, &tt));
    if (tt.bakeMs != 0) std::fprintf(stderr, "  unexpected re-bake for spatial-only change: %s\n", what);
    CHECK(tt.bakeMs == 0);
    CHECK(sim.setParams(base));
  };
  spatialOnly([](FilmParams& p) { p.settings.seed = 99; }, "seed");
  spatialOnly([](FilmParams& p) { p.filmRender.grain.particleAreaUm2 = 0.5; }, "grain area");
  spatialOnly([](FilmParams& p) { p.filmRender.halation.halationAmount = 2; }, "halation amount");
  spatialOnly([](FilmParams& p) { p.camera.lensBlurUm = 15; }, "lens blur");
  spatialOnly([](FilmParams& p) { p.scanner.unsharpMask = {1.0, 0.5}; }, "unsharp");
  spatialOnly([](FilmParams& p) { p.settings.tileWidth = 64; }, "tile");
  // the same parameters again: nothing re-bakes
  { FilmTimings tt; CHECK(sim.setParams(base, nullptr, &tt)); CHECK(tt.bakeMs == 0); }
  // every group of fields that feeds the bake does invalidate it
  auto bakes = [&](auto mutate, const char* what) {
    FilmParams p = base;
    mutate(p);
    FilmTimings tt;
    CHECK(sim.setParams(p, nullptr, &tt));
    if (tt.bakeMs <= 0) std::fprintf(stderr, "  stale bake for change of: %s\n", what);
    CHECK(tt.bakeMs > 0);
    CHECK(sim.setParams(base));
  };
  bakes([](FilmParams& p) { p.camera.exposureCompensationEv = 0.5; }, "exposure compensation");
  bakes([](FilmParams& p) { p.camera.filterUv = {0.5, 410, 8}; }, "uv filter");
  bakes([](FilmParams& p) { p.camera.filterIr = {0.5, 675, 15}; }, "ir filter");
  bakes([](FilmParams& p) { p.filmRender.densityCurveGamma = 1.1; }, "gamma");
  bakes([](FilmParams& p) { p.filmRender.densityCurveGammaRgb = {1.1, 1, 1}; }, "gamma rgb");
  bakes([](FilmParams& p) { p.filmRender.grain.densityMin = {0.05, 0.03, 0.03}; }, "density min");
  bakes([](FilmParams& p) { p.filmRender.dirCouplers.amount = 0.5; }, "dir amount");
  bakes([](FilmParams& p) { p.filmRender.dirCouplers.inhibitionSamelayer = 0.5; }, "dir inhibition");
  bakes([](FilmParams& p) { p.settings.applyStocksSpecifics = false; p.filmRender.dirCouplers.gammaSamelayerRgb = {0.3, 0.3, 0.3}; }, "dir gamma (no stock overrides)");
  bakes([](FilmParams& p) { p.enlarger.printExposure = 1.2; }, "print exposure");
  bakes([](FilmParams& p) { p.enlarger.yFilterShift = 4; }, "y filter");
  bakes([](FilmParams& p) { p.enlarger.mFilterShift = 4; }, "m filter");
  bakes([](FilmParams& p) { p.enlarger.preflashExposure = 0.01; }, "preflash");
  bakes([](FilmParams& p) { p.enlarger.illuminant = "D65"; }, "illuminant");
  bakes([](FilmParams& p) { p.printRender.glare.percent = 0.1; }, "glare percent");
  bakes([](FilmParams& p) { p.printRender.densityCurvesMorph.active = true; p.printRender.densityCurvesMorph.gammaFactor = 1.2; }, "morph");
  bakes([](FilmParams& p) { p.scanner.whiteCorrection = true; }, "white correction");
  bakes([](FilmParams& p) { p.io.inputGamutCompress = false; }, "input gamut");
  bakes([](FilmParams& p) { p.io.outputGamutCompress = false; }, "output gamut");
  bakes([](FilmParams& p) { p.io.inputKnee.power = 3; }, "input knee");
  bakes([](FilmParams& p) { p.settings.lutResolution = 17; }, "lut resolution");
  bakes([](FilmParams& p) { p.settings.applyWindow = false; }, "window");
  bakes([](FilmParams& p) { p.settings.neutralPrintFilters = false; }, "neutral filters");
  bakes([](FilmParams& p) { p.paper = "kodak_2383"; }, "paper");
  bakes([](FilmParams& p) { p.film = "kodak_ektar_100"; }, "film");
  bakes([](FilmParams& p) { p.enlarger.diffusionFilter.active = true; }, "enlarger diffusion filter");
}

// ---- concurrency ----------------------------------------------------------------------------------------

void testConcurrency() {
  auto lib = library();
  const ImageF in = gradientImage(96, 64, 3);
  ImageF ref;
  {
    FilmSimulator s(lib);
    CHECK(s.setParams(spatialParams()));
    CHECK(s.render(in, ref));
  }
  // concurrent acquireSimulator: every thread gets its own simulator and the right answer; the pool shrinks back
  std::atomic<int> bad{0};
  {
    std::vector<std::thread> ts;
    for (int i = 0; i < 8; ++i)
      ts.emplace_back([&, i] {
        const char* films[] = {"kodak_portra_400", "kodak_ektar_100"};
        for (int k = 0; k < 6; ++k) {
          FilmParams p = spatialParams();
          p.film = films[(i + k) % 2];
          auto lease = acquireSimulator(lib, p.film, p.paper);
          if (!lease || !lease->setParams(p)) {
            ++bad;
            continue;
          }
          ImageF o;
          if (!lease->render(in, o) || o.data.empty()) ++bad;
          if (p.film == "kodak_portra_400" && maxAbsDiff(ref, o) != 0.0) ++bad;
        }
      });
    for (auto& t : ts) t.join();
  }
  CHECK(bad == 0);
  // one simulator hammered by setParams and render from different threads: no crash, no torn state, finite output
  FilmSimulator shared(lib);
  CHECK(shared.setParams(spatialParams()));
  std::atomic<bool> stop{false};
  std::atomic<int> notFinite{0};
  std::thread renderer([&] {
    while (!stop) {
      ImageF o;
      if (shared.render(in, o) && !allFinite(o)) ++notFinite;
    }
  });
  for (int i = 0; i < 30; ++i) {
    FilmParams p = spatialParams();
    p.camera.exposureCompensationEv = (i % 5) * 0.2;
    p.filmRender.grain.particleAreaUm2 = 0.2 + 0.1 * (i % 3);
    CHECK(shared.setParams(p));
  }
  stop = true;
  renderer.join();
  CHECK(notFinite == 0);
}

void testPoolExceptions() {
  // an exception thrown inside a pool job is rethrown on the caller, the pool stays usable
  bool caught = false;
  try {
    dapple::parallelFor(
        1000, [](int i) { if (i == 777) throw std::runtime_error("boom"); }, 1);
  } catch (const std::runtime_error&) {
    caught = true;
  }
  CHECK(caught);
  std::atomic<int> n{0};
  dapple::parallelFor(1000, [&](int) { ++n; }, 1);
  CHECK(n == 1000);
}

// ---- polynomial exp error bounds ----------------------------------------------------------------------------

void testSpatialKernelExp() {
  const SpatialKernels& K = selectSpatialKernels();
  // exp2Scale: 2^(3.3219 x) for log10 exposures in [-6, 3] (the print-exposure encode)
  std::vector<float> v(4096);
  for (std::size_t i = 0; i < v.size(); ++i) v[i] = -6.f + 9.f * float(i) / float(v.size() - 1);
  std::vector<float> w = v;
  K.exp2Scale(w.data(), int(w.size()), 3.32192809f);
  double worst = 0;
  for (std::size_t i = 0; i < v.size(); ++i) {
    const double ref = std::exp2(double(v[i] * 3.32192809f));  // same float product: isolates the polynomial
    worst = std::max(worst, std::fabs(double(w[i]) - ref) / ref);
  }
  std::printf("  exp2Scale polynomial max relative error vs std::exp2: %.2e\n", worst);
  CHECK(worst < 4e-6);  // degree-5 Taylor on [-0.5, 0.5]: truncation error 2.4e-6
  // boostRow vs the double-precision formula
  const float fx0 = 0.4f, fX = 3.f, fa = 2.5f, fk = 0.2f;
  std::vector<float> r(2000);
  for (std::size_t i = 0; i < r.size(); ++i) r[i] = 0.01f + float(fX) * float(i) / float(r.size());
  std::vector<float> o = r;
  K.boostRow(o.data(), int(o.size()), fx0, fX, fa, fk);
  double worstAbs = 0, worstRel = 0;
  for (std::size_t i = 0; i < r.size(); ++i) {
    double ref = r[i];
    if (r[i] > fx0) {
      const double d = (double(r[i]) - fx0) / fX;
      ref = r[i] + double(fX) * fk * (std::exp(double(fa) * d) - double(fa) * d - 1.0);
    }
    worstAbs = std::max(worstAbs, std::fabs(o[i] - ref));
    worstRel = std::max(worstRel, std::fabs(o[i] - ref) / std::max(1e-3, ref));
  }
  std::printf("  boostRow max error vs std::exp: abs %.2e, rel %.2e\n", worstAbs, worstRel);
  CHECK(worstRel < 1e-5);
}

void testLibraryErrors() {
  // a failed lazy load is reported, not swallowed
  film::setLibraryPathLazy(fs::path("definitely") / "not" / "there.dfp");
  CHECK(film::defaultLibrary() == nullptr);
  CHECK(!film::lastLibraryError().empty());
  // restoring a library clears the error
  film::setLibrary(library());
  CHECK(film::defaultLibrary() != nullptr);
  CHECK(film::lastLibraryError().empty());
}

}  // namespace

int main(int argc, char** argv) {
  struct T {
    const char* id;
    const char* name;
    void (*fn)();
  };
  const T tests[] = {{"profiles", "profiles + film pack", testProfiles},
                     {"colorimetry", "colorimetry + CAM16", testColorimetry},
                     {"upsample", "spectral upsampling table", testUpsampleTable},
                     {"grey", "grey neutral through Portra 400 + Endura", testGreyNeutral},
                     {"spec_vectors", "spec reference vectors (info)", testSpecVectors},
                     {"baked_vs_direct", "baked vs direct dE2000", testBakedVsDirect},
                     {"dir_params", "DIR, params JSON, rebake", testDirAndParams},
                     {"render_paths", "render paths + hooks + auto exposure", testRenderPaths},
                     {"smoke_all", "all stocks smoke", testSmokeAllStocks},
                     {"gauss_mixture", "gaussian mixture engine", testGaussianMixtureEngine},
                     {"spatial_plans", "spatial plans (halation, DIR, diffusion filter, boost)", testSpatialPlans},
                     {"grain_stats", "grain statistics (mean-preserving, variance, stateless)", testGrainStatistics},
                     {"spatial_render", "spatial render (determinism, tiles, no-spatial gating, unsharp)", testSpatialRender},
                     {"glare_enlarger", "glare + enlarger diffusion + camera filter", testGlareAndEnlargerDiffusion},
                     {"spatial_extras", "half-res mixture, boost, single-layer grain, micro-structure", testSpatialExtras},
                     {"editparams_json", "EditParams film keys (JSON, combine, schema)", testEditParamsFilmJson},
                     {"develop_film", "develop pipeline with film (renderer)", testDevelopFilmIntegration},
                     {"film_match", "film exposure keeps the photo's key (film_match.h)", testFilmMatch},
                     {"c_api", "film C API", testFilmCApi},
                     {"validation", "parameter validation + extreme values", testParamValidation},
                     {"strong_guarantee", "setParams strong exception guarantee", testSetParamsStrongGuarantee},
                     {"robust_render", "NaN/Inf input, degenerate sizes, odd tiles (bit-identical)", testRobustRender},
                     {"render_rows", "renderRows == render", testRenderRows},
                     {"pack_format", "pack format DFP2 (LE, CRC, truncation, fuzz, unknown sections)", testPackFormat},
                     {"cache_keys", "bake cache: spatial-only changes never re-bake, bake fields always do", testCacheKeys},
                     {"concurrency", "concurrent acquireSimulator + setParams/render", testConcurrency},
                     {"pool_exceptions", "thread pool rethrows job exceptions", testPoolExceptions},
                     {"kernel_exp", "polynomial exp error bounds (spatial kernels)", testSpatialKernelExp},
                     {"library_errors", "library load errors are reported", testLibraryErrors}};
  // `dapple_film_tests` runs everything; `dapple_film_tests <id>...` runs the named tests (one ctest entry each);
  // `--list` prints the ids.
  std::vector<std::string> only;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--list")) {
      for (const T& t : tests) std::printf("%s\n", t.id);
      return 0;
    }
    only.push_back(argv[i]);
  }
  for (const std::string& o : only) {
    bool found = false;
    for (const T& t : tests) found = found || o == t.id;
    if (!found) {
      std::fprintf(stderr, "unknown test id '%s'\n", o.c_str());
      return 2;
    }
  }
  for (const T& t : tests) {
    if (!only.empty() && std::find(only.begin(), only.end(), std::string(t.id)) == only.end()) continue;
    const int before = g_failures;
    const auto t0 = std::chrono::steady_clock::now();
    t.fn();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("[%s] %s (%.0f ms)\n", g_failures == before ? " ok " : "FAIL", t.name, ms);
  }
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
