// GPU executor selection (docs/engine/pipeline-architecture.md 5.9) and the GPU renders of a session:
//   Unsupported  this plan cannot run on the GPU -> this frame on the CPU executor, the GPU stays in use (counted by GpuReason)
//   Transient    OOM / timeout / failed submission -> release the device copies and retry once; else this frame on the CPU
//   DeviceLost   -> the provider rebuilds the device once (executors re-create their copies from SourceStore + tables);
//                   a failed rebuild or a second loss gives the API up and the next one (Vulkan -> GLES 3.1 -> CPU) takes over
// The provider owns the device(s); the session owns one executor per device generation, and its ResultCache holds the executor's device copies.
#include <algorithm>
#include <cmath>

#include "engine/exec/engine_session_internal.h"
#include "local_eval.h"

namespace dapple::engine {

using session::Clock;
using session::msSince;
using session::totals;

namespace {
std::atomic<GpuProvider*> g_provider{nullptr};
}
void installGpuProvider(GpuProvider* p) { g_provider.store(p); }
GpuProvider* gpuProvider() { return g_provider.load(); }

GpuCounters gpuCounters() {
  const session::Totals& t = totals();
  return {t.gpuFrames.load(), t.gpuFallbacks.load(), t.gpuTransients.load(), t.gpuLost.load(), t.gpuDemotions.load()};
}

const char* gpuReasonName(GpuReason r) {
  switch (r) {
    case GpuReason::None: return "none";
    case GpuReason::Cancelled: return "cancelled";
    case GpuReason::Injected: return "injected";
    case GpuReason::PlanUnsupported: return "plan unsupported";
    case GpuReason::NodeWithoutGpu: return "node without GPU implementation";
    case GpuReason::FrameUnsupported: return "frame unsupported";
    case GpuReason::FrameTooLarge: return "frame too large";
    case GpuReason::SourceTooLarge: return "source too large";
    case GpuReason::LatticeUnusable: return "lattice unusable";
    case GpuReason::LookLutTooLarge: return "look LUT too large";
    case GpuReason::FilmUnavailable: return "film unavailable";
    case GpuReason::FilmEffect: return "film effect not ported";
    case GpuReason::FilmEnlargerTiled: return "film enlarger filter in a tiled export";
    case GpuReason::FilmTiledLocal: return "tiled film export with local detail";
    case GpuReason::NoSink: return "no tile sink";
    case GpuReason::SinkAborted: return "sink aborted";
    case GpuReason::OutOfMemory: return "out of memory";
    case GpuReason::DeviceError: return "device error";
    case GpuReason::Recording: return "recording failed";
    case GpuReason::DeviceLost: return "device lost";
  }
  return "?";
}

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
  cache_.eraseIf([](const ResultCache::Entry& e) { return isDeviceSlot(e.slot); });  // copies of the previous device
  std::string why;
  gpuOwned_ = p->createExecutor(why);
  gpuGen_ = p->generation();  // creating the first context bumps it
  if (!gpuOwned_) {
    if (why != gpuWhy_) {
      gpuWhy_ = why;
      session::gpuLog("GPU unavailable, using the CPU", why);
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
    const std::string what = std::string(gpuReasonName(go.reason)) + (go.error.empty() ? "" : ": " + go.error);
    if (gpuForce_) {
      session::gpuLog("forced executor failed", std::string("status ") + std::to_string(int(s)) + ", " + what);
      return -1;
    }
    switch (go.failure) {
      case GpuFailure::Transient:
        ++stats_.gpuTransients;
        totals().gpuTransients.fetch_add(1, std::memory_order_relaxed);
        session::gpuLog("frame failed", what);
        x->release();  // drop the device copies (resident source, film tables; the cache's device slots): rebuilt from the SourceStore
        cache_.eraseIf([](const ResultCache::Entry& e) { return isDeviceSlot(e.slot); });
        frameWhy_ = "transient GPU failure: " + what;
        if (attempt == 0) continue;
        return 1;
      case GpuFailure::DeviceLost: {
        ++stats_.gpuLost;
        totals().gpuLost.fetch_add(1, std::memory_order_relaxed);
        session::gpuLog("device lost", what);
        frameWhy_ = "device lost: " + what;
        if (pv && !gpuOverride_) {
          std::string why;
          if (pv->recover(why)) {
            session::gpuLog("device rebuilt", "");
          } else {
            totals().gpuDemotions.fetch_add(1, std::memory_order_relaxed);
            session::gpuLog(pv->enabled() ? "device rebuild failed, next backend" : "device rebuild failed, CPU for the rest of the session", why);
          }
          gpuOwned_.reset();
        }
        return 1;
      }
      default:
        ++stats_.gpuUnsupported;
        if (go.reason == GpuReason::FilmEnlargerTiled) ++stats_.gpuEnlargerTiled;
        totals().gpuFallbacks.fetch_add(1, std::memory_order_relaxed);
        frameWhy_ = go.error.empty() ? std::string(gpuReasonName(go.reason)) : go.error;
        return 1;
    }
  }
  return 1;
}

int EngineSession::renderGpu(FrameJob& job, const ParamSet& ps, const BakedLut& bake, const FrameDraft& draft, FrameResult& out) {
  const auto t0 = Clock::now();
  point::Blocks fp = draft.point;
  engine::point::hdrBind(job.point, job.src, fp.hdr, &bake.tone);  // single-image HDR: the field comes from the CPU source
  if (!fp.supported) {
    ++stats_.gpuUnsupported;
    totals().gpuFallbacks.fetch_add(1, std::memory_order_relaxed);
    frameWhy_ = "unsupported on the GPU: " + fp.why;
    if (gpuForce_) return out.error = frameWhy_, -1;
    return 1;
  }
  const FrameView view{&fp, &draft.blocks};
  // the GPU plan looks the frame-global solves up among the executor's device copies (the session cache's device slots): a frame whose dehaze /
  // clarity solve is still on the device skips the accumulation sweep and the solve
  PlanRequest rq = session::request(fp.geo.outW, fp.geo.outH, ExecMode::Whole, 0, 0, view);
  rq.device = true;
  const Plan plan = Planner::plan(graph_, rq, &cache_);
  session::notePlanned(stats_, msSince(t0));
  const std::shared_ptr<const std::vector<float>> over = overflowFor(ps, bake.n);
  GpuRun run{plan, graph_, cache_, fp, draft.blocks, *job.store, bake, over.get(), &job.point, GpuCancel{&latest_, latest_.load(std::memory_order_relaxed)}, {}, false};
  run.target.external = job.gpuTarget;
  run.interactive = job.request.interactive || job.gpuTarget != nullptr;  // a drag frame, or a frame into the presenter's image
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

int EngineSession::renderExactGpu(const EditParams& params, const engine::point::Params& point, const SourceImage& src, const ExactOptions& opt, Image8& out,
                                  std::string* err) {
  GpuProvider* pv = gpuProvider();
  if (!gpuOverride_ && !(pv && pv->enabled())) return 1;
  IGpuExecutor* x = gpuExecutor();
  if (!x) return 1;
  const int W = src.linear.width, H = src.linear.height;
  if (W < 1 || H < 1) return 1;
  FastOptions fo;
  fo.spatial = SpatialQuality::Full;
  point::Blocks fp = engine::point::resolveFrame(point, W, H, src.sceneReferred, fo, &src);
  if (!fp.supported) return 1;
  const std::shared_ptr<const local::Frame> lf = localadj::bind(point.p, src, false);
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
  const FrameBlocks fb = resolveFrameBlocks(params, point::facts(fp, fo, src.sceneReferred), {lf.get(), fp.warp.field.get()});
  Graph g = GraphBuilder::build(ps, FrameView{&fp, &fb});  // the exact variant: ToneColourExact, no table nodes
  if (const ValidationResult v = g.validate(); !v.ok) {
    if (err) *err = "engine graph: " + v.error;
    return -1;
  }
  g.computeKeys(ps);
  // The GPU evaluates the exact colour stage through the finest lattice (65^3 + the overflow lattice) baked with the exact analysis; the CPU
  // executor remains the per-pixel reference (docs/engine/pipeline-architecture.md 21).
  const std::shared_ptr<const BakedLut> bake = engine::point::bake(point, src, 65, nullptr, false);
  if (!bake) return 1;
  engine::point::hdrBind(point, src, fp.hdr, &bake->tone);
  const bool tiled = opt.mode == ExecMode::TiledExport;
  const int tile = opt.tile > 0 ? opt.tile : (x->caps().unifiedMemory ? 1024 : 2048);
  PlanRequest rq = session::request(fp.geo.outW, fp.geo.outH, tiled ? ExecMode::TiledExport : ExecMode::Whole, tiled ? tile : 0, std::size_t(1) << 30,
                                    FrameView{&fp, &fb});
  rq.filmTiles = true;
  rq.maxSide = int(x->caps().maxImage2D);
  rq.device = true;
  const Plan plan = Planner::plan(g, rq, nullptr);
  MemorySourceStore store(src);
  GpuRun run{plan, g, cache_, fp, fb, store, *bake, nullptr, &point, GpuCancel{&latest_, latest_.load(std::memory_order_relaxed)}, {}, false};
  out.width = fp.geo.outW;
  out.height = fp.geo.outH;
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
  FrameJob fj{e.params, e.point, e.image, nullptr, e.camera, e.lut, {}, {}, {}};
  fj.request = in.frame;
  fj.fastOptions.spatial = SpatialQuality::Full;
  const ParamSet ps = ParamSet::make(std::move(in));

  // the export's frame: the store downscaled to the long edge (image.cpp resizeToLongEdge), a little output sharpening after a downscale
  int rw = W, rh = H;
  const int srcLong = std::max(W, H);
  const bool down = e.outLongEdge > 0 && srcLong > e.outLongEdge;
  if (down) {
    const double scale = double(e.outLongEdge) / srcLong;
    rw = std::max(1, int(std::lround(W * scale)));
    rh = std::max(1, int(std::lround(H * scale)));
  }
  FrameDraft draft;
  draft.gpu = true;
  draft.point = engine::point::resolveFrame(e.point, rw, rh, e.image.sceneReferred, fj.fastOptions, &e.image);
  if (!draft.point.supported) return bail(1, "unsupported on the GPU: " + draft.point.why);
  draft.local = localadj::bind(e.point.p, e.image, false);
  draft.point.resizeSharpen = down ? e.resizeSharpen : 0.f;
  draft.blocks = resolveFrameBlocks(e.point.p, point::facts(draft.point, fj.fastOptions, e.image.sceneReferred), {draft.local.get(), draft.point.warp.field.get()});

  // one graph: its table nodes are evaluated here, then it is planned with the export's tiles
  graph_ = GraphBuilder::build(ps, draft.view());
  if (const ValidationResult v = graph_.validate(); !v.ok) return bail(-1, "engine graph: " + v.error);
  graph_.computeKeys(ps);
  FrameResult fr;
  const std::shared_ptr<const BakedLut> bake = runHostTables(fj, ps, e.latticeN, fr);
  if (!bake) return bail(-1, "tables");
  point::Blocks fp = draft.point;
  engine::point::hdrBind(e.point, e.image, fp.hdr, &bake->tone);
  // mobile / unified memory: tile 1024 keeps the export pool near 320 MB (pixel-architecture.md 16 rule 8)
  const int tile = e.tile > 0 ? e.tile : (x->caps().unifiedMemory ? 1024 : 2048);
  PlanRequest rq = session::request(fp.geo.outW, fp.geo.outH, ExecMode::TiledExport, tile, std::size_t(1) << 30, FrameView{&fp, &draft.blocks});
  rq.filmTiles = true;
  rq.maxSide = int(x->caps().maxImage2D);
  rq.device = true;
  Plan plan = Planner::plan(graph_, rq, nullptr);
  if (plan.hasFilm && plan.mode == ExecMode::TiledExport) {  // a film window grows the tile by the film's reach on every side: tiles wide enough that the windows cost at most 1.5 x
    const int want = std::min(plan.filmTileFor(1.5), std::max(64, rq.maxSide - 2 * (plan.maxHalo() + plan.filmHalo)));
    if (want > plan.grid.tile) {
      rq.tile = want;
      plan = Planner::plan(graph_, rq, nullptr);
    }
  }
  if (e.onSize) e.onSize(fp.geo.outW, fp.geo.outH);
  const std::shared_ptr<const std::vector<float>> over = overflowFor(ps, bake->n);
  GpuRun run{plan, graph_, cache_, fp, draft.blocks, e.store, *bake, over.get(), &e.point, GpuCancel{&latest_, latest_.load(std::memory_order_relaxed)}, e.target,
             e.forceStream};
  GpuOut go;
  const int r = runOnGpu(run, go);
  if (r == 0) {
    if (stats) *stats = go.stats;
    return 0;
  }
  if (r == -2) return bail(-1, "cancelled");
  return bail(r, go.error);
}

}  // namespace dapple::engine
