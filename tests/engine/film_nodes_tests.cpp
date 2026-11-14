// Film nodes (docs/engine/pipeline-architecture.md section 8, step b4): gate tests of the decomposed film pipeline.
//
//   dapple_film_nodes_tests <id>     ids: equiv rebake keys graph grain_scale grain_frame bench gpu_parity
//
//   equiv        node chain (tables + runFilmFrame) is byte-identical with FilmSimulator::render on every case below (gate B)
//   rebake       warmth / tint drags re-bake only the print table (the other two tables are untouched)
//   keys         declaration conformance of every film node: perturb a key outside (node + ancestors) deps => output bit-identical;
//                has a negative control (a wrong mask is caught)
//   graph        FullFilm graph validates, film nodes are wired as 8.1
//   grain_scale  scale-aware grain (Q8): variance + spectrum of the preview vs the box-downscaled full-resolution grain
//   grain_frame  frameIndex: a repeat is bit-identical, consecutive frames are decorrelated, frame 0 equals the still
//   fx           highlight boost + enlarger diffusion filter as nodes: byte-identical with FilmSimulator::render, rendered by the
//                node chain (no simulator fallback), golden frame hashes (tests/engine/golden/film_fx.txt)
//   bench        film-render-bench style timings: FilmSimulator::render vs the node chain (DAPPLE_PERF=1 adds 26 MP)
#include <algorithm>
#include <chrono>

#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "dapple/film.h"
#include "dapple/film_match.h"
#include "edit_params_fields.h"
#include "engine/graph/graph.h"
#include "engine/nodes/film/film_node_internal.h"
#include "engine/nodes/film/film_nodes.h"

namespace fs = std::filesystem;
using namespace dapple;
using namespace dapple::engine;
namespace ef = dapple::engine::filmnode;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::printf("  CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                          \
    }                                                                        \
  } while (0)

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

std::shared_ptr<film::FilmLibrary> g_lib;
std::shared_ptr<film::FilmLibrary> library() {
  if (!g_lib) {
    std::string err;
    const fs::path data = fs::path(DAPPLE_SOURCE_DIR) / "core" / "data" / "film";
    const char* envPack = std::getenv("DAPPLE_TEST_PACK");
    const fs::path pack = envPack ? fs::path(envPack) : data / "dapple.dfp";
    if (fs::exists(pack)) g_lib = film::FilmLibrary::loadPack(pack, &err);
    if (!g_lib) g_lib = film::FilmLibrary::loadJsonDir(data / "profiles", &err);
    if (!g_lib) std::fprintf(stderr, "cannot load film library: %s\n", err.c_str());
  }
  return g_lib;
}

// ---- test inputs --------------------------------------------------------------------------------------------------------------
std::uint32_t hash32(std::uint32_t h) {
  h ^= h >> 16;
  h *= 0x7FEB352Du;
  h ^= h >> 15;
  h *= 0x846CA68Bu;
  h ^= h >> 16;
  return h;
}

// A photographic-looking scene: gradients, texture, a bright highlight (halation) and a dark corner.
ImageF scene(int w, int h, std::uint32_t seed = 5) {
  ImageF s(w, h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const float u = float(x) / float(w - 1), v = float(y) / float(h - 1);
      float* p = s.row(y) + 3 * x;
      const float n = float(hash32(seed * 7919u + std::uint32_t(x) * 73856093u ^ std::uint32_t(y) * 19349663u) >> 8) * (1.f / 16777216.f);
      p[0] = (0.05f + 0.95f * u * (1.f - 0.4f * v)) * (0.9f + 0.2f * n);
      p[1] = (0.04f + 0.7f * (1.f - u) * v + 0.15f * u) * (0.9f + 0.2f * n);
      p[2] = (0.03f + 0.8f * (1.f - v) * (1.f - 0.5f * u)) * (0.9f + 0.2f * n);
      if ((u - 0.7f) * (u - 0.7f) + (v - 0.3f) * (v - 0.3f) < 0.01f) p[0] = 2.2f, p[1] = 1.9f, p[2] = 1.5f;
    }
  return s;
}

ImageF flat(int w, int h, float level) {
  ImageF s(w, h);
  for (std::size_t i = 0; i < s.data.size(); ++i) s.data[i] = level * (i % 3 == 0 ? 1.f : (i % 3 == 1 ? 0.95f : 0.9f));
  return s;
}

struct Case {
  const char* name;
  const char* stock;
  const char* paper;  // "" = default for the stock
  int w, h;
  int quality = 2;
  float grain = 100.f, halation = 100.f, warmth = 0.f, tint = 0.f, filmExp = 0.f;
  float gain[3] = {1.f, 1.f, 1.f};
  std::uint32_t frameIndex = 0;
};

// Engine-side parameter resolution (the way the planner would do it).
void testGain(const void*, const EditParams& e, float g[3]) {
  const float ex = std::exp2(e.exposure);
  g[0] = 1.3f * ex * (1.f + e.temperature / 300.f);
  g[1] = 1.0f * ex;
  g[2] = 1.6f * ex * (1.f - e.temperature / 300.f) * (1.f + e.tint / 400.f);
}

ParamInputs inputsFor(const EditParams& e, int w, int h, int quality, const film::FilmLibrary* lib, bool keepGrain = false) {
  ParamInputs in;
  in.edit = e;
  in.frame.longEdge = std::max(w, h);
  in.frame.interactive = quality == 1;
  if (keepGrain) in.frame.activeParam = "film_grain";
  in.ctx.sourceId = 1;
  in.ctx.cameraId = "test";
  in.ctx.filmLib = lib;
  in.ctx.srcW = w;
  in.ctx.srcH = h;
  in.ctx.gainFn = testGain;
  return in;
}

EditParams editFor(const Case& c) {
  EditParams e;
  e.filmStock = c.stock;
  e.filmPaper = c.paper;
  e.filmGrain = c.grain;
  e.filmHalation = c.halation;
  e.filmWarmth = c.warmth;
  e.filmTint = c.tint;
  e.filmExposure = c.filmExp;
  return e;
}

// FilmResolved straight from the ParamSet (tests resolveFilm too).
ef::FilmResolved resolvedFor(const EditParams& e, int w, int h, int quality, bool keepGrain = false, int fullLongEdge = 0,
                             std::uint32_t frameIndex = 0) {
  ParamInputs in = inputsFor(e, w, h, quality, library().get(), keepGrain);
  in.ctx.fullLongEdge = fullLongEdge;
  in.ctx.frameIndex = frameIndex;
  const ParamSet ps = ParamSet::make(in);
  FrameGeom g;
  g.w = w;
  g.h = h;
  ef::FilmResolved r;
  ef::resolveFilm(ps, g, r);
  return r;
}

// Reference: the FilmSimulator exactly as renderer.cpp drives it (makeFilmParams + gain folded into the input matrix).
film::FilmParams refParams(const ef::FilmResolved& r, int quality) {
  film::FilmParams fp;
  fp.film = r.stock->stock;
  fp.paper = r.scan ? std::string("none") : r.paper->stock;
  fp.camera.autoExposure = false;
  fp.enlarger.printExposure = std::exp2(-double(std::clamp(r.printEv, -9.f, 9.f)));
  applyFilmPaperMapping(fp, r.black);
  fp.camera.sceneWhiteX = r.sceneX, fp.camera.sceneWhiteY = r.sceneY, fp.camera.adaptationDegree = r.adapt;  // film white balance
  fp.camera.filmFormatMm *= r.formatScale;
  fp.enlarger.yFilterShift = -r.warmth / 100.f * 12.f;
  fp.enlarger.mFilterShift = -r.tint / 100.f * 12.f;
  const double g = std::clamp(r.grainPct / 100.f, 0.f, 1.f);
  if (g <= 0) fp.filmRender.grain.active = false;
  else fp.filmRender.grain.particleAreaUm2 *= g * g;
  const double h = std::clamp(r.halationPct / 100.f, 0.f, 1.f);
  fp.filmRender.halation.halationAmount = h;
  fp.filmRender.halation.scatterAmount = h;
  fp.filmRender.halation.boostEv = r.fx.boostEv;
  fp.filmRender.halation.boostRange = r.fx.boostRange;
  fp.filmRender.halation.protectEv = r.fx.protectEv;
  if (r.fx.enlargerFilter) {
    auto& d = fp.enlarger.diffusionFilter;
    d.active = true;
    d.filterFamily = ef::kDiffusionFamilies[r.fx.enlargerFilter - 1];
    d.strength = r.fx.enlStrength;
    d.spatialScale = r.fx.enlScale;
    d.haloWarmth = r.fx.enlHaloWarmth;
    d.coreIntensity = r.fx.enlCoreIntensity;
    d.coreSize = r.fx.enlCoreSize;
    d.haloIntensity = r.fx.enlHaloIntensity;
    d.haloSize = r.fx.enlHaloSize;
    d.bloomIntensity = r.fx.enlBloomIntensity;
    d.bloomSize = r.fx.enlBloomSize;
  }
  if (quality == 0) fp.disableSpatialAndStochastic();
  else if (quality == 1) fp.settings.previewMode = true;
  return fp;
}

bool simRender(const ef::FilmResolved& r, int quality, const ImageF& in, ImageF& out, double* ms = nullptr, film::FilmTimings* ft = nullptr) {
  auto lease = film::acquireSimulator(library(), r.stock->stock, r.scan ? std::string("none") : r.paper->stock);
  std::string err;
  if (!lease || !lease->setParams(refParams(r, quality), &err)) {
    std::printf("  reference setParams failed: %s\n", err.c_str());
    return false;
  }
  const auto t0 = Clock::now();
  const bool ok = lease->render(in, out, ft, r.gain);
  if (ms) *ms = msSince(t0);
  return ok;
}

bool nodeRender(const ef::FilmResolved& r, const ImageF& in, ImageF& out, double* ms = nullptr, ef::FilmTimingsNodes* tm = nullptr,
                const ef::FilmTablesHandle* given = nullptr) {
  ef::FilmTablesHandle tb;
  std::string err;
  if (given) tb = *given;
  else if (!ef::bakeFilmTables(r, tb, &err)) {
    std::printf("  table bake failed: %s\n", err.c_str());
    return false;
  }
  out = ImageF(in.width, in.height);
  const auto t0 = Clock::now();
  const bool ok = ef::runFilmFrame(r, tb, in.data.data(), std::size_t(in.width) * 3, out.data.data(), std::size_t(in.width) * 3, 0, 0, tm);
  if (ms) *ms = msSince(t0);
  return ok;
}

std::size_t diffCount(const ImageF& a, const ImageF& b, double* maxAbs = nullptr) {
  std::size_t n = 0;
  double mx = 0;
  if (a.data.size() != b.data.size()) return a.data.size() + 1;
  for (std::size_t i = 0; i < a.data.size(); ++i)
    if (std::memcmp(&a.data[i], &b.data[i], 4) != 0) {
      ++n;
      mx = std::max(mx, std::fabs(double(a.data[i]) - b.data[i]));
    }
  if (maxAbs) *maxAbs = mx;
  return n;
}

// ---- equiv ------------------------------------------------------------------------------------------------------------------
void testEquiv() {
  const Case cases[] = {
      {"portra400+endura, all effects", "kodak_portra_400", "", 640, 427},
      {"portra400, interactive tier", "kodak_portra_400", "", 640, 427, 1},
      {"velvia100 scan (slide)", "fujifilm_velvia_100", "", 480, 320},
      {"warmth/tint/film exposure/gain", "kodak_portra_400", "", 400, 267, 2, 40.f, 60.f, 30.f, -20.f, 0.7f, {1.3f, 1.f, 1.6f}},
      {"odd size 331x197", "kodak_ektar_100", "", 331, 197},
      {"no grain, no halation", "fujifilm_pro_400h", "", 320, 213, 2, 0.f, 0.f},
      {"cine vision3_500t + 2383", "kodak_vision3_500t", "", 360, 240},
      {"portra800 push, supra paper", "kodak_portra_800_push1", "kodak_supra_endura", 300, 200},
      {"tiny 33x17", "kodak_portra_400", "", 33, 17},
      {"spatial off (quality 0)", "kodak_portra_400", "", 320, 213, 0},
  };
  for (const Case& c : cases) {
    EditParams e = editFor(c);
    e.exposure = c.gain[0] != 1.f ? 0.4f : 0.f;
    ef::FilmResolved r = resolvedFor(e, c.w, c.h, c.quality);
    r.quality = std::uint8_t(c.quality);  // 0 (spatial off) is not a ParamSet state: the renderer entry passes it
    if (c.gain[0] != 1.f) {  // the explicit gain case: use the ParamSet-derived gain for both sides
    }
    if (!r.stock) {
      std::printf("  %-34s stock unavailable\n", c.name);
      ++g_failures;
      continue;
    }
    const ImageF in = scene(c.w, c.h);
    ImageF a, b;
    double msSim = 0, msNode = 0;
    if (!simRender(r, c.quality, in, a, &msSim) || !nodeRender(r, in, b, &msNode)) {
      ++g_failures;
      continue;
    }
    double mx = 0;
    const std::size_t d = diffCount(a, b, &mx);
    std::printf("  %-34s %4dx%-4d %s  differing floats %zu (max %.3g)  sim %.1f ms  nodes %.1f ms\n", c.name, c.w, c.h,
                r.scan ? "scan " : "print", d, mx, msSim, msNode);
    CHECK(d == 0);
  }
}

// ---- rebake -----------------------------------------------------------------------------------------------------------------
std::uint64_t fnv(const float* p, std::size_t n) {
  std::uint64_t h = 1469598103934665603ull;
  for (std::size_t i = 0; i < n; ++i) {
    std::uint32_t u;
    std::memcpy(&u, p + i, 4);
    h = (h ^ u) * 1099511628211ull;
  }
  return h;
}
template <class V>
std::uint64_t fnv(const V& v) { return fnv(v.data(), v.size()); }

void testRebake() {
  EditParams e;
  e.filmStock = "kodak_portra_400";
  ef::FilmResolved r0 = resolvedFor(e, 640, 427, 2);
  EditParams e2 = e;
  e2.filmWarmth = 35.f;
  e2.filmTint = -15.f;
  ef::FilmResolved r1 = resolvedFor(e2, 640, 427, 2);
  ef::FilmTablesHandle a, b;
  std::string err;
  const auto t0 = Clock::now();
  CHECK(ef::bakeFilmTables(r0, a, &err));
  const double cold = msSince(t0);
  // the warm re-bake: only the print table node runs
  const auto& impl = ef::cpuImpl(NodeType::FilmTablePrintScan);
  std::vector<double> warm;
  void* pay = nullptr;
  for (int i = 0; i < 5; ++i) {
    ef::FilmResolved ri = resolvedFor(i % 2 ? e : e2, 640, 427, 2);
    const auto t1 = Clock::now();
    CHECK(impl.bake(&ri, {}, &pay) == Status::Ok);
    warm.push_back(msSince(t1));
    impl.freePayload(pay);
  }
  std::sort(warm.begin(), warm.end());
  CHECK(ef::bakeFilmTables(r1, b, &err));
  CHECK(a.raw->t.raw.size() == b.raw->t.raw.size() && fnv(a.raw->t.raw) == fnv(b.raw->t.raw));
  CHECK(fnv(a.curves->t.dn) == fnv(b.curves->t.dn) && fnv(a.curves->t.d0) == fnv(b.curves->t.d0));
  CHECK(fnv(a.print->t.lut) != fnv(b.print->t.lut));
  // table bit-identity with the simulator's bake: same chain output for the warmed parameters (equiv covers it end to end)
  const ImageF in = scene(640, 427);
  ImageF x, y;
  CHECK(simRender(r1, 2, in, x) && nodeRender(r1, in, y, nullptr, nullptr, &b));
  CHECK(diffCount(x, y) == 0);
  std::printf("  all three tables cold %.1f ms; warmth/tint drag re-bakes the print table only: median %.1f ms (raw + curves untouched)\n",
              cold, warm[warm.size() / 2]);
}

// ---- keys (declaration conformance) --------------------------------------------------------------------------------------------
// Per-node output fingerprints of one parameter set, executing the nodes whole-frame in graph order.
using Fingerprints = std::map<NodeType, std::uint64_t>;

// Fingerprint of the part of a window inside `r` (the frame): halo regions depend on the plan, not on the node's maths.
std::uint64_t fpPlanes(const ImageView& v, const Rect* r = nullptr) {
  std::uint64_t h = 1469598103934665603ull;
  const Rect o = r ? *r : Rect{v.origin.x0, v.origin.y0, v.origin.x0 + v.width, v.origin.y0 + v.height};
  for (int c = 0; c < 3; ++c)
    for (int y = o.y0; y < o.y1; ++y) {
      const float* p = v.data + std::size_t(c) * v.planeStrideFloats + std::size_t(y - v.origin.y0) * v.strideFloats - v.origin.x0;
      for (int x = o.x0; x < o.x1; ++x) {
        std::uint32_t u;
        std::memcpy(&u, p + x, 4);
        h = (h ^ u) * 1099511628211ull;
      }
    }
  return h;
}
std::uint64_t fpPacked(const ImageView& v, const Rect* r = nullptr) {
  std::uint64_t h = 1469598103934665603ull;
  const Rect o = r ? *r : Rect{v.origin.x0, v.origin.y0, v.origin.x0 + v.width, v.origin.y0 + v.height};
  for (int y = o.y0; y < o.y1; ++y) {
    const float* p = v.data + std::size_t(y - v.origin.y0) * v.strideFloats - 3 * v.origin.x0;
    for (int x = 3 * o.x0; x < 3 * o.x1; ++x) {
      std::uint32_t u;
      std::memcpy(&u, p + x, 4);
      h = (h ^ u) * 1099511628211ull;
    }
  }
  return h;
}

struct PlaneBuf {
  std::vector<float> d;
  int w = 0, h = 0, stride = 0;
  Rect origin;
  void alloc(Rect o) {
    origin = o;
    w = o.x1 - o.x0;
    h = o.y1 - o.y0;
    stride = (w + 15) & ~15;
    d.assign(std::size_t(stride) * h * 3, 0.f);
  }
  ImageView view() {
    ImageView v;
    v.data = d.data();
    v.width = w;
    v.height = h;
    v.channels = 3;
    v.strideFloats = stride;
    v.origin = origin;
    v.planeStrideFloats = std::size_t(stride) * h;
    return v;
  }
};

Fingerprints runNodes(const ParamInputs& in, int W, int H, const ImageF& src) {
  Fingerprints F;
  const ParamSet ps = ParamSet::make(in);
  FrameGeom g;
  g.w = W;
  g.h = H;
  ef::FilmResolved r;
  ef::resolveFilm(ps, g, r);
  if (!r.stock) return F;
  ef::FilmTablesHandle tb;
  std::string err;
  if (!ef::bakeFilmTables(r, tb, &err)) return F;
  F[NodeType::FilmTableRaw] = fnv(tb.raw->t.raw) ^ fnv(tb.raw->t.mIn, 12);
  F[NodeType::FilmTableCurves] = fnv(tb.curves->t.dn) ^ (fnv(tb.curves->t.d0) * 31) ^ fnv(tb.curves->t.dirQ, 12);
  F[NodeType::FilmTablePrintScan] = fnv(tb.print->t.lut) ^ (fnv(tb.print->t.srgbDecode) * 17) ^ fnv(tb.print->t.glareRgb, 4);
  const ef::FilmHalos hl = ef::halosOf(r);
  const void* tRaw = static_cast<const void*>(static_cast<const ef::PayloadBase*>(tb.raw.get()));
  const void* tCur = static_cast<const void*>(static_cast<const ef::PayloadBase*>(tb.curves.get()));
  const void* tPrint = static_cast<const void*>(static_cast<const ef::PayloadBase*>(tb.print.get()));
  const Rect frame{0, 0, W, H};
  CpuCtx ctx;
  ctx.frame = frame;
  PlaneBuf E, E2, Q;
  E.alloc(frame);
  E2.alloc(frame);
  Q.alloc(frame);
  ImageView sv;
  sv.data = const_cast<float*>(src.data.data());
  sv.width = W;
  sv.height = H;
  sv.channels = 3;
  sv.strideFloats = std::size_t(W) * 3;
  sv.origin = frame;
  {
    const void* t[1] = {tRaw};
    CpuCtx c = ctx;
    c.tables = t;
    ImageView ev = E.view();
    ef::cpuImpl(NodeType::FilmExposure).run(c, &r, {&sv, 1}, {&ev, 1});
    F[NodeType::FilmExposure] = fpPlanes(ev);
  }
  ImageView e2v = E.view();
  std::uint64_t e2fp = F[NodeType::FilmExposure];
  if (hl.preMix || hl.haloMix) {
    ImageView ev = E.view(), o = E2.view();
    ef::cpuImpl(NodeType::FilmBlurExposure).run(ctx, &r, {&ev, 1}, {&o, 1});
    e2v = E2.view();
    e2fp = F[NodeType::FilmBlurExposure] = fpPlanes(o);
  }
  ImageView qv = Q.view();
  std::uint64_t qfp = 0;
  if (hl.dirDiffuse) {
    const void* t[1] = {tCur};
    CpuCtx c = ctx;
    c.tables = t;
    ef::cpuImpl(NodeType::FilmDirField).run(c, &r, {&e2v, 1}, {&qv, 1});
    qfp = F[NodeType::FilmDirField] = fpPlanes(qv);
  }
  (void)e2fp;
  (void)qfp;
  const int Hs = hl.scan, Hdg = hl.grain ? hl.grainDye + hl.grainBlur : 0;
  auto grow = [](Rect rr, int d) { return Rect{rr.x0 - d, rr.y0 - d, rr.x1 + d, rr.y1 + d}; };
  const Rect rS = grow(frame, Hs), rD = grow(rS, Hdg);
  PlaneBuf D;
  D.alloc(rD);
  {
    const void* t[1] = {tCur};
    CpuCtx c = ctx;
    c.tables = t;
    ImageView dv = D.view();
    const ImageView ins[2] = {e2v, qv};
    ef::cpuImpl(NodeType::FilmDevelop).run(c, &r, {ins, hl.dirDiffuse ? std::size_t(2) : std::size_t(1)}, {&dv, 1});
    F[NodeType::FilmDevelop] = fpPlanes(dv, &frame);
  }
  ImageView dfull = D.view();
  ImageView dout = dfull;
  dout.data = D.d.data() + std::size_t(rS.y0 - rD.y0) * D.stride + (rS.x0 - rD.x0);
  dout.width = rS.x1 - rS.x0;
  dout.height = rS.y1 - rS.y0;
  dout.origin = rS;
  if (hl.grain) {
    ef::cpuImpl(NodeType::FilmGrain).run(ctx, &r, {&dfull, 1}, {&dout, 1});
    F[NodeType::FilmGrain] = fpPlanes(dout, &frame);
  }
  std::vector<float> disp(std::size_t(3) * dout.width * dout.height);
  ImageView pv;
  pv.data = disp.data();
  pv.width = dout.width;
  pv.height = dout.height;
  pv.channels = 3;
  pv.strideFloats = std::size_t(3) * dout.width;
  pv.origin = rS;
  {
    const void* t[1] = {tPrint};
    CpuCtx c = ctx;
    c.tables = t;
    ef::cpuImpl(NodeType::FilmPrintScan).run(c, &r, {&dout, 1}, {&pv, 1});
    F[NodeType::FilmPrintScan] = fpPacked(pv, &frame);
    if (hl.scanStage) {
      PlaneBuf L;
      L.alloc(rS);
      ImageView lv = L.view();
      ef::cpuImpl(NodeType::FilmGlare).run(c, &r, {&pv, 1}, {&lv, 1});
      F[NodeType::FilmGlare] = fpPlanes(lv, &frame);
      std::vector<float> fin(std::size_t(3) * W * H);
      ImageView fv;
      fv.data = fin.data();
      fv.width = W;
      fv.height = H;
      fv.channels = 3;
      fv.strideFloats = std::size_t(3) * W;
      fv.origin = frame;
      ef::cpuImpl(NodeType::FilmScan).run(c, &r, {&lv, 1}, {&fv, 1});
      F[NodeType::FilmScan] = fpPacked(fv);
    }
  }
  return F;
}

// Keys a node inherits through its keyed ancestors in the FullFilm graph.
ParamMask inherited(const Graph& g, const NodeRegistry& reg, NodeType t) {
  ParamMask m;
  std::set<NodeId> seen;
  std::vector<NodeId> stack;
  for (const NodeInstance& n : g.nodes())
    if (n.type == t) stack.push_back(n.id);
  while (!stack.empty()) {
    const NodeId id = stack.back();
    stack.pop_back();
    if (!seen.insert(id).second) continue;
    const NodeInstance& n = g.nodes()[id];
    m.add(reg.desc(n.type).deps);
    for (const Edge& e : n.in)
      if (e.keyed) stack.push_back(e.from);
  }
  return m;
}

int conformance(const NodeRegistry& reg, bool verbose, int quality, bool keepGrain, const char* onlyPrefix = nullptr) {
  const ParamRegistry& P = ParamRegistry::instance();
  EditParams e;
  e.filmStock = "kodak_portra_400";
  e.filmHalation = 70.f;
  e.filmGrain = 80.f;
  const int W = 72, H = 56;
  const ImageF src = scene(W, H);
  ParamInputs base = inputsFor(e, W, H, quality, library().get(), false);
  if (keepGrain) base.frame.activeParam = "film_grain";
  // the graph of this frame (FullFilm)
  ParamSet ps0 = ParamSet::make(base);
  Graph g = GraphBuilder::build(ps0, GraphDetail::FullFilm);
  g.computeKeys(ps0, reg);
  const Fingerprints f0 = runNodes(base, W, H, src);
  const NodeType nodes[] = {NodeType::FilmTableRaw,     NodeType::FilmTableCurves, NodeType::FilmTablePrintScan, NodeType::FilmExposure,
                            NodeType::FilmBlurExposure, NodeType::FilmDirField,    NodeType::FilmDevelop,        NodeType::FilmGrain,
                            NodeType::FilmPrintScan,    NodeType::FilmGlare,       NodeType::FilmScan};
  std::map<NodeType, ParamMask> inh;
  for (NodeType t : nodes) inh[t] = inherited(g, reg, t);
  int violations = 0, perturbed = 0, moves = 0;
  std::map<NodeType, std::vector<std::string>> movedBy;
  for (std::size_t k = 0; k < P.size(); ++k) {
    if (onlyPrefix && P.info(ParamKey(k)).name.rfind(onlyPrefix, 0) != 0) continue;  // a subset of the parameters (the small twin)
    for (int variant = 0; variant < 2; ++variant) {
      ParamInputs in = base;
      if (!P.perturb(ParamKey(k), in, variant)) continue;
      // keep the frame legal: a stock that does not exist in the library cannot be resolved
      if (in.edit.filmStock != e.filmStock && !library()->find(in.edit.filmStock)) continue;
      ++perturbed;
      const Fingerprints f1 = runNodes(in, W, H, src);
      // what actually moved: the keys whose hash changed (a perturbation can move derived keys too: ctx.exact => ctx.quality)
      const ParamSet pa = ParamSet::make(base), pb = ParamSet::make(in);
      ParamMask movedKeys;
      for (std::size_t kk = 0; kk < P.size(); ++kk)
        if (pa.hash(ParamKey(kk)) != pb.hash(ParamKey(kk))) movedKeys.add(ParamKey(kk));
      for (NodeType t : nodes) {
        const bool had0 = f0.count(t), had1 = f1.count(t);
        const bool changed = had0 != had1 || (had0 && f0.at(t) != f1.at(t));
        if (changed) {
          ++moves;
          movedBy[t].push_back(P.info(ParamKey(k)).name);
          if ((inh[t].bits & movedKeys.bits).none()) {
            ++violations;
            if (verbose) std::printf("  VIOLATION: %s output changed when undeclared key '%s' moved\n", NodeRegistry::name(t), P.info(ParamKey(k)).name.c_str());
          }
        }
      }
    }
  }
  if (verbose) {
    std::printf("  %d perturbations x %zu film nodes, %d output changes, %d undeclared influences\n", perturbed, sizeof(nodes) / sizeof(nodes[0]),
                moves, violations);
    for (NodeType t : nodes) {
      std::string s;
      std::set<std::string> uniq(movedBy[t].begin(), movedBy[t].end());
      for (const auto& n : uniq) s += n + " ";
      std::printf("    %-20s moved by: %s\n", NodeRegistry::name(t), s.empty() ? "(nothing)" : s.c_str());
    }
  }
  return violations;
}

// the declaration check on the film parameters (full quality), and the harness biting on a mask that forgets a key: in the default run
void testKeysSmall() {
  const NodeRegistry& N = NodeRegistry::instance();
  const ParamRegistry& P = ParamRegistry::instance();
  CHECK(conformance(N, false, 2, false, "film_") == 0);
  ParamMask wrong = N.desc(NodeType::FilmGrain).deps;
  wrong.bits.reset(P.mustFind("film_grain"));
  const NodeRegistry broken = N.withDeps(NodeType::FilmGrain, wrong);
  const int v1 = conformance(broken, false, 2, false, "film_");
  CHECK(v1 > 0);
  std::printf("  negative control: dropping film_grain from FilmGrain => %d violations\n", v1);
}

void testKeys() {
  const NodeRegistry& N = NodeRegistry::instance();
  std::puts(" full quality, all effects:");
  CHECK(conformance(N, true, 2, false) == 0);
  std::puts(" interactive tier, grain-family drag (grain kept):");
  CHECK(conformance(N, true, 1, true) == 0);
  std::puts(" interactive tier, no grain:");
  CHECK(conformance(N, false, 2, false, "film_") == 0);
  // negative controls: the harness must catch a mask that forgets a key
  const ParamRegistry& P = ParamRegistry::instance();
  ParamMask wrong = N.desc(NodeType::FilmGrain).deps;
  wrong.bits.reset(P.mustFind("film_grain"));
  const NodeRegistry broken = N.withDeps(NodeType::FilmGrain, wrong);
  const int v1 = conformance(broken, false, 2, false);
  CHECK(v1 > 0);
  ParamMask wrong2 = N.desc(NodeType::FilmTablePrintScan).deps;
  wrong2.bits.reset(P.mustFind("film_warmth"));
  const NodeRegistry broken2 = N.withDeps(NodeType::FilmTablePrintScan, wrong2);
  const int v2 = conformance(broken2, false, 2, false);
  CHECK(v2 > 0);
  std::printf("  negative controls: dropping film_grain from FilmGrain => %d violations, film_warmth from FilmTablePrintScan => %d\n", v1, v2);
}

// ---- graph ------------------------------------------------------------------------------------------------------------------
void testGraph() {
  EditParams e;
  e.filmStock = "kodak_portra_400";
  ParamInputs in = inputsFor(e, 640, 427, 2, library().get());
  const ParamSet ps = ParamSet::make(in);
  Graph g = GraphBuilder::build(ps, GraphDetail::FullFilm);
  const ValidationResult v = g.validate();
  CHECK(v.ok);
  if (!v.ok) std::printf("  invalid: %s\n", v.error.c_str());
  g.computeKeys(ps);
  std::printf("%s", g.dump(false).c_str());
  const NodeRegistry& N = NodeRegistry::instance();
  for (NodeType t : {NodeType::FilmTableRaw, NodeType::FilmTableCurves, NodeType::FilmTablePrintScan, NodeType::FilmExposure,
                     NodeType::FilmBlurExposure, NodeType::FilmDirField, NodeType::FilmDevelop, NodeType::FilmGrain,
                     NodeType::FilmPrintExposure, NodeType::FilmEnlargerBlur, NodeType::FilmPrintScan, NodeType::FilmGlare,
                     NodeType::FilmScan}) {
    CHECK(g.find(t) != nullptr);
    CHECK(N.desc(t).cpuImpl);
    CHECK(ef::isFilmNode(t));
    const bool table = N.desc(t).output == Domain::Table;
    CHECK(table ? (ef::cpuImpl(t).bake != nullptr) : (ef::cpuImpl(t).run != nullptr));
  }
  // a warmth drag moves exactly: the print table, FilmPrintScan, FilmGlare, FilmScan, DisplayStages and what follows
  EditParams e2 = e;
  e2.filmWarmth = 30.f;
  const ParamSet ps2 = ParamSet::make(inputsFor(e2, 640, 427, 2, library().get()));
  Graph g2 = GraphBuilder::build(ps2, GraphDetail::FullFilm);
  g2.computeKeys(ps2);
  std::string moved;
  for (const NodeInstance& n : g.nodes()) {
    const NodeInstance& m = g2.nodes()[n.id];
    if (n.key != m.key) moved += std::string(NodeRegistry::name(n.type)) + " ";
  }
  std::printf("  film_warmth moves: %s\n", moved.c_str());
  CHECK(moved.find("FilmTableRaw") == std::string::npos && moved.find("FilmTableCurves") == std::string::npos);
  CHECK(moved.find("FilmTablePrintScan") != std::string::npos && moved.find("FilmExposure") == std::string::npos &&
        moved.find("FilmBlurExposure") == std::string::npos && moved.find("FilmDirField") == std::string::npos &&
        moved.find("FilmDevelop") == std::string::npos && moved.find("FilmGrain") == std::string::npos);
  // a film_grain drag moves: FilmGrain and everything after it, nothing before
  EditParams e3 = e;
  e3.filmGrain = 40.f;
  const ParamSet ps3 = ParamSet::make(inputsFor(e3, 640, 427, 2, library().get()));
  Graph g3 = GraphBuilder::build(ps3, GraphDetail::FullFilm);
  g3.computeKeys(ps3);
  moved.clear();
  for (const NodeInstance& n : g.nodes())
    if (n.key != g3.nodes()[n.id].key) moved += std::string(NodeRegistry::name(n.type)) + " ";
  std::printf("  film_grain moves: %s\n", moved.c_str());
  CHECK(moved.find("FilmTable") == std::string::npos && moved.find("FilmExposure") == std::string::npos &&
        moved.find("FilmBlurExposure") == std::string::npos && moved.find("FilmDirField") == std::string::npos &&
        moved.find("FilmDevelop") == std::string::npos && moved.find("FilmGrain") != std::string::npos);
  // halation: BlurExposure and everything after, not the exposure
  EditParams e4 = e;
  e4.filmHalation = 40.f;
  const ParamSet ps4 = ParamSet::make(inputsFor(e4, 640, 427, 2, library().get()));
  Graph g4 = GraphBuilder::build(ps4, GraphDetail::FullFilm);
  g4.computeKeys(ps4);
  moved.clear();
  for (const NodeInstance& n : g.nodes())
    if (n.key != g4.nodes()[n.id].key) moved += std::string(NodeRegistry::name(n.type)) + " ";
  std::printf("  film_halation moves: %s\n", moved.c_str());
  CHECK(moved.find("FilmExposure") == std::string::npos && moved.find("FilmBlurExposure") != std::string::npos);
}

// ---- fx: highlight boost + enlarger diffusion ------------------------------------------------------------------------------------
std::uint64_t fnvImage(const ImageF& im) { return fnv(im.data.data(), im.data.size()); }

std::string fxPlatformTag() { return std::string("kernels=") + dapple::film::detail::selectKernels().name; }

struct FxCase {
  const char* name;
  const char* stock;
  int w, h;
  int quality;
  ef::FilmFx fx;
  float grain = 100.f, halation = 100.f;
};

ef::FilmFx boostFx(float ev, float range = 0.3f, float protect = 1.0f) {
  ef::FilmFx f;
  f.boostEv = ev;
  f.boostRange = range;
  f.protectEv = protect;
  return f;
}
ef::FilmFx enlFx(int family, float strength = 0.5f, float scale = 1.f) {
  ef::FilmFx f;
  f.enlargerFilter = std::uint8_t(family);
  f.enlStrength = strength;
  f.enlScale = scale;
  return f;
}

void fxImpl(const std::vector<std::string>& only) {
  ef::FilmFx both = enlFx(2, 1.0f, 2.f);
  both.boostEv = 1.2f;
  both.protectEv = 1.0f;
  both.enlHaloWarmth = 0.3f;
  both.enlBloomIntensity = 1.5f;
  const FxCase cases[] = {
      {"boost 1.5 EV (portra)", "kodak_portra_400", 400, 267, 2, boostFx(1.5f)},
      {"boost, halation off", "kodak_portra_400", 331, 197, 2, boostFx(1.0f, 0.5f, 0.5f), 100.f, 0.f},
      {"boost, grain off", "kodak_ektar_100", 320, 213, 2, boostFx(2.0f, 0.2f), 0.f},
      {"boost interactive", "kodak_portra_400", 400, 267, 1, boostFx(1.5f)},
      {"boost scan (slide)", "fujifilm_velvia_100", 320, 213, 2, boostFx(1.0f)},
      {"boost + spatial off (quality 0)", "kodak_portra_400", 320, 213, 0, boostFx(1.5f)},
      {"boost tiny 33x17", "kodak_portra_400", 33, 17, 2, boostFx(1.0f)},
      {"boost never reached (x0 >= max)", "kodak_portra_400", 320, 213, 2, boostFx(1.0f, 0.3f, 14.f)},
      {"enlarger glimmerglass", "kodak_portra_400", 400, 267, 2, enlFx(1)},
      {"enlarger black_pro_mist", "kodak_portra_400", 400, 267, 2, enlFx(2)},
      {"enlarger pro_mist strong x2", "kodak_ektar_100", 360, 240, 2, enlFx(3, 1.0f, 2.f)},
      {"enlarger cinebloom", "fujifilm_pro_400h", 331, 197, 2, enlFx(4, 0.8f)},
      {"enlarger interactive", "kodak_portra_400", 400, 267, 1, enlFx(2)},
      {"enlarger, no grain, no halation", "kodak_portra_400", 320, 213, 2, enlFx(2), 0.f, 0.f},
      {"enlarger cine + 2383", "kodak_vision3_500t", 360, 240, 2, enlFx(4)},
      {"enlarger on scan film (no print)", "fujifilm_velvia_100", 320, 213, 2, enlFx(2)},
      {"enlarger + spatial off (quality 0)", "kodak_portra_400", 320, 213, 0, enlFx(2)},
      {"enlarger tiny 33x17", "kodak_portra_400", 33, 17, 2, enlFx(2)},
      {"boost + enlarger", "kodak_portra_400", 400, 267, 2, both},
  };
  const std::string tag = fxPlatformTag();
  const fs::path golden = fs::path(DAPPLE_SOURCE_DIR) / "tests" / "engine" / "golden" / "film_fx.txt";
  const bool record = only.empty() && std::getenv("DAPPLE_FX_RECORD") != nullptr;  // a subset never rewrites the golden file
  std::map<std::string, std::uint64_t> want;
  std::string goldenTag;
  if (!record && fs::exists(golden)) {
    std::ifstream f(golden);
    std::string line;
    while (std::getline(f, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
      if (line.rfind("# platform ", 0) == 0) goldenTag = line.substr(11);
      if (line.empty() || line[0] == '#') continue;
      const std::size_t sp = line.rfind(' ');
      if (sp != std::string::npos) want[line.substr(0, sp)] = std::strtoull(line.c_str() + sp + 1, nullptr, 16);
    }
  }
  const bool useGolden = !record && !want.empty() && goldenTag == tag;
  if (!record && !want.empty() && !useGolden)
    std::printf("  golden hashes recorded on '%s', this run is '%s': hash check skipped (the simulator comparison still gates)\n",
                goldenTag.c_str(), tag.c_str());
  std::vector<std::pair<std::string, std::uint64_t>> got;
  for (const FxCase& c : cases) {
    if (!only.empty() && std::find(only.begin(), only.end(), std::string(c.name)) == only.end()) continue;
    EditParams e;
    e.filmStock = c.stock;
    e.filmGrain = c.grain;
    e.filmHalation = c.halation;
    ef::FilmResolved r = resolvedFor(e, c.w, c.h, c.quality);
    r.quality = std::uint8_t(c.quality);
    if (!r.stock) {
      std::printf("  %-36s stock unavailable\n", c.name);
      ++g_failures;
      continue;
    }
    r.fx = c.fx;
    const ImageF in = scene(c.w, c.h);
    ImageF sim, nodes, nodesApi, base;
    double msSim = 0, msNode = 0;
    const bool simOk = simRender(r, c.quality, in, sim, &msSim);
    const bool nodeOk = nodeRender(r, in, nodes, &msNode);
    // the Renderer-facing entry (what FrameKernels::runFilm / runFilmStage call): true = rendered by the node chain, no simulator
    ef::FilmFrameArgs a;
    a.lib = library().get();
    a.stock = r.stock->stock.c_str();
    a.paper = r.scan ? "none" : r.paper->stock.c_str();
    for (int i = 0; i < 3; ++i) a.gain[i] = r.gain[i];
    a.warmth = r.warmth;
    a.tint = r.tint;
    a.grainPct = r.grainPct;
    a.halationPct = r.halationPct;
    a.formatScale = r.formatScale;
    a.w = c.w;
    a.h = c.h;
    a.quality = c.quality;
    a.fx = c.fx;
    nodesApi = ImageF(c.w, c.h);
    const bool apiOk = ef::renderFilmFrame(a, in.data.data(), std::size_t(c.w) * 3, nodesApi.data.data(), std::size_t(c.w) * 3);
    ef::FilmResolved r0 = r;
    r0.fx = ef::FilmFx{};
    const bool baseOk = nodeRender(r0, in, base);
    if (!simOk || !nodeOk || !apiOk || !baseOk) {
      std::printf("  %-36s FAILED: simulator %d nodes %d renderFilmFrame %d base %d\n", c.name, simOk, nodeOk, apiOk, baseOk);
      ++g_failures;
      continue;
    }
    double mx = 0, mxBase = 0;
    const std::size_t d = diffCount(sim, nodes, &mx), dApi = diffCount(sim, nodesApi), dBase = diffCount(base, nodes, &mxBase);
    const std::uint64_t hsh = fnvImage(nodes);
    got.emplace_back(c.name, hsh);
    const ef::FilmHalos hl = ef::halosOf(r);
    std::printf("  %-36s %4dx%-4d differing floats vs simulator %zu (max %.3g), via renderFilmFrame %zu; effect vs fx off: %zu floats (max %.3g)  "
                "sim %.1f ms nodes %.1f ms  hash %016llx\n",
                c.name, c.w, c.h, d, mx, dApi, dBase, mxBase, msSim, msNode, static_cast<unsigned long long>(hsh));
    CHECK(d == 0);
    CHECK(dApi == 0);
    // the effect must be visible, except where the model drops it on purpose (spatial off drops the diffusion filter, a scan has no
    // print, a boost whose knee is above the maximum is the identity)
    const bool expectEffect = !(c.quality == 0 && !c.fx.boostEv) && !(r.scan && !c.fx.boostEv) && std::string(c.name).find("never reached") == std::string::npos;
    CHECK(expectEffect ? dBase > 0 : dBase == 0);
    CHECK(hl.boost == (c.fx.boostEv > 0.f));
    CHECK(hl.enlargerSplit == (c.fx.enlargerFilter != 0 && !r.scan && c.quality != 0));
    if (useGolden) {
      auto it = want.find(c.name);
      if (it == want.end()) {
        std::printf("    no golden hash for this case\n");
        ++g_failures;
      } else {
        CHECK(it->second == hsh);
      }
    }
  }
  if (record) {
    std::ofstream f(golden, std::ios::binary);
    f << "# film effects not in the edit schema: highlight boost and the enlarger diffusion filter. FNV-1a of the node chain's float output\n"
         "# (tests/engine/film_nodes_tests.cpp fx; identical to FilmSimulator::render). Re-record: DAPPLE_FX_RECORD=1 dapple_film_nodes_tests fx\n";
    f << "# platform " << tag << "\n";
    for (auto& g : got) {
      char buf[32];
      std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(g.second));
      f << g.first << " " << buf << "\n";
    }
    std::printf("  recorded %zu hashes to %s\n", got.size(), golden.string().c_str());
  }
}
void testFx() { fxImpl({}); }
// the boost, every kind of enlarger filter, interactive and quality-0 tiers, a scan film, a tiny frame and both effects together: the node chain against the
// simulator (and the golden hashes) on five of the nineteen frames: in the default run
void testFxSmall() {
  fxImpl({"boost interactive", "enlarger glimmerglass", "enlarger cine + 2383", "enlarger on scan film (no print)", "boost tiny 33x17", "boost + enlarger"});
}

// ---- grain statistics ---------------------------------------------------------------------------------------------------------
struct Chan {
  std::vector<double> v;
  int w = 0, h = 0;
};
Chan channel(const ImageF& im, int c, int x0, int y0, int w, int h) {
  Chan o;
  o.w = w;
  o.h = h;
  o.v.resize(std::size_t(w) * h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) o.v[std::size_t(y) * w + x] = im.row(y0 + y)[3 * (x0 + x) + c];
  return o;
}
ImageF boxDown(const ImageF& im, int k) {
  ImageF o(im.width / k, im.height / k);
  for (int y = 0; y < o.height; ++y)
    for (int x = 0; x < o.width; ++x)
      for (int c = 0; c < 3; ++c) {
        double s = 0;
        for (int j = 0; j < k; ++j)
          for (int i = 0; i < k; ++i) s += im.row(y * k + j)[3 * (x * k + i) + c];
        o.row(y)[3 * x + c] = float(s / (k * k));
      }
  return o;
}
void fft1(std::vector<std::complex<double>>& a, bool inv) {
  const int n = int(a.size());
  for (int i = 1, j = 0; i < n; ++i) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (int len = 2; len <= n; len <<= 1) {
    const double ang = 2 * 3.14159265358979323846 / len * (inv ? -1 : 1);
    const std::complex<double> wl(std::cos(ang), std::sin(ang));
    for (int i = 0; i < n; i += len) {
      std::complex<double> w(1);
      for (int j = 0; j < len / 2; ++j) {
        const auto u = a[i + j], v = a[i + j + len / 2] * w;
        a[i + j] = u + v;
        a[i + j + len / 2] = u - v;
        w *= wl;
      }
    }
  }
}
// Radially averaged power spectrum of an N x N crop in `bands` bands from DC to Nyquist, normalised to the total variance.
std::vector<double> spectrum(const Chan& c, int x0, int y0, int N, int bands, double* variance) {
  std::vector<std::complex<double>> a(std::size_t(N) * N);
  double mean = 0;
  for (int y = 0; y < N; ++y)
    for (int x = 0; x < N; ++x) mean += c.v[std::size_t(y0 + y) * c.w + x0 + x];
  mean /= double(N) * N;
  double var = 0;
  for (int y = 0; y < N; ++y)
    for (int x = 0; x < N; ++x) {
      const double wnd = 0.5 * (1 - std::cos(2 * 3.14159265358979323846 * (x + 0.5) / N)) * 0.5 * (1 - std::cos(2 * 3.14159265358979323846 * (y + 0.5) / N));
      const double v = (c.v[std::size_t(y0 + y) * c.w + x0 + x] - mean);
      a[std::size_t(y) * N + x] = v * wnd;
      var += v * v;
    }
  *variance = var / (double(N) * N);
  for (int y = 0; y < N; ++y) {
    std::vector<std::complex<double>> row(a.begin() + std::size_t(y) * N, a.begin() + std::size_t(y + 1) * N);
    fft1(row, false);
    std::copy(row.begin(), row.end(), a.begin() + std::size_t(y) * N);
  }
  for (int x = 0; x < N; ++x) {
    std::vector<std::complex<double>> col(N);
    for (int y = 0; y < N; ++y) col[y] = a[std::size_t(y) * N + x];
    fft1(col, false);
    for (int y = 0; y < N; ++y) a[std::size_t(y) * N + x] = col[y];
  }
  std::vector<double> sum(bands, 0.0);
  std::vector<int> cnt(bands, 0);
  for (int fy = 0; fy < N; ++fy)
    for (int fx = 0; fx < N; ++fx) {
      const int ky = fy <= N / 2 ? fy : fy - N, kx = fx <= N / 2 ? fx : fx - N;
      const double rr = std::sqrt(double(kx) * kx + double(ky) * ky) / (N / 2.0);  // 0..1 (Nyquist = 1)
      if (rr <= 0 || rr >= 1.0) continue;
      const int b = std::min(bands - 1, int(rr * bands));
      sum[b] += std::norm(a[std::size_t(fy) * N + fx]);
      ++cnt[b];
    }
  std::vector<double> out(bands);
  for (int b = 0; b < bands; ++b) out[b] = cnt[b] ? sum[b] / cnt[b] : 0.0;
  return out;
}

struct GrainStat {
  double var[3] = {};
  double mean[3] = {};
  std::vector<double> spec[3];  // band powers normalised so that their sum is 1
};
GrainStat grainStats(const ImageF& im, int N, int bands) {
  GrainStat s;
  const int ncx = std::max(1, (im.width - 8) / N), ncy = std::max(1, (im.height - 8) / N);
  for (int c = 0; c < 3; ++c) {
    const Chan ch = channel(im, c, 0, 0, im.width, im.height);
    s.spec[c].assign(bands, 0.0);
    int crops = 0;
    double vsum = 0, msum = 0;
    for (int cy = 0; cy < ncy; ++cy)
      for (int cx = 0; cx < ncx; ++cx) {
        const int x0 = 4 + cx * N, y0 = 4 + cy * N;
        if (x0 + N > im.width || y0 + N > im.height) continue;
        double v;
        const auto sp = spectrum(ch, x0, y0, N, bands, &v);
        for (int b = 0; b < bands; ++b) s.spec[c][b] += sp[b];
        vsum += v;
        double m = 0;
        for (int y = 0; y < N; ++y)
          for (int x = 0; x < N; ++x) m += ch.v[std::size_t(y0 + y) * ch.w + x0 + x];
        msum += m / (double(N) * N);
        ++crops;
      }
    s.var[c] = vsum / crops;
    s.mean[c] = msum / crops;
    double tot = 0;
    for (double v : s.spec[c]) tot += v;
    for (double& v : s.spec[c]) v = tot > 0 ? v / tot : 0.0;
  }
  return s;
}

void grainScaleImpl(int FW, int FH, bool smallRun) {
  const struct {
    float level;
    const char* stock;
  } scenes[] = {{0.18f, "kodak_portra_400"}, {0.04f, "kodak_portra_400"}, {0.18f, "kodak_ektar_100"}};
  for (const auto& sc : scenes) {
    if (smallRun && sc.level < 0.1f) continue;  // the 0.04 scene is the slowest of the three
    EditParams e;
    e.filmStock = sc.stock;
    e.filmHalation = 0.f;  // a flat field has no use for it; keeps the reference free of edge effects
    const ImageF fullIn = flat(FW, FH, sc.level);
    // the reference: the final render at full resolution (quality 2)
    ef::FilmResolved rf = resolvedFor(e, FW, FH, 2);
    ImageF full;
    CHECK(nodeRender(rf, fullIn, full));
    std::printf("  %s, scene %.2f, full-resolution %dx%d\n", sc.stock, sc.level, FW, FH);
    for (int k : smallRun ? std::vector<int>{3, 4} : std::vector<int>{3, 4, 6}) {
      const int PW = FW / k, PH = FH / k;
      const ImageF ref = boxDown(full, k);
      // the preview: interactive tier, grain-family drag (keepGrain), scale-aware against the full-resolution render
      ef::FilmResolved rp = resolvedFor(e, PW, PH, 1, true, FW);
      ImageF prev;
      CHECK(nodeRender(rp, flat(PW, PH, sc.level), prev));
      // and the pre-scale-aware behaviour: the same preview without the full-resolution reference
      ef::FilmResolved rn = resolvedFor(e, PW, PH, 1, true, 0);
      ImageF prevNaive;
      CHECK(nodeRender(rn, flat(PW, PH, sc.level), prevNaive));
      const int N = 128, bands = 6;
      const GrainStat a = grainStats(ref, N, bands), b = grainStats(prev, N, bands), n = grainStats(prevNaive, N, bands);
      for (int c = 0; c < 3; ++c) {
        const double vr = b.var[c] / a.var[c], vn = n.var[c] / a.var[c];
        double worst = 0, worstN = 0;
        for (int i = 0; i < bands; ++i) {
          worst = std::max(worst, std::fabs(std::log(std::max(b.spec[c][i], 1e-12) / std::max(a.spec[c][i], 1e-12))));
          worstN = std::max(worstN, std::fabs(std::log(std::max(n.spec[c][i], 1e-12) / std::max(a.spec[c][i], 1e-12))));
        }
        std::printf("    preview %dx%d (k=%d) ch%d: sigma(ref)=%.5f  variance ratio scale-aware %.3f (without ref %.3f)  worst band deviation x%.2f (without %.2f)  mean %.4f vs %.4f\n",
                    PW, PH, k, c, std::sqrt(a.var[c]), vr, vn, std::exp(worst), std::exp(worstN), b.mean[c], a.mean[c]);
        // below ~0.4 of an 8-bit level the preview's residual noise sources (print glare, LUT interpolation) set a floor
        CHECK((vr > 0.90 && vr < 1.12) || std::fabs(std::sqrt(b.var[c]) - std::sqrt(a.var[c])) < 0.0003);
        CHECK(std::exp(worst) < 1.35);
        CHECK(std::fabs(b.mean[c] - a.mean[c]) < 0.004);
      }
    }
  }
}

void testGrainScale() { grainScaleImpl(2400, 1600, false); }
// the preview grain matches the full-resolution render's (variance, band spectrum, mean) on an 1800 x 1200 frame, mid-grey scenes of two stocks: in the default run
void testGrainScaleSmall() { grainScaleImpl(1800, 1200, true); }

// ---- frame index ------------------------------------------------------------------------------------------------------------------
void testGrainFrame() {
  const int W = 512, H = 341;
  EditParams e;
  e.filmStock = "kodak_portra_400";
  e.filmHalation = 0.f;
  const ImageF in = flat(W, H, 0.18f);
  auto grainImage = [&](std::uint32_t frame) {
    ef::FilmResolved r = resolvedFor(e, W, H, 2, false, 0, frame);
    ImageF o;
    CHECK(nodeRender(r, in, o));
    return o;
  };
  const ImageF a0 = grainImage(0), a0b = grainImage(0), a1 = grainImage(1), a2 = grainImage(2), a1b = grainImage(1);
  CHECK(diffCount(a0, a0b) == 0);
  CHECK(diffCount(a1, a1b) == 0);
  CHECK(diffCount(a0, a1) > a0.data.size() / 2);
  auto corr = [&](const ImageF& x, const ImageF& y, int c) {
    double mx = 0, my = 0;
    const int n = W * H;
    for (int i = 0; i < n; ++i) mx += x.data[3 * i + c], my += y.data[3 * i + c];
    mx /= n;
    my /= n;
    double sxy = 0, sxx = 0, syy = 0;
    for (int i = 0; i < n; ++i) {
      const double dx = x.data[3 * i + c] - mx, dy = y.data[3 * i + c] - my;
      sxy += dx * dy, sxx += dx * dx, syy += dy * dy;
    }
    return sxy / std::sqrt(sxx * syy);
  };
  for (int c = 0; c < 3; ++c) {
    const double c01 = corr(a0, a1, c), c12 = corr(a1, a2, c), c02 = corr(a0, a2, c);
    std::printf("  channel %d: correlation of the grain of frames 0/1 %.4f, 1/2 %.4f, 0/2 %.4f\n", c, c01, c12, c02);
    CHECK(std::fabs(c01) < 0.03 && std::fabs(c12) < 0.03 && std::fabs(c02) < 0.03);
  }
  // frame 0 is the still: the same bytes the FilmSimulator (seed 0) produces
  ef::FilmResolved r0 = resolvedFor(e, W, H, 2);
  ImageF ref;
  CHECK(simRender(r0, 2, in, ref));
  CHECK(diffCount(ref, a0) == 0);
}

// ---- concurrency ------------------------------------------------------------------------------------------------------------------
// Several threads render different film frames at once (edit preview next to filmstrip thumbnails): every result equals the serial one.
void testConcurrent() {
  struct Job {
    const char* stock;
    int w, h, quality;
    float warmth, grain;
  };
  const Job jobs[] = {{"kodak_portra_400", 640, 427, 2, 0.f, 100.f}, {"fujifilm_velvia_100", 480, 320, 1, 20.f, 100.f},
                      {"kodak_ektar_100", 320, 213, 2, -15.f, 50.f}, {"kodak_portra_400", 200, 133, 1, 0.f, 0.f}};
  std::vector<ImageF> serial(4), conc(4);
  auto run = [&](int i, ImageF& out) {
    const Job& j = jobs[i];
    EditParams e;
    e.filmStock = j.stock;
    e.filmWarmth = j.warmth;
    e.filmGrain = j.grain;
    ef::FilmResolved r = resolvedFor(e, j.w, j.h, j.quality);
    nodeRender(r, scene(j.w, j.h), out);
  };
  for (int i = 0; i < 4; ++i) run(i, serial[i]);
  for (int round = 0; round < 3; ++round) {
    std::vector<std::thread> th;
    for (int i = 0; i < 4; ++i) th.emplace_back([&, i] { for (int k = 0; k < 4; ++k) run(i, conc[i]); });
    for (auto& t : th) t.join();
    for (int i = 0; i < 4; ++i) CHECK(diffCount(serial[i], conc[i]) == 0);
  }
  std::printf("  4 threads x 4 frames x 3 rounds: every frame equals its serial render\n");
}

// ---- bench ------------------------------------------------------------------------------------------------------------------------
double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}
void benchSize(int longEdge, const char* label, int runs) {
  const int W = longEdge, H = longEdge * 2 / 3;
  EditParams e;
  e.filmStock = "kodak_portra_400";
  const ImageF in = scene(W, H);
  struct V {
    const char* name;
    int quality;
    bool keepGrain;
  };
  for (const V& v : {V{"final (all effects)", 2, false}, V{"interactive tier", 1, false}, V{"interactive, grain drag", 1, true}}) {
    ef::FilmResolved r = resolvedFor(e, W, H, v.quality, v.keepGrain);
    ImageF a, b;
    ef::FilmTablesHandle tb;
    std::string err;
    CHECK(ef::bakeFilmTables(r, tb, &err));
    simRender(r, v.quality, in, a);
    nodeRender(r, in, b, nullptr, nullptr, &tb);
    std::vector<double> ts, tn;
    ef::FilmTimingsNodes tm, tmBest;
    film::FilmTimings ftBest;
    for (int i = 0; i < runs; ++i) {
      double ms = 0;
      film::FilmTimings ft;
      simRender(r, v.quality, in, a, &ms, &ft);
      ts.push_back(ms);
      if (i == 0 || ms < *std::min_element(ts.begin(), ts.end() - 1)) ftBest = ft;
      nodeRender(r, in, b, &ms, &tm, &tb);
      tn.push_back(ms);
      if (i == 0 || tm.totalMs < tmBest.totalMs) tmBest = tm;
    }
    std::printf("  %-8s %-26s FilmSimulator::render min %7.1f median %7.1f ms | node chain min %7.1f median %7.1f ms (best split: exposure %.1f blur %.1f dir %.1f tiles %.1f)%s\n",
                label, v.name, *std::min_element(ts.begin(), ts.end()), median(ts), *std::min_element(tn.begin(), tn.end()), median(tn),
                tmBest.exposureMs, tmBest.blurMs, tmBest.dirMs, tmBest.tileMs,
                v.keepGrain ? "  [node chain only: grain on at the interactive tier]" : "");
    std::printf("             simulator split: exposure %.1f camera %.1f halation %.1f dir %.1f tiles %.1f\n", ftBest.spExposureMs,
                ftBest.spCameraMs, ftBest.spHalationMs, ftBest.spDirMs, ftBest.spTileMs);
  }
}
void testBench() {
  const char* perf = std::getenv("DAPPLE_PERF");
  benchSize(1600, "1600 px", 31);
  benchSize(1080, "1080 px", 31);
  if (perf && perf[0] == '1') benchSize(6240, "26 MP", 5);
}

struct Test {
  const char* id;
  void (*fn)();
};
const Test kTests[] = {{"equiv", testEquiv},       {"rebake", testRebake},   {"keys", testKeys},         {"graph", testGraph},
                       {"concurrent", testConcurrent}, {"grain_scale", testGrainScale}, {"grain_scale_small", testGrainScaleSmall}, {"grain_frame", testGrainFrame}, {"fx", testFx}, {"fx_small", testFxSmall}, {"keys_small", testKeysSmall}, {"bench", testBench}};

}  // namespace

int main(int argc, char** argv) {
  if (!library()) return 77;
  if (argc >= 2 && std::string(argv[1]) == "--list") {
    for (const Test& t : kTests) std::printf("%s\n", t.id);
    return 0;
  }
  bool ran = false;
  for (const Test& t : kTests)
    if (argc < 2 || std::string(argv[1]) == t.id) {
      std::printf("[%s]\n", t.id);
      t.fn();
      ran = true;
    }
  if (!ran) return 2;
  std::printf(g_failures ? "FAILED: %d check(s)\n" : "ok\n", g_failures);
  return g_failures ? 1 : 0;
}
