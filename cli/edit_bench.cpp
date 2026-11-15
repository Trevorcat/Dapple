// dapple edit-bench <raw> [--repeat N] [--threads N]
//
// Benchmark of the editor's interactive path: opens a session the way the app does (preview quality,
// long edge 2048), then times, per stage and per scenario, what one slider-drag frame costs.
// DAPPLE_THREADS=N limits the engine thread pool (to emulate an 8-core phone on a bigger desktop).

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "dapple/auto_adjust.h"
#include "dapple/camera.h"
#include "dapple/dapple_c.h"
#include "dapple/decoder.h"
#include "dapple/edit_params.h"
#include "dapple/renderer.h"
#include "dapple/tone.h"
#include "engine/exec/engine_session.h"
#include "colour_metrics.h"
#include "engine/nodes/dehaze/dehaze.h"
#include "engine/nodes/detail/detail.h"
#include "engine/nodes/grain/grain.h"
#include "engine/nodes/nr/nr.h"
#include "spatial.h"

namespace fs = std::filesystem;
using namespace dapple;

namespace dapple::benchtool {
namespace {

// A frame of the engine's baked variant (lattice `n`, fast analysis as the interactive frames take it) as float, through `es` (its result cache is
// what lets a later frame reuse the lattice of an earlier one). `proxy` is the tone proxy of the session.
ImageF bakedFrameF(engine::EngineSession& es, const EditParams& p, const CameraProfile& cam, const SourceImage& proxy, const SourceImage& src, int n,
                   bool fastAnalysis) {
  Renderer r(p, cam, nullptr);
  r.setToneProxy(&proxy);
  engine::FrameJob job{p, r, src, &proxy, cam, nullptr, {}, {}, {}};
  job.request.bakeSize = n;
  job.request.fastAnalysis = fastAnalysis;
  job.request.noise = false;
  job.request.interactive = fastAnalysis;
  job.fastOptions.spatial = SpatialQuality::Off;
  job.fastOptions.noise = false;
  ImageF out;
  job.outFloat = &out;
  engine::FrameResult res;
  if (!es.renderFrame(job, res)) std::fprintf(stderr, "edit-bench: engine frame failed: %s\n", res.error.c_str());
  return out;
}
ImageF exactF(const EditParams& p, const CameraProfile& cam, const SourceImage& proxy, const SourceImage& src) {
  Renderer r(p, cam, nullptr);
  r.setToneProxy(&proxy);
  engine::EngineSession es;
  engine::EngineSession::ExactOptions o;
  o.spatial = false;
  o.hasProxy = true;
  ImageF out;
  o.outFloat = &out;
  Image8 unused;
  std::string err;
  if (!es.renderExact(p, r, src, o, unused, &err)) std::fprintf(stderr, "edit-bench: engine exact render failed: %s\n", err.c_str());
  return out;
}

using Clock = std::chrono::steady_clock;

// One neighbourhood stage alone on a whole picture (the stage functions the CPU nodes run; NR amounts go through the NR family's block).
// the develop amounts of the stages the bench runs one at a time
struct BenchAmounts {
  float nrLuma = 0.f, nrColor = 0.f, dehaze = 0.f, clarity = 0.f, texture = 0.f, sharpen = 0.f, sharpRadius = 1.f, grain = 0.f;
};
void benchSpatial(ImageF& img, const BenchAmounts& a) {
  const float nrLuma = a.nrLuma, nrColor = a.nrColor, dehaze = a.dehaze, grain = a.grain;
  const int w = img.width, h = img.height;
  const float scale = float(std::max(w, h)) / 2048.f;
  SpatialScratch sc;
  if (const engine::nr::LumaBlock b = engine::nr::lumaFor(nrLuma, scale); b.active()) stageNRLuma(img, sc, b.radius, b.blend, b.inv2);
  if (const engine::nr::ChromaBlock b = engine::nr::chromaFor(nrColor, scale); b.active())
    stageNRChroma(img, sc, b.factor, b.lowRadius, b.lumaRadius, b.blend, b.inv2);
  if (const engine::dehaze::Block b = engine::dehaze::dehazeFor(dehaze, scale); b.solve()) {
    DehazeSolve sv;
    std::vector<float> low;
    int lw = 0, lh = 0;
    dehazeLowGrid(img, low, lw, lh);
    dehazeSolveGrid(low, lw, lh, b.minRadius, b.boxRadius, b.omega, sv);
    stageDehazeApply(img, sc, 0, 0, w, h, sv);
  } else if (b.active()) {
    stageDehazeVeil(img, sc, b.veil, b.veilAir);
  }
  if (const engine::detail::Block b = engine::detail::detailFor(a.clarity, a.texture, a.sharpen, a.sharpRadius, 0.f, scale); b.active())
    stageDetail(img, sc, b, nullptr, 0, 0, w, h);
  if (const engine::grain::Block b = engine::grain::grainFor(grain, scale); b.active()) stageGrain(img, b.period, b.inv, b.amp, 0, 0);
}
double msSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

// median of `n` runs after one warm-up
double timeMs(int n, const std::function<void()>& fn) {
  fn();
  std::vector<double> v;
  for (int i = 0; i < n; ++i) {
    const auto t0 = Clock::now();
    fn();
    v.push_back(msSince(t0));
  }
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

struct Row {
  std::string name;
  double ms;
};

void printRows(const char* title, const std::vector<Row>& rows) {
  std::printf("\n%s\n", title);
  double total = 0;
  for (const Row& r : rows) std::printf("  %-46s %8.2f ms\n", r.name.c_str(), r.ms);
  (void)total;
}

}  // namespace

int editBench(const std::vector<std::string>& args) {
  std::string path;
  int repeat = 8;
  int onlyScenario = 0;  // --scenario N: run just scenario N (1 or 2)
  bool quick = false;    // --quick: skip the per-stage tables and the accuracy section (calibration runs)
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--repeat" && i + 1 < args.size()) repeat = std::atoi(args[++i].c_str());
    else if (args[i] == "--scenario" && i + 1 < args.size()) onlyScenario = std::atoi(args[++i].c_str());
    else if (args[i] == "--quick") quick = true;
    else path = args[i];
  }
  if (path.empty()) {
    std::fputs("usage: dapple edit-bench <raw> [--repeat N]   (env DAPPLE_THREADS=N)\n", stderr);
    return 2;
  }
  const fs::path p(std::u8string(path.begin(), path.end()));
  const auto decoders = DecoderRegistry::withBuiltins();
  const auto cameras = CameraRegistry::withBuiltins();
  const ImageDecoder* dec = decoders.find(p);
  if (!dec) {
    std::fputs("unsupported file\n", stderr);
    return 1;
  }

  // ---- open as the app does: preview (half size) decode, resized to 2048 ----
  auto t0 = Clock::now();
  auto src = dec->decode(p, DecodeOptions{DecodeQuality::Preview});
  if (!src) {
    std::fprintf(stderr, "decode: %s\n", src.error().message.c_str());
    return 1;
  }
  const double decodeMs = msSince(t0);
  t0 = Clock::now();
  src->linear = resizeToLongEdge(src->linear, 2048);
  const double resizeOpenMs = msSince(t0);
  const CameraProfile cam = effectiveCamera(cameras, src->meta);
  std::printf("%s\nsession source %dx%d (decode %.0f ms + resize %.0f ms)\n", path.c_str(), src->linear.width,
              src->linear.height, decodeMs, resizeOpenMs);

  // ---- scenarios ----
  EditParams autoP = computeAutoAdjust(*src, cam).correction;
  autoP.toneStrength = 60.f;  // app default (settings.toneStrength)
  EditParams heavy = autoP;
  heavy.clarity = 40;
  heavy.texture = 30;
  heavy.nrLuma = 30;
  heavy.sharpen = 80;
  heavy.dehaze = 25;
  heavy.vibrance = 20;
  heavy.hsl[kOrange].saturation = 15;
  heavy.gradeShadows = {220, 30, 0};
  heavy.gradeHighlights = {40, 25, 0};
  struct Scenario {
    const char* name;
    EditParams params;
  };
  const Scenario scen[] = {{"default auto edit", autoP}, {"clarity/texture/NR/sharpen/dehaze", heavy}};

  DappleSession* sess = dapple_session_open(path.c_str(), DAPPLE_QUALITY_PREVIEW, 2048);
  if (!sess) {
    std::fprintf(stderr, "c api open failed: %s\n", dapple_last_error());
    return 1;
  }

  int scenNo = 0;
  for (const Scenario& sc : scen) {
    ++scenNo;
    if (onlyScenario && scenNo != onlyScenario) continue;
    std::printf("\n================ %s ================\n", sc.name);
    // --- stage breakdown on the core API (what one exact frame executes) ---
    std::vector<Row> rows;
    SourceImage scaled;
    scaled.sceneReferred = src->sceneReferred;
    rows.push_back({"1 resizeToLongEdge 2048 -> 1600", timeMs(repeat, [&] { scaled.linear = resizeToLongEdge(src->linear, 1600); })});
    if (!quick) {
    EditParams noSpatial = sc.params;
    noSpatial.clarity = noSpatial.texture = noSpatial.dehaze = noSpatial.grain = 0;
    noSpatial.sharpen = 0;
    noSpatial.nrLuma = 0;
    noSpatial.nrColor = 0;
    ImageF pointOut;
    rows.push_back({"2 pointwise (tone/colour/curves), incl. toneMove analysis", timeMs(repeat, [&] {
                      std::string err;
                      engine::renderImageF(noSpatial, cam, nullptr, scaled, false, pointOut, &err);
                    })});
    rows.push_back({"  2a toneMove analysis only (resize 256 + proxy render)",
                    timeMs(repeat, [&] { (void)analyzeTone(scaled, cam, sc.params); })});
    auto spatialOnly = [&](const char* name, auto&& setup) {
      BenchAmounts am;
      setup(am);
      rows.push_back({name, timeMs(repeat, [&] {
                        ImageF im = pointOut;
                        benchSpatial(im, am);
                      })});
    };
    rows.push_back({"  (copy of float image, for reference)", timeMs(repeat, [&] { ImageF im = pointOut; (void)im; })});
    spatialOnly("3a spatial: sharpen", [&](BenchAmounts& s) { s.sharpen = sc.params.sharpen / 100.f; s.sharpRadius = sc.params.sharpRadius / 10.f; });
    spatialOnly("3b spatial: chroma NR", [&](BenchAmounts& s) { s.nrColor = sc.params.nrColor / 100.f; });
    spatialOnly("3c spatial: luma NR", [&](BenchAmounts& s) { s.nrLuma = std::max(sc.params.nrLuma, 30.f) / 100.f; });
    spatialOnly("3d spatial: clarity", [&](BenchAmounts& s) { s.clarity = 0.4f; });
    spatialOnly("3e spatial: texture", [&](BenchAmounts& s) { s.texture = 0.3f; });
    spatialOnly("3f spatial: dehaze", [&](BenchAmounts& s) { s.dehaze = 0.25f; });
    spatialOnly("3g spatial: grain", [&](BenchAmounts& s) { s.grain = 0.2f; });
    rows.push_back({"4 engine exact render total (point+spatial+u8)", timeMs(repeat, [&] {
                      Image8 o8;
                      std::string err;
                      engine::renderImage8(sc.params, cam, nullptr, scaled, o8, &err);
                    })});
    printRows("per-stage, core API, 1600 long edge (exact)", rows);
    }

    // --- end to end through the C API, as the app calls it ---
    const std::string pj = toJson(sc.params);
    std::vector<Row> e2e;
    if (!quick) {
    e2e.push_back({"exact: dapple_session_render(1600)", timeMs(repeat, [&] {
                     DappleImage im{};
                     dapple_session_render(sess, pj.c_str(), nullptr, 1600, &im);
                     dapple_free_image(&im);
                   })});
    e2e.push_back({"exact: dapple_session_analyze_params_json", timeMs(repeat, [&] {
                     char* j = dapple_session_analyze_params_json(sess, pj.c_str());
                     dapple_free_string(j);
                   })});
    const double exactFrame = e2e[0].ms + e2e[1].ms;
    e2e.push_back({"exact frame = render + analyze", exactFrame});
    printRows("C API end to end (exact)", e2e);
    }

    // ---- new path: baked lattice + one call per frame ----
    std::vector<Row> nw;
    auto frame = [&](const char* name, std::string (*vary)(const EditParams&, int), int flags, int longEdge, bool analysis) {
      int k = 0;
      return Row{name, timeMs(repeat, [&] {
                   const std::string j = vary ? vary(sc.params, ++k) : pj;
                   DappleFrameRequest rq{};
                   rq.size = sizeof rq;
                   rq.long_edge = longEdge;
                   rq.flags = flags | (analysis ? DAPPLE_FRAME_ANALYSIS : 0);
                   DappleImage im{};
                   char* an = nullptr;
                   dapple_session_render_frame(sess, j.c_str(), nullptr, &rq, &im, &an);
                   dapple_free_image(&im);
                   dapple_free_string(an);
                 })};
    };
    // a slider drag changes one point-wise parameter per frame -> lattice re-baked every frame
    auto dragExposure = [](const EditParams& b, int k) {
      EditParams q = b;
      q.exposure = b.exposure + 0.002f * float(k % 100);
      return toJson(q);
    };
    // spatial-only drag (clarity): lattice cache hit
    auto dragClarity = [](const EditParams& b, int k) {
      EditParams q = b;
      q.clarity = float(k % 60);
      return toJson(q);
    };
    // colour drag (saturation): the colour lattice itself changes -> re-baked every frame
    auto dragSaturation = [](const EditParams& b, int k) {
      EditParams q = b;
      q.saturation = b.saturation + 0.1f * float(k % 100);
      return toJson(q);
    };
    nw.push_back(frame("interactive 1080, exposure drag (re-bake 41^3)", dragExposure, DAPPLE_FRAME_INTERACTIVE, 1080, true));
    nw.push_back(frame("interactive 1080, saturation drag (bake 41^3)", dragSaturation, DAPPLE_FRAME_INTERACTIVE, 1080, true));
    nw.push_back(frame("interactive 1080, clarity drag (lattice cached)", dragClarity, DAPPLE_FRAME_INTERACTIVE, 1080, true));
    nw.push_back(frame("interactive 1600, exposure drag", dragExposure, DAPPLE_FRAME_INTERACTIVE, 1600, true));
    nw.push_back(frame("final 1600, params unchanged (lattice cached)", nullptr, 0, 1600, true));
    nw.push_back(frame("final 1600 after point-wise change (bake 65^3)", dragExposure, 0, 1600, true));
    nw.push_back(frame("reference (exact) 1600 + analysis", nullptr, DAPPLE_FRAME_EXACT, 1600, true));
    printRows("new path: dapple_session_render_frame (image + analysis in one call)", nw);

    // stage breakdown of the new path (core API)
    {
      std::vector<Row> st;
      const SourceImage& s1600 = scaled;
      SourceImage proxy;
      proxy.sceneReferred = src->sceneReferred;
      proxy.linear = resizeToLongEdge(src->linear, 256);
      Renderer rr(sc.params, cam, nullptr);
      rr.setToneProxy(&proxy);
      std::shared_ptr<const BakedLut> lut;
      st.push_back({"bake 41^3 (incl. analysis)", timeMs(repeat, [&] { lut = rr.bake(s1600, 41); })});
      st.push_back({"bake 49^3", timeMs(repeat, [&] { lut = rr.bake(s1600, 49); })});
      st.push_back({"bake 65^3", timeMs(repeat, [&] { lut = rr.bake(s1600, 65); })});
      printRows("table stages (core API)", st);

      // accuracy: baked vs direct, point-wise stages only, over all pixels of the 1600 render
      EditParams ns = sc.params;
      ns.clarity = ns.texture = ns.dehaze = ns.grain = 0;
      ns.sharpen = ns.nrLuma = ns.nrColor = 0;
      const ImageF ref = exactF(ns, cam, proxy, s1600);
      for (int n : quick ? std::vector<int>{} : std::vector<int>{17, 25, 29, 33, 37, 41, 49, 65}) {
        engine::EngineSession es;
        const ImageF got = bakedFrameF(es, ns, cam, proxy, s1600, n, false);
        std::vector<double> de;
        de.reserve(ref.data.size() / 3 / 3 + 1);
        for (std::size_t i = 0; i < ref.data.size() / 3; i += 3) {
          const auto a = metrics::srgbToLab(&ref.data[3 * i]);
          const auto b = metrics::srgbToLab(&got.data[3 * i]);
          de.push_back(metrics::deltaE2000(a, b));
        }
        std::sort(de.begin(), de.end());
        double mean = 0;
        for (double d : de) mean += d;
        mean /= double(de.size());
        std::printf("  baked %2d^3 vs direct: mean dE2000 %.4f  p99 %.3f  p99.9 %.3f  max %.2f\n", n, mean,
                    de[std::size_t(de.size() * 0.99)], de[std::size_t(de.size() * 0.999)], de.back());
      }
    }
  }
  // ---- accuracy of what a drag frame shows (point-wise stages; spatial off in both) vs the exact pipeline, float,
  // same metric as above. Tone drag: the lattice of the final frame (65^3) is reused with the fast analysis (what
  // dapple_session_render_frame does for a 光线 / white-balance drag); colour drag: a fresh n^3 bake. ----
  if (!quick) {
    std::printf("\ndrag frames vs exact (point-wise, 1600, float, mean dE2000)\n");
    SourceImage s1600;
    s1600.sceneReferred = src->sceneReferred;
    s1600.linear = resizeToLongEdge(src->linear, 1600);
    SourceImage proxy;
    proxy.sceneReferred = src->sceneReferred;
    proxy.linear = resizeToLongEdge(src->linear, 256);
    auto meanDe = [](const ImageF& a, const ImageF& b) {
      if (a.data.size() != b.data.size() || a.data.empty()) return -1.0;
      double m = 0;
      std::size_t n = 0;
      for (std::size_t i = 0; i < a.data.size() / 3; i += 3, ++n)
        m += metrics::deltaE2000(metrics::srgbToLab(&a.data[3 * i]), metrics::srgbToLab(&b.data[3 * i]));
      return m / double(n);
    };
    int sn = 0;
    for (const Scenario& sc : scen) {
      ++sn;
      if (onlyScenario && sn != onlyScenario) continue;
      EditParams ns = sc.params;
      ns.clarity = ns.texture = ns.dehaze = ns.grain = 0;
      ns.sharpen = ns.nrLuma = ns.nrColor = 0;
      for (float ev : {0.35f, -0.6f}) {
        EditParams q = ns;
        q.exposure += ev;
        const ImageF ref = exactF(q, cam, proxy, s1600);
        double de[2];
        for (int fastAn = 1; fastAn >= 0; --fastAn) {
          engine::EngineSession es;  // the final frame's 65^3 lattice is cached; the drag frame reuses it, only the tone table is rebuilt
          bakedFrameF(es, ns, cam, proxy, s1600, 65, false);
          de[fastAn] = meanDe(bakedFrameF(es, q, cam, proxy, s1600, 41, fastAn != 0), ref);
        }
        std::printf("  %-34s exposure %+.2f EV, reused 65^3: fast analysis %.4f  exact analysis %.4f\n", sc.name, ev, de[1], de[0]);
      }
      EditParams q = ns;
      q.saturation += 12.f;
      const ImageF ref = exactF(q, cam, proxy, s1600);
      for (int n : {41, 49, 57, 65}) {
        engine::EngineSession es;  // as a drag frame: fresh lattice, fast analysis
        std::printf("  %-34s saturation +12, bake %d^3: %.4f\n", sc.name, n, meanDe(bakedFrameF(es, q, cam, proxy, s1600, n, true), ref));
      }
    }
  }
  {
    char* info = dapple_engine_info_json();  // frame engine / bake counters, for comparing runs
    if (info) std::printf("\nengine info: %s\n", info);
    dapple_free_string(info);
  }
  dapple_session_close(sess);
  return 0;
}

}  // namespace dapple::benchtool
