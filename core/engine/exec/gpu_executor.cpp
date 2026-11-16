// GpuExecutor (IGpuExecutor): validates a Plan against the nodes' GpuImpls, records it (gpu_walk.cpp) over the plan's tiles with the
// device copies of the frame (gpu_device_copies.cpp), and reports what happened: Status, GpuFailure and the typed GpuReason. A whole frame
// records every stage into one command list; a tiled export records one job per tile and sweep of the plan (the CPU executor's windows).
// The device is used in turns (pipeline-architecture.md 52): a whole frame is one turn (a background one hands the device over between its
// uploads and its recording), a tiled export one turn per tile; the export's sink runs outside them.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#if defined(__ANDROID__)
#include <android/log.h>
#endif

#include "engine/exec/big_core.h"
#include "engine/exec/gpu_executor_internal.h"

namespace dapple::engine::gpu {

namespace {

GpuReason reasonOf(hal::Status s) { return s == hal::Status::DeviceLost ? GpuReason::DeviceLost : s == hal::Status::OutOfMemory ? GpuReason::OutOfMemory : GpuReason::DeviceError; }

GpuFailure classOf(GpuReason r, hal::Status s) {
  switch (r) {
    case GpuReason::None:
    case GpuReason::Cancelled: return GpuFailure::None;
    case GpuReason::DeviceLost: return GpuFailure::DeviceLost;
    case GpuReason::OutOfMemory:
    case GpuReason::Recording: return GpuFailure::Transient;
    case GpuReason::DeviceError: return s == hal::Status::Unsupported || s == hal::Status::InvalidArgument ? GpuFailure::Unsupported : GpuFailure::Transient;
    default: return GpuFailure::Unsupported;
  }
}

// Every node of the plan's sweeps and film stage must have a GpuImpl.
bool everyNodeRecords(const GpuRun& in, std::string& which) {
  bool ok = true;
  in.plan.forEachNode([&](NodeId id) {
    if (!ok || id >= in.graph.nodes().size()) return;
    const NodeType t = in.graph.nodes()[id].type;
    if (NodeRegistry::instance().desc(t).output == Domain::Table) return;  // baked on the CPU, uploaded
    if (!gpuImplFor(t)) ok = false, which = NodeRegistry::name(t);
  });
  return ok;
}

}  // namespace

Executor::Executor(std::shared_ptr<Context> c) : ctx_(std::move(c)) { epoch_ = ctx_->epoch(); }

Executor::~Executor() {
  std::lock_guard<std::mutex> lk(runMtx_);
  releaseLocked();
}

void Executor::release() {
  std::lock_guard<std::mutex> lk(runMtx_);
  releaseLocked();
}

// Drops this executor's own device copies (the shared source goes when the last executor using it lets go); handed to the deferred destruction.
void Executor::releaseLocked() {
  rs_.reset();
  store_ = nullptr;
  film_.reset();
  if (epoch_ == ctx_->epoch()) {
    for (hal::BufferH& b : tileRb_) core().retire(b);
    DeviceTurn t(ctx_->gate(), DeviceGate::Prio::Background);
    core().collect();
  }
  tileRb_[0] = tileRb_[1] = hal::BufferH{};
}

Executor::Turn::Turn(Executor& x, const GpuRun& in)
    : x_(x), in_(in),
      t_(x.ctx_->gate(), x.prio_,
         in.plan.mode == ExecMode::TiledExport ? "export tile" : x.prio_ == DeviceGate::Prio::Interactive ? "interactive frame" : "background frame") {
  x_.turn_ = &t_;
}

Executor::Turn::~Turn() {
  if (t_.held() && x_.epoch_ == x_.ctx_->epoch()) {
    x_.core().collect();  // device copies released while frames were in flight, now unused
    // One device budget (the pool budget) for the frames' working images and the session's cached device copies: the cache gets what the
    // pool's working set leaves, at least 1/8 of the budget (the current frame's tables and solves). Set between frames, so a plan's cache
    // hits stay.
    const std::size_t budget = x_.dev().poolBudget(), pooled = x_.core().pool().pooledBytes();
    in_.cache.setDeviceLimit(std::max(budget / 8, budget > pooled ? budget - pooled : std::size_t(0)));
  }
  x_.turn_ = nullptr;
}

bool Executor::turnOk() {
  if (ctx_->epoch() != epoch_) return fail(GpuReason::DeviceError, "the device was rebuilt during this frame");  // transient: the session retries
  if (dev().state() == hal::DeviceState::Lost) return fail(GpuReason::DeviceLost, "device lost");
  return true;
}

bool Executor::yieldTurn() { return !turn_ || !turn_->yield() || turnOk(); }

// A rebuilt device invalidates every handle: forget them (the lost device is kept alive by the Context).
void Executor::checkEpoch(const GpuRun& in) {
  const std::uint64_t e = ctx_->epoch();
  if (e == epoch_) return;
  in.cache.eraseIf([](const ResultCache::Entry& x) { return isDeviceSlot(x.slot); });
  film_.reset();
  rs_.reset();
  store_ = nullptr;
  tileRb_[0] = tileRb_[1] = hal::BufferH{};  // the lost device's
  epoch_ = e;
}

bool Executor::fail(GpuReason why, const std::string& msg) {
  reason_ = why;
  failure_ = classOf(why, hal::Status::Ok);
  err_ = msg;
  return false;
}
bool Executor::failStatus(hal::Status s, const std::string& msg) {
  reason_ = reasonOf(s);
  failure_ = classOf(reason_, s);
  err_ = msg.empty() ? dev().lastError() : msg;
  return false;
}
bool Executor::failRecord(const GpuRecorder& rec, const std::string& err) {
  return fail(rec.unsupported ? GpuReason::FilmEffect : dev().state() == hal::DeviceState::Lost ? GpuReason::DeviceLost : GpuReason::Recording, err);
}

std::uint64_t Executor::residentLimit() { return std::min<std::uint64_t>(std::uint64_t(double(dev().caps().deviceLocalBytes) * 0.35), 1536ull << 20); }
bool Executor::canResident(int w, int h) {
  const auto& c = dev().caps();
  return w > 0 && h > 0 && std::uint32_t(w) <= c.maxImage2D && std::uint32_t(h) <= c.maxImage2D && std::uint64_t(w) * h * 8 <= residentLimit();
}

hal::Status Executor::submit(GpuGraph& g, hal::CommandList& cl, hal::FenceH* fence, std::string& err) {
  hal::FenceH f;
  const hal::Status s = runGraph(dev(), g, cl, &f, err);
  if (s == hal::Status::Ok) core().submitted(f);
  if (fence) *fence = f;
  return s;
}

void Executor::recorderFor(GpuRecorder& rec, int slot) {
  rec.weights = core().slot(slot).weights;
  rec.taps = core().slot(slot).taps;
  rec.boxScanMin = core().boxScanOk ? -1 : 0;
  rec.gBuf = core().gBuf;
  rec.partBuf = core().partBuf;
}

bool Executor::validate(const GpuRun& in) {
  const Plan& pl = in.plan;
  if (pl.unsupported) return fail(GpuReason::PlanUnsupported, std::string("plan unsupported: ") + pl.why);
  if (!in.frame.supported) return fail(GpuReason::FrameUnsupported, "unsupported on the GPU: " + in.frame.why);
  if (pl.hasFilm) {  // a film frame (section 20): film stage -> display stages + blend + vignette -> window nodes
    if (!in.frame.film.active) return fail(GpuReason::FilmUnavailable, "film frame without film arguments");
    filmnode::FilmResolved fr;
    if (!filmResolve(in, fr)) return false;
    if (pl.mode == ExecMode::TiledExport && !filmnode::filmWindowable(fr))
      return fail(GpuReason::FilmEnlargerTiled,
                  "the enlarger diffusion filter blurs the whole frame (mixture halo = 4 sigma of the widest component, pyramid levels need aligned windows): no tiled export, CPU executor");
  }
  std::string which;
  if (!everyNodeRecords(in, which)) return fail(GpuReason::NodeWithoutGpu, "node " + which + " has no GPU implementation");
  if (pl.sweeps() == 0) return fail(GpuReason::PlanUnsupported, "plan without a pixel sweep");
  return true;
}

Status Executor::run(const GpuRun& in, GpuOut& out) {
  std::lock_guard<std::mutex> lk(runMtx_);
  [[maybe_unused]] detail::BigCoreScope pin;
  prio_ = in.interactive ? DeviceGate::Prio::Interactive : DeviceGate::Prio::Background;
  checkEpoch(in);
  failure_ = GpuFailure::None;
  reason_ = GpuReason::None;
  err_.clear();
  out.failure = GpuFailure::None;
  out.reason = GpuReason::None;
  Status s;
  if (injectCount_ > 0) {
    --injectCount_;
    failure_ = injected_;
    reason_ = GpuReason::Injected;
    err_ = "injected failure (test)";
    s = failure_ == GpuFailure::Unsupported ? Status::Unsupported : Status::Internal;
  } else if (!validate(in)) {
    s = Status::Unsupported;
  } else if (in.cancel.cancelled()) {
    fail(GpuReason::Cancelled, "cancelled");
    s = Status::Cancelled;
  } else {
    bindSource(in.store);
    s = in.plan.mode == ExecMode::TiledExport ? runTiled(in, out) : runWhole(in, out);
    if (s != Status::Ok && s != Status::Cancelled && failure_ == GpuFailure::None) failure_ = GpuFailure::Transient, reason_ = GpuReason::DeviceError;
  }
  out.failure = failure_;
  out.reason = reason_;
  out.error = err_;
  return s;
}

// Keeps the pool's free images within the pool budget (the cache's device limit follows the pool's working set, see run()).
static double trimPool(Core& core) {
  const auto t0 = Clock::now();
  GpuPool& pool = core.pool();
  const std::size_t budget = core.dev().poolBudget();
  if (pool.freeBytes() > budget) pool.trim(budget);
  return msSince(t0);
}

// ---- whole frame ----------------------------------------------------------------------------------------------------------------------
Status Executor::runWhole(const GpuRun& in, GpuOut& out) {
  const auto t0 = Clock::now();
  const point::Blocks& P = in.frame;
  const hal::DeviceCaps& caps = ctx_->caps();
  const int fw = P.geo.outW, fh = P.geo.outH;
  if (fw <= 0 || fh <= 0 || std::uint32_t(fw) > caps.maxImage2D || std::uint32_t(fh) > caps.maxImage2D)
    return fail(GpuReason::FrameTooLarge, "frame size outside the device limits"), Status::Unsupported;
  if (!canResident(W_, H_)) return fail(GpuReason::SourceTooLarge, "source too large for a resident GPU copy"), Status::Unsupported;
  // CPU preparation without the device: the film's tables and plan (process-wide caches; a first bake is 100-300 ms on a phone)
  filmnode::FilmResolved fres;
  if (in.plan.hasFilm) {
    // all intermediates of the film (E, E2, q, D, grain, linear light, ...) are RGBA32F frames: bounded by the device memory
    if (std::uint64_t(fw) * fh * 288 > std::uint64_t(double(caps.deviceLocalBytes) * 0.45))
      return fail(GpuReason::FrameTooLarge, "film frame too large for the device memory"), Status::Unsupported;
    if (!filmResolve(in, fres)) return Status::Unsupported;
    filmnode::FilmTablesHandle warm;
    filmnode::tablesFor(fres, warm);
    filmnode::planOf(fres);
  }

  const double prepMs = msSince(t0);
  Turn turn(*this, in);
  const auto tTurn = Clock::now();
  if (!turnOk()) return failed(Status::Internal);
  hal::Device& d = dev();
  // the parts of uploadMs (GpuStats; waits for the device between them are not subtracted here, they are rare inside one turn)
  auto lap = [t = Clock::now()]() mutable {
    const auto n = Clock::now();
    const double ms = std::chrono::duration<double, std::milli>(n - t).count();
    t = n;
    return ms;
  };
  const std::uint64_t poolBefore = core().pool().created();
  resizeBuilt_ = 0;
  bufBeginMs_ = bufWaitMs_ = 0;
  if (!ensureResident()) return failed(Status::Internal);
  out.stats.residentMs = lap();
  const std::shared_ptr<const hal::ImageH> S = resizedSource(P.geo.srcW, P.geo.srcH);
  if (!S) return failed(Status::Internal);
  out.stats.resizeMs = lap();
  out.stats.resizeBuilt = resizeBuilt_;
  // the solves the plan found on the device: held before any upload of this frame can evict them
  std::vector<std::pair<NodeId, std::shared_ptr<DeviceSolve>>> hits;
  for (const CacheOp& op : in.plan.cache) {
    const GpuImpl* gi = gpuImplFor(op.type);
    if (!gi || !gi->solve || op.kind != CacheOp::Lookup || !op.hit) continue;
    auto sp = in.cache.find(slotOf(op.type, true), op.key).as<DeviceSolve>();
    if (!sp || sp->w != cdiv(fw, 4) || sp->h != cdiv(fh, 4)) return fail(GpuReason::DeviceError, "kept solve missing"), Status::Internal;
    hits.emplace_back(op.node, std::move(sp));
  }
  bool lattice = false;  // a node of the plan reads the colour tables
  in.plan.forEachNode([&](NodeId id) {
    const GpuImpl* gi = gpuImplFor(in.graph.nodes()[id].type);
    lattice |= gi && gi->lattice;
  });
  std::shared_ptr<DeviceTables> L;
  std::shared_ptr<DeviceFilm> DF;
  std::shared_ptr<DeviceDisplay> DD;
  lap();
  if (in.plan.hasFilm) {
    if (!(DF = ensureFilm(fres))) return failed(Status::Internal);
    if (!(DD = ensureDisplay(in))) return failed(Status::Internal);
  }
  out.stats.filmUpMs = lap();
  if (lattice && !(L = ensureTables(in))) return failed(Status::Internal);
  out.stats.tablesUpMs = lap();
  std::shared_ptr<DeviceBuffer> HD;
  int warpOff = 0;
  if ((P.hdr.active() || P.warp.active()) && !(HD = ensureGeoAux(in, warpOff))) return failed(Status::Internal);
  out.stats.auxMs = lap();
  std::shared_ptr<DeviceBuffer> LDsc, LPl;
  if (!ensureLocal(in, LDsc, LPl)) return failed(Status::Internal);
  out.stats.localMs = lap();
  // a background frame hands the device to a waiting interactive frame between its uploads and its recording; everything it binds is held
  const double waited0 = turn.waited();
  if (!yieldTurn()) return failed(Status::Internal);
  const auto tUp = Clock::now();
  // the device copies of this frame (source, tables, buffers), without the waits for the device (they are turnWaitMs)
  const double turnWaitMs = turn.waited(), uploadMs = std::max(0.0, std::chrono::duration<double, std::milli>(tUp - tTurn).count() - (turnWaitMs - waited0));
  if (in.cancel.cancelled()) return fail(GpuReason::Cancelled, "cancelled"), Status::Cancelled;

  auto cl = d.begin();
  if (!cl) return failStatus(hal::Status::Timeout, ""), Status::Internal;
  const int slot = cl->slot();
  // zero readback only when the device can write the presenter's image and the frame fits it; otherwise the frame is read back as usual
  const hal::ExternalImage* ext = in.target.external;
  const bool external = ext && caps.externalImageWrite && ext->handle && std::uint32_t(fw) <= ext->w && std::uint32_t(fh) <= ext->h;
  const std::size_t bytes = std::size_t(fw) * fh * 4;
  std::string err;
  if (!external && !core().ensureReadback(slot, bytes, err)) return fail(GpuReason::OutOfMemory, err), Status::Internal;
  GpuGraph g(d, core().pool());
  GpuRecorder rec(d, g, err);
  recorderFor(rec, slot);

  Walk w(rec, in, true);
  w.interior = w.work = WRect{0, 0, fw, fh};
  w.job.src.img = g.external(*S);
  if (L) w.job.lut = lutBind(g, *L);
  if (HD && P.hdr.active()) w.job.lut.hdr = g.externalBuf(HD->buf);
  if (HD && P.warp.active()) w.job.src.warp = g.externalBuf(HD->buf), w.job.src.warpOff = warpOff;
  if (LDsc && LPl) {
    w.job.ldesc = g.externalBuf(LDsc->buf);
    w.job.lplanes = g.externalBuf(LPl->buf);
    w.needAux = in.plan.needAux;
  }
  if (DF) {
    w.fres = &fres;
    w.ftab = &DF->gt;
    w.fplan = &DF->pr;
    w.job.disp = DD->bind(g);
    w.job.strength = P.film.strength;
  }
  // frame-global solves: a hit binds the kept image (the plan pruned the sweep and the solve), a miss is copied into a kept image for the next frame
  struct Store {
    NodeId node;
    CacheSlot slot;
    Hash128 key;
    std::shared_ptr<DeviceSolve> keep;
    std::size_t bytes;
  };
  std::vector<Store> stores;
  for (const auto& [node, sp] : hits) w.solves.push_back(SolveRes{node, Accum::None, -1, g.external(sp->img)});
  for (const CacheOp& op : in.plan.cache) {
    const GpuImpl* gi = gpuImplFor(op.type);
    if (!gi || !gi->solve) continue;
    const int gw = cdiv(fw, 4), gh = cdiv(fh, 4);
    const CacheSlot cs = slotOf(op.type, true);
    SolveRes* res = w.solveOf(op.node);
    if (!res) {
      w.solves.push_back(SolveRes{op.node, Accum::None, -1, -1});
      res = &w.solves.back();
    }
    if (op.kind == CacheOp::Store) {
      auto sp = std::make_shared<DeviceSolve>();
      sp->ctx = ctx_;
      sp->epoch = epoch_;
      sp->w = gw;
      sp->h = gh;
      const hal::Format f = gi->solvedRgba ? hal::Format::RGBA32F : hal::Format::R32F;
      const hal::Status cs2 = d.createImage({f, std::uint32_t(gw), std::uint32_t(gh), 1}, &sp->img);
      if (cs2 != hal::Status::Ok) return failStatus(cs2, ""), Status::Internal;
      stores.push_back({op.node, cs, op.key, sp, std::size_t(gw) * gh * (gi->solvedRgba ? 16 : 4)});
    }
  }
  for (const Stage& st : in.plan.stages) {
    if (st.kind == StageKind::FilmFrame) {
      if (!w.filmStage()) return failRecord(rec, err), failed(Status::Internal);
    } else if (st.kind == StageKind::TileSweep) {
      if (!w.sweep(st) || !w.accumulate(st)) return failRecord(rec, err), failed(Status::Internal);
    } else if (st.kind == StageKind::GlobalSolve) {
      if (!w.solve(st)) return failRecord(rec, err), failed(Status::Internal);
      for (const Store& ss : stores)
        if (ss.node == st.solveNode) g.copyImage(w.solveOf(st.solveNode)->solved, g.external(ss.keep->img), std::uint32_t(ss.keep->w), std::uint32_t(ss.keep->h));
    }
  }
  if (w.o8 < 0) return fail(GpuReason::PlanUnsupported, "plan has no output sweep"), Status::Unsupported;
  if (external) g.copyImageToExternal(w.o8, *ext, std::uint32_t(fw), std::uint32_t(fh));
  else g.copyImageToBuffer(w.o8, core().slot(slot).readback, 0, 0, std::uint32_t(fw), std::uint32_t(fh));

  const auto tSub = Clock::now();
  hal::FenceH fence;
  hal::Status s = submit(g, *cl, &fence, err);
  if (s != hal::Status::Ok) return failStatus(s, err), Status::Internal;
  out.stats.graphRecMs = lastRunGraphSplit().recordMs;
  out.stats.queueSubmitMs = lastRunGraphSplit().queueMs;
  out.stats.bufBeginMs = bufBeginMs_;
  out.stats.bufWaitMs = bufWaitMs_;
  keepWarmAfterSubmit(*ctx_);  // touch hint: a keep-warm unit queued behind this frame (no-op outside a window; never ahead of the frame)
  const auto tWait = Clock::now();
  for (Store& ss : stores) {
    ResultCache::Entry e;
    e.slot = ss.slot;
    e.key = e.family = ss.key;
    e.payload = ss.keep;
    e.deviceBytes = ss.bytes;
    in.cache.put(std::move(e));
  }
  Clock::time_point t1 = tWait;
  double copyMs = 0;
  if (!external) {
    s = d.wait(fence, kWaitNs);
    if (s != hal::Status::Ok) return failStatus(s, ""), Status::Internal;
    t1 = Clock::now();
    auto* px = static_cast<std::uint8_t*>(std::malloc(bytes));
    if (!px) return fail(GpuReason::OutOfMemory, "out of memory"), Status::OutOfMemory;
    std::memcpy(px, core().slot(slot).readback.mapped, bytes);
    copyMs = msSince(t1);
    out.rgba = px;
  }
  out.w = fw;
  out.h = fh;
  const double trimMs = trimPool(core());
  GpuStats& st = out.stats;
  const Core::Stats cs = core().statsOf(fence);
  st.wallMs = msSince(t0);
  st.gpuMs = cs.gpuMs;
  st.passes = cs.passes;
  st.barriers = cs.barriers;
  st.parts = cs.parts;
  st.tiles = 1;
  st.sweeps = in.plan.sweeps();
  st.passTimes.clear();
  for (const auto& t : cs.times) st.passTimes.push_back({t.name, t.ms});
  {  // DAPPLE_GPU_PROFILE=1 + DAPPLE_GPU_PASSLOG: the per-pass GPU times of every frame go to the log
    static const bool logPasses = std::getenv("DAPPLE_GPU_PROFILE") != nullptr && std::getenv("DAPPLE_GPU_PASSLOG") != nullptr;
    if (logPasses && !cs.times.empty()) {
      std::string line = "gpu passes:";
      char b[64];
      for (const auto& t : cs.times) {
        std::snprintf(b, sizeof b, " %s %.2f;", t.name.c_str(), t.ms);
        line += b;
      }
#if defined(__ANDROID__)
      __android_log_print(ANDROID_LOG_INFO, "dapple_engine", "%s", line.c_str());
#else
      std::fprintf(stderr, "dapple_engine: %s\n", line.c_str());
#endif
    }
  }
  st.uploadMs = uploadMs;
  st.poolCreated = int(core().pool().created() - poolBefore);
  st.turnWaitMs = turnWaitMs;
  st.prepMs = prepMs;
  st.readbackMs = copyMs;
  st.trimMs = trimMs;
  st.recordMs = std::chrono::duration<double, std::milli>(tSub - tUp).count();
  st.submitMs = std::chrono::duration<double, std::milli>(tWait - tSub).count();
  st.waitMs = std::chrono::duration<double, std::milli>(t1 - tWait).count();
  return Status::Ok;
}

// ---- tiled export: the plan's tiles, one job per tile and sweep -------------------------------------------------------------------------
// One device turn per tile (and per global solve): between two tiles any waiting session gets the device, an interactive frame first. The
// tile's pixels go into one of two readback buffers of this export; they are waited for in the next tile's turn and handed to the sink
// outside any turn (the sink may encode or write a file).
Status Executor::runTiled(const GpuRun& in, GpuOut& out) {
  const auto t0 = Clock::now();
  const point::Blocks& P = in.frame;
  const hal::DeviceCaps& caps = ctx_->caps();
  if (!in.target.sink) return fail(GpuReason::NoSink, "no tile sink"), Status::Unsupported;
  const int rw = P.geo.srcW, rh = P.geo.srcH;
  const resize::Block rb = resize::resolve(W_, H_, rw, rh);
  const bool down = rw < W_ || rh < H_;
  const int halo = in.plan.maxHalo(), tile = in.plan.grid.tile;
  if (tile + 2 * halo > int(caps.maxImage2D)) return fail(GpuReason::FrameTooLarge, "device image limit too small for the plan's tiles"), Status::Unsupported;

  const bool filmFrame = in.plan.hasFilm;
  if (filmFrame && in.plan.needAux)  // the side planes of a film frame come from its whole-frame input pass
    return fail(GpuReason::FilmTiledLocal, "tiled film export with local detail ops (docs section 40.7): CPU executor"), Status::Unsupported;
  bool lattice = false;
  in.plan.forEachNode([&](NodeId id) {
    const GpuImpl* gi = gpuImplFor(in.graph.nodes()[id].type);
    lattice |= gi && gi->lattice;
  });
  filmnode::FilmResolved fres;
  if (filmFrame) {  // the whole-frame part of the film (the level-2 planes of its mixtures) is kept on the device while the tiles run; the source is resident
    if (std::uint64_t(P.geo.outW) * P.geo.outH * 128 > std::uint64_t(double(caps.deviceLocalBytes) * 0.5))
      return fail(GpuReason::FrameTooLarge, "film frame too large for the device memory"), Status::Unsupported;
    if (!filmResolve(in, fres)) return Status::Unsupported;
    if (tile + 2 * (halo + filmnode::filmWindowHalo(fres) + filmnode::filmMixHalo(fres)) > int(caps.maxImage2D))  // the film window grows the work window
      return fail(GpuReason::FrameTooLarge, "device image limit too small for the film windows"), Status::Unsupported;
    filmnode::FilmTablesHandle warm;  // CPU bakes without the device (process-wide caches)
    filmnode::tablesFor(fres, warm);
    filmnode::planOf(fres);
  }
  // the source: resident (small enough) or streamed per tile; a film export keeps it resident (the whole-frame film part reads all of it)
  const bool resident = filmFrame ? (canResident(W_, H_) && canResident(rw, rh))
                                  : (!in.forceStream && canResident(W_, H_) && canResident(rw, rh) && std::uint64_t(W_) * H_ <= 40000000ull);
  if (filmFrame && !resident) return fail(GpuReason::FilmNotResident, "film export needs the source resident on the device"), Status::Unsupported;

  Turn turn(*this, in);
  if (!turnOk()) return failed(Status::Internal);
  hal::Device& d = dev();
  GpuPool& pool = core().pool();
  const double gpu0 = d.gpuAccumMs();  // (other sessions' work in between counts too)
  std::shared_ptr<DeviceTables> L;
  if (lattice && !(L = ensureTables(in))) return failed(Status::Internal);
  std::shared_ptr<DeviceBuffer> HD;
  int warpOff = 0;
  if ((P.hdr.active() || P.warp.active()) && !(HD = ensureGeoAux(in, warpOff))) return failed(Status::Internal);
  std::shared_ptr<DeviceBuffer> LDsc, LPl;
  if (!ensureLocal(in, LDsc, LPl)) return failed(Status::Internal);
  std::shared_ptr<DeviceFilm> DF;
  std::shared_ptr<DeviceDisplay> DD;
  if (filmFrame) {
    if (!(DF = ensureFilm(fres))) return failed(Status::Internal);
    if (!(DD = ensureDisplay(in))) return failed(Status::Internal);
  }
  std::shared_ptr<const hal::ImageH> S;  // held across the tiles' turns
  if (resident) {
    if (!ensureResident()) return failed(Status::Internal);
    if (!(S = resizedSource(rw, rh))) return failed(Status::Internal);
  }

  // persistent images of the export: the 1/4 grids and solves of the GlobalSolve stages, the whole-frame film part. Released through the
  // deferred destruction (the last tiles may still be in flight when an error returns early); forgotten when the device was rebuilt meanwhile
  // (they belong to the lost one).
  struct Keep {
    Executor& x;
    std::deque<hal::ImageH> images;  // stable addresses
    ~Keep() {
      if (x.epoch_ != x.ctx_->epoch()) return;
      for (hal::ImageH& i : images) x.core().retire(i);
    }
    hal::ImageH* add() {
      images.emplace_back();
      return &images.back();
    }
  } keep{*this, {}};
  const int glw = cdiv(P.geo.outW, 4), glh = cdiv(P.geo.outH, 4);
  struct Global {
    NodeId node;
    Accum accum;
    hal::ImageH* grid;
    hal::ImageH* solved;
  };
  std::vector<Global> globals;
  for (const Stage& st : in.plan.stages) {
    if (st.kind != StageKind::GlobalSolve) continue;
    const GpuImpl* gi = gpuImplFor(in.graph.nodes()[st.solveNode].type);
    if (!gi || !gi->solve) return fail(GpuReason::NodeWithoutGpu, "solve without a GPU implementation"), Status::Unsupported;
    Global gl{st.solveNode, st.accum, keep.add(), keep.add()};
    hal::Status s = d.createImage({hal::Format::RGBA32F, std::uint32_t(glw), std::uint32_t(glh), 1}, gl.grid);
    if (s == hal::Status::Ok)
      s = d.createImage({gi->solvedRgba ? hal::Format::RGBA32F : hal::Format::R32F, std::uint32_t(glw), std::uint32_t(glh), 1}, gl.solved);
    if (s != hal::Status::Ok) return failStatus(s, ""), Status::Internal;
    globals.push_back(gl);
  }
  auto bindGlobals = [&](GpuGraph& g, Walk& w) {
    for (const Global& gl : globals) w.solves.push_back(SolveRes{gl.node, gl.accum, g.external(*gl.grid), g.external(*gl.solved)});
  };

  // ---- film: the level-2 planes of the mixtures over the whole frame, once (1/16 of the pixels and less; windows recompute the rest); the tiles read
  // them as persistent images. One tile covering the frame builds them itself. ----
  hal::ImageH *filmLow[3] = {nullptr, nullptr, nullptr};
  bool filmAny = false;
  if (filmFrame && (P.geo.outW > tile || P.geo.outH > tile)) {
    auto cl = d.begin();
    if (!cl) return failStatus(hal::Status::Timeout, ""), Status::Internal;
    std::string perr;
    GpuGraph g(d, pool);
    GpuRecorder rec(d, g, perr);
    recorderFor(rec, cl->slot());
    Walk wp(rec, in, true);
    wp.interior = wp.work = WRect{0, 0, P.geo.outW, P.geo.outH};
    wp.job.src.img = g.external(*S);
    if (HD && P.warp.active()) wp.job.src.warp = g.externalBuf(HD->buf), wp.job.src.warpOff = warpOff;
    wp.fres = &fres;
    wp.ftab = &DF->gt;
    wp.fplan = &DF->pr;
    if (LDsc && LPl) {
      wp.job.ldesc = g.externalBuf(LDsc->buf);
      wp.job.lplanes = g.externalBuf(LPl->buf);
    }
    filmnode::FilmPre pre;
    if (!wp.filmLowPass(pre)) return failRecord(rec, perr), failed(Status::Internal);
    filmAny = pre.any;
    int lw = 0, lh = 0;
    filmnode::filmLowSize(P.geo.outW, P.geo.outH, lw, lh);
    hal::Status ms = hal::Status::Ok;
    const gpu::H lows[3] = {pre.lowPre, pre.lowHalo, pre.lowDir};
    for (int i = 0; i < 3; ++i) {
      if (lows[i] < 0) continue;
      filmLow[i] = keep.add();
      if (ms == hal::Status::Ok) ms = d.createImage({hal::Format::RGBA16F, std::uint32_t(lw), std::uint32_t(lh), 1}, filmLow[i]);
      if (ms == hal::Status::Ok) g.copyImage(lows[i], g.external(*filmLow[i]), std::uint32_t(lw), std::uint32_t(lh));
    }
    if (ms != hal::Status::Ok) return failStatus(ms, ""), Status::Internal;
    hal::FenceH fence;
    ms = submit(g, *cl, &fence, perr);
    if (ms == hal::Status::Ok) ms = d.wait(fence, kWaitNs);
    if (ms != hal::Status::Ok) return failStatus(ms, perr), Status::Internal;
    if (in.cancel.cancelled()) return fail(GpuReason::Cancelled, "cancelled"), Status::Cancelled;
  }

  // An output tile: `pend` is submitted (its readback buffer is being written), `ready` has been waited for and goes to the sink next.
  struct Pending {
    int buf = -1;
    hal::FenceH fence;
    WRect interior;
  };
  Pending pend, ready;
  int passes = 0, barriers = 0, parts = 0;
  const Core::UploadTimes up0 = core().uploadTimes();
  double tSrc = 0, tUp = 0, tRec = 0, tSub = 0, tWait = 0, tSink = 0, tTurn = 0;  // host time per stage of the tile loop (GpuStats::tSrcMs ...)
  std::string err;
  auto readbackFor = [&](int i, std::size_t bytes) -> bool {  // inside the turn
    hal::BufferH& b = tileRb_[i];
    if (b && b.bytes >= bytes) return true;
    core().retire(b);
    b = hal::BufferH{};
    const hal::Status s = d.createBuffer({bytes, hal::MemKind::Readback, false}, &b);
    return s == hal::Status::Ok || failStatus(s, "");
  };
  auto waitPending = [&]() -> bool {  // inside the turn
    if (pend.buf < 0) return true;
    const auto tw = Clock::now();
    const hal::Status ws = d.wait(pend.fence, kWaitNs);
    tWait += msSince(tw);
    if (ws != hal::Status::Ok) return failStatus(ws, "");
    ready = pend;
    pend.buf = -1;
    return true;
  };
  auto sinkReady = [&]() -> bool {  // outside the turn: the buffer is this export's, the next tile writes the other one
    if (ready.buf < 0) return true;
    const auto ts = Clock::now();
    const bool ok = in.target.sink(ready.interior.x0, ready.interior.y0, ready.interior.w(), ready.interior.h(),
                                   static_cast<const std::uint8_t*>(tileRb_[ready.buf].mapped), std::size_t(ready.interior.w()) * 4);
    ready.buf = -1;
    tSink += msSince(ts);
    if (!ok) fail(GpuReason::SinkAborted, "export aborted by the sink");
    return ok;
  };
  auto wrect = [](const Rect& r) { return WRect{r.x0, r.y0, r.x1, r.y1}; };

  // The source window of a streamed tile (the plan's, the CPU executor's), read before the tile's turn: the store may demosaic a band of the
  // picture for it (hundreds of ms on a phone), host work that must not hold the device while an interactive frame waits.
  struct SrcRead {
    SrcWin win;
    const float* base = nullptr;
    std::size_t pitch = 0;
  };
  auto readSource = [&](const TileWin& tw, SrcRead& sr) -> bool {
    const WRect fp = wrect(tw.source);
    sr.win = SrcWin{fp.x0, fp.y0, fp.x1, fp.y1};
    if (resident) return true;
    if (down) sr.win = lanczosWindow(rb, fp);
    const auto ts = Clock::now();
    sr.base = store_->window(sr.win.x0, sr.win.y0, sr.win.w(), sr.win.h(), sr.pitch);
    tSrc += msSince(ts);
    return sr.base || fail(GpuReason::DeviceError, "source store cannot be read");
  };

  // one sweep over one tile of the plan (inside the tile's turn)
  auto runTile = [&](const Stage& st, const TileWin& tw, const SrcRead& sr) -> bool {
    const WRect interior = wrect(tw.interior), work = wrect(tw.work);
    // 1. the source window of a streamed export is uploaded first (the upload takes frame slots of its own)
    hal::ImageH acquired;
    bool haveAcq = false;
    const WRect fp = wrect(tw.source);
    const SrcWin win = sr.win;
    if (!resident) {
      const hal::Status as = pool.acquire(hal::Format::RGBA16F, std::uint32_t(win.w()), std::uint32_t(win.h()), &acquired);
      if (as != hal::Status::Ok) return failStatus(as, "");
      haveAcq = true;
      const auto tu = Clock::now();
      // Vulkan starts every command buffer with a memory barrier over earlier submissions and GLES runs one context in order (settleUpload): the tile's graph is
      // ordered after the copy on the GPU, the CPU goes on with the next window while the device works. The staging slot is written again only after begin()
      // has waited for it. (The wait sat in front of the previous tile's GPU work: 500 ms of a 61 MP export on a phone.)
      const bool upOk = core().uploadRgbF32(sr.base, sr.pitch, win.w(), win.h(), acquired, 0, 0, err, {}, ctx_->api() != hal::Api::Vulkan && ctx_->api() != hal::Api::Gles31);
      tUp += msSince(tu);
      if (!upOk) {
        pool.release(acquired);
        return fail(d.state() == hal::DeviceState::Lost ? GpuReason::DeviceLost : GpuReason::DeviceError, err);
      }
    }
    const int rbuf = pend.buf == 0 ? 1 : 0;  // the buffer the sink is not reading
    if (st.output && !readbackFor(rbuf, std::size_t(interior.w()) * interior.h() * 4)) {
      if (haveAcq) pool.release(acquired);
      return false;
    }
    const auto tr = Clock::now();
    auto cl = d.begin();
    if (!cl) {
      if (haveAcq) pool.release(acquired);
      return failStatus(hal::Status::Timeout, "");
    }
    const int slot = cl->slot();
    GpuGraph g(d, pool);
    GpuRecorder rec(d, g, err);
    recorderFor(rec, slot);
    rec.allocW = int(pool.bucket(std::uint32_t(std::min(P.geo.outW, tile + 2 * halo))));
    rec.allocH = int(pool.bucket(std::uint32_t(std::min(P.geo.outH, tile + 2 * halo))));
    Walk w(rec, in, false);
    w.interior = interior;
    w.work = work;
    if (L) w.job.lut = lutBind(g, *L);
    if (HD && P.hdr.active()) w.job.lut.hdr = g.externalBuf(HD->buf);
    if (HD && P.warp.active()) w.job.src.warp = g.externalBuf(HD->buf), w.job.src.warpOff = warpOff;
    if (LDsc && LPl) {
      w.job.ldesc = g.externalBuf(LDsc->buf);
      w.job.lplanes = g.externalBuf(LPl->buf);
      w.needAux = in.plan.needAux;
    }
    if (filmFrame) {
      w.fres = &fres;
      w.ftab = &DF->gt;
      w.fplan = &DF->pr;
      w.job.disp = DD->bind(g);
      w.job.strength = P.film.strength;
      w.pre.any = filmAny;
      if (filmLow[0]) w.pre.lowPre = g.external(*filmLow[0]);
      if (filmLow[1]) w.pre.lowHalo = g.external(*filmLow[1]);
      if (filmLow[2]) w.pre.lowDir = g.external(*filmLow[2]);
    }
    bindGlobals(g, w);
    bool ok = true;
    if (resident) {
      w.job.src.img = g.external(*S);
    } else {
      const H a = g.external(acquired);
      if (down) {
        const H s2 = recordResize(rec, a, win, rb, fp);
        if (s2 < 0) ok = false;
        w.job.src.img = s2;
        w.job.src.ox = fp.x0;
        w.job.src.oy = fp.y0;
      } else {
        w.job.src.img = a;
        w.job.src.ox = win.x0;
        w.job.src.oy = win.y0;
      }
    }
    if (ok && filmFrame) ok = w.filmWindowStage();
    if (ok) ok = w.sweep(st) && w.accumulate(st);
    if (ok && st.output) {
      if (w.o8 < 0) ok = false;
      else
        g.copyImageToBuffer(w.o8, tileRb_[rbuf], std::uint32_t(interior.x0 - work.x0), std::uint32_t(interior.y0 - work.y0), std::uint32_t(interior.w()),
                            std::uint32_t(interior.h()));
    }
    if (!ok) {
      if (haveAcq) pool.release(acquired);
      return failRecord(rec, err);
    }
    hal::FenceH fence;
    tRec += msSince(tr);
    const auto tsub = Clock::now();
    const hal::Status s = submit(g, *cl, &fence, err);
    tSub += msSince(tsub);
    if (haveAcq) pool.release(acquired);
    if (s != hal::Status::Ok) return failStatus(s, err);
    {
      const auto sst = d.stats(fence);
      passes += sst.passes;
      barriers += sst.barriers;
      parts += sst.parts;
    }
    if (st.output) {
      if (!waitPending()) return false;  // previous tile: its GPU work ran while this one was recorded
      pend.buf = rbuf;
      pend.fence = fence;
      pend.interior = interior;
    }
    return true;
  };

  // the GlobalSolve stage of a tiled run: its own small graph over the accumulated 1/4 grid
  auto globalPass = [&](const Stage& st) -> bool {
    auto cl = d.begin();
    if (!cl) return failStatus(hal::Status::Timeout, "");
    GpuGraph g(d, pool);
    GpuRecorder rec(d, g, err);
    recorderFor(rec, cl->slot());
    rec.W = glw;
    rec.H_ = glh;
    Walk w(rec, in, false);
    bindGlobals(g, w);
    w.job.gridW = glw;
    w.job.gridH = glh;
    SolveRes* s = w.solveOf(st.solveNode);
    const H kept = s ? s->solved : -1;
    if (!s || !w.solve(st)) return failRecord(rec, err);
    g.copyImage(s->solved, kept, std::uint32_t(glw), std::uint32_t(glh));
    const hal::Status ss = submit(g, *cl, nullptr, err);
    return ss == hal::Status::Ok || failStatus(ss, err);
  };

  // the next job's turn: the device may have been rebuilt or lost while this export waited
  auto take = [&]() {
    struct Lap {
      double& acc;
      Clock::time_point t;
      ~Lap() { acc += msSince(t); }
    } lap{tTurn, Clock::now()};
    turn.acquire();
    return turnOk();
  };
  int sweeps = 0, tiles = 0;
  turn.release();  // every job below takes its own turn (the first tile's source is read without one)
  for (const Stage& st : in.plan.stages) {
    if (st.kind == StageKind::TileSweep) {
      ++sweeps;
      for (const TileWin& t : st.tiles) {
        if (in.cancel.cancelled()) return fail(GpuReason::Cancelled, "cancelled"), Status::Cancelled;
        SrcRead sr;
        if (!readSource(t, sr)) return in.cancel.cancelled() ? (fail(GpuReason::Cancelled, "cancelled"), Status::Cancelled) : failed(Status::Internal);
        if (!take() || !runTile(st, t, sr)) return failed(Status::Internal);
        turn.release();
        if (!sinkReady()) return failed(Status::Internal);
        tiles += st.output;
      }
      if (st.output) {
        if (!take() || !waitPending()) return failed(Status::Internal);
        turn.release();
        if (!sinkReady()) return failed(Status::Internal);
      }
    } else if (st.kind == StageKind::GlobalSolve) {
      if (!take() || !globalPass(st)) return failed(Status::Internal);
      turn.release();
    }
  }
  if (!take()) return failed(Status::Internal);
  if (d.waitAll() != hal::Status::Ok) return failStatus(hal::Status::Timeout, ""), Status::Internal;
  trimPool(core());
  out.w = P.geo.outW;
  out.h = P.geo.outH;
  out.stats.wallMs = msSince(t0);
  out.stats.gpuMs = d.gpuAccumMs() - gpu0;
  out.stats.passes = passes;
  out.stats.barriers = barriers;
  out.stats.parts = parts;
  out.stats.tiles = tiles;
  out.stats.sweeps = sweeps;
  out.stats.tSrcMs = tSrc, out.stats.tUpMs = tUp, out.stats.tRecMs = tRec, out.stats.tSubMs = tSub, out.stats.tWaitMs = tWait, out.stats.tSinkMs = tSink, out.stats.tTurnMs = tTurn;
  {
    const Core::UploadTimes up1 = core().uploadTimes();
    out.stats.tUpCopyMs = up1.copyMs - up0.copyMs, out.stats.tUpSubMs = up1.submitMs - up0.submitMs, out.stats.tUpWaitMs = up1.waitMs - up0.waitMs;
  }
  return Status::Ok;
}

std::unique_ptr<IGpuExecutor> Context::createExecutor() { return std::make_unique<Executor>(shared_from_this()); }

}  // namespace dapple::engine::gpu
