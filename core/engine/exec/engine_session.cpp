#include "engine/exec/engine_session.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>

#include "engine/exec/cpu_executor.h"
#include "engine/plan/plan.h"

namespace dapple::engine {

namespace {

std::atomic<int> g_override{-1};
std::atomic<std::uint64_t> g_nextId{1};

struct Totals {
  std::atomic<std::uint64_t> frames{0}, fullHits{0}, latticeReuse{0}, latticeBake{0};
  std::atomic<std::uint64_t> planned{0}, planUs{0}, execUs{0}, sweeps{0}, tiles{0}, solveHits{0}, cancelled{0}, fallbacks{0};
};
Totals& totals() {
  static Totals t;
  return t;
}

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

Mode envMode() {
  static const Mode m = [] {
    const char* e = std::getenv("DAPPLE_ENGINE");
    if (!e || !*e || std::strcmp(e, "0") == 0 || std::strcmp(e, "off") == 0) return Mode::Off;
    if (std::strcmp(e, "legacy") == 0) return Mode::LegacyBakes;
    if (std::strcmp(e, "tables") == 0) return Mode::Tables;
    return Mode::Full;  // "full", "1", anything else
  }();
  return m;
}

Hash128 combine(const Hash128& a, const Hash128& b) {
  Hasher h;
  h.hash(a);
  h.hash(b);
  return h.finish();
}

Hash128 withOrder(const Hash128& family, int order) {
  Hasher h;
  h.hash(family);
  h.i32(order);
  return h.finish();
}

template <class T>
std::shared_ptr<T> as(const std::shared_ptr<void>& p) {
  return std::static_pointer_cast<T>(p);
}

}  // namespace

Mode mode() {
  const int o = g_override.load(std::memory_order_relaxed);
  return o >= 0 ? Mode(o) : envMode();
}
void setModeOverride(int m) { g_override.store(m, std::memory_order_relaxed); }

// ---- table node evaluators: thin wrappers over the Renderer::bake pieces -------------------------------------------------

std::shared_ptr<std::vector<float>> evalColourLattice(const TableEnv& env, int n) {
  return std::make_shared<std::vector<float>>(env.renderer.bakeColourLattice(env.src, n));
}

Renderer::TonePre evalToneAnalysisPre(const TableEnv& env, std::vector<float>* lattice, int n, bool fast) {
  return env.renderer.bakeTonePre(env.src, lattice, n, fast);
}

ToneTableOut evalToneTable(const TableEnv& env, const Renderer::TonePre* pre) {
  BakedLut tmp;
  env.renderer.bakeToneTable(env.src, pre ? *pre : Renderer::TonePre{}, tmp);
  return {std::move(tmp.tone), std::move(tmp.toneMove)};
}

std::vector<float> evalLatticeOverflow(const TableEnv& env, int n, float maxV) {
  BakedLut base;
  base.rgb.resize(4);  // only its presence matters: the overflow lattice is a function of the colour stage alone
  return env.renderer.bakeOverflow(env.src, base, n, maxV);
}

ToneAnalysis evalAnalysis(const TableEnv& env, const Renderer::TonePre* pre) {
  return env.renderer.bakeAnalysis(env.src, pre ? *pre : Renderer::TonePre{});
}

TableKeys effectiveTableKeys(const Graph& g, const ParamSet& ps, int nLattice) {
  TableKeys k;
  const NodeInstance* latN = g.find(NodeType::ColourLattice);
  const NodeInstance* ttN = g.find(NodeType::ToneTable);
  const NodeInstance* preN = g.find(NodeType::ToneAnalysisPre);
  const NodeInstance* anN = g.find(NodeType::Analysis);
  const NodeInstance* ovN = g.find(NodeType::LatticeOverflow);
  if (latN) k.lattice = withOrder(latN->family, nLattice);
  if (preN) {
    k.hasPre = true;
    k.pre = combine(preN->key, k.lattice);
  }
  k.tone = ttN ? ttN->key : Hash128{};
  if (ttN && ps.edit().toneStrength > 0.f && preN) k.tone = combine(ttN->key, k.pre);
  if (anN) {
    k.hasAnalysis = true;
    k.analysis = combine(anN->key, combine(k.tone, k.lattice));
  }
  if (ovN) {
    k.hasOverflow = true;
    k.overflow = ovN->key;
  }
  return k;
}

// ---- session ------------------------------------------------------------------------------------------------------------

EngineSession::EngineSession() : id_(g_nextId.fetch_add(1)) {
  cache_.setLimit(NodeType::Dehaze, 2);  // frame-global solves (1/4 grids: 15 MB each at 60 MP)
  cache_.setLimit(NodeType::Detail, 2);
}

namespace {

// What the planner needs besides the graph: frame size, neighbourhood amounts after quality / region / film.
PlanRequest makeRequest(const FrameKernels& fk, ExecMode mode, int tile, std::size_t budget, int channels) {
  PlanRequest rq;
  rq.frameW = fk.width();
  rq.frameH = fk.height();
  rq.mode = mode;
  rq.tile = tile;
  rq.memBudget = budget;
  const SpatialParams& sp = fk.spatial();
  rq.nrLuma = sp.nrLuma;
  rq.nrColor = sp.nrColor;
  rq.dehaze = sp.dehaze;
  rq.clarity = sp.clarity;
  rq.texture = sp.texture;
  rq.sharpen = sp.sharpen;
  rq.sharpRadius = sp.sharpRadius;
  rq.grain = sp.grain;
  rq.scale = fk.spatialScale();
  rq.vignette = fk.vignetteActive();
  rq.useGeo = fk.useGeo();
  rq.outChannels = channels;
  return rq;
}

void addStats(EngineStats& st, const ExecStats& es) {
  st.sweeps += std::uint64_t(es.sweeps);
  st.tiles += std::uint64_t(es.tiles);
  st.solveHits += std::uint64_t(es.solveHits);
  st.solveBuilds += std::uint64_t(es.solveBuilds);
  st.streamed += es.streamed ? 1 : 0;
}

}  // namespace

int EngineSession::renderFull(FrameJob& job, const ParamSet& ps, const BakedLut& bake, FrameResult& out) {
  FrameKernels fk(job.renderer, job.src, &bake, job.fastOptions);
  if (!fk.valid()) return 1;
  const auto t0 = Clock::now();
  const FramePlan fp = fk.framePlan();
  Graph g = GraphBuilder::build(ps, GraphDetail::Full, &fp);
  const ValidationResult v = g.validate();
  if (!v.ok) {
    out.error = "engine graph: " + v.error;
    return -1;
  }
  g.computeKeys(ps);
  const Plan plan = Planner::plan(g, makeRequest(fk, ExecMode::Whole, 0, 0, 4), &cache_);
  {
    const double ms = msSince(t0);
    stats_.planMs += ms;
    totals().planUs.fetch_add(std::uint64_t(ms * 1000.0), std::memory_order_relaxed);
  }
  ++stats_.planned;
  totals().planned.fetch_add(1, std::memory_order_relaxed);
  if (plan.unsupported) return 1;
  const int w = fk.width(), h = fk.height();
  auto* rgba = static_cast<std::uint8_t*>(std::malloc(std::size_t(w) * h * 4));
  if (!rgba) {
    out.error = "out of memory";
    return -1;
  }
  ExecEnv env{fk, g, cache_, CancelToken{&latest_, latest_.load(std::memory_order_relaxed)}, rgba, 4};
  ExecStats es;
  const auto t1 = Clock::now();
  const Status s = CpuExecutor::run(plan, env, &es);
  {
    const double ms = msSince(t1);
    stats_.execMs += ms;
    totals().execUs.fetch_add(std::uint64_t(ms * 1000.0), std::memory_order_relaxed);
  }
  addStats(stats_, es);
  totals().sweeps.fetch_add(std::uint64_t(es.sweeps), std::memory_order_relaxed);
  totals().tiles.fetch_add(std::uint64_t(es.tiles), std::memory_order_relaxed);
  totals().solveHits.fetch_add(std::uint64_t(es.solveHits), std::memory_order_relaxed);
  if (s == Status::Ok) {
    out.rgba = rgba;
    out.w = w;
    out.h = h;
    return 0;
  }
  std::free(rgba);
  if (s == Status::Cancelled) {
    ++stats_.cancelled;
    totals().cancelled.fetch_add(1, std::memory_order_relaxed);
    out.error = "cancelled";
    return -1;
  }
  ++stats_.fallbacks;
  totals().fallbacks.fetch_add(1, std::memory_order_relaxed);
  if (s == Status::OutOfMemory) {
    out.error = "out of memory";
    return -1;
  }
  return 1;  // Unsupported (film stage failed ...): the legacy renderer takes the frame
}

bool EngineSession::renderExact(const EditParams& params, const Renderer& renderer, const SourceImage& src,
                                const ExactOptions& opt, Image8& out, std::string* err, ExecStats* statsOut) {
  FastOptions fo;
  fo.spatial = opt.spatial ? SpatialQuality::Full : SpatialQuality::Off;
  FrameKernels fk(renderer, src, nullptr, fo);
  if (!fk.valid()) {
    if (err) *err = "empty source";
    return false;
  }
  ParamInputs in;
  in.edit = params;
  in.frame.exact = true;
  in.ctx.sourceId = id_;
  in.ctx.sceneReferred = src.sceneReferred;
  in.ctx.hasProxy = opt.hasProxy;
  const std::shared_ptr<const film::FilmLibrary> lib = film::defaultLibrary();
  in.ctx.filmLib = lib.get();
  in.ctx.srcW = src.linear.width;
  in.ctx.srcH = src.linear.height;
  const ParamSet ps = ParamSet::make(std::move(in));
  const FramePlan fp = fk.framePlan();
  Graph g = GraphBuilder::build(ps, GraphDetail::Full, &fp);
  const ValidationResult v = g.validate();
  if (!v.ok) {
    if (err) *err = "engine graph: " + v.error;
    return false;
  }
  g.computeKeys(ps);
  const auto t0 = Clock::now();
  ResultCache local;
  ResultCache& cache = opt.sessionCache ? cache_ : local;
  const Plan plan = Planner::plan(g, makeRequest(fk, opt.mode, opt.tile, opt.memBudget, 3), &cache);
  stats_.planMs += msSince(t0);
  ++stats_.planned;
  out.width = fk.width();
  out.height = fk.height();
  out.data.resize(std::size_t(out.width) * out.height * 3);
  ExecEnv env{fk, g, cache, CancelToken{&latest_, latest_.load(std::memory_order_relaxed)}, out.data.data(), 3};
  ExecStats es;
  Status s = plan.unsupported ? Status::Unsupported : CpuExecutor::run(plan, env, &es);
  addStats(stats_, es);
  if (statsOut) *statsOut = es;
  if (s == Status::Unsupported) {  // e.g. the film stage failed: the reference renderer takes the frame
    ++stats_.fallbacks;
    out = renderer.render(src);
    return true;
  }
  if (s != Status::Ok) {
    if (err) *err = s == Status::Cancelled ? "cancelled" : "out of memory";
    if (s == Status::Cancelled) ++stats_.cancelled;
    return false;
  }
  return true;
}

std::shared_ptr<const BakedLut> EngineSession::bakeLegacy(FrameJob& job, int n, FrameResult& out) {
  // Step a2: the lookup the C layer used to do, unchanged, inside the engine.
  const std::string pk = job.renderer.pointKey() + "#";
  std::shared_ptr<const BakedLut> bake;
  for (auto& b : bakes_)
    if (b->n >= std::clamp(n, 17, 65) && b->key.size() > pk.size() && b->key.compare(0, pk.size(), pk) == 0 &&
        (!bake || b->n > bake->n))
      bake = b;
  if (bake) {
    out.bake = FrameResult::Bake::FullHit;
    return bake;
  }
  const std::string ck = job.renderer.colourKey();
  const BakedLut* from = nullptr;
  for (auto& b : bakes_)
    if (b->colourKey == ck && b->n >= std::clamp(n, 17, 65) && (!from || b->n > from->n)) from = b.get();
  out.bake = from ? FrameResult::Bake::LatticeReuse : FrameResult::Bake::LatticeBake;
  bake = job.renderer.bake(job.src, n, from, job.request.fastAnalysis);
  bakes_.push_back(bake);
  if (bakes_.size() > 3) bakes_.erase(bakes_.begin());
  return bake;
}

std::shared_ptr<const BakedLut> EngineSession::bakeTables(FrameJob& job, const ParamSet& ps, int n, FrameResult& out,
                                                         Renderer::TonePre* preOut, bool* haveAnalysis) {
  (void)ps;
  const Graph& g = graph_;
  const NodeInstance* latN = g.find(NodeType::ColourLattice);
  const NodeInstance* preN = g.find(NodeType::ToneAnalysisPre);
  const NodeInstance* anN = g.find(NodeType::Analysis);
  const NodeInstance* ovN = g.find(NodeType::LatticeOverflow);
  const int nReq = std::clamp(n, 17, 65);
  const TableEnv env{job.renderer, job.src};
  const bool move = job.params.toneStrength > 0.f;

  // the finest cached lattice of this colour stage that is at least as fine as asked for
  std::shared_ptr<std::vector<float>> lattice;
  int latticeN = 0;
  Hash128 latticeKey;
  if (ResultCache::Entry* e = cache_.findBest(NodeType::ColourLattice, latN->family, nReq)) {
    lattice = as<std::vector<float>>(e->payload);
    latticeN = e->order;
    latticeKey = e->key;
  }

  TableKeys tk;  // keys under the lattice actually used (see effectiveTableKeys)
  auto deriveKeys = [&] { tk = effectiveTableKeys(g, ps, latticeN); };
  if (lattice) deriveKeys();

  Renderer::TonePre pre;
  bool preReady = false;
  auto ensurePre = [&]() -> Renderer::TonePre& {
    if (preReady) return pre;
    if (ResultCache::Entry* e = cache_.find(NodeType::ToneAnalysisPre, tk.pre)) {
      pre = *as<Renderer::TonePre>(e->payload);
    } else {
      pre = evalToneAnalysisPre(env, lattice.get(), latticeN, job.request.fastAnalysis);
      ResultCache::Entry ne;
      ne.key = ne.family = tk.pre;
      ne.payload = std::make_shared<Renderer::TonePre>(pre);
      ne.bytes = sizeof(Renderer::TonePre) + pre.toneMove.size() * 4;
      cache_.put(NodeType::ToneAnalysisPre, std::move(ne));
      ++stats_.preBuilds;
    }
    preReady = true;
    return pre;
  };

  std::shared_ptr<const BakedLut> bl;
  if (lattice) {
    if (ResultCache::Entry* e = cache_.find(NodeType::LegacyRender, combine(tk.tone, tk.lattice))) {
      bl = as<BakedLut>(e->payload);
      out.bake = FrameResult::Bake::FullHit;
    }
  }
  if (!bl) {
    if (lattice) {
      out.bake = FrameResult::Bake::LatticeReuse;
      bakeCounters().latticeReused.fetch_add(1, std::memory_order_relaxed);
    } else {
      out.bake = FrameResult::Bake::LatticeBake;
      bakeCounters().latticeEvaluated.fetch_add(1, std::memory_order_relaxed);
      lattice = evalColourLattice(env, nReq);
      latticeN = nReq;
      latticeKey = withOrder(latN->family, nReq);
      ResultCache::Entry ne;
      ne.key = latticeKey;
      ne.family = latN->family;
      ne.order = nReq;
      ne.payload = lattice;
      ne.bytes = lattice->size() * sizeof(float);
      cache_.put(NodeType::ColourLattice, std::move(ne));
      deriveKeys();
    }
    ToneTableOut tt;
    if (ResultCache::Entry* e = cache_.find(NodeType::ToneTable, tk.tone)) {
      tt = *as<ToneTableOut>(e->payload);
    } else {
      const Renderer::TonePre* p = (move && preN) ? &ensurePre() : nullptr;
      tt = evalToneTable(env, p);
      ResultCache::Entry ne;
      ne.key = ne.family = tk.tone;
      ne.payload = std::make_shared<ToneTableOut>(tt);
      ne.bytes = (tt.tone.size() + tt.toneMove.size()) * 4;
      cache_.put(NodeType::ToneTable, std::move(ne));
      ++stats_.toneTableBuilds;
    }
    auto nb = std::make_shared<BakedLut>();
    nb->n = latticeN;
    nb->rgb = *lattice;
    nb->tone = std::move(tt.tone);
    nb->toneMove = std::move(tt.toneMove);
    const Hash128 ak = combine(tk.tone, tk.lattice);
    nb->key = ak.hex() + "#" + std::to_string(latticeN);
    nb->colourKey = latN->family.hex();
    ResultCache::Entry ae;
    ae.key = ak;
    ae.family = tk.tone;
    ae.order = latticeN;
    ae.ref = latticeKey;
    ae.payload = nb;
    ae.bytes = nb->rgb.size() * 4 + nb->tone.size() * 4;
    cache_.put(NodeType::LegacyRender, std::move(ae));
    // a lattice lives exactly as long as a retained bake refers to it (the old bakes[3] kept it inside each bake)
    const auto& live = cache_.entries(NodeType::LegacyRender);
    cache_.eraseIf(NodeType::ColourLattice, [&](const ResultCache::Entry& le) {
      for (const auto& a : live)
        if (a.ref == le.key) return false;
      return true;
    });
    bl = nb;
  }

  if (ovN) {  // overflow lattice for a GPU executor: cached by its own key, not part of the CPU frame
    if (!cache_.find(NodeType::LatticeOverflow, tk.overflow)) {
      ResultCache::Entry ne;
      ne.key = ne.family = tk.overflow;
      auto v = std::make_shared<std::vector<float>>(evalLatticeOverflow(env, 33, 8.f));
      ne.bytes = v->size() * 4;
      ne.payload = v;
      cache_.put(NodeType::LatticeOverflow, std::move(ne));
    }
  }

  if (anN) {
    if (ResultCache::Entry* e = cache_.find(NodeType::Analysis, tk.analysis)) {
      out.analysis = *as<ToneAnalysis>(e->payload);
    } else {
      const Renderer::TonePre* p = preN ? &ensurePre() : nullptr;
      out.analysis = evalAnalysis(env, p);
      ResultCache::Entry ne;
      ne.key = ne.family = tk.analysis;
      ne.payload = std::make_shared<ToneAnalysis>(out.analysis);
      ne.bytes = out.analysis.histogram.size() * 4 + 128;
      cache_.put(NodeType::Analysis, std::move(ne));
      ++stats_.analysisBuilds;
    }
    out.hasAnalysis = true;
    if (haveAnalysis) *haveAnalysis = true;
  }
  (void)preOut;
  return bl;
}

bool EngineSession::renderFrame(FrameJob& job, FrameResult& out) {
  const auto t0 = Clock::now();
  ParamInputs in;
  in.edit = job.params;
  in.frame = job.request;
  if (job.fastOptions.region.active()) {  // the region is part of what a frame (and its cached solves) depends on
    const RenderRegion& R = job.fastOptions.region;
    in.frame.region[0] = R.x0, in.frame.region[1] = R.y0, in.frame.region[2] = R.x1, in.frame.region[3] = R.y1;
    in.frame.regionLongEdge = R.outLongEdge;
  }
  in.ctx.sourceId = id_;
  in.ctx.sceneReferred = job.src.sceneReferred;
  in.ctx.hasProxy = job.proxy != nullptr;
  in.ctx.cameraId = job.camera.id;
  in.ctx.lutIdentity = reinterpret_cast<std::uintptr_t>(job.lut);
  const std::shared_ptr<const film::FilmLibrary> lib = film::defaultLibrary();
  in.ctx.filmLib = lib.get();
  in.ctx.srcW = job.src.linear.width;
  in.ctx.srcH = job.src.linear.height;
  const ParamSet ps = ParamSet::make(std::move(in));
  graph_ = GraphBuilder::build(ps, GraphDetail::Legacy);
  const ValidationResult v = graph_.validate();
  if (!v.ok) {
    out.error = "engine graph: " + v.error;
    return false;
  }
  graph_.computeKeys(ps);
  stats_.graphMs += msSince(t0);

  const int n = job.request.bakeSize;
  bool haveAnalysis = false;
  std::shared_ptr<const BakedLut> bake;
  if (mode() == Mode::LegacyBakes) bake = bakeLegacy(job, n, out);
  else bake = bakeTables(job, ps, n, out, nullptr, &haveAnalysis);

  // ---- LegacyRender: the existing renderer ------------------------------------------------------------------------------
  int w = 0, h = 0;
  std::uint8_t* rgba = nullptr;
  if (job.gpuHook) rgba = job.gpuHook(*bake, w, h);
  if (!rgba && mode() == Mode::Full) {
    const int r = renderFull(job, ps, *bake, out);
    if (r < 0) return false;
    if (r == 0) {
      rgba = out.rgba;
      w = out.w;
      h = out.h;
    }
  }
  if (!rgba) rgba = job.renderer.renderBakedRgba(job.src, *bake, job.fastOptions, w, h);
  if (!rgba) {
    out.error = "out of memory";
    return false;
  }
  out.rgba = rgba;
  out.w = w;
  out.h = h;
  if (mode() == Mode::LegacyBakes && job.request.wantAnalysis) {
    out.analysis = bake->analysis;
    out.hasAnalysis = true;
  }

  ++stats_.frames;
  totals().frames.fetch_add(1, std::memory_order_relaxed);
  switch (out.bake) {
    case FrameResult::Bake::FullHit: ++stats_.fullHits; totals().fullHits.fetch_add(1, std::memory_order_relaxed); break;
    case FrameResult::Bake::LatticeReuse: ++stats_.latticeReuse; totals().latticeReuse.fetch_add(1, std::memory_order_relaxed); break;
    case FrameResult::Bake::LatticeBake: ++stats_.latticeBake; totals().latticeBake.fetch_add(1, std::memory_order_relaxed); break;
  }
  return true;
}

std::string infoJson() {
  const Totals& t = totals();
  const BakeCounters& c = bakeCounters();
  const Mode m = mode();
  std::string j = "{\"engine\":\"";
  j += m == Mode::Off ? "off" : (m == Mode::LegacyBakes ? "legacy" : (m == Mode::Tables ? "tables" : "full"));
  j += "\",\"frames\":" + std::to_string(t.frames.load());
  j += ",\"fullHits\":" + std::to_string(t.fullHits.load());
  j += ",\"latticeReuse\":" + std::to_string(t.latticeReuse.load());
  j += ",\"latticeBake\":" + std::to_string(t.latticeBake.load());
  if (m == Mode::Full) {
    const std::uint64_t n = t.planned.load();
    j += ",\"planned\":" + std::to_string(n);
    j += ",\"planUsPerFrame\":" + std::to_string(n ? t.planUs.load() / n : 0);
    j += ",\"execUsPerFrame\":" + std::to_string(n ? t.execUs.load() / n : 0);
    j += ",\"sweeps\":" + std::to_string(t.sweeps.load()) + ",\"tiles\":" + std::to_string(t.tiles.load());
    j += ",\"solveHits\":" + std::to_string(t.solveHits.load()) + ",\"cancelled\":" + std::to_string(t.cancelled.load());
    j += ",\"fallbacks\":" + std::to_string(t.fallbacks.load());
  }
  j += ",\"bakeLatticeEvaluated\":" + std::to_string(c.latticeEvaluated.load());
  j += ",\"bakeLatticeReused\":" + std::to_string(c.latticeReused.load());
  j += ",\"bakeToneTables\":" + std::to_string(c.toneTables.load());
  j += "}";
  return j;
}

}  // namespace dapple::engine
