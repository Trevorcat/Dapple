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
#include "colour_metrics.h"
#include "spatial.h"

namespace fs = std::filesystem;
using namespace dapple;

namespace dapple::benchtool {
namespace {

using Clock = std::chrono::steady_clock;
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
  bool quick = false;    // --quick: skip the legacy per-stage tables and the accuracy section (calibration runs)
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
  const CameraProfile& cam = cameras.resolve(src->meta.make, src->meta.model);
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
    // --- stage breakdown on the core API (what one legacy frame executes) ---
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
                      Renderer r(noSpatial, cam, nullptr);
                      pointOut = r.renderFloat(scaled, false);
                    })});
    rows.push_back({"  2a toneMove analysis only (resize 256 + proxy render)",
                    timeMs(repeat, [&] { (void)analyzeTone(scaled, cam, sc.params); })});
    auto spatialOnly = [&](const char* name, auto&& setup) {
      SpatialParams sp;
      setup(sp);
      rows.push_back({name, timeMs(repeat, [&] {
                        ImageF im = pointOut;
                        applySpatial(im, sp);
                      })});
    };
    rows.push_back({"  (copy of float image, for reference)", timeMs(repeat, [&] { ImageF im = pointOut; (void)im; })});
    spatialOnly("3a spatial: sharpen", [&](SpatialParams& s) { s.sharpen = sc.params.sharpen / 100.f; s.sharpRadius = sc.params.sharpRadius / 10.f; });
    spatialOnly("3b spatial: chroma NR", [&](SpatialParams& s) { s.nrColor = sc.params.nrColor / 100.f; });
    spatialOnly("3c spatial: luma NR", [&](SpatialParams& s) { s.nrLuma = std::max(sc.params.nrLuma, 30.f) / 100.f; });
    spatialOnly("3d spatial: clarity", [&](SpatialParams& s) { s.clarity = 0.4f; });
    spatialOnly("3e spatial: texture", [&](SpatialParams& s) { s.texture = 0.3f; });
    spatialOnly("3f spatial: dehaze", [&](SpatialParams& s) { s.dehaze = 0.25f; });
    spatialOnly("3g spatial: grain", [&](SpatialParams& s) { s.grain = 0.2f; });
    rows.push_back({"4 Renderer::render total (point+spatial+u8)", timeMs(repeat, [&] {
                      Renderer r(sc.params, cam, nullptr);
                      (void)r.render(scaled);
                    })});
    printRows("per-stage, core API, 1600 long edge (legacy)", rows);
    }

    // --- end to end through the C API, as the app calls it ---
    const std::string pj = toJson(sc.params);
    std::vector<Row> e2e;
    if (!quick) {
    e2e.push_back({"legacy: dapple_session_render(1600)", timeMs(repeat, [&] {
                     DappleImage im{};
                     dapple_session_render(sess, pj.c_str(), nullptr, 1600, &im);
                     dapple_free_image(&im);
                   })});
    e2e.push_back({"legacy: dapple_session_analyze_params_json", timeMs(repeat, [&] {
                     char* j = dapple_session_analyze_params_json(sess, pj.c_str());
                     dapple_free_string(j);
                   })});
    const double legacyFrame = e2e[0].ms + e2e[1].ms;
    e2e.push_back({"legacy frame = render + analyze", legacyFrame});
    printRows("C API end to end (legacy)", e2e);
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
      lut = rr.bake(s1600, 49);
      FastOptions fo;
      fo.spatial = SpatialQuality::Off;
      st.push_back({"lattice pass 1067x1600 -> RGBA8 (no spatial)", timeMs(repeat, [&] {
                      int w, h;
                      std::free(rr.renderBakedRgba(s1600, *lut, fo, w, h));
                    })});
      fo.spatial = SpatialQuality::Full;
      st.push_back({"lattice pass + all spatial, 1600", timeMs(repeat, [&] {
                      int w, h;
                      std::free(rr.renderBakedRgba(s1600, *lut, fo, w, h));
                    })});
      SourceImage s1080;
      s1080.sceneReferred = src->sceneReferred;
      s1080.linear = resizeToLongEdge(src->linear, 1080);
      fo.spatial = SpatialQuality::Interactive;
      fo.noise = false;
      st.push_back({"lattice pass + interactive spatial, 1080", timeMs(repeat, [&] {
                      int w, h;
                      std::free(rr.renderBakedRgba(s1080, *lut, fo, w, h));
                    })});
      fo.spatial = SpatialQuality::Off;
      st.push_back({"lattice pass only, 1080", timeMs(repeat, [&] {
                      int w, h;
                      std::free(rr.renderBakedRgba(s1080, *lut, fo, w, h));
                    })});
      printRows("new path stages (core API)", st);

      // accuracy: baked vs direct, point-wise stages only, over all pixels of the 1600 render
      EditParams ns = sc.params;
      ns.clarity = ns.texture = ns.dehaze = ns.grain = 0;
      ns.sharpen = ns.nrLuma = ns.nrColor = 0;
      Renderer rd(ns, cam, nullptr);
      rd.setToneProxy(&proxy);
      const ImageF ref = rd.renderFloat(s1600, false);
      for (int n : quick ? std::vector<int>{} : std::vector<int>{17, 25, 29, 33, 37, 41, 49, 65}) {
        auto l = rd.bake(s1600, n);
        FastOptions o2;
        o2.spatial = SpatialQuality::Off;
        const ImageF got = rd.renderBakedFloat(s1600, *l, o2);
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
      FastOptions off;
      off.spatial = SpatialQuality::Off;
      Renderer r0(ns, cam, nullptr);
      r0.setToneProxy(&proxy);
      const auto base = r0.bake(s1600, 65);
      for (float ev : {0.35f, -0.6f}) {
        EditParams q = ns;
        q.exposure += ev;
        Renderer r(q, cam, nullptr);
        r.setToneProxy(&proxy);
        const ImageF ref = r.renderFloat(s1600, false);
        const auto fast = r.bake(s1600, 41, base.get(), true);
        const auto exactAn = r.bake(s1600, 41, base.get(), false);
        std::printf("  %-34s exposure %+.2f EV, reused 65^3: fast analysis %.4f  exact analysis %.4f\n", sc.name, ev,
                    meanDe(r.renderBakedFloat(s1600, *fast, off), ref), meanDe(r.renderBakedFloat(s1600, *exactAn, off), ref));
      }
      EditParams q = ns;
      q.saturation += 12.f;
      Renderer r(q, cam, nullptr);
      r.setToneProxy(&proxy);
      const ImageF ref = r.renderFloat(s1600, false);
      for (int n : {41, 49, 57, 65}) {
        const auto l = r.bake(s1600, n, nullptr, true);  // as a drag frame: fresh lattice, fast analysis
        std::printf("  %-34s saturation +12, bake %d^3: %.4f\n", sc.name, n, meanDe(r.renderBakedFloat(s1600, *l, off), ref));
      }
    }
  }
  {
    char* info = dapple_engine_info_json();  // frame engine / bake counters (DAPPLE_ENGINE), for comparing runs
    if (info) std::printf("\nengine info: %s\n", info);
    dapple_free_string(info);
  }
  dapple_session_close(sess);
  return 0;
}

}  // namespace dapple::benchtool
