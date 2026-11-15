// EngineSession: the frame policy (docs/engine/pipeline-architecture.md section 3, 5.9, section 50). One graph per frame: built from the
// frame's draft blocks (the families' blocks resolved before the bake, at the size of the executor tried first), its table nodes evaluated
// (host_tables.cpp), then planned and run on the GPU executor where it supports the plan (gpu_dispatch.cpp), else on the CPU executor. Nodes
// whose bound blocks turn out to be the identity (HDR once the tone table is known) are elided by the planner.
#include "engine/exec/engine_session.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#if defined(__ANDROID__)
#include <android/log.h>
#include <sys/system_properties.h>
#endif

#include "engine/exec/cpu_executor.h"
#include "engine/exec/engine_session_internal.h"
#include "local_eval.h"
#include "engine/exec/sched_diag.h"
#include "parallel.h"
#include "warp_field.h"

namespace dapple::engine::filmnode {
void filmAsyncTableStats(unsigned long long*, unsigned long long*, unsigned long long*);
}
namespace dapple::engine {

namespace session {

Totals& totals() {
  static Totals t;
  return t;
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

void gpuLog(const char* what, const std::string& why) {
#if defined(__ANDROID__)
  __android_log_print(ANDROID_LOG_WARN, "dapple-gpu", "%s: %s", what, why.c_str());
#else
  std::fprintf(stderr, "dapple-gpu: %s: %s\n", what, why.c_str());
#endif
}


void addStats(EngineStats& st, const ExecStats& es) {
  st.sweeps += std::uint64_t(es.sweeps);
  st.tiles += std::uint64_t(es.tiles);
  st.solveHits += std::uint64_t(es.solveHits);
  st.solveBuilds += std::uint64_t(es.solveBuilds);
  st.streamed += es.streamed ? 1 : 0;
}

PlanRequest request(int w, int h, ExecMode mode, int tile, std::size_t budget, const FrameView& view) {
  PlanRequest rq;
  rq.frameW = w;
  rq.frameH = h;
  rq.mode = mode;
  rq.tile = tile;
  rq.memBudget = budget;
  rq.view = view;
  return rq;
}

void keyGraph(Graph& g, const ParamSet& ps, bool holdSolves) {
  if (holdSolves) g.computeKeysHoldingSolves(ps);
  else g.computeKeys(ps);
}

void notePlanned(EngineStats& st, double ms) {
  st.planMs += ms;
  totals().planUs.fetch_add(std::uint64_t(ms * 1000.0), std::memory_order_relaxed);
  ++st.planned;
  totals().planned.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace session

using session::Clock;
using session::msSince;
using session::totals;

namespace {

std::atomic<std::uint64_t> g_nextId{1};

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

void diagLine(const std::string& l) {
#if defined(__ANDROID__)
  __android_log_print(ANDROID_LOG_INFO, "dapple_sched", "%s", l.c_str());
#else
  std::fprintf(stderr, "dapple_sched: %s\n", l.c_str());
#endif
}

// debug.dapple.diag_sched / DAPPLE_DIAG_SCHED (section 56): at most every 2 s, from the thread that ran a frame, the placement of that thread and of
// the pools' workers, and a fixed workload timed three ways (threads spawned by this thread on every cpu, this thread alone, the frame pool).
void schedReport() {
  static std::atomic<std::int64_t> last{0};
  const std::int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
  std::int64_t prev = last.load();
  if (now - prev < 2000 || !last.compare_exchange_strong(prev, now)) return;
  std::string l = "frame thread " + sched::describe(sched::placementOf());
  auto pool = [&](const char* name, ThreadPool& p) {
    l += std::string(" | ") + name + " " + std::to_string(p.size()) + ":";
    for (int t : p.workerTids()) {
      const sched::Placement q = sched::placementOf(t);
      char b[96];
      std::snprintf(b, sizeof b, " [n%d c%d %llx %s]", q.nice, q.cpus, (unsigned long long)q.mask, q.cpuset.c_str());
      l += b;
    }
  };
  pool("pool", ThreadPool::instance());
  pool("bgpool", ThreadPool::background());
  const double spawned = sched::fixedWorkMs(0), single = sched::fixedWorkMs(1, 32);
  std::atomic<std::uint32_t> sink{0};
  const auto t0 = Clock::now();
  ThreadPool::instance().run(256, 1, [&](int a, int b) {
    for (int i = a; i < b; ++i) {
      float x = 1.0f + float(i) * 1e-6f, y = 0.999f;
      for (int k = 0; k < 200000; ++k) x = x * y + 1e-4f, y = y * 0.99999f + 1e-5f;
      sink.fetch_add(std::uint32_t(x * 1000.f), std::memory_order_relaxed);
    }
  });
  const double onPool = msSince(t0);
  char b[200];
  std::snprintf(b, sizeof b, " | work256 spawned-all %.1f ms, pool %.1f ms, single-thread 32 chunks %.1f ms", spawned, onPool, single);
  diagLine(l + b);
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
  if (sched::diagSched()) schedReport();
  if (!doLog || !engineLogOn()) return;
  const SpatialQuality q = job.fastOptions.spatial;
  const char* tier = q == SpatialQuality::Interactive ? "interactive" : q == SpatialQuality::Full ? "full" : "off";
  char line[512];
  const FrameResult::CpuSplit& c = out.cpu;
  int n = std::snprintf(line, sizeof line, "frame %dx%d tier=%s film=%s exec=%s%s%s | gpu %llu cpu %llu", out.w, out.h, tier, engine::point::filmActive(job.point.p) ? "on" : "off",
                        exec.c_str(), onGpu ? "" : " reason=", onGpu ? "" : reason.c_str(), (unsigned long long)g, (unsigned long long)cp);
  // where the CPU side of the frame went (ms): match = film exposure solve, renderer = point::resolve, graph, tables, exec = plan + executor incl. GPU wait
  if (n > 0 && n < int(sizeof line))
    std::snprintf(line + n, sizeof line - std::size_t(n), " | cpu_ms %.1f (match %.1f point %.1f graph %.1f tables %.1f exec %.1f; prep %.1f turn %.1f upload %.1f gpu %.1f) max %.1f",
                  c.totalMs, c.matchMs, c.rendererMs, c.graphMs, c.tablesMs, c.execMs, out.gpu.prepMs, out.gpu.turnWaitMs, out.gpu.uploadMs, out.gpu.gpuMs,
                  std::max(maxMs, c.totalMs));
  if (const std::size_t len = std::strlen(line); len + 96 < sizeof line)  // inside `tables`: the table nodes baked this frame and the bake assembly
    std::snprintf(line + len, sizeof line - len, " | tables_ms lat %.1f pre %.1f tone %.1f ov %.1f an %.1f asm %.1f", c.latticeMs, c.preMs, c.toneMs, c.overflowMs,
                  c.analysisMs, c.assembleMs);
  if (engine::point::filmActive(job.point.p)) {  // background print tables of the film drag (wave 8): frames that used the previous step, tables finished, requests
    unsigned long long su = 0, bk = 0, rq = 0;
    filmnode::filmAsyncTableStats(&su, &bk, &rq);
    const std::size_t len = std::strlen(line);
    if (len + 64 < sizeof line) std::snprintf(line + len, sizeof line - len, " | film_tables stale %llu baked %llu requested %llu", su, bk, rq);
  }
  if (onGpu && sched::diagUpload()) {  // debug.dapple.diag_upload (section 56): the parts of uploadMs, resized-source builds, pool image creations
    const auto& g2 = out.gpu;
    char u[300];
    std::snprintf(u, sizeof u, "up res %.1f rsz %.1f(%d) film %.1f tab %.1f (begin %.1f wait %.1f) aux %.1f loc %.1f rec %.1f sub %.1f (graph %.1f queue %.1f) wait %.1f trim %.1f poolNew %d",
                  g2.residentMs, g2.resizeMs, g2.resizeBuilt, g2.filmUpMs, g2.tablesUpMs, g2.bufBeginMs, g2.bufWaitMs, g2.auxMs, g2.localMs, g2.recordMs, g2.submitMs,
                  g2.graphRecMs, g2.queueSubmitMs, g2.waitMs, g2.trimMs, g2.poolCreated);
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "dapple_engine", "frame %dx%d %s", out.w, out.h, u);
#else
    std::fprintf(stderr, "dapple_engine: frame %dx%d %s\n", out.w, out.h, u);
#endif
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
  engine::point::inputGain(e, *g->camera, g->sceneReferred, out);
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

EngineSession::EngineSession() : id_(g_nextId.fetch_add(1)) {}

// The frame's blocks before the bake, at the size of the executor tried first: the GPU renders the store resized to the size of `src` (or
// srcLongEdge) with the export's output sharpening after a downscale; the CPU renders `src`. HDR is not bound yet (it needs the tone table).
EngineSession::FrameDraft EngineSession::draftFrame(const FrameJob& job, bool gpu) const {
  FrameDraft d;
  d.gpu = gpu;
  int rw = job.src.linear.width, rh = job.src.linear.height;
  if (gpu && job.srcLongEdge > 0) {
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
  d.point = engine::point::resolveFrame(job.point, rw, rh, job.src.sceneReferred, job.fastOptions, &job.src);
  d.local = localadj::bind(job.point.p, job.src, job.fastOptions.spatial == SpatialQuality::Interactive);
  if (gpu && job.resizeSharpen > 0.f && (rw < job.store->width() || rh < job.store->height())) d.point.resizeSharpen = job.resizeSharpen;  // adds the Detail node
  d.blocks = resolveFrameBlocks(job.point.p, point::facts(d.point, job.fastOptions, job.src.sceneReferred), {d.local.get(), d.point.warp.field.get()});
  return d;
}

int EngineSession::renderFull(FrameJob& job, const ParamSet& ps, const BakedLut& bake, const FrameDraft& draft, FrameResult& out) {
  if (draft.gpu) {  // Vulkan -> GLES 3.1 -> CPU (5.9)
    const int g = renderGpu(job, ps, bake, draft, out);
    if (g != 1) return g;
  } else {
    GpuProvider* pv = gpuProvider();
    frameWhy_ = !pv ? "no GPU provider (build without the GPU executor)" : !pv->enabled() ? pv->inactiveReason() : "no source store (host frame)";
  }
  point::CpuFrame fk(job.point, job.src, &bake, job.fastOptions);
  if (!fk.valid()) {
    frameWhy_ += std::string(frameWhy_.empty() ? "" : "; ") + "empty source";
    return 1;
  }
  const auto t0 = Clock::now();
  const FrameBlocks fb = fk.frameBlocks();
  const FrameView view{&fk.blocks(), &fb};
  if (draft.gpu) {  // the graph was wired for the GPU's blocks: the CPU's may wire differently (rare: then its own graph)
    Graph cg = GraphBuilder::build(ps, view);
    if (cg.structureHash() != graph_.structureHash()) {
      session::keyGraph(cg, ps, job.holdSolves);
      graph_ = std::move(cg);
    }
  }
  const Plan plan = Planner::plan(graph_, session::request(fk.width(), fk.height(), ExecMode::Whole, 0, 0, view), &cache_);
  session::notePlanned(stats_, msSince(t0));
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
  ExecEnv env{fk, graph_, cache_, CancelToken{&latest_, latest_.load(std::memory_order_relaxed)}, rgba, job.outFloat ? 3 : 4, job.outFloat};
  ExecStats es;
  const auto t1 = Clock::now();
  const Status s = CpuExecutor::run(plan, env, &es);
  {
    const double ms = msSince(t1);
    stats_.execMs += ms;
    totals().execUs.fetch_add(std::uint64_t(ms * 1000.0), std::memory_order_relaxed);
  }
  session::addStats(stats_, es);
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
  return 1;  // Unsupported (film stage failed ...): the caller reports an explicit error, there is no second point
}

bool EngineSession::renderExact(const EditParams& params, const engine::point::Params& point, const SourceImage& src,
                                const ExactOptions& opt, Image8& out, std::string* err, ExecStats* statsOut) {
  const bool streaming = bool(opt.sourceWindow) && bool(opt.rows);  // section 42
  if (!opt.outFloat && opt.spatial && !opt.sessionCache && !streaming) {  // export / reference frames: the GPU executor first (exact variant on the dense lattice)
    const int gr = renderExactGpu(params, point, src, opt, out, err);
    if (gr == 0) return true;
    if (gr < 0) return false;
  }
  FastOptions fo;
  fo.spatial = opt.spatial ? SpatialQuality::Full : SpatialQuality::Off;
  point::CpuFrame fk(point, src, nullptr, fo);
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
  const point::Blocks& fp = fk.blocks();
  const FrameBlocks fb = fk.frameBlocks();
  Graph g = GraphBuilder::build(ps, FrameView{&fp, &fb});
  const ValidationResult v = g.validate();
  if (!v.ok) {
    if (err) *err = "engine graph: " + v.error;
    return false;
  }
  g.computeKeys(ps);
  const auto t0 = Clock::now();
  ResultCache local;
  ResultCache& cache = opt.sessionCache ? cache_ : local;
  PlanRequest prq = session::request(fk.width(), fk.height(), opt.mode, opt.tile, opt.memBudget, FrameView{&fp, &fb});
  // a streamed film frame keeps its tiles: the film plane arrives in bands (section 42.6); env DAPPLE_FILM_TILES=1: the whole-frame film export in
  // tiles too (comparisons at the same tile size)
  prq.filmTiles = streaming || (std::getenv("DAPPLE_FILM_TILES") && std::getenv("DAPPLE_FILM_TILES")[0] == '1');
  Plan plan = Planner::plan(g, prq, &cache);
  if (streaming && opt.tile > 0 && !std::getenv("DAPPLE_EXPORT_STREAM_TILE")) {  // a wide halo (local clarity / dehaze: ~400 px at 60 MP) costs (T + 2h)^2 / T^2 per tile: wider tiles then
    const int h = plan.maxHalo();
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
  int winX0 = 0, winY0 = 0, winX1 = 0, winY1 = 0;  // source rect of the window the point::CpuFrame holds
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
    env.prepare = [&, W](const TileWin& tw) {  // W is a block local: capture it by value, the executor runs after the block
      // the plan's source window of the tile (the same the GPU executor streams)
      int sx0 = tw.source.x0, sy0 = tw.source.y0, sx1 = tw.source.x1, sy1 = tw.source.y1;
      if (haveWin && sx0 >= winX0 && sy0 >= winY0 && sx1 <= winX1 && sy1 <= winY1) return true;
      // the source of the whole band row when that is not much more than the band (no or quarter-turn geometry), else of this tile alone
      const int y0 = tw.work.y0, y1 = tw.work.y1;
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
  session::addStats(stats_, es);
  if (statsOut) *statsOut = es;
  if (s == Status::Unsupported) {
    ++stats_.fallbacks;
    // No second point behind the engine: a film whose stock / paper is not in the library or whose table bake failed is an explicit error
    // like any other unsupported plan.
    if (err)
      *err = "engine: plan not supported by the CPU executor" + (std::string(plan.why ? plan.why : "").empty() ? std::string() : " (" + std::string(plan.why) + ")") +
             (engine::point::filmActive(point.p) ? (filmWhy.empty() ? " [film: unknown stock / paper or failed table bake]" : " [" + filmWhy + "]") : "");
    return false;
  }
  if (s != Status::Ok) {
    if (err) *err = s == Status::Cancelled ? "cancelled" : "out of memory";
    if (s == Status::Cancelled) ++stats_.cancelled;
    return false;
  }
  return true;
}

bool EngineSession::renderFrame(FrameJob& job, FrameResult& out) {
  const auto t0 = Clock::now();
  // the table stage is a few short pool jobs between serial pieces (lattice, the proxy's tonal analysis, overflow): wake the workers now and
  // keep them spinning until the tables are done, so no job waits for a worker to come out of an idle core
  std::optional<ThreadPool::WarmScope> warm;
  if (!sys::inBackground()) warm.emplace(ThreadPool::instance());  // background frames run on the background pool or serially
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
  // the one graph of the frame: its table nodes are evaluated first, then it is planned against the bound blocks of the executor
  const FrameDraft draft = draftFrame(job, gpuFrame);
  graph_ = GraphBuilder::build(ps, draft.view());
  const ValidationResult v = graph_.validate();
  if (!v.ok) {
    out.error = "engine graph: " + v.error;
    return false;
  }
  session::keyGraph(graph_, ps, job.holdSolves);
  stats_.graphMs += msSince(t0);

  const int n = job.request.bakeSize;
  const auto tTables = Clock::now();
  out.cpu.graphMs = msSince(t0);
  const std::shared_ptr<const BakedLut> bake = runHostTables(job, ps, n, out);
  warm.reset();
  out.cpu.tablesMs = msSince(tTables);
  if (!bake) {
    if (out.error.empty()) out.error = "tables";
    return false;
  }

  // ---- the frame: GPU executor where it supports the plan, else the CpuExecutor on the same graph ---------------------------------
  const auto tExec = Clock::now();
  const int r = renderFull(job, ps, *bake, draft, out);
  out.cpu.execMs = msSince(tExec);
  out.cpu.matchMs = job.matchMs;
  out.cpu.rendererMs = job.rendererMs;
  out.cpu.totalMs = msSince(t0) + job.matchMs + job.rendererMs;
  if (r < 0) return false;
  if (r != 0) {
    // neither executor took the plan. There is no second point behind it: that is an explicit error (also for a film whose stock / paper is not
    // in the library or whose table bake failed; highlight boost and the enlarger diffusion filter are film nodes).
    out.error = "engine: plan not supported by any executor" + (frameWhy_.empty() ? std::string() : " (" + frameWhy_ + ")") +
                (engine::point::filmActive(job.point.p) ? " [film: unknown stock / paper or failed table bake]" : "");
    return false;
  }
  if (!out.rgba && !out.external && !out.floatOut) {
    out.error = "out of memory";
    return false;
  }
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

bool renderImage8(const EditParams& params, const CameraProfile& camera, const Lut3D* lut, const SourceImage& src, Image8& out,
                  std::string* err, float toneReach) {
  engine::point::Params point = engine::point::resolve(params, camera, lut);
  point.toneReach = toneReach;
  EngineSession session;
  EngineSession::ExactOptions o;
  if (std::size_t(src.linear.width) * src.linear.height > 16000000u) o.mode = ExecMode::TiledExport;  // as the C layer's exports
  return session.renderExact(params, point, src, o, out, err);
}

bool renderImageF(const EditParams& params, const CameraProfile& camera, const Lut3D* lut, const SourceImage& src, bool spatial,
                  ImageF& out, std::string* err) {
  engine::point::Params point = engine::point::resolve(params, camera, lut);
  EngineSession session;
  EngineSession::ExactOptions o;
  o.spatial = spatial;
  o.outFloat = &out;
  Image8 unused;
  return session.renderExact(params, point, src, o, unused, err);
}

std::string infoJson() {
  const session::Totals& t = totals();
  const BakeCounters& c = bakeCounters();
  std::string j = "{\"engine\":\"full\",\"frames\":" + std::to_string(t.frames.load());
  j += ",\"fullHits\":" + std::to_string(t.fullHits.load());
  j += ",\"latticeReuse\":" + std::to_string(t.latticeReuse.load());
  j += ",\"latticeBake\":" + std::to_string(t.latticeBake.load());
  const std::uint64_t n = t.planned.load();
  j += ",\"planned\":" + std::to_string(n);
  j += ",\"planUsPerFrame\":" + std::to_string(n ? t.planUs.load() / n : 0);
  j += ",\"execUsPerFrame\":" + std::to_string(n ? t.execUs.load() / n : 0);
  j += ",\"sweeps\":" + std::to_string(t.sweeps.load()) + ",\"tiles\":" + std::to_string(t.tiles.load());
  j += ",\"solveHits\":" + std::to_string(t.solveHits.load()) + ",\"cancelled\":" + std::to_string(t.cancelled.load());
  j += ",\"fallbacks\":" + std::to_string(t.fallbacks.load());
  j += ",\"bakeLatticeEvaluated\":" + std::to_string(c.latticeEvaluated.load());
  j += ",\"bakeLatticeReused\":" + std::to_string(c.latticeReused.load());
  j += ",\"bakeToneTables\":" + std::to_string(c.toneTables.load());
  j += "}";
  return j;
}

}  // namespace dapple::engine
