// dapple film-bench | film-render-bench: per-stage timings of the film engine (median of N runs).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>

#include "colour_metrics.h"
#include "dapple/camera.h"
#include "dapple/developer.h"
#include "dapple/edit_params.h"
#include "dapple/film.h"
#include "dapple/renderer.h"
#include "film_common.h"

namespace fs = std::filesystem;
using namespace dapple;
using namespace dapple::film;

namespace dapple::filmtool {

ImageF makeBenchImage(const std::string& in, double mp, std::string& err) {
  ImageF src;
  if (!in.empty() && !readPfm(P(in), src, err)) return {};
  const int W = int(std::sqrt(mp * 1e6 * 1.5)), H = int(mp * 1e6 / W);
  ImageF img(W, H);
  for (int y = 0; y < H; ++y) {
    float* r = img.row(y);
    for (int x = 0; x < W; ++x) {
      if (!src.empty()) {
        // mirror-tile the photo so local statistics stay photographic
        int sx = x % (2 * src.width), sy = y % (2 * src.height);
        if (sx >= src.width) sx = 2 * src.width - 1 - sx;
        if (sy >= src.height) sy = 2 * src.height - 1 - sy;
        std::memcpy(r + 3 * x, src.row(sy) + 3 * sx, 12);
      } else {
        const float u = float(x) / W, v = float(y) / H;
        r[3 * x] = 0.02f + 2.0f * u * u;
        r[3 * x + 1] = 0.02f + 1.2f * v;
        r[3 * x + 2] = 0.02f + 0.9f * (1 - u) * v;
      }
    }
  }
  return img;
}

double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

int cmdBench(const std::vector<std::string>& args) {
  const Args a = parse(args);
  std::string err;
  FilmParams p = paramsFromArgs(a, err);
  if (!err.empty()) {
    std::fprintf(stderr, "film-bench: %s\n", err.c_str());
    return 2;
  }
  if (!a.has("--with-effects")) p.disableSpatialAndStochastic();
  auto lib = loadLibrary(a.get("--pack"), err);
  if (!lib) {
    std::fprintf(stderr, "film-bench: %s\n", err.c_str());
    return 1;
  }
  const int runs = std::max(3, std::atoi(a.get("--runs", "9").c_str()));
  const double mp = std::atof(a.get("--mp", "26").c_str());
  ImageF big = makeBenchImage(a.get("--in"), mp, err);
  if (big.empty()) {
    std::fprintf(stderr, "film-bench: %s\n", err.c_str());
    return 1;
  }
  ImageF small = resizeToLongEdge(big, 1600);
  std::printf("CPU: %s, %u hardware threads, kernel set: ", cpuName().c_str(), std::thread::hardware_concurrency());
  {
    FilmSimulator probe(lib);
    std::printf("%s\n", probe.kernelName());
  }
  std::printf("film %s + %s, input %dx%d (%.1f MP) and %dx%d, median of %d runs, no spatial effects\n\n", p.film.c_str(),
              p.paper.c_str(), big.width, big.height, double(big.width) * big.height / 1e6, small.width, small.height, runs);

  // bake: cold (new simulator: raw LUT built) and warm (parameter change, raw LUT cached)
  std::vector<double> cold, coldRaw, coldCurves, coldNeutral, coldLut, warm;
  for (int i = 0; i < runs; ++i) {
    FilmSimulator s(lib);
    FilmTimings t;
    if (!s.setParams(p, &err, &t)) {
      std::fprintf(stderr, "film-bench: %s\n", err.c_str());
      return 1;
    }
    cold.push_back(t.bakeMs);
    coldRaw.push_back(t.rawLutMs);
    coldCurves.push_back(t.curvesMs);
    coldNeutral.push_back(t.neutralMs);
    coldLut.push_back(t.lutMs);
  }
  FilmSimulator sim(lib);
  FilmTimings t0;
  sim.setParams(p, &err, &t0);
  for (int i = 0; i < runs; ++i) {
    FilmParams q = p;
    q.enlarger.printExposure = 1.0 + 0.01 * (i + 1);  // forces a re-bake, raw LUT cached
    FilmTimings t;
    sim.setParams(q, &err, &t);
    warm.push_back(t.bakeMs);
  }
  sim.setParams(p, &err, &t0);
  std::printf("bake, cold (incl. raw LUT)   %7.1f ms   [raw LUT %.1f, curves+DIR %.1f, neutral filtration+gain %.1f, 33^3 print+scan LUT %.1f]\n",
              median(cold), median(coldRaw), median(coldCurves), median(coldNeutral), median(coldLut));
  std::printf("bake, warm (raw LUT cached)  %7.1f ms\n", median(warm));

  ImageF out;
  FilmTimings t;
  sim.render(small, out, &t);  // warm-up
  std::vector<double> prev, full, st, sd, sp, sa;
  for (int i = 0; i < runs; ++i) {
    sim.render(small, out, &t);
    prev.push_back(t.runMs);
  }
  sim.render(big, out, &t);
  for (int i = 0; i < runs; ++i) {
    sim.render(big, out, &t);
    full.push_back(t.runMs);
  }
  for (int i = 0; i < runs; ++i) {
    sim.renderStaged(big, out, &t);
    sa.push_back(t.exposureMs);
    sd.push_back(t.developMs);
    sp.push_back(t.printScanMs);
    st.push_back(t.runMs);
  }
  std::vector<double> meter;
  for (int i = 0; i < 3; ++i) {
    const auto tm = Clock::now();
    FilmSimulator::meterEv(big, "center_weighted");
    meter.push_back(msSince(tm));
  }
  const double fullMs = median(full);
  std::printf("\n%.1f MP full resolution, no spatial effects\n", double(big.width) * big.height / 1e6);
  std::printf("  fused streaming run        %7.1f ms   (%.1f Mpx/s)\n", fullMs, double(big.width) * big.height / 1e3 / fullMs);
  std::printf("  staged run (same kernels)  %7.1f ms   [exposure+log %.1f | develop+DIR %.1f | print+scan LUT %.1f]  (thread-time / hw threads)\n",
              median(st), median(sa), median(sd), median(sp));
  std::printf("%dx%d preview, cached bake   %7.2f ms\n", small.width, small.height, median(prev));
  std::printf("auto-exposure meter (26 MP, center_weighted, 256 px thumbnail)  %7.1f ms\n", median(meter));

  // ---- spatial effects: full resolution, preview, and per-effect ablation -------------------------------------------
  {
    FilmParams full = paramsFromArgs(a, err);  // spatial + stochastic effects as given (defaults: everything on)
    auto timeIt = [&](const FilmParams& q, const ImageF& img, FilmTimings* last = nullptr) {
      FilmSimulator s(lib);
      FilmTimings t, bt2;
      std::string e2;
      if (!s.setParams(q, &e2, &bt2)) return -1.0;
      s.render(img, out, &t);  // warm-up (workspace allocation, first touch)
      std::vector<double> v;
      for (int i = 0; i < runs; ++i) {
        s.render(img, out, &t);
        v.push_back(t.runMs);
      }
      if (last) *last = t;
      return median(v);
    };
    const double mpix = double(big.width) * big.height / 1e6;
    FilmTimings st;
    const double allMs = timeIt(full, big, &st);
    std::printf("\n%.1f MP full resolution, ALL spatial + stochastic effects (halation, DIR diffusion, grain, glare, unsharp)\n", mpix);
    std::printf("  total                      %7.1f ms   (%.1f Mpx/s)   [exposure->planar %.1f | camera filter/blur %.1f | scatter+halation %.1f | DIR field+diffusion %.1f | tile pass (develop, grain, LUT, glare, unsharp, encode) %.1f]\n",
                allMs, mpix * 1e3 / allMs, st.spExposureMs, st.spCameraMs, st.spHalationMs, st.spDirMs, st.spTileMs);
    FilmTimings sst;
    const double smallAll = timeIt(full, small, &sst);
    FilmParams prev = full;
    prev.settings.previewMode = true;
    const double smallPrev = timeIt(prev, small);
    std::printf("%dx%d preview, cached bake: all effects %.2f ms; preview_mode (spec s7: no grain / DIR diffusion / blurs) %.2f ms\n", small.width,
                small.height, smallAll, smallPrev);
    std::printf("  preview all-effects stages: exposure->planar %.2f | camera %.2f | scatter+halation %.2f | DIR %.2f | tile pass %.2f ms\n",
                sst.spExposureMs, sst.spCameraMs, sst.spHalationMs, sst.spDirMs, sst.spTileMs);
    std::printf("\nper-effect cost at %.1f MP (each enabled alone on top of the no-spatial pipeline), total ms:\n", mpix);
    struct Abl {
      const char* name;
      void (*apply)(FilmParams&);
    };
    const Abl abl[] = {
        {"none (no-spatial)", [](FilmParams&) {}},
        {"scatter + halation", [](FilmParams& q) { q.filmRender.halation.active = true; }},
        {"DIR diffusion", [](FilmParams& q) { q.filmRender.dirCouplers.diffusionSizeUm = 20; q.filmRender.dirCouplers.diffusionTailWeight = 0.06; }},
        {"grain", [](FilmParams& q) { q.filmRender.grain.active = true; q.filmRender.grain.blur = 0.65; q.filmRender.grain.blurDyeCloudsUm = 1.0; }},
        {"glare (stochastic)", [](FilmParams& q) { q.printRender.glare.active = true; q.printRender.glare.blur = 0.5; }},
        {"unsharp mask 0.7 px", [](FilmParams& q) { q.scanner.unsharpMask = {0.7, 0.7}; }},
        {"camera Pro-Mist filter", [](FilmParams& q) { q.camera.diffusionFilter.active = true; }},
        {"scatter+halation+DIR+grain+glare+unsharp", [](FilmParams&) {}},
    };
    for (const Abl& ab : abl) {
      FilmParams q = full;
      if (std::strcmp(ab.name, "scatter+halation+DIR+grain+glare+unsharp") != 0) {
        q.disableSpatialAndStochastic();
        q.debug.deactivateSpatialEffects = false;
        q.debug.deactivateStochasticEffects = false;
        q.filmRender.dirCouplers.diffusionSizeUm = 0;
        q.filmRender.halation.active = false;
        q.filmRender.grain.active = false;
        q.printRender.glare.active = false;
        q.scanner.unsharpMask = {0, 0};
        ab.apply(q);
      }
      std::printf("  %-42s %7.1f ms\n", ab.name, timeIt(q, big));
    }
    // tile size sweep (all effects, full resolution): the default tile is 256 x 64
    std::printf("\ntile size sweep at %.1f MP, all effects (settings.tile_width x tile_height; default 256x64), total ms:\n", mpix);
    const int tiles[][2] = {{64, 16}, {128, 32}, {128, 64}, {256, 32}, {256, 64}, {256, 128}, {512, 64}, {512, 128}, {1024, 64}};
    for (const auto& tl : tiles) {
      FilmParams q = full;
      q.settings.tileWidth = tl[0];
      q.settings.tileHeight = tl[1];
      std::printf("  %4d x %-4d %7.1f ms\n", tl[0], tl[1], timeIt(q, big));
    }
  }

  // accuracy of the baked path against the direct path on a pixel sample
  {
    double worst = 0, mean = 0;
    int n = 0;
    uint32_t seed = 1;
    auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return double(seed >> 8) / double(1u << 24); };
    for (int i = 0; i < 2000; ++i) {
      const double rgb[3] = {rnd() * rnd() * 1.5, rnd() * rnd() * 1.5, rnd() * rnd() * 1.5};
      double d[3];
      sim.renderPixelDirect(rgb, d);
      const float f[3] = {float(rgb[0]), float(rgb[1]), float(rgb[2])};
      float o[3];
      sim.renderPixelBaked(f, o);
      const float da[3] = {float(d[0]), float(d[1]), float(d[2])};
      const double de = metrics::deltaE2000(metrics::srgbToLab(da), metrics::srgbToLab(o));
      worst = std::max(worst, de);
      mean += de;
      ++n;
    }
    std::printf("\nbaked vs direct (2000 random colours): mean dE2000 %.4f, max %.3f\n", mean / n, worst);
  }
  return 0;
}

// dapple film-render-bench [--runs N] [--mp 26] [--film ID] [--paper ID]
// The develop pipeline with the film simulation switched on (Renderer): full resolution and 1600 px preview, with and
// without spatial effects, one cold render and repeated renders that reuse the cached bake.
int cmdRenderBench(const std::vector<std::string>& args) {
  const Args a = parse(args);
  std::string err;
  auto lib = loadLibrary(a.get("--pack"), err);
  if (!lib) {
    std::fprintf(stderr, "film-render-bench: %s\n", err.c_str());
    return 1;
  }
  film::setLibrary(lib);
  const int runs = std::max(3, std::atoi(a.get("--runs", "7").c_str()));
  SourceImage big;
  big.linear = makeBenchImage(a.get("--in"), std::atof(a.get("--mp", "26").c_str()), err);
  if (big.linear.empty()) {
    std::fprintf(stderr, "film-render-bench: %s\n", err.c_str());
    return 1;
  }
  const CameraRegistry cams = CameraRegistry::withBuiltins();
  const CameraProfile& cam = cams.resolve("Sony", "ILCE-7RM5");
  EditParams film;
  film.filmStock = a.get("--film", "kodak_portra_400");
  film.filmPaper = a.get("--paper", "");
  const EditParams normal;
  SourceImage proxy;
  proxy.linear = resizeToLongEdge(big.linear, 256);
  std::printf("CPU: %s, %u hardware threads\n", cpuName().c_str(), std::thread::hardware_concurrency());
  std::printf("%dx%d (%.1f MP) source, film %s, median of %d runs\n\n", big.linear.width, big.linear.height,
              double(big.linear.width) * big.linear.height / 1e6, film.filmStock.c_str(), runs);

  auto mk = [&](const EditParams& p) {
    Renderer r(p, cam, nullptr);
    r.setToneProxy(&proxy);
    return r;
  };
  auto timeIt = [&](const EditParams& p, SpatialQuality q, int long_edge, bool freshBake, int n) {
    Renderer r = mk(p);
    FastOptions fo;
    fo.spatial = q;
    fo.region.outLongEdge = long_edge;
    std::vector<double> v;
    std::shared_ptr<const BakedLut> bake;
    for (int i = 0; i < n + 1; ++i) {
      const auto t0 = Clock::now();
      if (freshBake || !bake) bake = mk(p).bake(big, 33);
      int w = 0, h = 0;
      std::uint8_t* px = r.renderBakedRgba(big, *bake, fo, w, h);
      std::free(px);
      if (i > 0) v.push_back(msSince(t0));  // first run warms workspaces / pool
    }
    return median(v);
  };
  struct Row {
    const char* name;
    SpatialQuality q;
    int edge;
  };
  const Row rows[] = {{"26 MP full, no spatial", SpatialQuality::Off, 0},
                      {"26 MP full, all effects (halation, DIR, grain, ...)", SpatialQuality::Full, 0},
                      {"1600 px preview, no spatial", SpatialQuality::Off, 1600},
                      {"1600 px preview, all effects", SpatialQuality::Full, 1600},
                      {"1600 px preview, interactive quality", SpatialQuality::Interactive, 1600}};
  std::printf("%-56s %10s %10s %10s\n", "ms (RGBA8 out)", "normal", "film cold", "film cached");
  for (const Row& r : rows) {
    const double n0 = timeIt(normal, r.q, r.edge, false, runs);
    const double cold = timeIt(film, r.q, r.edge, true, runs);
    const double warm = timeIt(film, r.q, r.edge, false, runs);
    std::printf("%-56s %10.1f %10.1f %10.1f\n", r.name, n0, cold, warm);
  }
  // slider drag: every frame changes a non-spatial film parameter (print warmth) -> re-bake of the print path
  {
    Renderer r0 = mk(film);
    auto bake = r0.bake(big, 33);
    FastOptions fo;
    fo.spatial = SpatialQuality::Interactive;
    fo.region.outLongEdge = 1600;
    std::vector<double> v;
    for (int i = 0; i < runs + 1; ++i) {
      EditParams p = film;
      p.filmWarmth = float(i) * 3.f;
      const auto t0 = Clock::now();
      Renderer r = mk(p);
      auto b = r.bake(big, 33);
      int w = 0, h = 0;
      std::free(r.renderBakedRgba(big, *b, fo, w, h));
      if (i > 0) v.push_back(msSince(t0));
    }
    std::printf("\n1600 px interactive frame while dragging print warmth (bake + render): %.1f ms\n", median(v));
  }
  return 0;
}

}  // namespace dapple::filmtool
