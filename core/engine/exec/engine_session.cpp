#include "engine/exec/engine_session.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#if defined(__ANDROID__)
#include <android/log.h>
#include <sys/system_properties.h>
#endif

#include "engine/exec/cpu_executor.h"
#include "local_eval.h"
#include "engine/plan/plan.h"

namespace dapple::engine::filmnode {
void filmAsyncTableStats(unsigned long long*, unsigned long long*, unsigned long long*);
}
namespace dapple::engine {

namespace {

std::atomic<int> g_override{-1};
std::atomic<std::uint64_t> g_nextId{1};

struct Totals {
  std::atomic<std::uint64_t> gpuFrames{0}, gpuFallbacks{0}, gpuTransients{0}, gpuLost{0}, gpuDemotions{0};
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
    // Default: Full. Override to the old Renderer path (the golden oracle / A/B switch): env DAPPLE_ENGINE=off|0, or on Android
    // the system property debug.dapple.engine (read when the env var is unset).
    const char* e = std::getenv("DAPPLE_ENGINE");
#if defined(__ANDROID__)
    char propBuf[PROP_VALUE_MAX] = {0};
    if ((!e || !*e) && __system_property_get("debug.dapple.engine", propBuf) > 0 && propBuf[0]) e = propBuf;
#endif
    if (!e || !*e) return Mode::Full;
    if (std::strcmp(e, "0") == 0 || std::strcmp(e, "off") == 0) return Mode::Off;
    return Mode::Full;  // "full", "1", anything else (the retired "legacy" / "tables" modes included)
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

namespace {
std::atomic<GpuProvider*> g_provider{nullptr};
}
void installGpuProvider(GpuProvider* p) { g_provider.store(p); }
GpuProvider* gpuProvider() { return g_provider.load(); }

GpuCounters gpuCounters() {
  const Totals& t = totals();
  return {t.gpuFrames.load(), t.gpuFallbacks.load(), t.gpuTransients.load(), t.gpuLost.load(), t.gpuDemotions.load()};
}

bool engineInUse() {
  if (mode() != Mode::Off) return true;
  GpuProvider* p = gpuProvider();
  return p && p->enabled();
}

Mode mode() {
  const int o = g_override.load(std::memory_order_relaxed);
  return o >= 0 ? Mode(o) : envMode();
}
void setModeOverride(int m) { g_override.store(m, std::memory_order_relaxed); }

// ---- table node evaluators: thin wrappers over the Renderer::bake pieces -------------------------------------------------

std::shared_ptr<void> bakeNode(NodeType type, const void* resolved, std::span<const void* const> inputs) {
  const CpuImpl* impl = cpuImpl(type);
  if (!impl || !impl->bake) return nullptr;
  void* p = nullptr;
  if (impl->bake(resolved, inputs, &p) != Status::Ok || !p) return nullptr;
  void (*fr)(void*) = impl->freePayload;
  return std::shared_ptr<void>(p, [fr](void* q) {
    if (fr) fr(q);
  });
}

std::shared_ptr<std::vector<float>> evalColourLattice(const TableEnv& env, int n) {
  const TableBakeCtx c{&env, n, false, 0.f};
  return as<std::vector<float>>(bakeNode(NodeType::ColourLattice, &c));
}

Renderer::TonePre evalToneAnalysisPre(const TableEnv& env, std::vector<float>* lattice, int n, bool fast) {
  const TableBakeCtx c{&env, n, fast, 0.f};
  const void* in[1] = {lattice};
  const auto p = as<Renderer::TonePre>(bakeNode(NodeType::ToneAnalysisPre, &c, in));
  return p ? *p : Renderer::TonePre{};
}

ToneTableOut evalToneTable(const TableEnv& env, const Renderer::TonePre* pre) {
  const TableBakeCtx c{&env, 0, false, 0.f};
  const void* in[1] = {pre};
  const auto p = as<ToneTableOut>(bakeNode(NodeType::ToneTable, &c, in));
  return p ? *p : ToneTableOut{};
}

std::vector<float> evalLatticeOverflow(const TableEnv& env, int n, float maxV) {
  const TableBakeCtx c{&env, n, false, maxV};
  const auto p = as<std::vector<float>>(bakeNode(NodeType::LatticeOverflow, &c));
  return p ? *p : std::vector<float>{};
}

ToneAnalysis evalAnalysis(const TableEnv& env, const Renderer::TonePre* pre) {
  const TableBakeCtx c{&env, 0, false, 0.f};
  const void* in[1] = {pre};
  const auto p = as<ToneAnalysis>(bakeNode(NodeType::Analysis, &c, in));
  return p ? *p : ToneAnalysis{};
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
  cache_.setLimit(NodeType::Resize, 4);  // scaled copies of the source (resizedSource)
}

namespace {

// Local adjustments (docs section 40): the LocalDetail halo and the amount planes of a frame.
void localRequest(PlanRequest& rq, const local::Frame* lf) {
  if (!lf || !lf->anyWindow) return;
  rq.localHalo = local::detailRadii(*lf, rq.scale).halo;
  local::AuxTarget t;
  rq.localAux = local::auxLayout(*lf, t);
}

// The resolved local adjustments of a GPU frame (the CPU frames get theirs from FrameKernels). Film frames: none in v1 (40.7).
void bindLocal(FramePlan& fp, const Renderer& r, const SourceImage& src, bool interactive) {
  if (r.params().local.empty()) return;
  fp.local = local::bind(r.params().local, src, interactive);
  fp.localDetail = fp.local && fp.local->anyWindow;
}

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
  localRequest(rq, fk.localFrame());
  return rq;
}

void addStats(EngineStats& st, const ExecStats& es) {
  st.sweeps += std::uint64_t(es.sweeps);
  st.tiles += std::uint64_t(es.tiles);
  st.solveHits += std::uint64_t(es.solveHits);
  st.solveBuilds += std::uint64_t(es.solveBuilds);
  st.streamed += es.streamed ? 1 : 0;
}

// Plan request from a resolved frame (the GPU path has no FrameKernels: the executor reads the FramePlan).
PlanRequest requestFromFrame(const FramePlan& fp, ExecMode mode, int tile, std::size_t budget, bool filmTiles = false) {
  PlanRequest rq;
  rq.filmTiles = filmTiles;
  rq.frameW = fp.outW;
  rq.frameH = fp.outH;
  rq.mode = mode;
  rq.tile = tile;
  rq.memBudget = budget;
  rq.nrLuma = fp.nrLuma;
  rq.nrColor = fp.nrColor;
  rq.dehaze = fp.dehaze;
  rq.clarity = fp.clarity;
  rq.texture = fp.texture;
  rq.sharpen = fp.sharpen;
  rq.sharpRadius = fp.sharpRadius;
  rq.grain = fp.grain;
  rq.scale = fp.spatialScale;
  rq.vignette = fp.vignette != 0.f;
  rq.useGeo = fp.useGeo;
  rq.outChannels = 4;
  localRequest(rq, fp.local.get());
  return rq;
}

void gpuLog(const char* what, const std::string& why) {
#if defined(__ANDROID__)
  __android_log_print(ANDROID_LOG_WARN, "dapple-gpu", "%s: %s", what, why.c_str());
#else
  std::fprintf(stderr, "dapple-gpu: %s: %s\n", what, why.c_str());
#endif
}

// ---- per-frame executor accounting + throttled log (executorStatsJson, docs/engine/pipeline-architecture.md 24) --------------------------------
struct ExecLog {
  std::mutex mu;
  std::map<std::string, std::uint64_t> reasons;  // CPU frames by the reason the GPU did not render them (at most kMaxReasons distinct texts)
  std::uint64_t gpu = 0, cpu = 0, sinceLog = 0;
  double maxTotalMs = 0;  // slowest frame since the last log line
  std::string lastExec, lastReason;
};
constexpr std::size_t kMaxReasons = 48;
ExecLog& execLog() {
  static ExecLog* l = new ExecLog;  // leaked: frames may still be counted while statics are destroyed
  return *l;
}

bool engineLogOn() {
#if defined(__ANDROID__)
  return true;
#else
  const char* e = std::getenv("DAPPLE_ENGINE_LOG");
  return e && e[0] == '1';
#endif
}

std::string jsonStr(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') o.push_back('\\');
    if (static_cast<unsigned char>(c) >= 32) o.push_back(c);
  }
  return o;
}

// One frame of the engine finished: count it and log "frame WxH tier film exec reason" once per 30 frames and whenever the executor or reason changes.
void noteFrame(const FrameJob& job, const FrameResult& out) {
  const std::string exec = out.executor.empty() ? "cpu" : out.executor;
  const bool onGpu = exec == "vulkan" || exec == "gles31" || exec == "metal";
  const std::string reason = onGpu ? std::string() : (out.why.empty() ? std::string("none given") : out.why);
  ExecLog& L = execLog();
  bool doLog = false;
  double maxMs = 0;
  std::uint64_t g = 0, cp = 0;
  {
    std::lock_guard<std::mutex> lk(L.mu);
    if (onGpu) ++L.gpu;
    else {
      ++L.cpu;
      auto it = L.reasons.find(reason);
      if (it != L.reasons.end()) ++it->second;
      else ++L.reasons[L.reasons.size() < kMaxReasons ? reason : std::string("(other)")];
    }
    L.maxTotalMs = std::max(L.maxTotalMs, out.cpu.totalMs);
    const bool changed = exec != L.lastExec || reason != L.lastReason;
    static const bool every = std::getenv("DAPPLE_FRAME_PROFILE") != nullptr;  // one line per frame (bench)
    // one line per frame on a phone too: `setprop debug.dapple.frame_profile 1` (read per frame, a cheap property lookup)
    bool everyProp = false;
#if defined(__ANDROID__)
    {
      char pv[PROP_VALUE_MAX] = {0};
      everyProp = __system_property_get("debug.dapple.frame_profile", pv) > 0 && pv[0] == '1';
    }
#endif
    // a frame whose CPU side alone took over 25 ms is always logged (wave 8: any frame over 33 ms in a drag must be attributable from logcat)
    const bool slow = out.cpu.totalMs > 25.0;
    doLog = changed || ++L.sinceLog >= 30 || every || everyProp || slow;
    if (doLog) {
      L.sinceLog = 0;
      maxMs = L.maxTotalMs;
      L.maxTotalMs = 0;
      L.lastExec = exec;
      L.lastReason = reason;
    }
    g = L.gpu;
    cp = L.cpu;
  }
  if (!doLog || !engineLogOn()) return;
  const SpatialQuality q = job.fastOptions.spatial;
  const char* tier = q == SpatialQuality::Interactive ? "interactive" : q == SpatialQuality::Full ? "full" : "off";
  char line[512];
  const FrameResult::CpuSplit& c = out.cpu;
  int n = std::snprintf(line, sizeof line, "frame %dx%d tier=%s film=%s exec=%s%s%s | gpu %llu cpu %llu", out.w, out.h, tier, job.renderer.filmActive() ? "on" : "off",
                        exec.c_str(), onGpu ? "" : " reason=", onGpu ? "" : reason.c_str(), (unsigned long long)g, (unsigned long long)cp);
  // where the CPU side of the frame went (ms): match = film exposure solve, renderer = Renderer ctor, graph, tables, exec = plan + executor incl. GPU wait
  if (n > 0 && n < int(sizeof line))
    std::snprintf(line + n, sizeof line - std::size_t(n), " | cpu_ms %.1f (match %.1f renderer %.1f graph %.1f tables %.1f exec %.1f; upload %.1f gpu %.1f) max %.1f", c.totalMs, c.matchMs,
                  c.rendererMs, c.graphMs, c.tablesMs, c.execMs, out.gpu.uploadMs, out.gpu.gpuMs, std::max(maxMs, c.totalMs));
  if (job.renderer.filmActive()) {  // background print tables of the film drag (wave 8): frames that used the previous step, tables finished, requests
    unsigned long long su = 0, bk = 0, rq = 0;
    filmnode::filmAsyncTableStats(&su, &bk, &rq);
    const std::size_t len = std::strlen(line);
    if (len + 64 < sizeof line) std::snprintf(line + len, sizeof line - len, " | film_tables stale %llu baked %llu requested %llu", su, bk, rq);
  }
#if defined(__ANDROID__)
  __android_log_print(ANDROID_LOG_INFO, "dapple_engine", "%s", line);
#else
  std::fprintf(stderr, "dapple_engine: %s\n", line);
#endif
}

// SessionCtx::gainFn: white balance x exposure of the Gain node (what the film folds into its input matrix)
struct GainCtx {
  const CameraProfile* camera;
  bool sceneReferred;
};
void gainOf(const void* user, const EditParams& e, float out[3]) {
  const auto* g = static_cast<const GainCtx*>(user);
  Renderer::inputGain(e, *g->camera, g->sceneReferred, out);
}

}  // namespace

std::string executorStatsJson() {
  ExecLog& L = execLog();
  std::string j;
  std::string api;
  {
    std::lock_guard<std::mutex> lk(L.mu);
    api = L.lastExec.empty() ? "none" : L.lastExec;
    j = "{\"gpuFrames\":" + std::to_string(L.gpu) + ",\"cpuFrames\":" + std::to_string(L.cpu) + ",\"deviceLost\":" + std::to_string(totals().gpuLost.load()) +
        ",\"demotions\":" + std::to_string(totals().gpuDemotions.load()) + ",\"api\":\"" + api + "\",\"lastReason\":\"" + jsonStr(L.lastReason) + "\",\"unsupported\":{";
    bool first = true;
    for (const auto& kv : L.reasons) {
      j += std::string(first ? "" : ",") + "\"" + jsonStr(kv.first) + "\":" + std::to_string(kv.second);
      first = false;
    }
  }
  const local::Counters lc = local::counters();  // local adjustments (docs section 40): plane builds / cache hits
  j += "},\"local\":{\"binds\":" + std::to_string(lc.binds) + ",\"brushBuilds\":" + std::to_string(lc.brushBuilds) +
       ",\"brushPrefixHits\":" + std::to_string(lc.brushPrefixHits) + ",\"rasterBuilds\":" + std::to_string(lc.rasterBuilds) +
       ",\"guideBuilds\":" + std::to_string(lc.guideBuilds) + ",\"planeHits\":" + std::to_string(lc.planeHits);
  return j + "}}";
}

int EngineSession::renderFull(FrameJob& job, const ParamSet& ps, const BakedLut& bake, FrameResult& out) {
  if (gpuUsable(job)) {  // Vulkan -> GLES 3.1 -> CPU (5.9)
    const int g = renderGpu(job, ps, bake, out);
    if (g != 1) return g;
  } else {
    GpuProvider* pv = gpuProvider();
    frameWhy_ = !pv ? "no GPU provider (build without the GPU executor)" : !pv->enabled() ? pv->inactiveReason() : "no source store (host frame)";
  }
  FrameKernels fk(job.renderer, job.src, &bake, job.fastOptions);
  if (!fk.valid()) {
    frameWhy_ += std::string(frameWhy_.empty() ? "" : "; ") + "empty source";
    return 1;
  }
  const auto t0 = Clock::now();
  const FramePlan fp = fk.framePlan();
  Graph g = GraphBuilder::build(ps, GraphDetail::FullFilm, &fp);
  const ValidationResult v = g.validate();
  if (!v.ok) {
    out.error = "engine graph: " + v.error;
    return -1;
  }
  if (job.holdSolves) g.computeKeysHoldingSolves(ps);
  else g.computeKeys(ps);
  const Plan plan = Planner::plan(g, makeRequest(fk, ExecMode::Whole, 0, 0, 4), &cache_);
  {
    const double ms = msSince(t0);
    stats_.planMs += ms;
    totals().planUs.fetch_add(std::uint64_t(ms * 1000.0), std::memory_order_relaxed);
  }
  ++stats_.planned;
  totals().planned.fetch_add(1, std::memory_order_relaxed);
  if (plan.unsupported) {
    frameWhy_ += std::string(frameWhy_.empty() ? "" : "; ") + "cpu plan unsupported: " + std::string(plan.why ? plan.why : "?");
    return 1;
  }
  const int w = fk.width(), h = fk.height();
  auto* rgba = job.outFloat ? nullptr : static_cast<std::uint8_t*>(std::malloc(std::size_t(w) * h * 4));
  if (!rgba && !job.outFloat) {
    out.error = "out of memory";
    return -1;
  }
  ExecEnv env{fk, g, cache_, CancelToken{&latest_, latest_.load(std::memory_order_relaxed)}, rgba, job.outFloat ? 3 : 4, job.outFloat};
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
    out.floatOut = job.outFloat != nullptr;
    out.executor = "cpu";
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

int EngineSession::renderExactGpu(const EditParams& params, const Renderer& renderer, const SourceImage& src, const ExactOptions& opt, Image8& out,
                                  std::string* err) {
  GpuProvider* pv = gpuProvider();
  if (!gpuOverride_ && !(pv && pv->enabled())) return 1;
  IGpuExecutor* x = gpuExecutor();
  if (!x) return 1;
  const int W = src.linear.width, H = src.linear.height;
  if (W < 1 || H < 1) return 1;
  FastOptions fo;
  fo.spatial = SpatialQuality::Full;
  FramePlan fp = renderer.framePlan(W, H, src.sceneReferred, fo);
  if (!fp.supported) return 1;
  bindLocal(fp, renderer, src, false);
  ParamInputs in;
  in.edit = params;
  in.frame.exact = true;
  in.ctx.sourceId = id_;
  in.ctx.sceneReferred = src.sceneReferred;
  in.ctx.hasProxy = opt.hasProxy;
  const std::shared_ptr<const film::FilmLibrary> lib = film::defaultLibrary();
  in.ctx.filmLib = lib.get();
  in.ctx.srcW = W;
  in.ctx.srcH = H;
  const ParamSet ps = ParamSet::make(std::move(in));
  Graph g = GraphBuilder::build(ps, GraphDetail::FullFilm, &fp);  // the exact variant: ToneColourExact, no table nodes
  if (const ValidationResult v = g.validate(); !v.ok) {
    if (err) *err = "engine graph: " + v.error;
    return -1;
  }
  g.computeKeys(ps);
  // The GPU evaluates the exact colour stage through the finest lattice (65^3 + the overflow lattice) baked with the exact analysis; the CPU
  // executor remains the per-pixel reference (docs/engine/pipeline-architecture.md 21).
  const std::shared_ptr<const BakedLut> bake = renderer.bake(src, 65, nullptr, false);
  if (!bake) return 1;
  renderer.hdrBind(src, fp.hdr, &bake->tone);
  const bool tiled = opt.mode == ExecMode::TiledExport;
  const int tile = opt.tile > 0 ? opt.tile : (x->caps().unifiedMemory ? 1024 : 2048);
  const Plan plan = Planner::plan(g, requestFromFrame(fp, tiled ? ExecMode::TiledExport : ExecMode::Whole, tiled ? tile : 0, std::size_t(1) << 30, true), nullptr);
  MemorySourceStore store(src);
  GpuRun run{plan, g, fp, store, *bake, nullptr, &renderer, 0.f, GpuCancel{&latest_, latest_.load(std::memory_order_relaxed)}, {}, false};
  out.width = fp.outW;
  out.height = fp.outH;
  out.data.assign(std::size_t(out.width) * out.height * 3, 0);
  if (tiled) {
    run.target.sink = [&](int x0, int y0, int tw, int th, const std::uint8_t* rgba, std::size_t stride) {
      for (int j = 0; j < th; ++j) {
        const std::uint8_t* s = rgba + std::size_t(j) * stride;
        std::uint8_t* d = out.data.data() + (std::size_t(y0 + j) * std::size_t(out.width) + std::size_t(x0)) * 3;
        for (int i = 0; i < tw; ++i) {
          d[3 * i] = s[4 * i];
          d[3 * i + 1] = s[4 * i + 1];
          d[3 * i + 2] = s[4 * i + 2];
        }
      }
      return true;
    };
  }
  GpuOut go;
  const int r = runOnGpu(run, go);
  if (r == 0) {
    if (!tiled) {
      if (!go.rgba || go.w != out.width || go.h != out.height) {
        std::free(go.rgba);
        return 1;
      }
      for (std::size_t i = 0, n = std::size_t(out.width) * out.height; i < n; ++i) {
        out.data[3 * i] = go.rgba[4 * i];
        out.data[3 * i + 1] = go.rgba[4 * i + 1];
        out.data[3 * i + 2] = go.rgba[4 * i + 2];
      }
      std::free(go.rgba);
    }
    return 0;
  }
  if (r == -2) {
    if (err) *err = "cancelled";
    return -1;
  }
  if (r < 0 && err) *err = go.error;
  return r;
}

bool EngineSession::renderExact(const EditParams& params, const Renderer& renderer, const SourceImage& src,
                                const ExactOptions& opt, Image8& out, std::string* err, ExecStats* statsOut) {
  const bool streaming = bool(opt.sourceWindow) && bool(opt.rows);  // section 42
  if (!opt.outFloat && opt.spatial && !opt.sessionCache && !streaming) {  // export / reference frames: the GPU executor first (exact variant on the dense lattice)
    const int gr = renderExactGpu(params, renderer, src, opt, out, err);
    if (gr == 0) return true;
    if (gr < 0) return false;
  }
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
  Graph g = GraphBuilder::build(ps, GraphDetail::FullFilm, &fp);
  const ValidationResult v = g.validate();
  if (!v.ok) {
    if (err) *err = "engine graph: " + v.error;
    return false;
  }
  g.computeKeys(ps);
  const auto t0 = Clock::now();
  ResultCache local;
  ResultCache& cache = opt.sessionCache ? cache_ : local;
  PlanRequest prq = makeRequest(fk, opt.mode, opt.tile, opt.memBudget, 3);
  // a streamed film frame keeps its tiles: the film plane arrives in bands (section 42.6); env DAPPLE_FILM_TILES=1: the whole-frame film export in
  // tiles too (comparisons at the same tile size)
  prq.filmTiles = streaming || (std::getenv("DAPPLE_FILM_TILES") && std::getenv("DAPPLE_FILM_TILES")[0] == '1');
  Plan plan = Planner::plan(g, prq, &cache);
  if (streaming && opt.tile > 0 && !std::getenv("DAPPLE_EXPORT_STREAM_TILE")) {  // a wide halo (local clarity / dehaze: ~400 px at 60 MP) costs (T + 2h)^2 / T^2 per tile: wider tiles then
    int h = 0;
    for (const Stage& st : plan.stages) h = std::max({h, st.halo.x, st.halo.y});
    const int want = std::min(1024, (2 * h + 63) / 64 * 64);
    if (want > opt.tile && plan.grid.nx * plan.grid.ny > 1) {
      prq.tile = want;
      plan = Planner::plan(g, prq, &cache);
    }
  }
  if (std::getenv("DAPPLE_PLAN_LOG")) {  // diagnostics: the stages of an exact render
    std::string l = "plan " + std::to_string(plan.grid.tile) + " tile, " + std::to_string(plan.grid.nx) + "x" + std::to_string(plan.grid.ny) + ":";
    for (const Stage& st : plan.stages) {
      l += st.kind == StageKind::TileSweep ? " [sweep" : st.kind == StageKind::GlobalSolve ? " [solve" : st.kind == StageKind::FilmFrame ? " [film" : " [tables";
      for (const Group& gr : st.groups)
        for (NodeId id : gr.nodes) l += std::string(" ") + NodeRegistry::name(g.nodes()[id].type);
      l += "]";
    }
    std::fprintf(stderr, "%s\n", l.c_str());
  }
  stats_.planMs += msSince(t0);
  ++stats_.planned;
  ExecEnv env{fk, g, cache, CancelToken{&latest_, latest_.load(std::memory_order_relaxed)}, nullptr, 3};
  std::vector<std::uint8_t> band;  // streaming export: the band in flight
  int winX0 = 0, winY0 = 0, winX1 = 0, winY1 = 0;  // source rect of the window the FrameKernels hold
  bool haveWin = false;
  std::string filmWhy;  // streaming export: why the film could not run in bands
  if (streaming) {
    if (opt.outFloat || plan.unsupported) {
      if (err) *err = "engine: streaming export needs an 8-bit tiled plan";
      return false;
    }
    const int W = fk.width(), H = fk.height();
    const int bandRows = plan.grid.nx * plan.grid.ny == 1 ? H : std::min(H, plan.grid.tile);
    band.assign(std::size_t(bandRows) * std::size_t(W) * 3, 0);
    out.width = W;
    out.height = H;
    out.data.clear();
    env.out = band.data();
    // the source window of a source rect (kept while the next rects fall inside it)
    auto useWindow = [&](int sx0, int sy0, int sx1, int sy1) {
      if (haveWin && sx0 >= winX0 && sy0 >= winY0 && sx1 <= winX1 && sy1 <= winY1) return true;
      fk.setSourceWindow(nullptr, 0, 0);
      haveWin = false;
      const ImageF* w = opt.sourceWindow(sx0, sy0, sx1, sy1);
      if (!w || w->width != sx1 - sx0 || w->height != sy1 - sy0) return false;
      fk.setSourceWindow(w, sx0, sy0);
      winX0 = sx0, winY0 = sy0, winX1 = sx1, winY1 = sy1;
      haveWin = true;
      return true;
    };
    fk.setSourceProvider(useWindow);
    env.filmBegin = [&] { return fk.filmStreamBegin(&filmWhy); };
    env.filmRows = [&](int y0, int y1, ImageF& o) { return fk.filmStreamRows(y0, y1, o); };
    env.filmAux = [&](int x0, int y0, int x1, int y1, const local::AuxTarget& a) { return fk.filmStreamAux(x0, y0, x1, y1, a); };
    env.prepare = [&](int x0, int y0, int x1, int y1) {
      int sx0, sy0, sx1, sy1;
      fk.sourceRect(x0, y0, x1, y1, sx0, sy0, sx1, sy1);
      if (haveWin && sx0 >= winX0 && sy0 >= winY0 && sx1 <= winX1 && sy1 <= winY1) return true;
      // the source of the whole band row when that is not much more than the band (no or quarter-turn geometry), else of this tile alone
      int bx0, by0, bx1, by1;
      fk.sourceRect(0, y0, W, y1, bx0, by0, bx1, by1);
      if (double(bx1 - bx0) * double(by1 - by0) <= 2.5 * double(W) * double(y1 - y0)) sx0 = bx0, sy0 = by0, sx1 = bx1, sy1 = by1;
      fk.setSourceWindow(nullptr, 0, 0);
      haveWin = false;
      const ImageF* w = opt.sourceWindow(sx0, sy0, sx1, sy1);
      if (!w || w->width != sx1 - sx0 || w->height != sy1 - sy0) return false;
      fk.setSourceWindow(w, sx0, sy0);
      winX0 = sx0, winY0 = sy0, winX1 = sx1, winY1 = sy1;
      haveWin = true;
      return true;
    };
    env.bandDone = [&](int y0, int y1) { return opt.rows(y0, y1 - y0, band.data()); };
  } else if (opt.outFloat) {
    env.outF = opt.outFloat;
  } else {
    out.width = fk.width();
    out.height = fk.height();
    out.data.resize(std::size_t(out.width) * out.height * 3);
    env.out = out.data.data();
  }
  ExecStats es;
  if (opt.beforeExec) opt.beforeExec();
  Status s = plan.unsupported ? Status::Unsupported : CpuExecutor::run(plan, env, &es);
  if (streaming) {
    fk.setSourceWindow(nullptr, 0, 0);
    fk.setSourceProvider(nullptr);
  }
  addStats(stats_, es);
  if (statsOut) *statsOut = es;
  if (s == Status::Unsupported) {
    ++stats_.fallbacks;
    // No second renderer behind the engine: a film whose stock / paper is not in the library or whose table bake failed is an explicit error
    // like any other unsupported plan.
    if (err)
      *err = "engine: plan not supported by the CPU executor" + (std::string(plan.why ? plan.why : "").empty() ? std::string() : " (" + std::string(plan.why) + ")") +
             (renderer.filmActive() ? (filmWhy.empty() ? " [film: unknown stock / paper or failed table bake]" : " [" + filmWhy + "]") : "");
    return false;
  }
  if (s != Status::Ok) {
    if (err) *err = s == Status::Cancelled ? "cancelled" : "out of memory";
    if (s == Status::Cancelled) ++stats_.cancelled;
    return false;
  }
  return true;
}

std::shared_ptr<const BakedLut> EngineSession::runHostTables(FrameJob& job, const ParamSet& ps, int n, FrameResult& out) {
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
      const TableBakeCtx bc{&env, latticeN, job.request.fastAnalysis, 0.f};
      const void* in[1] = {lattice.get()};
      const auto made = as<Renderer::TonePre>(bakeNode(NodeType::ToneAnalysisPre, &bc, in));
      pre = made ? *made : Renderer::TonePre{};
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
      const TableBakeCtx bc{&env, nReq, false, 0.f};
      lattice = as<std::vector<float>>(bakeNode(NodeType::ColourLattice, &bc));
      if (!lattice) {
        out.error = "colour lattice";
        return nullptr;
      }
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
      const TableBakeCtx bc{&env, latticeN, false, 0.f};
      const void* in[1] = {p};
      const auto made = as<ToneTableOut>(bakeNode(NodeType::ToneTable, &bc, in));
      tt = made ? *made : ToneTableOut{};
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
      const TableBakeCtx bc{&env, 33, false, 8.f};
      auto v = as<std::vector<float>>(bakeNode(NodeType::LatticeOverflow, &bc));
      if (!v) v = std::make_shared<std::vector<float>>();
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
      const TableBakeCtx bc{&env, latticeN, false, 0.f};
      const void* in[1] = {p};
      const auto made = as<ToneAnalysis>(bakeNode(NodeType::Analysis, &bc, in));
      out.analysis = made ? *made : ToneAnalysis{};
      ResultCache::Entry ne;
      ne.key = ne.family = tk.analysis;
      ne.payload = std::make_shared<ToneAnalysis>(out.analysis);
      ne.bytes = out.analysis.histogram.size() * 4 + 128;
      cache_.put(NodeType::Analysis, std::move(ne));
      ++stats_.analysisBuilds;
    }
    out.hasAnalysis = true;
  }
  return bl;
}

bool EngineSession::renderFrame(FrameJob& job, FrameResult& out) {
  const auto t0 = Clock::now();
  frameWhy_.clear();
  ParamInputs in;
  in.edit = job.params;
  in.frame = job.request;
  const bool gpuFrame = gpuUsable(job);
  if (gpuFrame) in.frame.wantOverflow = true;  // the device needs the overflow lattice next to the lattice (HostTable payload)
  if (job.fastOptions.region.active()) {  // the region is part of what a frame (and its cached solves) depends on
    const RenderRegion& R = job.fastOptions.region;
    in.frame.region[0] = R.x0, in.frame.region[1] = R.y0, in.frame.region[2] = R.x1, in.frame.region[3] = R.y1;
    in.frame.regionLongEdge = R.outLongEdge;
  }
  in.ctx.sourceId = id_;
  in.ctx.sceneReferred = job.src.sceneReferred;
  in.ctx.hasProxy = job.proxy != nullptr;
  in.ctx.cameraId = job.camera.id;
  in.ctx.lutIdentity = job.lut ? std::uintptr_t(job.lut->contentId()) : 0;  // content hash, not the pointer
  const std::shared_ptr<const film::FilmLibrary> lib = film::defaultLibrary();
  in.ctx.filmLib = lib.get();
  in.ctx.srcW = job.src.linear.width;
  in.ctx.srcH = job.src.linear.height;
  in.ctx.fullLongEdge = job.fastOptions.fullLongEdge;  // scale-aware film grain (14 Q8)
  in.ctx.frameIndex = job.fastOptions.frameIndex;
  const GainCtx gainCtx{&job.camera, job.src.sceneReferred};
  in.ctx.gainFn = &gainOf;
  in.ctx.gainUser = &gainCtx;
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
  const auto tTables = Clock::now();
  out.cpu.graphMs = msSince(t0);
  const std::shared_ptr<const BakedLut> bake = runHostTables(job, ps, n, out);
  out.cpu.tablesMs = msSince(tTables);
  if (!bake) {
    if (out.error.empty()) out.error = "tables";
    return false;
  }

  // ---- the frame: GPU executor where it supports the plan, else the CpuExecutor on the same plan ---------------------------
  int w = 0, h = 0;
  std::uint8_t* rgba = nullptr;
  const auto tExec = Clock::now();
  const int r = renderFull(job, ps, *bake, out);
  out.cpu.execMs = msSince(tExec);
  out.cpu.matchMs = job.matchMs;
  out.cpu.rendererMs = job.rendererMs;
  out.cpu.totalMs = msSince(t0) + job.matchMs + job.rendererMs;
  if (r < 0) return false;
  if (r == 0) {
    rgba = out.rgba;
    w = out.w;
    h = out.h;
  } else {
    // r == 1: neither executor took the plan. There is no second renderer behind it: that is an explicit error (also for a film whose stock /
    // paper is not in the library or whose table bake failed; highlight boost and the enlarger diffusion filter are film nodes).
    out.error = "engine: plan not supported by any executor" + (frameWhy_.empty() ? std::string() : " (" + frameWhy_ + ")") +
                (job.renderer.filmActive() ? " [film: unknown stock / paper or failed table bake]" : "");
    return false;
  }
  if (!rgba && !out.external && !out.floatOut) {
    out.error = "out of memory";
    return false;
  }
  out.rgba = rgba;
  out.w = w;
  out.h = h;
  if (out.executor.empty()) out.executor = "cpu";
  out.why = frameWhy_;
  noteFrame(job, out);

  ++stats_.frames;
  totals().frames.fetch_add(1, std::memory_order_relaxed);
  switch (out.bake) {
    case FrameResult::Bake::FullHit: ++stats_.fullHits; totals().fullHits.fetch_add(1, std::memory_order_relaxed); break;
    case FrameResult::Bake::LatticeReuse: ++stats_.latticeReuse; totals().latticeReuse.fetch_add(1, std::memory_order_relaxed); break;
    case FrameResult::Bake::LatticeBake: ++stats_.latticeBake; totals().latticeBake.fetch_add(1, std::memory_order_relaxed); break;
  }
  return true;
}

// ---- GPU executor selection (docs/engine/pipeline-architecture.md 5.9) --------------------------------------------------------------
//   Unsupported  this plan cannot run on the GPU -> this frame on the CPU executor, the GPU stays in use (counted)
//   Transient    OOM / timeout / failed submission -> release the device copies and retry once; else this frame on the CPU
//   DeviceLost   -> the provider rebuilds the device once (executors re-create their copies from SourceStore + tables);
//                   a failed rebuild or a second loss gives the API up and the next one (Vulkan -> GLES 3.1 -> CPU) takes over
// The provider owns the device(s); the session owns one executor (device copies of this photo) per device generation.

void EngineSession::useGpuExecutor(IGpuExecutor* x, bool force) {
  gpuOverride_ = x;
  gpuForce_ = force && x;
  if (x) gpuOwned_.reset();
}

void EngineSession::injectGpuFailure(GpuFailure f, int count) {
  if (IGpuExecutor* x = gpuExecutor()) x->injectFailure(f, count);
}

bool EngineSession::gpuUsable(const FrameJob& job) const {
  if (!job.store || job.outFloat) return false;
  if (gpuOverride_) return true;
  GpuProvider* p = gpuProvider();
  return p && p->enabled();
}

IGpuExecutor* EngineSession::gpuExecutor() {
  if (gpuOverride_) return gpuOverride_;
  GpuProvider* p = gpuProvider();
  if (!p || !p->enabled()) return nullptr;
  if (gpuOwned_ && gpuGen_ == p->generation()) return gpuOwned_.get();
  gpuOwned_.reset();
  std::string why;
  gpuOwned_ = p->createExecutor(why);
  gpuGen_ = p->generation();  // creating the first context bumps it
  if (!gpuOwned_) {
    if (why != gpuWhy_) {
      gpuWhy_ = why;
      gpuLog("GPU unavailable, using the CPU", why);
    }
    return nullptr;
  }
  return gpuOwned_.get();
}

// 0 = `go` holds the result, 1 = the CPU takes it, -1 = failed (forced executor), -2 = cancelled
int EngineSession::runOnGpu(const GpuRun& run, GpuOut& go) {
  GpuProvider* pv = gpuProvider();
  for (int attempt = 0; attempt < 2; ++attempt) {
    IGpuExecutor* x = gpuExecutor();
    if (!x) {
      frameWhy_ = gpuWhy_.empty() ? "no GPU executor" : gpuWhy_;
      return 1;
    }
    go = GpuOut{};
    const Status s = x->run(run, go);
    if (s == Status::Ok) {
      ++stats_.gpuFrames;
      totals().gpuFrames.fetch_add(1, std::memory_order_relaxed);
      gpuLastApi_ = x->api();
      return 0;
    }
    std::free(go.rgba);
    go.rgba = nullptr;
    if (s == Status::Cancelled) return -2;
    if (gpuForce_) {
      gpuLog("forced executor failed", std::string("status ") + std::to_string(int(s)) + ", failure " + std::to_string(int(go.failure)) + ": " + go.error);
      return -1;
    }
    switch (go.failure) {
      case GpuFailure::Transient:
        ++stats_.gpuTransients;
        totals().gpuTransients.fetch_add(1, std::memory_order_relaxed);
        gpuLog("frame failed", go.error);
        x->release();  // drop the device copies (pools, resident source): they are rebuilt from the SourceStore
        frameWhy_ = "transient GPU failure: " + go.error;
        if (attempt == 0) continue;
        return 1;
      case GpuFailure::DeviceLost: {
        ++stats_.gpuLost;
        totals().gpuLost.fetch_add(1, std::memory_order_relaxed);
        gpuLog("device lost", go.error);
        frameWhy_ = "device lost: " + go.error;
        if (pv && !gpuOverride_) {
          std::string why;
          if (pv->recover(why)) {
            gpuLog("device rebuilt", "");
          } else {
            totals().gpuDemotions.fetch_add(1, std::memory_order_relaxed);
            gpuLog(pv->enabled() ? "device rebuild failed, next backend" : "device rebuild failed, CPU for the rest of the session", why);
          }
          gpuOwned_.reset();
        }
        return 1;
      }
      default:
        ++stats_.gpuUnsupported;
        if (go.error.find("enlarger diffusion") != std::string::npos) ++stats_.gpuEnlargerTiled;
        totals().gpuFallbacks.fetch_add(1, std::memory_order_relaxed);
        frameWhy_ = go.error.empty() ? "unsupported (no reason given)" : go.error;
        return 1;
    }
  }
  return 1;
}

int EngineSession::renderGpu(FrameJob& job, const ParamSet& ps, const BakedLut& bake, FrameResult& out) {
  const auto t0 = Clock::now();
  int rw = job.src.linear.width, rh = job.src.linear.height;  // the frame is rendered at the size of the CPU copy unless asked otherwise
  if (job.srcLongEdge > 0) {
    const int W = job.store->width(), H = job.store->height(), longEdge = std::max(W, H);
    if (longEdge > job.srcLongEdge) {
      const double scale = double(job.srcLongEdge) / longEdge;  // image.cpp resizeToLongEdge
      rw = std::max(1, int(std::lround(W * scale)));
      rh = std::max(1, int(std::lround(H * scale)));
    } else {
      rw = W;
      rh = H;
    }
  }
  FramePlan fp = job.renderer.framePlan(rw, rh, job.src.sceneReferred, job.fastOptions);
  job.renderer.hdrBind(job.src, fp.hdr, &bake.tone);  // single-image HDR: the field comes from the CPU source (a pure function of source + white balance)
  bindLocal(fp, job.renderer, job.src, job.fastOptions.spatial == SpatialQuality::Interactive);
  if (job.resizeSharpen > 0.f && (rw < job.store->width() || rh < job.store->height())) fp.resizeSharpen = job.resizeSharpen;  // adds the Detail node
  if (!fp.supported) {
    ++stats_.gpuUnsupported;
    totals().gpuFallbacks.fetch_add(1, std::memory_order_relaxed);
    frameWhy_ = "unsupported on the GPU: " + fp.why;
    if (gpuForce_) return out.error = frameWhy_, -1;
    return 1;
  }
  Graph g = GraphBuilder::build(ps, GraphDetail::FullFilm, &fp);
  const ValidationResult v = g.validate();
  if (!v.ok) {
    out.error = "engine graph: " + v.error;
    return -1;
  }
  if (job.holdSolves) g.computeKeysHoldingSolves(ps);
  else g.computeKeys(ps);
  // the GPU plan is made against the executor's own cache of device solves (the session cache holds CPU payloads): a frame whose dehaze / clarity
  // solve is still on the device skips the accumulation sweep and the solve
  IGpuExecutor* gx = gpuExecutor();
  const Plan plan = Planner::plan(g, requestFromFrame(fp, ExecMode::Whole, 0, 0), gx ? gx->solveCache() : nullptr);
  {
    const double ms = msSince(t0);
    stats_.planMs += ms;
    totals().planUs.fetch_add(std::uint64_t(ms * 1000.0), std::memory_order_relaxed);
  }
  ++stats_.planned;
  totals().planned.fetch_add(1, std::memory_order_relaxed);
  const std::vector<float>* over = overflowFor(ps, bake.n);
  GpuRun run{plan, g, fp, *job.store, bake, over, &job.renderer, job.resizeSharpen, GpuCancel{&latest_, latest_.load(std::memory_order_relaxed)}, {}, false};
  run.target.external = job.gpuTarget;
  GpuOut go;
  const auto t1 = Clock::now();
  const int r = runOnGpu(run, go);
  if (r == 0) {
    const double ms = msSince(t1);
    stats_.execMs += ms;
    totals().execUs.fetch_add(std::uint64_t(ms * 1000.0), std::memory_order_relaxed);
    out.rgba = go.rgba;
    out.w = go.w;
    out.h = go.h;
    out.external = job.gpuTarget != nullptr && go.rgba == nullptr;
    out.gpu = go.stats;
    out.executor = gpuLastApi_ == hal::Api::Gles31 ? "gles31" : gpuLastApi_ == hal::Api::Metal ? "metal" : "vulkan";
    return 0;
  }
  if (r == -2) {
    ++stats_.cancelled;
    totals().cancelled.fetch_add(1, std::memory_order_relaxed);
    out.error = "cancelled";
    return -1;
  }
  if (r < 0) out.error = go.error.empty() ? "GPU frame failed" : go.error;
  return r;
}

// LatticeOverflow payload of the frame (cached by runHostTables when the request asked for it), or null.
const std::vector<float>* EngineSession::overflowFor(const ParamSet& ps, int n) const {
  const TableKeys tk = effectiveTableKeys(graph_, ps, n);
  if (!tk.hasOverflow) return nullptr;
  const ResultCache::Entry* e = cache_.peek(NodeType::LatticeOverflow, tk.overflow);
  return e ? static_cast<const std::vector<float>*>(e->payload.get()) : nullptr;
}

int EngineSession::exportGpu(GpuExport& e, GpuStats* stats, std::string* err) {
  auto bail = [&](int rc, const std::string& m) {
    if (err) *err = m;
    return rc;
  };
  GpuProvider* pv = gpuProvider();
  if (!gpuOverride_ && !(pv && pv->enabled())) return 1;
  IGpuExecutor* x = gpuExecutor();
  if (!x) return 1;
  const int W = e.image.linear.width, H = e.image.linear.height;
  ParamInputs in;
  in.edit = e.params;
  in.frame.bakeSize = e.latticeN;
  in.frame.fastAnalysis = false;
  in.frame.noise = true;
  in.frame.wantOverflow = true;
  in.frame.longEdge = e.outLongEdge;
  in.ctx.sourceId = id_;
  in.ctx.sceneReferred = e.image.sceneReferred;
  in.ctx.hasProxy = false;
  in.ctx.cameraId = e.camera.id;
  in.ctx.lutIdentity = e.lut ? std::uintptr_t(e.lut->contentId()) : 0;
  const std::shared_ptr<const film::FilmLibrary> lib = film::defaultLibrary();
  in.ctx.filmLib = lib.get();
  in.ctx.srcW = W;
  in.ctx.srcH = H;
  FrameJob fj{e.params, e.renderer, e.image, nullptr, e.camera, e.lut, {}, {}, {}};
  fj.request = in.frame;
  fj.fastOptions.spatial = SpatialQuality::Full;
  const ParamSet ps = ParamSet::make(std::move(in));
  graph_ = GraphBuilder::build(ps, GraphDetail::Legacy);
  if (const ValidationResult v = graph_.validate(); !v.ok) return bail(-1, "engine graph: " + v.error);
  graph_.computeKeys(ps);
  FrameResult fr;
  const std::shared_ptr<const BakedLut> bake = runHostTables(fj, ps, e.latticeN, fr);
  if (!bake) return bail(-1, "tables");

  int rw = W, rh = H;  // image.cpp resizeToLongEdge
  const int srcLong = std::max(W, H);
  const bool down = e.outLongEdge > 0 && srcLong > e.outLongEdge;
  if (down) {
    const double scale = double(e.outLongEdge) / srcLong;
    rw = std::max(1, int(std::lround(W * scale)));
    rh = std::max(1, int(std::lround(H * scale)));
  }
  FramePlan fp = e.renderer.framePlan(rw, rh, e.image.sceneReferred, fj.fastOptions);
  e.renderer.hdrBind(e.image, fp.hdr, &bake->tone);
  if (!fp.supported) return bail(1, "unsupported on the GPU: " + fp.why);
  bindLocal(fp, e.renderer, e.image, false);
  fp.resizeSharpen = down ? e.resizeSharpen : 0.f;
  Graph g = GraphBuilder::build(ps, GraphDetail::FullFilm, &fp);
  if (const ValidationResult v = g.validate(); !v.ok) return bail(-1, "engine graph: " + v.error);
  g.computeKeys(ps);
  // mobile / unified memory: tile 1024 keeps the export pool near 320 MB (pixel-architecture.md 16 rule 8)
  const int tile = e.tile > 0 ? e.tile : (x->caps().unifiedMemory ? 1024 : 2048);
  const Plan plan = Planner::plan(g, requestFromFrame(fp, ExecMode::TiledExport, tile, std::size_t(1) << 30, true), nullptr);
  if (e.onSize) e.onSize(fp.outW, fp.outH);
  GpuRun run{plan, g, fp, e.store, *bake, overflowFor(ps, bake->n), &e.renderer, fp.resizeSharpen,
             GpuCancel{&latest_, latest_.load(std::memory_order_relaxed)}, e.target, e.forceStream};
  GpuOut go;
  const int r = runOnGpu(run, go);
  if (r == 0) {
    if (stats) *stats = go.stats;
    return 0;
  }
  if (r == -2) return bail(-1, "cancelled");
  return bail(r, go.error);
}

// ---- the Resize node (CpuImpl::bake): the session source at a smaller long edge, cached ------------------------------------------------

std::shared_ptr<const SourceImage> EngineSession::resizedSource(const SourceImage& full, int longEdge, bool cacheLarge) {
  const int own = std::max(full.linear.width, full.linear.height);
  if (longEdge <= 0 || longEdge >= own) return nullptr;
  Hasher h;
  h.u64(id_);
  h.i32(longEdge);
  h.i32(full.linear.width);
  h.i32(full.linear.height);
  const Hash128 key = h.finish();
  std::lock_guard<std::mutex> lock(resizeMtx_);
  if (const ResultCache::Entry* e = cache_.peek(NodeType::Resize, key)) {  // LRU: a hit moves the entry to the back
    ResultCache::Entry moved = *e;
    std::shared_ptr<const SourceImage> hit = as<const SourceImage>(moved.payload);
    cache_.eraseIf(NodeType::Resize, [&](const ResultCache::Entry& x) { return x.key == key; });
    cache_.put(NodeType::Resize, std::move(moved));
    return hit;
  }
  const ResizeBakeCtx ctx{&full, longEdge};
  std::shared_ptr<void> made = bakeNode(NodeType::Resize, &ctx);
  if (!made) return nullptr;
  auto img = as<const SourceImage>(made);
  if (!cacheLarge && img->linear.data.size() * sizeof(float) > (64u << 20)) return img;
  ResultCache::Entry ne;
  ne.key = ne.family = key;
  ne.order = longEdge;
  ne.payload = made;
  ne.bytes = img->linear.data.size() * sizeof(float);
  cache_.put(NodeType::Resize, std::move(ne));
  // at most ~160 MB of scaled copies (the newest always stays), as the C layer's scaledSource did
  std::size_t total = 0;
  for (const auto& e : cache_.entries(NodeType::Resize)) total += e.bytes;
  while (cache_.entries(NodeType::Resize).size() > 1 && total > (160u << 20)) {
    const Hash128 oldest = cache_.entries(NodeType::Resize).front().key;
    total -= cache_.entries(NodeType::Resize).front().bytes;
    cache_.eraseIf(NodeType::Resize, [&](const ResultCache::Entry& x) { return x.key == oldest; });
  }
  return img;
}

bool renderImage8(const EditParams& params, const CameraProfile& camera, const Lut3D* lut, const SourceImage& src, Image8& out,
                  std::string* err, float toneReach) {
  Renderer renderer(params, camera, lut);
  renderer.setToneReach(toneReach);
  EngineSession session;
  EngineSession::ExactOptions o;
  if (std::size_t(src.linear.width) * src.linear.height > 16000000u) o.mode = ExecMode::TiledExport;  // as the C layer's exports
  return session.renderExact(params, renderer, src, o, out, err);
}

bool renderImageF(const EditParams& params, const CameraProfile& camera, const Lut3D* lut, const SourceImage& src, bool spatial,
                  ImageF& out, std::string* err) {
  Renderer renderer(params, camera, lut);
  EngineSession session;
  EngineSession::ExactOptions o;
  o.spatial = spatial;
  o.outFloat = &out;
  Image8 unused;
  return session.renderExact(params, renderer, src, o, unused, err);
}

std::string infoJson() {
  const Totals& t = totals();
  const BakeCounters& c = bakeCounters();
  const Mode m = mode();
  std::string j = "{\"engine\":\"";
  j += m == Mode::Off ? "off" : "full";
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
