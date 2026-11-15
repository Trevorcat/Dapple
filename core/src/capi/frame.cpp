#include "session.h"

static std::string toneJson(const ToneAnalysis& a) {
  std::string j = "{\"histogram\":[";
  for (std::size_t i = 0; i < a.histogram.size(); ++i) j += (i ? "," : "") + n(a.histogram[i]);
  j += "],\"p5\":" + n(a.p5) + ",\"p50\":" + n(a.p50) + ",\"p95\":" + n(a.p95);
  j += ",\"nearest\":" + q(a.nearest) + ",\"key\":" + q(a.key) + ",\"range\":" + q(a.range);
  j += ",\"distance\":" + n(a.distance) + ",\"margin\":" + n(a.margin) + "}";
  return j;
}

static thread_local bool tlHintFrame = false;  // set on the touch-hint worker: its frames are not real frames

// Foreground frames run on ONE dedicated, long-lived render thread: a stable tid for the ADPF hint session, pinned
// to the performance cores on big.LITTLE. The calling isolate thread just waits (no CPU) for the result.
class RenderThread {
 public:
  static RenderThread& instance() {
    static RenderThread* r = new RenderThread;
    return *r;
  }
  void run(const std::function<void()>& fn) {
    std::lock_guard<std::mutex> serial(serial_);
    std::unique_lock<std::mutex> lock(m_);
    job_ = &fn;
    done_ = false;
    cv_.notify_all();
    cv_.wait(lock, [&] { return done_; });
    job_ = nullptr;
  }
  int tid() const { return tid_.load(); }

 private:
  RenderThread() {
    std::thread([this] { loop(); }).detach();
  }
  void loop() {
    if (sys::topology().hetero() && !std::getenv("DAPPLE_THREADS")) sys::setAffinity(sys::topology().perf);
    sys::setInteractivePriority();
    tid_.store(sys::gettid_());
    sys::g_class = 1;
    std::unique_lock<std::mutex> lock(m_);
    for (;;) {
      cv_.wait(lock, [&] { return job_ && !done_; });
      lock.unlock();
      (*job_)();
      lock.lock();
      done_ = true;
      cv_.notify_all();
    }
  }
  std::mutex serial_, m_;
  std::condition_variable cv_;
  const std::function<void()>* job_ = nullptr;
  bool done_ = true;
  std::atomic<int> tid_{0};
};

static bool nearEq(float a, float b) { return std::fabs(a - b) < 1e-6f; }
static thread_local double tlSwatchTablesMs = 0, tlSwatchExecMs = 0;
static thread_local int tlSwatchGpu = 0;
static thread_local double tlSwatchMatchMs = 0;  // film match time of the swatch frames of this thread's current dapple_session_base_previews call

// `quiet` (base swatches): `paramsIn` replaces the JSON, the frame leaves the session's "last frame" memory (noise decision, kept LUT) alone and skips
// noise reduction, so a background swatch never changes what the editor's next frame does.
static bool renderFrame(DappleSession* s, const char* paramsJson, const char* lutPath, const DappleFrameRequest& rq,
                 DappleImage* out, char** analysisOut, const EditParams* paramsIn = nullptr) {
  if (!ensureSource(s)) return false;
  if (!paramsIn && paramsJson && !(rq.flags & DAPPLE_FRAME_INTERACTIVE) && !tlHintFrame) {  // before the session lock: re-creating a plane renders the session
    EditParams pe;
    if (parseParams(paramsJson, pe)) recreateLocalRastersSoft(s, pe);
  }
  std::lock_guard<std::mutex> lock(s->mtx);
  EditParams params;
  if (paramsIn)
    params = *paramsIn;
  else if (!parseParams(paramsJson, params))
    return false;
  const bool quiet = paramsIn != nullptr;
  const auto tMatch0 = std::chrono::steady_clock::now();
  params = withFilmKey(s, params, lutPath, (rq.flags & DAPPLE_FRAME_INTERACTIVE) != 0 && !quiet);  // a swatch wants the exact match, not an interpolated one
  const double matchMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tMatch0).count();
  if (quiet) tlSwatchMatchMs += matchMs;
  std::string lutFile = lutPath && lutPath[0] ? lutPath : params.lutPath;
  std::shared_ptr<const Lut3D> lut;
  if (!lutFile.empty()) {
    lut = loadLut(lutFile);
    if (!lut) return false;
  }
  const bool interactive = (rq.flags & DAPPLE_FRAME_INTERACTIVE) != 0;
  const int own = std::max(s->source.linear.width, s->source.linear.height);

  RenderRegion region;
  const bool hasRegion = rq.region[2] > rq.region[0] && rq.region[3] > rq.region[1];
  int srcLong = rq.long_edge > 0 ? std::min(rq.long_edge, own) : own;
  if (hasRegion) {
    region.x0 = rq.region[0];
    region.y0 = rq.region[1];
    region.x1 = rq.region[2];
    region.y1 = rq.region[3];
    region.outLongEdge = std::max(1, rq.region_long_edge);
    const float frac = std::max(region.x1 - region.x0, region.y1 - region.y0);
    const int need = int(std::ceil(float(region.outLongEdge) / std::max(frac, 1e-3f) * 1.15f));
    srcLong = std::min(own, (need + 31) / 32 * 32);
  }
  const std::shared_ptr<const SourceImage> hold = s->engine.resizedSource(s->source, srcLong);  // the Resize node
  const SourceImage& src = hold ? *hold : s->source;

  if (!s->proxy) {
    s->proxy = std::make_shared<SourceImage>();
    s->proxy->sceneReferred = s->source.sceneReferred;
    s->proxy->linear = resizeToLongEdge(s->source.linear, 256);
  }
  const auto tRen0 = std::chrono::steady_clock::now();
  engine::point::Params point = engine::point::resolve(params, *s->camera, lut.get());
  engine::point::setToneProxy(point, s->proxy.get());
  const double rendererMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tRen0).count();
  // Grain (14 Q8): a drag of a grain-family slider keeps grain in interactive frames; a frame smaller than the full
  // render previews it with grain statistically matched to the downscaled full-resolution grain.
  const std::string activeParam = rq.active_param ? rq.active_param : "";
  const bool keepGrain = interactive && (activeParam == "grain" || activeParam == "film_grain");
  const int fullLongEdge = (!hasRegion && srcLong < own && !std::getenv("DAPPLE_GRAIN_SCALE_OFF")) ? own : 0;  // diagnostics switch

  if (rq.flags & DAPPLE_FRAME_EXACT) {
    // reference pipeline (no region)
    Image8 img;
    if (!engineRenderExact(s, params, point, src, img)) return false;
    if (!fillImage(img, out)) return false;
    if (analysisOut && (rq.flags & DAPPLE_FRAME_ANALYSIS))
    {
      ToneAnalysis a = analyzeTone(*s->proxy, *s->camera, params);
      applyFilmHistogram(a, *s->proxy, *s->camera, params);
      *analysisOut = dupString(toneJson(a));
    }
    return true;
  }

  // 57^3: smallest lattice within mean dE2000 0.5 of the exact path on colour drags (edit-bench); 65^3 for finals
  const int n = rq.bake_size > 0 ? rq.bake_size : (interactive ? 57 : 65);
#ifdef DAPPLE_HAVE_GPU
  dapple::engine::gpu::installProvider();
#endif
  // Frame engine: graph + table nodes + ResultCache.
  dapple::engine::FrameJob job{params, point, src, s->proxy.get(), *s->camera, lut.get(), {}, {}, {}};
  job.matchMs = matchMs;
  job.rendererMs = rendererMs;
  job.request.longEdge = rq.long_edge;
  job.request.interactive = interactive;
  job.request.activeParam = rq.active_param ? rq.active_param : "";
  job.request.wantAnalysis = analysisOut && (rq.flags & DAPPLE_FRAME_ANALYSIS);
  job.request.bakeSize = n;
  job.request.fastAnalysis = interactive && !std::getenv("DAPPLE_EXACT_ANALYSIS");
  job.request.noise = !quiet && (!interactive || !s->haveLast || !nearEq(s->lastParams.nrLuma, params.nrLuma) ||
                                 !nearEq(s->lastParams.nrColor, params.nrColor));
  job.fastOptions.spatial = interactive ? SpatialQuality::Interactive : SpatialQuality::Full;
  job.fastOptions.noise = job.request.noise;
  job.fastOptions.region = region;
  job.fastOptions.keepGrain = keepGrain;
  // a table slider moves: a print table that is not cached is baked coarsely, the frame after the drag bakes the exact one (section 26)
  job.fastOptions.coarseFilm = interactive && !activeParam.empty() && activeParam != "grain" && activeParam != "film_grain" && activeParam != "film_strength" &&
                               activeParam != "film_halation" && !std::getenv("DAPPLE_FILM_COARSE_OFF");
  job.fastOptions.fullLongEdge = fullLongEdge;
  if (!quiet) {
    s->lutKeep = lut;
    s->lastParams = params;
    s->haveLast = true;
  }
#ifdef DAPPLE_HAVE_GPU
  if (dapple::engine::gpu::runtimeEnabled()) job.store = sourceStoreOf(s);  // the GPU executor reads the full-resolution source
#endif
  dapple::hal::ExternalImage ext;
  if (rq.external && rq.external->image) {  // zero readback: the presenter's image on the GPU device (honoured by the GPU executor only)
    ext.handle = rq.external->image;
    ext.fmt = dapple::hal::Format::RGBA8;
    ext.w = rq.external->width;
    ext.h = rq.external->height;
    ext.layoutIn = rq.external->layout_in;
    ext.layoutOut = rq.external->layout_out;
    ext.waitSemaphore = rq.external->wait_semaphore;
    ext.signalSemaphore = rq.external->signal_semaphore;
    job.gpuTarget = &ext;
  }
  dapple::engine::FrameResult res;
  if (!s->engine.renderFrame(job, res)) {
    setError(res.error.empty() ? "engine failure" : res.error);
    return false;
  }
  if (quiet) {
    tlSwatchTablesMs += res.cpu.tablesMs;
    tlSwatchExecMs += res.cpu.execMs;
    tlSwatchGpu += res.executor != "cpu";
  }
  out->width = res.w;
  out->height = res.h;
  out->rgba = res.rgba;
  if (analysisOut && (rq.flags & DAPPLE_FRAME_ANALYSIS)) {
    ToneAnalysis a = res.analysis;
    if (engine::point::filmActive(point.p) && res.rgba) applyFrameHistogram(a, res.rgba, res.w, res.h);
    *analysisOut = dupString(toneJson(a));
  }
  return true;
}

char* dapple_session_analyze_json(DappleSession* session) { return dapple_session_analyze_params_json(session, nullptr); }

char* dapple_session_analyze_params_json(DappleSession* session, const char* params_json) {
  return guarded(
      [&]() -> char* {
        if (!session) {
          setError("null session");
          return nullptr;
        }
        if (!ensureSource(session)) return nullptr;
        EditParams params;
        if (!parseParams(params_json, params)) return nullptr;
        params = withFilmKey(session, params);
        ToneAnalysis a = analyzeTone(session->source, *session->camera, params);
        applyFilmHistogram(a, session->source, *session->camera, params);
        return dupString(toneJson(a));
      },
      nullptr);
}

int32_t dapple_session_render(DappleSession* session, const char* params_json, const char* lut_path,
                              int32_t out_long_edge, DappleImage* out) {
  return guarded(
      [&]() -> int32_t {
        if (!out) {
          setError("null output");
          return DAPPLE_ERROR;
        }
        out->width = out->height = 0;
        out->rgba = nullptr;
        Image8 img;
        if (!renderSession(session, params_json, lut_path, out_long_edge, img)) return DAPPLE_ERROR;
        return fillImage(img, out) ? DAPPLE_OK : DAPPLE_ERROR;
      },
      DAPPLE_ERROR);
}

// Touch hint (wave 8): on pointer-down the editor calls this before the first parameter change. A short burst of the newest interactive frame is rendered on a
// background thread (the same frames a drag renders, results dropped) so the GPU clock that fell during the pause is up when the first real frame arrives;
// the burst ends at `burst_ms` or as soon as a real frame is requested. It runs only while a finger is down (one burst per call, never two at once).
// Round 10: the default is the GPU keep-warm pump (gpu_runtime.h keepWarm): GPU-only units of ~1.5 ms (at most two ahead of a frame, none submitted while
// a real frame is pending, more queued behind each frame of the drag), so a finger that moves at once is not held behind a burst frame. DAPPLE_HINT_MODE=frames selects the frame burst below
// (measurement only), =off disables the hint.
static int32_t touchHintImpl(DappleSession* session, int32_t burst_ms);
int32_t dapple_session_touch_hint(DappleSession* session, int32_t burst_ms) {
  return guarded([&]() -> int32_t { return touchHintImpl(session, burst_ms); }, DAPPLE_ERROR);
}
static int32_t touchHintImpl(DappleSession* session, int32_t burst_ms) {
  if (!session || burst_ms == 0) return DAPPLE_OK;
  const char* hintMode = std::getenv("DAPPLE_HINT_MODE");
  if (hintMode && std::strcmp(hintMode, "off") == 0) return DAPPLE_OK;
  if (!(hintMode && std::strcmp(hintMode, "frames") == 0)) {
#ifdef DAPPLE_HAVE_GPU
    dapple::engine::gpu::keepWarm(burst_ms);  // no-op when frames do not run on the GPU
#endif
    return DAPPLE_OK;
  }
  const bool prepareOnly = burst_ms < 0;  // one burst-size frame: builds the plans of that size now (idle), so the first real burst does not
  bool expected = false;
  if (!session->hintRunning.compare_exchange_strong(expected, true)) return DAPPLE_OK;
  std::string json, lut, active;
  DappleFrameRequest rq{};
  {
    std::lock_guard<std::mutex> hl(session->hintMtx);
    if (!session->hintHave) {
      session->hintRunning.store(false);
      return DAPPLE_OK;
    }
    json = session->hintJson, lut = session->hintLut, active = session->hintActive;
    rq = session->hintRq;
  }
  rq.flags |= DAPPLE_FRAME_INTERACTIVE;
  {  // the burst renders a larger frame than the drag does (DAPPLE_HINT_SCALE, default 3x the long edge): the GPU has to be close to saturated for its governor to step up
    const char* e = std::getenv("DAPPLE_HINT_SCALE");
    const double sc = e ? std::atof(e) : 3.0;
    if (sc > 0.0 && rq.long_edge > 0) rq.long_edge = std::max<std::int32_t>(rq.long_edge, std::int32_t(double(rq.long_edge) * sc));
  }
  session->matchBusy.fetch_add(1);  // the session destructor waits for the worker
  try {
  std::thread([session, json, lut, active, rq, burst_ms, prepareOnly]() mutable {
    tlHintFrame = true;
    const std::uint64_t seq0 = session->frameSeq.load();
    const auto t0 = std::chrono::steady_clock::now();
    rq.active_param = active.empty() ? nullptr : active.c_str();
    do {
      DappleImage img{};
      if (dapple_session_render_frame(session, json.c_str(), lut.empty() ? nullptr : lut.c_str(), &rq, &img, nullptr) != DAPPLE_OK) break;
      dapple_free_image(&img);
    } while (!prepareOnly && std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() < double(burst_ms) && session->frameSeq.load() == seq0);
    session->hintRunning.store(false);
    session->matchBusy.fetch_sub(1);
  }).detach();
  } catch (...) {  // no thread: undo the books (the destructor waits on matchBusy)
    session->hintRunning.store(false);
    session->matchBusy.fetch_sub(1);
    throw;
  }
  return DAPPLE_OK;
}

int32_t dapple_session_render_frame(DappleSession* session, const char* params_json, const char* lut_path,
                                    const DappleFrameRequest* request, DappleImage* out, char** analysis_json) {
  return guarded(
      [&]() -> int32_t {
        if (analysis_json) *analysis_json = nullptr;
        if (!out) {
          setError("null output");
          return DAPPLE_ERROR;
        }
        out->width = out->height = 0;
        out->rgba = nullptr;
        if (!session) {
          setError("null session");
          return DAPPLE_ERROR;
        }
        DappleFrameRequest rq{};
        if (request) {  // size-versioned: a shorter (older) struct leaves the newer fields zero
          const std::size_t n = request->size > 0 ? std::min<std::size_t>(std::size_t(request->size), sizeof rq) : sizeof rq;
          std::memcpy(&rq, request, n);
          rq.size = std::int32_t(sizeof rq);
        }
#ifdef DAPPLE_HAVE_GPU
        struct KeepWarmBracket {  // a real foreground frame is pending: the keep-warm pump submits nothing until it returned
          bool on;
          explicit KeepWarmBracket(bool o) : on(o) { if (on) dapple::engine::gpu::keepWarmFrame(1); }
          ~KeepWarmBracket() { if (on) dapple::engine::gpu::keepWarmFrame(-1); }
        } keepWarmBracket(!tlHintFrame && !(rq.flags & DAPPLE_FRAME_BACKGROUND));
#endif
        if (!tlHintFrame) {  // a real frame: remembered for the touch hint, and it ends a running burst
          session->frameSeq.fetch_add(1, std::memory_order_relaxed);
          if ((rq.flags & DAPPLE_FRAME_INTERACTIVE) && params_json) {
            std::lock_guard<std::mutex> hl(session->hintMtx);
            session->hintJson = params_json;
            session->hintLut = lut_path ? lut_path : "";
            session->hintActive = rq.active_param ? rq.active_param : "";
            session->hintRq = rq;
            session->hintRq.active_param = nullptr;
            session->hintRq.external = nullptr;
            session->hintHave = true;
          }
        }
        bool ok = false;
        if (rq.flags & DAPPLE_FRAME_BACKGROUND) {
          // low priority work (detail session...): this thread, never the shared pool (the background pool while no finger is down). Efficiency cores and
          // nice 10 only while a finger is on a control: with no finger down the frame is the sharp picture the user is waiting for (the zoomed detail after
          // a finger-up), 3x slower on the efficiency cores of a Note10+ (sysperf.h setBackground).
          sys::setBackground(true, sys::interacting());
          try {
            ok = renderFrame(session, params_json, lut_path, rq, out, analysis_json);
          } catch (...) {
            sys::setBackground(false);
            throw;
          }
          sys::setBackground(false);
          return ok ? DAPPLE_OK : DAPPLE_ERROR;
        }
        const bool timed = (rq.flags & DAPPLE_FRAME_INTERACTIVE) != 0;
        const bool hintFrame = tlHintFrame;  // the touch hint's keep-warm frames are never shown: not in the trace
        const std::function<void()> job = [&] {
          const auto t0 = std::chrono::steady_clock::now();
          if (!hintFrame && dapple::ftrace::on()) dapple::ftrace::beginFrame();
          if (timed) {
            std::vector<std::int32_t> tids = ThreadPool::instance().workerTids();
            tids.push_back(RenderThread::instance().tid());
            sys::adpf().ensure(tids);
          }
          try {
            ok = renderFrame(session, params_json, lut_path, rq, out, analysis_json);
          } catch (const std::exception& e) {
            setError(e.what());
            ok = false;
          }
          if (timed)
            sys::adpf().reportDuration(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
          if (!hintFrame)
            dapple::ftrace::endFrame(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), rq.long_edge, out->width, out->height,
                                   timed ? 1 : 0, ok);
        };
        RenderThread::instance().run(job);
        return ok ? DAPPLE_OK : DAPPLE_ERROR;
      },
      DAPPLE_ERROR);
}

int32_t dapple_session_cancel(DappleSession* session) {
  if (!session) return DAPPLE_ERROR;
  session->engine.cancel();  // lock-free: the render in flight (any thread) polls the ticket
  session->exportEngine.cancel();
  return DAPPLE_OK;
}

int32_t dapple_session_render_look_previews(DappleSession* session, const char* params_json, int32_t out_long_edge,
                                            DappleImage* out) {
  return guarded(
      [&]() -> int32_t {
        if (!out) {
          setError("null output");
          return DAPPLE_ERROR;
        }
        for (int i = 0; i < 9; ++i) out[i] = DappleImage{0, 0, nullptr};
        if (!session) {
          setError("null session");
          return DAPPLE_ERROR;
        }
        if (!ensureSource(session)) return DAPPLE_ERROR;
        EditParams base;
        if (!parseParams(params_json, base)) return DAPPLE_ERROR;
        base.hasPalette = false;
        base.lookStrength = 100.f;
        for (int i = 0; i < 9; ++i) {
          EditParams p = base;
          p.look = looks()[i].id;
          Image8 img;
          if (!renderSessionParams(session, p, nullptr, out_long_edge, img) || !fillImage(img, &out[i])) {
            for (int j = 0; j < i; ++j) dapple_free_image(&out[j]);
            return DAPPLE_ERROR;
          }
        }
        return DAPPLE_OK;
      },
      DAPPLE_ERROR);
}

int32_t dapple_session_render_tone_previews(DappleSession* session, const char* params_json, int32_t out_long_edge,
                                            float strength, DappleImage* out) {
  return guarded(
      [&]() -> int32_t {
        if (!out) {
          setError("null output");
          return DAPPLE_ERROR;
        }
        for (int i = 0; i < 9; ++i) out[i] = DappleImage{0, 0, nullptr};
        if (!session) {
          setError("null session");
          return DAPPLE_ERROR;
        }
        if (!ensureSource(session)) return DAPPLE_ERROR;
        EditParams base;
        if (!parseParams(params_json, base)) return DAPPLE_ERROR;
        // Previews show each class at full strength with a wide reach so the nine cells are
        // clearly different; `strength` only matters when given explicitly (>= 0).
        const float s = strength >= 0.f ? std::min(100.f, strength) : 100.f;
        for (int i = 0; i < 9; ++i) {
          EditParams p = base;
          p.toneTarget = toneClasses()[i].name;
          p.toneStrength = s;
          Image8 img;
          if (!renderSessionParams(session, p, nullptr, out_long_edge, img, 20.f) || !fillImage(img, &out[i])) {
            for (int j = 0; j < i; ++j) dapple_free_image(&out[j]);
            return DAPPLE_ERROR;
          }
        }
        return DAPPLE_OK;
      },
      DAPPLE_ERROR);
}

// ---- Base swatches (dapple_session_base_previews) ----
// One small interactive frame per base id on the session's own picture: the shared work (the downscaled source of that long edge, the tone proxy, geometry, the engine
// plans and the film tables) is the session's and is reused from one base to the next; only the base changes. Frames use the coarse film tier (prebaked tables), run on
// the calling thread at background priority and yield to the editor: between two bases the call waits while a finger is down (dapple_set_interaction), or while a
// touch-hint burst runs, and it holds the session lock only for one frame at a time.
int32_t dapple_session_base_previews(DappleSession* session, const char* params_json, const char* const* base_ids, int32_t n, int32_t long_edge,
                                     DappleImage* out) {
  return guarded(
      [&]() -> int32_t {
        if (!out || n <= 0 || n > 256) {
          setError("base previews: bad output");
          return DAPPLE_ERROR;
        }
        for (int i = 0; i < n; ++i) out[i] = DappleImage{0, 0, nullptr};
        if (!session) {
          setError("null session");
          return DAPPLE_ERROR;
        }
        if (!ensureSource(session)) return DAPPLE_ERROR;
        EditParams base;
        if (!parseParams(params_json, base)) return DAPPLE_ERROR;
        const std::uint64_t mine = session->swatchSeq.fetch_add(1) + 1;  // a newer request or a cancel makes this one stop
        const auto cancelled = [&] { return session->swatchSeq.load() != mine; };
        const auto lib = film::defaultLibrary();
        struct Background {  // the calling thread is a background thread for the whole call
          Background() { sys::setBackground(true); }
          ~Background() { sys::setBackground(false); }
        } background;
        int done = 0;
        tlSwatchMatchMs = tlSwatchTablesMs = tlSwatchExecMs = 0;
        tlSwatchGpu = 0;
        const auto tBatch = std::chrono::steady_clock::now();
        for (int i = 0; i < n && !cancelled(); ++i) {
          while ((sys::interacting() || session->hintRunning.load()) && !cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(4));
          if (cancelled()) break;
          EditParams p = base;
          std::string id = base_ids && base_ids[i] ? base_ids[i] : "";
          if (!id.empty() && !isModernId(id) && !(lib && lib->find(id))) {
            stageLog("base previews: unknown base '%.40s', standard rendering", id.c_str());
            id.clear();
          }
          p.filmStock = id;
          DappleFrameRequest rq{};
          rq.size = std::int32_t(sizeof rq);
          rq.long_edge = long_edge > 0 ? long_edge : 96;
          rq.flags = DAPPLE_FRAME_INTERACTIVE | DAPPLE_FRAME_BACKGROUND;
          rq.bake_size = 33;  // a 96 px picture does not need the 57-node lattice of a drag frame: a modern base bakes in a fraction of the time
          rq.active_param = "film_swatch";  // not a grain slider; a table-moving name: the coarse print tier (cached tables, never a bake on this thread)
          DappleImage img{0, 0, nullptr};
          bool ok = false;
          try {
            ok = renderFrame(session, nullptr, nullptr, rq, &img, nullptr, &p);
          } catch (const std::exception& e) {
            setError(e.what());
          }
          if (ok && img.rgba) {
            out[i] = img;
            ++done;
          } else {
            dapple_free_image(&img);
          }
        }
        stageLog("base previews: %d of %d at %d px in %.0f ms (film match %.0f, tables %.0f, exec %.0f; %d on GPU)%s", done, n, long_edge > 0 ? long_edge : 96, msSince(tBatch),
                 tlSwatchMatchMs, tlSwatchTablesMs, tlSwatchExecMs, tlSwatchGpu, cancelled() ? " cancelled" : "");
        if (done == 0 && !cancelled()) return DAPPLE_ERROR;  // every frame failed (the error of the last one is set)
        return DAPPLE_OK;
      },
      DAPPLE_ERROR);
}

void dapple_session_base_previews_cancel(DappleSession* session) {
  if (session) session->swatchSeq.fetch_add(1);
}
