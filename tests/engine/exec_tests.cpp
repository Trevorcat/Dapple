// Planner / CpuExecutor tests (docs/engine/pipeline-architecture.md 10.2 steps a5 - a10, gates S and B).
//
//   dapple_exec_tests <id>      ids: whole tiles plan cancel analysis
//     whole     the exact variant through Planner + CpuExecutor (Whole) is byte-identical to Renderer::render
//     tiles     tile invariance: TiledExport (several tile sizes, per node and combined) vs Whole: dE2000 mean < 0.01, max 1 code;
//               also the halo conformance of every window node (a halo that is too small shows up as an edge seam)
//     plan      Plan structure: fusion, sweeps, cached solves prune sweeps, film plans are Whole
//     cancel    latest-wins: a cancel during a render returns "cancelled" promptly; the next render is unaffected
//     analysis  the tonal analysis follows crop / straighten / vignette (the stale-analysis bug of the legacy keys)
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#endif

#include "../deterministic.h"
#include "colour_metrics.h"
#include "dapple/camera.h"
#include "dapple/edit_params.h"
#include "dapple/renderer.h"
#include "dapple/tone.h"
#include "engine/exec/cpu_executor.h"
#include "engine/exec/engine_session.h"
#include "engine/plan/plan.h"

namespace {

using namespace dapple;
using namespace dapple::engine;
using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

int g_failures = 0;
#define CHECK(cond)                                                                   \
  do {                                                                                \
    if (!(cond)) {                                                                    \
      std::printf("  CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
      ++g_failures;                                                                   \
    }                                                                                 \
  } while (0)

std::uint32_t hash32(std::uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}

SourceImage makeScene(int w, int h, bool scene = true, std::uint32_t seed = 5) {
  SourceImage s;
  s.sceneReferred = scene;
  s.linear = ImageF(w, h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const float u = float(x) / float(w - 1), v = float(y) / float(h - 1);
      float* p = s.linear.row(y) + 3 * x;
      const float n = float(hash32(seed * 7919u + std::uint32_t(x) * 73856093u ^ std::uint32_t(y) * 19349663u) >> 8) * (1.f / 16777216.f);
      const float tex = ((x / 24 + y / 24) & 1) ? 1.f : 0.85f;  // blocks: edges for the clarity / sharpen stages
      p[0] = (0.05f + 0.95f * u * (1.f - 0.4f * v)) * (0.9f + 0.2f * n) * tex;
      p[1] = (0.04f + 0.7f * (1.f - u) * v + 0.15f * u) * (0.9f + 0.2f * n) * tex;
      p[2] = (0.03f + 0.8f * (1.f - v) * (1.f - 0.5f * u)) * (0.9f + 0.2f * n) * tex;
      if ((u - 0.7f) * (u - 0.7f) + (v - 0.3f) * (v - 0.3f) < 0.01f) p[0] = 2.2f, p[1] = 1.9f, p[2] = 1.5f;
      if (!scene)
        for (int c = 0; c < 3; ++c) p[c] = std::min(p[c], 1.f);
    }
  return s;
}

const CameraProfile& cam() {
  static const CameraRegistry reg = CameraRegistry::withBuiltins();
  return reg.resolve("SONY", "ILCE-7M4");
}

struct Cmp {
  double meanDE = 0, maxDE = 0;
  int maxCode = 0;
  std::size_t differing = 0, total = 0;
};

Cmp compare(const Image8& a, const Image8& b) {
  Cmp c;
  if (a.width != b.width || a.height != b.height) {
    c.maxCode = 999;
    return c;
  }
  const std::size_t n = std::size_t(a.width) * a.height;
  c.total = n;
  double sum = 0;
  for (std::size_t i = 0; i < n; ++i) {
    bool diff = false;
    for (int k = 0; k < 3; ++k) {
      const int d = std::abs(int(a.data[3 * i + k]) - int(b.data[3 * i + k]));
      c.maxCode = std::max(c.maxCode, d);
      diff |= d != 0;
    }
    if (!diff) continue;
    ++c.differing;
    const float fa[3] = {a.data[3 * i] / 255.f, a.data[3 * i + 1] / 255.f, a.data[3 * i + 2] / 255.f};
    const float fb[3] = {b.data[3 * i] / 255.f, b.data[3 * i + 1] / 255.f, b.data[3 * i + 2] / 255.f};
    const double de = metrics::deltaE2000(metrics::srgbToLab(fa), metrics::srgbToLab(fb));
    sum += de;
    c.maxDE = std::max(c.maxDE, de);
  }
  c.meanDE = sum / double(n);
  return c;
}

bool exactRender(EngineSession& es, const EditParams& p, const SourceImage& src, ExecMode mode, int tile, Image8& out,
                 ExecStats* st = nullptr) {
  const Renderer r(p, cam(), nullptr);
  EngineSession::ExactOptions o;
  o.mode = mode;
  o.tile = tile;
  o.sessionCache = true;
  std::string err;
  const bool ok = es.renderExact(p, r, src, o, out, &err, st);
  if (!ok) std::printf("  renderExact failed: %s\n", err.c_str());
  return ok;
}

// one named parameter set; a node alone or all together
struct Case {
  const char* name;
  EditParams p;
};

EditParams withBase() {
  EditParams p;
  p.exposure = 0.2f;
  p.contrast = 8.f;
  p.sharpen = 0.f;  // default sharpen/nrColor are non-zero: each case sets what it tests
  p.nrColor = 0.f;
  return p;
}

std::vector<Case> cases() {
  std::vector<Case> v;
  EditParams p = withBase();
  p.nrLuma = 50.f;
  v.push_back({"nr_luma", p});
  p = withBase();
  p.nrColor = 60.f;
  v.push_back({"nr_chroma", p});
  p = withBase();
  p.dehaze = 45.f;
  v.push_back({"dehaze", p});
  p = withBase();
  p.dehaze = -40.f;
  v.push_back({"dehaze_veil", p});
  p = withBase();
  p.clarity = 40.f;
  v.push_back({"clarity", p});
  p = withBase();
  p.texture = 50.f;
  v.push_back({"texture", p});
  p = withBase();
  p.sharpen = 80.f;
  p.sharpRadius = 1.5f;
  v.push_back({"sharpen", p});
  p = withBase();
  p.grain = 30.f;
  v.push_back({"grain", p});
  p = withBase();
  p.vignette = -35.f;
  p.crop = "4:5";
  p.straighten = 3.f;
  v.push_back({"geometry_vignette", p});
  p = withBase();
  p.nrLuma = 35.f;
  p.nrColor = 40.f;
  p.dehaze = 30.f;
  p.clarity = 35.f;
  p.texture = 20.f;
  p.sharpen = 60.f;
  p.grain = 20.f;
  p.vignette = -25.f;
  p.rotate = 90;
  v.push_back({"all", p});
  return v;
}

// ---- whole: byte-identical to the reference renderer ------------------------------------------------------------------------
void testWhole() {
  const SourceImage src = makeScene(613, 421);
  EngineSession es;
  for (const Case& c : cases()) {
    Image8 ref, got;
    const Renderer r(c.p, cam(), nullptr);
    ref = r.render(src);
    CHECK(exactRender(es, c.p, src, ExecMode::Whole, 0, got));
    const bool same = ref.width == got.width && ref.height == got.height && ref.data == got.data;
    std::printf("  %-18s whole vs Renderer::render: %s\n", c.name, same ? "byte-identical" : "DIFFERENT");
    CHECK(same);
  }
}

// ---- tiles: tile invariance + halo conformance -----------------------------------------------------------------------------
void testTiles() {
  const SourceImage small = makeScene(701, 523);  // sizes that are not multiples of the tile or of 4
  EngineSession es;
  for (const Case& c : cases()) {
    Image8 whole;
    CHECK(exactRender(es, c.p, small, ExecMode::Whole, 0, whole));
    for (int tile : {64, 128, 192}) {
      Image8 t;
      ExecStats st;
      EngineSession esT;  // fresh cache: the frame-global solves are accumulated tile by tile (a shared cache would hold the Whole run's)
      CHECK(exactRender(esT, c.p, small, ExecMode::TiledExport, tile, t, &st));
      ExecStats st2;
      Image8 t2;
      CHECK(exactRender(esT, c.p, small, ExecMode::TiledExport, tile, t2, &st2));  // second run: solves come from the cache
      CHECK(t2.data == t.data);
      if (st2.sweeps >= st.sweeps && st.sweeps > 1) {
        std::printf("  cached solves did not remove sweeps (%d -> %d)\n", st.sweeps, st2.sweeps);
        ++g_failures;
      }
      const Cmp m = compare(whole, t);
      std::printf("  %-18s tile %3d: %2d tiles %d sweeps  dE mean %.5f max %.3f  max code %d  differing %.4f%%\n", c.name, tile,
                  st.tiles, st.sweeps, m.meanDE, m.maxDE, m.maxCode, m.total ? 100.0 * double(m.differing) / double(m.total) : 0.0);
      CHECK(m.meanDE < 0.01);
      CHECK(m.maxCode <= 1);
    }
  }
  // a larger radii scale (a 3.4 MP frame: scale 1.1 -> halos of tens of pixels), all stages together
  const SourceImage big = makeScene(2301, 1499, true, 9);
  EditParams p = cases().back().p;
  p.rotate = 0;
  Image8 whole, t;
  CHECK(exactRender(es, p, big, ExecMode::Whole, 0, whole));
  ExecStats st;
  EngineSession esT;
  CHECK(exactRender(esT, p, big, ExecMode::TiledExport, 512, t, &st));
  const Cmp m = compare(whole, t);
  std::printf("  big all-stages     tile 512: %d tiles %d sweeps  dE mean %.5f max %.3f  max code %d\n", st.tiles, st.sweeps,
              m.meanDE, m.maxDE, m.maxCode);
  CHECK(m.meanDE < 0.01);
  CHECK(m.maxCode <= 1);
  // no stage at all: tile boundaries are invisible by construction
  EditParams q = withBase();
  Image8 w2, t2;
  CHECK(exactRender(es, q, big, ExecMode::Whole, 0, w2));
  CHECK(exactRender(es, q, big, ExecMode::TiledExport, 256, t2));
  CHECK(w2.data == t2.data);
}

// ---- plan ----------------------------------------------------------------------------------------------------------------------
Graph graphFor(const EditParams& p, const SourceImage& src, bool exact, FrameKernels** fkOut, std::unique_ptr<FrameKernels>& hold,
               std::unique_ptr<Renderer>& rh, PlanRequest& rq) {
  rh = std::make_unique<Renderer>(p, cam(), nullptr);
  FastOptions fo;
  hold = std::make_unique<FrameKernels>(*rh, src, nullptr, fo);
  ParamInputs in;
  in.edit = p;
  in.frame.exact = exact;
  in.ctx.srcW = src.linear.width;
  in.ctx.srcH = src.linear.height;
  const ParamSet ps = ParamSet::make(std::move(in));
  const FramePlan fp = hold->framePlan();
  Graph g = GraphBuilder::build(ps, GraphDetail::Full, &fp);
  g.computeKeys(ps);
  const SpatialParams& sp = hold->spatial();
  rq = PlanRequest{};
  rq.frameW = hold->width();
  rq.frameH = hold->height();
  rq.nrLuma = sp.nrLuma, rq.nrColor = sp.nrColor, rq.dehaze = sp.dehaze, rq.clarity = sp.clarity, rq.texture = sp.texture;
  rq.sharpen = sp.sharpen, rq.sharpRadius = sp.sharpRadius, rq.grain = sp.grain, rq.scale = hold->spatialScale();
  if (fkOut) *fkOut = hold.get();
  return g;
}

void testPlan() {
  const SourceImage src = makeScene(640, 480);
  std::unique_ptr<FrameKernels> fk;
  std::unique_ptr<Renderer> rh;
  PlanRequest rq;

  EditParams plain = withBase();
  Graph g = graphFor(plain, src, true, nullptr, fk, rh, rq);
  Plan p = Planner::plan(g, rq, nullptr);
  CHECK(!p.unsupported);
  CHECK(p.sweeps() == 1);
  CHECK(p.stages.back().packInChain);  // nothing between the chain and the 8-bit pack
  CHECK(p.stages.back().groups.size() == 1);
  std::printf("  plain: %d sweep(s), head nodes %zu\n", p.sweeps(), p.stages.back().groups[0].nodes.size());

  EditParams all = cases().back().p;
  g = graphFor(all, src, true, nullptr, fk, rh, rq);
  p = Planner::plan(g, rq, nullptr);
  // dehaze (airlight) and clarity (guided filter) are frame-global: grid sweep, solve, grid sweep, solve, final sweep
  CHECK(p.sweeps() == 3);
  int solves = 0;
  for (const Stage& s : p.stages) solves += s.kind == StageKind::GlobalSolve;
  CHECK(solves == 2);
  CHECK(p.stages.back().output);
  CHECK(p.stages.back().groups.size() == 1 + 5);  // head + NRLuma, NRChroma, Dehaze, Detail, Grain
  std::printf("  all stages: %d sweeps, %d solves, final halo %d px, peak %.1f MB (Whole)\n", p.sweeps(), solves,
              p.stages.back().halo.x, double(p.peakCpuBytes) / 1048576.0);

  // a solve that is in the cache disappears from the plan, and so does the sweep that would have produced its grid
  ResultCache cache;
  for (const NodeInstance& n : g.nodes())
    if (n.type == NodeType::Dehaze || n.type == NodeType::Detail) {
      ResultCache::Entry e;
      e.key = e.family = n.key;
      cache.put(n.type, std::move(e));
    }
  p = Planner::plan(g, rq, &cache);
  CHECK(p.sweeps() == 1);
  std::printf("  all stages with cached solves: %d sweep(s)\n", p.sweeps());

  // tiling: the planner picks a tile from the memory budget and the stage halos
  rq.frameW = 9504;
  rq.frameH = 6336;  // 60 MP
  rq.mode = ExecMode::TiledExport;
  rq.memBudget = 500u << 20;
  rq.scale = 9504.f / 2048.f;
  p = Planner::plan(g, rq, nullptr);
  CHECK(p.grid.tile >= 256 && p.grid.tile % 4 == 0);
  CHECK(p.peakCpuBytes <= (std::size_t(500) << 20) + (std::size_t(100) << 20));  // + the 1/4 grids
  std::printf("  60 MP export under 500 MB: tile %d, %dx%d tiles, halo %d px, peak %.0f MB\n", p.grid.tile, p.grid.nx, p.grid.ny,
              p.stages.back().halo.x, double(p.peakCpuBytes) / 1048576.0);
}

// ---- cancel --------------------------------------------------------------------------------------------------------------------
void testCancel() {
  const SourceImage src = makeScene(3000, 2000, true, 3);
  EditParams p = cases().back().p;
  p.rotate = 0;
  EngineSession es;
  Image8 out;
  // Deterministic: the cancel lands after the request took its ticket and before the first tile (no wall-clock involved).
  {
    const Renderer r(p, cam(), nullptr);
    EngineSession::ExactOptions o;
    o.mode = ExecMode::TiledExport;
    o.tile = 512;
    o.beforeExec = [&] { es.cancel(); };
    std::string err;
    CHECK(!es.renderExact(p, r, src, o, out, &err));
    CHECK(err == "cancelled");
    CHECK(es.stats().cancelled == 1);
  }
  // a request submitted after the cancel is unaffected (latest wins)
  // the session still renders, and the result equals a render that was never cancelled
  Image8 a, b;
  CHECK(exactRender(es, p, src, ExecMode::TiledExport, 512, a));
  EngineSession es2;
  CHECK(exactRender(es2, p, src, ExecMode::TiledExport, 512, b));
  CHECK(a.data == b.data);
}

// ---- analysis --------------------------------------------------------------------------------------------------------------
void testAnalysis() {
  const SourceImage src = makeScene(400, 300);
  EditParams a = withBase(), b = a;
  b.crop = "1:1";
  const ToneAnalysis ta = analyzeTone(src, cam(), a), tb = analyzeTone(src, cam(), b);
  CHECK(ta.histogram != tb.histogram);  // the analysis really depends on the crop ...
  EditParams c = a;
  c.vignette = -60.f;
  CHECK(analyzeTone(src, cam(), c).histogram != ta.histogram);  // ... and on the vignette
  // ... so the keys that identify a bake must tell them apart
  const Renderer ra(a, cam(), nullptr), rb(b, cam(), nullptr), rc(c, cam(), nullptr);
  CHECK(ra.pointKey() != rb.pointKey());
  CHECK(ra.pointKey() != rc.pointKey());
  CHECK(ra.colourKey() == rb.colourKey());  // the colour lattice does not care
  CHECK(ra.colourKey() == rc.colourKey());
  // and the graph: the Analysis node declares the keys
  for (int variant : {0, 1}) {
    ParamInputs in;
    in.edit = a;
    in.frame.wantAnalysis = true;
    ParamInputs in2 = in;
    if (variant == 0) in2.edit.crop = "1:1";
    else in2.edit.vignette = -60.f;
    const ParamSet p1 = ParamSet::make(in), p2 = ParamSet::make(in2);
    Graph g1 = GraphBuilder::build(p1), g2 = GraphBuilder::build(p2);
    g1.computeKeys(p1);
    g2.computeKeys(p2);
    CHECK(g1.find(NodeType::Analysis)->key != g2.find(NodeType::Analysis)->key);
    CHECK(g1.find(NodeType::ColourLattice)->family == g2.find(NodeType::ColourLattice)->family);
  }
}

// ---- bench: the pixel pipeline of the executor vs Renderer::renderBakedRgba (same BakedLut), alternating, medians ---------------
void testBench() {
  struct V {
    const char* name;
    EditParams p;
    SpatialQuality q;
  };
  std::vector<V> vs;
  EditParams a = withBase();
  vs.push_back({"point only (1600x1067)", a, SpatialQuality::Off});
  EditParams b = a;
  b.sharpen = 40.f;
  b.nrColor = 25.f;
  vs.push_back({"default spatial, interactive", b, SpatialQuality::Interactive});
  vs.push_back({"default spatial, final", b, SpatialQuality::Full});
  EditParams c = cases().back().p;
  c.rotate = 0;
  vs.push_back({"all stages, final", c, SpatialQuality::Full});
  EditParams d = a;
  d.crop = "4:5";
  d.straighten = 2.f;
  d.vignette = -30.f;
  vs.push_back({"geometry + vignette", d, SpatialQuality::Off});
  const SourceImage src = makeScene(1600, 1067);
  for (const V& v : vs) {
    const Renderer r(v.p, cam(), nullptr);
    const std::shared_ptr<const BakedLut> bake = r.bake(src, 57);
    FastOptions fo;
    fo.spatial = v.q;
    EngineSession es;
    ParamInputs in;
    in.edit = v.p;
    in.ctx.srcW = src.linear.width;
    in.ctx.srcH = src.linear.height;
    const ParamSet ps = ParamSet::make(in);
    std::vector<double> tl, te, ts;
    for (int i = 0; i < 31; ++i) {
      int w = 0, h = 0;
      auto t0 = Clock::now();
      std::uint8_t* legacy = r.renderBakedRgba(src, *bake, fo, w, h);
      const double l = msSince(t0);
      std::free(legacy);
      t0 = Clock::now();
      FrameKernels fk(r, src, bake.get(), fo);
      const FramePlan fp = fk.framePlan();
      Graph g = GraphBuilder::build(ps, GraphDetail::Full, &fp);
      g.computeKeys(ps);
      PlanRequest rq;
      rq.frameW = fk.width(), rq.frameH = fk.height();
      rq.nrLuma = fk.spatial().nrLuma, rq.nrColor = fk.spatial().nrColor, rq.dehaze = fk.spatial().dehaze;
      rq.clarity = fk.spatial().clarity, rq.texture = fk.spatial().texture, rq.sharpen = fk.spatial().sharpen;
      rq.sharpRadius = fk.spatial().sharpRadius, rq.grain = fk.spatial().grain, rq.scale = fk.spatialScale();
      const Plan plan = Planner::plan(g, rq, &es.cache());
      if (i > 0) ts.push_back(msSince(t0));
      std::vector<std::uint8_t> out(std::size_t(w) * h * 4);
      ExecEnv env{fk, g, es.cache(), CancelToken{}, out.data(), 4};
      const Status st = CpuExecutor::run(plan, env);
      const double e = msSince(t0);
      CHECK(st == Status::Ok);
      if (i > 0) {
        tl.push_back(l);
        te.push_back(e);
      }
    }
    std::sort(ts.begin(), ts.end());
    std::printf("    (setup: FrameKernels + graph + keys + plan: median %.3f ms)\n", ts[ts.size() / 2]);
    std::sort(tl.begin(), tl.end());
    std::sort(te.begin(), te.end());
    std::printf("  %-32s legacy median %6.2f min %6.2f   executor median %6.2f min %6.2f   ratio %.3f (median)\n", v.name, tl[tl.size() / 2],
                tl[0], te[te.size() / 2], te[0], te[te.size() / 2] / tl[tl.size() / 2]);
  }
}

// ---- export60: a 60 MP synthetic export under a memory budget (not in ctest: needs ~1.5 GB) -------------------------------------
void testExport60() {
#ifdef _WIN32
  auto workingSet = [] {
    PROCESS_MEMORY_COUNTERS pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
    return std::pair<double, double>(double(pmc.WorkingSetSize) / 1048576.0, double(pmc.PeakWorkingSetSize) / 1048576.0);
  };
  const int W = 9504, H = 6336;
  SourceImage src;
  src.sceneReferred = true;
  src.linear = ImageF(W, H);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      float* p = src.linear.row(y) + 3 * x;
      const float u = float(x) / float(W), v = float(y) / float(H);
      const float n = float(hash32(std::uint32_t(x) * 73856093u ^ std::uint32_t(y) * 19349663u) >> 8) * (1.f / 16777216.f);
      p[0] = (0.05f + 0.9f * u) * (0.9f + 0.2f * n);
      p[1] = (0.04f + 0.7f * (1.f - u) * v) * (0.9f + 0.2f * n);
      p[2] = (0.03f + 0.8f * (1.f - v)) * (0.9f + 0.2f * n);
    }
  EditParams p = cases().back().p;
  p.rotate = 0;
  EngineSession es;
  const Renderer r(p, cam(), nullptr);
  EngineSession::ExactOptions o;
  o.mode = ExecMode::TiledExport;
  o.memBudget = 500u << 20;
  Image8 out;
  std::string err;
  ExecStats st;
  const auto before = workingSet();
  const auto t0 = Clock::now();
  const bool ok = es.renderExact(p, r, src, o, out, &err, &st);
  const double ms = msSince(t0);
  const auto after = workingSet();
  std::printf("  60 MP (%dx%d) all stages, TiledExport: %s in %.1f s, %d tiles %d sweeps\n", W, H, ok ? "ok" : err.c_str(), ms / 1000.0,
              st.tiles, st.sweeps);
  std::printf("  working set before %.0f MB (source 722 MB resident), peak during %.0f MB => added at peak %.0f MB (incl. the %.0f MB RGB8 output)\n",
              before.first, after.second, after.second - before.first, double(out.data.size()) / 1048576.0);
  CHECK(ok);
#else
  std::printf("  (Windows only)\n");
#endif
}

}  // namespace

int main(int argc, char** argv) {
  struct T {
    const char* id;
    void (*fn)();
  };
  const T tests[] = {{"whole", testWhole}, {"tiles", testTiles}, {"plan", testPlan}, {"cancel", testCancel}, {"analysis", testAnalysis}, {"bench", testBench}, {"export60", testExport60}};
  if (argc >= 2 && std::string(argv[1]) == "--list") {
    for (const T& t : tests) std::printf("%s\n", t.id);
    return 0;
  }
  bool ran = false;
  for (const T& t : tests) {
    if (argc >= 2 && std::string(argv[1]) != t.id) continue;
    ran = true;
    std::printf("[%s]\n", t.id);
    const int before = g_failures;
    t.fn();
    std::printf("[%s] %s\n", t.id, g_failures == before ? "ok" : "FAILED");
  }
  if (!ran) {
    std::fprintf(stderr, "unknown test id\n");
    return 2;
  }
  return g_failures ? 1 : 0;
}
