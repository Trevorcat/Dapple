#include "session.h"

namespace capi {

// ---- streaming export (docs/engine/pipeline-architecture.md section 42) -------------------------------------------------------------------
bool ensureSource(DappleSession* s) {
  if (!s || !s->lazyOpen.load()) return true;
  try {
    std::lock_guard<std::recursive_mutex> lk(s->lazyMtx);
    if (!s->lazy) return true;
    const auto t0 = std::chrono::steady_clock::now();
    s->source = materializeFull(*s->lazy);
    s->lazy.reset();
    s->streamA.reset();
    s->lazyOpen = false;
    stageLog("session materialised %dx%d in %.0f ms (a call needs the whole picture)", s->source.linear.width, s->source.linear.height, msSince(t0));
    if (s->source.linear.empty()) {
      setError("cannot decode the full picture");
      return false;
    }
    return true;
  } catch (const std::exception& e) {
    setError(std::string("exception: ") + e.what());
  } catch (...) {
    setError("unknown exception");
  }
  return false;
}

}  // namespace capi

namespace {

// The progress of an export (per band): false = the caller cancelled (dapple_session_set_export_progress).
bool exportProgressOk(DappleSession* s, float f) { return !s->exportProgress || s->exportProgress(std::clamp(f, 0.f, 1.f), "export", s->exportProgressUser) == 0; }

// The exact reductions of a lazy picture with at least `want` (one pass, kept by the session; a long edge or a guide grid not yet there runs the
// pass again with the union). Also gives the session the 256 px proxies the film match and the modern white point read (what they would make of
// the whole picture). Null = cancelled / failed (error set).
std::shared_ptr<const xstream::Analyses> lazyAnalyses(DappleSession* s, xstream::Wants want, float progressFrom, float progressTo) {
  std::lock_guard<std::recursive_mutex> lk(s->lazyMtx);
  if (!s->lazy) return nullptr;
  if (const auto have = s->streamA) {
    bool ok = true;
    for (int le : want.longEdges) ok &= have->at(le) != nullptr || le >= std::max(s->lazy->width, s->lazy->height);
    for (const auto& g : want.guideGrids) {
      bool found = false;
      if (have->guide)
        for (const auto& gg : have->guide->grids) found |= gg.gw == g.first && gg.gh == g.second;
      ok &= found;
    }
    if (ok) return have;
    for (const auto& r : have->reduced)
      if (std::find(want.longEdges.begin(), want.longEdges.end(), r.first) == want.longEdges.end()) want.longEdges.push_back(r.first);
    if (have->guide)
      for (const auto& gg : have->guide->grids)
        if (std::find(want.guideGrids.begin(), want.guideGrids.end(), std::make_pair(gg.gw, gg.gh)) == want.guideGrids.end()) want.guideGrids.emplace_back(gg.gw, gg.gh);
  }
  want.longEdges.erase(std::remove_if(want.longEdges.begin(), want.longEdges.end(), [&](int le) { return le <= 0 || le >= std::max(s->lazy->width, s->lazy->height); }),
                       want.longEdges.end());
  if (!s->bands) s->bands = std::make_shared<xstream::BandSource>(s->lazy->mosaic);
  xstream::BandSource& bs = *s->bands;
  {  // the row cache: what the budget leaves after the streaming machinery itself (measured on a phone: ~2700 full-width float rows, ~4100 for a
     // film: 310 / 470 MB at 61 MP), at most half the budget; DAPPLE_EXPORT_CACHE_MB overrides (0 = off)
    // phones: at most 256 MB (42.9): the cache buys 10-40 % of the time, the peak is what gets an app killed
    const xstream::Budget bud = xstream::currentBudget();
    const std::size_t budget = bud.bytes;
    const std::size_t machinery = std::size_t(s->lazy->width) * (s->streamFilm ? 4100u : 2700u) * 12u;
    // X-Trans: a window demosaiced again costs ~4x a Bayer one (Note10+, 40 MP film: 29 windows = 26 s of 45 s with a 227 MB cache), so the whole
    // picture is cached when it is at most 512 MB (40 MP: 481 MB) and the budget has room for it
    std::size_t cap = bud.rowCacheCap;
    const std::size_t whole = std::size_t(s->lazy->width) * std::size_t(s->lazy->height) * 12u;
    if (s->lazy->mosaic && s->lazy->mosaic->cfa == CfaKind::XTrans && whole <= (std::size_t(512) << 20) && !mem::deviceClass().lowRam.load())
      cap = std::max(cap, whole);
    std::size_t cb = budget > machinery ? std::min({budget - machinery, budget / 2, cap}) : 0;
    if (const char* e = std::getenv("DAPPLE_EXPORT_CACHE_MB")) cb = std::size_t(std::max(0L, std::atol(e))) << 20;
    bs.setCacheBudget(cb);
  }
  bs.setCancel(s->exportEngine.cancelCell(), s->exportEngine.ticket());
  auto a = std::make_shared<xstream::Analyses>();
  std::string err;
  mem::mark("analyse");
  if (!xstream::analyse(bs, s->lazy->meta, want, *a, [&](float f) { return exportProgressOk(s, progressFrom + (progressTo - progressFrom) * f); }, &err)) {
    setError(err == "cancelled" ? "cancelled" : "streaming export: " + err);
    return nullptr;
  }
  s->streamA = a;
  if (!s->proxy) s->proxy = a->p256;
  {
    std::lock_guard<std::mutex> ml(s->matchMtx);
    if (!s->keyProxy) s->keyProxy = a->p256;
  }
  stageLog("export_stream analyses %dx%d: %d pass(es) %.0f ms = window reads %.0f (demosaic %.0f, band copies %.0f) + row cache copies %.0f + shadow samples %.0f + reductions %.0f + guide / grid %.0f + other %.0f", s->lazy->width,
           s->lazy->height, a->passes, a->ms, a->msWindow, bs.demosaicMs(), bs.bandCopyMs(), a->msKeep, a->msLuma, a->msFeed, a->msGrid,
           a->ms - a->msWindow - a->msKeep - a->msLuma - a->msFeed - a->msGrid);
  stageLog("export_stream analyses %dx%d: %d pass(es) %.0f ms (demosaic %.0f ms, %.1f MP), shadow floor %d, %d reductions, guide grids %d, %s demosaic, row cache %.0f MB (%d rows)", s->lazy->width,
           s->lazy->height, a->passes, a->ms, bs.demosaicMs(), bs.demosaicMpx(), a->shadow ? 1 : 0, int(a->reduced.size()), a->guide ? int(a->guide->grids.size()) : 0, bs.gpu() ? "GPU" : "CPU",
           double(bs.cacheBytes()) / 1048576.0, bs.cachedRows());
  return a;
}

// 8-bit sRGB -> Display P3 on rows (srgb8ToDisplayP3 of the whole picture, band by band: a per-pixel table + matrix).
void srgb8RowsToDisplayP3(std::uint8_t* rgb, std::size_t pixels) {
  Image8 tmp;
  tmp.width = int(pixels), tmp.height = 1;
  tmp.data.assign(rgb, rgb + pixels * 3);
  srgb8ToDisplayP3(tmp);
  std::memcpy(rgb, tmp.data.data(), pixels * 3);
}

}  // namespace

namespace capi {

int streamRender(DappleSession* s, const EditParams& paramsIn, const char* lutPath, int32_t outLongEdge, const std::function<void(int, int)>& onSize,
                 const RowSink& sink, const std::vector<int>& extraLongEdges) {
  if (!s->lazyOpen.load() || !s->lazy) return 0;
  const int W = s->lazy->width, H = s->lazy->height, own = std::max(W, H);
  const bool reduced = outLongEdge > 0 && outLongEdge < own;
  s->streamFilm = engine::point::filmActive(paramsIn);
  if (paramsIn.outputSpace == 1 && s->streamFilm) {
    // as renderSessionParams: a film is rendered as sRGB, then converted to Display P3 (band by band). The film match of the P3 params is
    // solved first, as renderSessionParams does (the match cache of the session remembers it: the sRGB solve that follows starts from it)
    xstream::Wants want;
    want.longEdges = extraLongEdges;
    if (reduced) want.longEdges.push_back(outLongEdge);
    want.guideGrids = local::guideGridSizes(paramsIn.local, W, H, false);
    want.guide = !want.guideGrids.empty();
    if (!lazyAnalyses(s, want, 0.f, reduced ? 1.f : 0.3f)) return -1;
    (void)withFilmKey(s, paramsIn, lutPath);
    EditParams q = paramsIn;
    q.outputSpace = 0;
    int ow = 0;
    return streamRender(
        s, q, lutPath, outLongEdge, [&](int w, int h) { ow = w, onSize(w, h); },
        [&](int y0, int n, const std::uint8_t* rgb) {
          std::vector<std::uint8_t> b(rgb, rgb + std::size_t(n) * ow * 3);
          srgb8RowsToDisplayP3(b.data(), std::size_t(n) * ow);
          return sink(y0, n, b.data());
        },
        extraLongEdges);
  }
  // a film stock streams on the CPU in bands (section 42.6): the GPU executor keeps a film frame resident (it would read the whole picture)
  const bool film = engine::point::filmActive(paramsIn);
  xstream::Wants want;
  want.longEdges = extraLongEdges;
  if (reduced) want.longEdges.push_back(outLongEdge);
  want.guideGrids = local::guideGridSizes(paramsIn.local, W, H, false);
  want.guide = !want.guideGrids.empty();
  const auto a = lazyAnalyses(s, want, 0.f, reduced ? 1.f : 0.3f);
  if (!a) return -1;
  const EditParams params = withFilmKey(s, paramsIn, lutPath);
  std::string lutFile = lutPath && lutPath[0] ? lutPath : params.lutPath;
  std::shared_ptr<const Lut3D> lut;
  if (!lutFile.empty()) {
    lut = loadLut(lutFile);
    if (!lut) return -1;
  }
  engine::point::Params point = engine::point::resolve(params, *s->camera, lut.get());
  engine::point::setAnalysisSources(point, a->p256.get(), a->grid.get(), a->p512.get());
  const xstream::GuideScope guideScope(a->guide.get());
  const SourceImage virt = xstream::sizeOnly(s->lazy->meta, W, H);
  if (!s->bands) s->bands = std::make_shared<xstream::BandSource>(s->lazy->mosaic);
  xstream::BandSource& bs = *s->bands;  // with the rows the analysis pass left in its cache
  bs.setShadowFloor(a->shadow);
  const std::uint64_t hits0 = bs.cacheHits(), windows0 = bs.windows();
  const float p0 = reduced ? 0.f : 0.3f;
  int bandsDone = 0, outH = 0;
  auto progressSink = [&](int y0, int n, const std::uint8_t* rgb) {
    ++bandsDone;
    if (!exportProgressOk(s, p0 + (1.f - p0) * float(y0 + n) / float(std::max(1, outH)))) return false;
    return sink(y0, n, rgb);
  };
  auto sized = [&](int w, int h) {
    outH = h;
    onSize(w, h);
  };
  mem::mark("render");
#ifdef DAPPLE_HAVE_GPU
  dapple::engine::gpu::installProvider();
  if (!film && dapple::engine::gpu::runtimeEnabled() && (outLongEdge <= 0 || outLongEdge >= 1500)) {  // as renderSessionParams: the GPU export, its tiles streamed
    std::lock_guard<std::mutex> lock(s->exportMtx);
    bs.setCancel(s->exportEngine.cancelCell(), s->exportEngine.ticket());
    const double mpx0 = bs.demosaicMpx(), dms0 = bs.demosaicMs();  // the render pass's share of the counters (the analysis pass came first)
    xstream::BandSourceStore store(bs);
    dapple::engine::EngineSession::GpuExport e{params, point, virt, store, *s->camera, lut.get()};
    e.outLongEdge = outLongEdge;
    e.forceStream = true;
    // tiles of 512: the device pool (shared memory on phones) stays small; the GPU executor is tile-invariant (same pixels at any tile size)
    e.tile = 512;
    if (const char* te = std::getenv("DAPPLE_EXPORT_STREAM_TILE")) e.tile = std::max(64, std::atoi(te) / 4 * 4);
    std::vector<std::uint8_t> band, row;
    int ow = 0, bandY = -1;
    bool aborted = false;
    double packMs = 0, handMs = 0;  // the sink's 4 -> 3 channel pack, the band's hand-off (progress + encoder push, which waits for the strip before)
    e.onSize = [&](int w, int h) {
      ow = w;
      sized(w, h);
    };
    e.target.sink = [&](int x, int y, int tw, int th, const std::uint8_t* rgba, std::size_t stride) {
      const auto tp = std::chrono::steady_clock::now();
      if (y != bandY) {  // tiles arrive in raster order: a new band row (every pixel of it is written by its tiles)
        bandY = y;
        band.resize(std::size_t(th) * ow * 3);
      }
      for (int j = 0; j < th; ++j) {
        const std::uint8_t* src = rgba + std::size_t(j) * stride;
        std::uint8_t* dst = band.data() + (std::size_t(j) * ow + x) * 3;
        // the readback memory is mapped device memory (uncached on a phone: narrow loads from it are slow): the row is copied out with wide loads first,
        // then packed RGBA -> RGB with one 4-byte copy per pixel (the alpha byte is overwritten by the next pixel's red), the last pixel by its three bytes
        row.resize(std::size_t(tw) * 4);
        std::memcpy(row.data(), src, std::size_t(tw) * 4);
        const std::uint8_t* rp = row.data();
        int i = 0;
        for (; i + 1 < tw; ++i) std::memcpy(dst + 3 * i, rp + 4 * i, 4);
        if (i < tw) dst[3 * i] = rp[4 * i], dst[3 * i + 1] = rp[4 * i + 1], dst[3 * i + 2] = rp[4 * i + 2];
      }
      packMs += msSince(tp);
      if (x + tw < ow) return true;
      bandY = -1;
      const auto th0 = std::chrono::steady_clock::now();
      const bool handed = progressSink(y, th, band.data());
      handMs += msSince(th0);
      if (!handed) return !(aborted = true);
      return true;
    };
    std::string err;
    dapple::engine::GpuStats gst;
    bs.resetRowCounts();
    const auto tg0 = std::chrono::steady_clock::now();
    const int r = s->exportEngine.exportGpu(e, &gst, &err);
    const double gpuWall = msSince(tg0);
    stageLog("export_stream GPU %dx%d -> long edge %d: %s (render pass %llu windows, %.1f MP of %.1f MP, demosaic %.0f ms %s%s; rows demosaiced: %d, twice or more: %d, from the cache: %d)", W, H, int(outLongEdge),
             r == 0 ? "ok" : r > 0 ? "GPU declined" : err.c_str(), (unsigned long long)(bs.windows() - windows0), bs.demosaicMpx() - mpx0, double(W) * H / 1e6, bs.demosaicMs() - dms0,
             bs.gpu() ? "GPU" : "CPU", bs.fellBack() ? ", fell back" : "", bs.rowsDemosaiced(), bs.rowsDemosaicedAgain(), bs.rowsNotDemosaiced());
    stageLog("export_stream GPU render %.0f ms = source windows %.0f (demosaic %.0f, copies %.0f) + upload %.0f (host copy %.0f, convert submit %.0f, device wait %.0f) + record %.0f + submit %.0f + wait for the previous tile %.0f + sink %.0f (pack %.0f, hand-off %.0f) + turn %.0f + other %.0f; GPU busy %.0f ms, %d tiles",
             gpuWall, gst.tSrcMs, bs.demosaicMs() - dms0, gst.tSrcMs - (bs.demosaicMs() - dms0), gst.tUpMs, gst.tUpCopyMs, gst.tUpSubMs, gst.tUpWaitMs, gst.tRecMs, gst.tSubMs, gst.tWaitMs, gst.tSinkMs, packMs, handMs, gst.tTurnMs,
             gpuWall - gst.tSrcMs - gst.tUpMs - gst.tRecMs - gst.tSubMs - gst.tWaitMs - gst.tSinkMs - gst.tTurnMs, gst.gpuMs, gst.tiles);
    if (r == 0) return 1;
    if (r < 0 || aborted) {
      setError(aborted ? "cancelled" : (err.empty() ? "GPU export failed" : err));
      return -1;
    }
    // r == 1: the CPU below (onSize starts the output again)
  }
#endif
  if (reduced) {  // the exact reduction, rendered whole (as resizedSource + renderExact of the whole-frame path)
    const std::shared_ptr<SourceImage> R = a->at(outLongEdge);
    if (!R) return 0;
    Image8 img;
    if (!engineRenderExact(s, params, point, *R, img)) return -1;
    sized(img.width, img.height);
    for (int y = 0; y < img.height; y += 512) {
      const int n = std::min(512, img.height - y);
      if (!progressSink(y, n, img.data.data() + std::size_t(y) * img.width * 3)) {
        setError("cancelled");
        return -1;
      }
    }
    return 1;
  }
  std::lock_guard<std::mutex> lock(s->exportMtx);
  bs.setCancel(s->exportEngine.cancelCell(), s->exportEngine.ticket());
  int tile = film ? 256 : 512;  // one JPEG strip per band (a film band carries its E2 / DIR planes: half the rows)
  if (const char* e = std::getenv("DAPPLE_EXPORT_STREAM_TILE")) tile = std::max(64, std::atoi(e) / 4 * 4);
  std::string err;
  const auto t0 = std::chrono::steady_clock::now();
  int fw = 0, fh = 0;
  const bool ok = xstream::renderBands(s->exportEngine, params, point, virt, bs, tile, s->proxy != nullptr, [&](int w, int h) { fw = w, fh = h, sized(w, h); },
                                       progressSink, &err);
  stageLog("export_stream CPU %dx%d: %s in %.0f ms (%d bands, %llu windows demosaiced, %llu from the row cache, demosaic %.0f ms %s, largest window %.0f MB)", fw, fh,
           ok ? "ok" : err.c_str(), msSince(t0), bandsDone, (unsigned long long)(bs.windows() - windows0), (unsigned long long)(bs.cacheHits() - hits0), bs.demosaicMs(),
           bs.gpu() ? "GPU" : "CPU", double(bs.peakWindowBytes()) / 1048576.0);
  if (!ok) {
    if (err.rfind("engine: plan not supported", 0) == 0) return 0;
    setError(err);
    return -1;
  }
  return 1;
}

// ---- streaming export (section 42): the JPEG / Ultra HDR export of a lazy session -------------------------------------------------------------
// An edit the open did not foresee (a film stock: ~55 B/px more than a plain export) on a session that holds the whole float picture: when the
// whole-frame export would not fit, the session gives its picture back and exports in bands from the cached mosaic (same pixels).
void demoteToStream(DappleSession* s, const EditParams& ep, bool force) {
  if (s->lazyOpen.load() || s->fullRawPath.empty()) return;
  if (xstream::envMode() == xstream::Mode::Off || (xstream::envMode() == xstream::Mode::On && !force)) return;
  const int W = s->source.linear.width, H = s->source.linear.height;
  const std::size_t held = std::size_t(W) * H * 12;  // the float picture is already there
  const bool film = engine::point::filmActive(ep);
  std::size_t need = xstream::predictWholePeak(W, H, film);
  bool gpuFits = true;
#ifdef DAPPLE_HAVE_GPU
  {  // the GPU executor renders a whole-frame export where it can: on unified memory its images come out of the same RAM; its own heap must hold it
    const GpuWhole g = gpuWholeCheck(W, H, film);
    need += g.extra;
    gpuFits = g.fits;
  }
#endif
  std::string how;
  const std::size_t budget = xstream::budgetBytes(&how);
  if (!force && gpuFits && need <= held + budget) return;
  std::string why;
  auto z = openFullLazy(s->fullRawPath, DecodeOptions{}, &why);
  if (!z || z->width != W || z->height != H) return;
  std::lock_guard<std::recursive_mutex> lk(s->lazyMtx);
  ImageF().data.swap(s->source.linear.data);
  s->source.linear = ImageF();
  s->lazy = std::make_unique<LazyFull>(std::move(*z));
  s->lazyOpen = true;
  if (force) stageLog("export: the GPU could not allocate the whole frame: the session switches to bands (picture released)");
  else if (!gpuFits) stageLog("export: the GPU's memory budget cannot hold the whole frame: the session switches to bands (picture released)");
  else
    stageLog("export: whole frame needs %.0f MB more, the device has %.0f MB (%s): the session switches to bands (picture released)", double(need - held) / 1048576.0,
             double(budget) / 1048576.0, how.c_str());
}

// A whole-frame render gave up because the GPU ran out of memory (renderSessionParams set retryBands): switch the session to bands. True = the
// export continues with streamExportJpeg.
bool retryInBands(DappleSession* s, const EditParams& ep) {
  if (!s->retryBands) return false;
  s->retryBands = false;
  demoteToStream(s, ep, true);
  return s->lazyOpen.load();
}

bool wholeFrameFits(DappleSession* s, const EditParams& ep) {
  if (!s->lazy) return true;
  const std::size_t need = xstream::predictWholePeak(s->lazy->width, s->lazy->height, engine::point::filmActive(ep));
  const std::size_t budget = xstream::budgetBytes();
  if (xstream::envMode() == xstream::Mode::On || need <= budget) return true;
  char b[256];
  std::snprintf(b, sizeof b, "export: this edit needs the whole %dx%d picture in memory (about %.0f MB) and the device has about %.0f MB for it; nothing was written",
                s->lazy->width, s->lazy->height, double(need) / 1048576.0, double(budget) / 1048576.0);
  setError(b);
  stageLog("%s", b);
  return false;
}

}  // namespace capi
