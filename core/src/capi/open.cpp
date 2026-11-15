#include "session.h"

#if defined(__ANDROID__) || defined(__linux__)
#include <malloc.h>
#endif

static thread_local int tlExportFilm = -1;  // dapple_export_intent: the next FULL open on this thread exports an edit with (1) / without (0) a film stock

// Streaming export (section 42): the FULL open of a mosaic RAW whose whole-frame export would not fit the device (xstream::openLazy) keeps the cached
// mosaic and the metadata only; with long_edge > 0 the reduction is built band by band (bit-identical to resizeToLongEdge of the full decode) and
// the session is an ordinary one. Null = open it the usual way (the mosaic and the anchor are cached: no second unpack).
static DappleSession* openLazySession(const fs::path& p, const DecodeOptions& opts, int32_t longEdge, bool* mosaicPath) {
  const auto t0 = std::chrono::steady_clock::now();
  std::string why;
  auto z = openFullLazy(p, opts, &why);
  if (!z) return nullptr;
  if (mosaicPath) *mosaicPath = true;
  // the budget is what is available NOW, after dropping what the export can do without (other photos' mosaics, film workspaces, idle AI models)
  const std::size_t foot0 = mem::footprint();
  std::size_t trimmed = xstream::trimForExport(z->mosaic->key);
  dapple_ai_release(1);  // every model (42.9: an export never runs one, except to re-create a missing plane, which loads it again)
#ifdef DAPPLE_HAVE_GPU
  if (engine::GpuProvider* gp = engine::gpuProvider()) trimmed += gp->trimPool(0);  // the editor's free pool images (up to 512 MB on a phone)
#endif
#if defined(__ANDROID__) && defined(M_PURGE)
  mallopt(M_PURGE, 0);  // the pages freed above go back to the OS before the footprint is read
#endif
  const std::size_t foot1 = mem::footprint();
  std::string policy;
  // the edit when the app announced it (dapple_export_intent on this thread): a film export decides here instead of decoding the whole picture
  // first and giving it back at the export (demoteToStream)
  const bool film = tlExportFilm == 1;
  tlExportFilm = -1;
  std::size_t gpuExtra = 0;
  bool gpuFits = true;
#ifdef DAPPLE_HAVE_GPU
  {
    const GpuWhole g = gpuWholeCheck(z->width, z->height, film);  // unified memory: the GPU export's images come out of the same RAM (42.9)
    gpuExtra = g.extra;
    gpuFits = g.fits;
  }
#endif
  const bool lazy = xstream::openLazy(z->width, z->height, &policy, gpuExtra, film) || (!gpuFits && xstream::envMode() == xstream::Mode::Auto);
  stageLog("session_open %s: trim before the export decision released %.0f MB of caches (footprint %.0f -> %.0f MB); %s -> %s", pathToUtf8(p.filename()).c_str(),
           double(trimmed) / 1048576.0, double(foot0) / 1048576.0, double(foot1) / 1048576.0, policy.c_str(), lazy ? "stream" : "whole frame");
  if (!lazy) return nullptr;
  auto session = std::make_unique<DappleSession>();
  session->source = z->meta;
  session->cameraOwned = dapple::effectiveCamera(cameras(), session->source.meta);
  session->camera = &session->cameraOwned;
  if (longEdge > 0 && std::max(z->width, z->height) > longEdge) {
    xstream::BandSource bs(z->mosaic);
    xstream::Wants w;
    w.longEdges = {int(longEdge)};
    xstream::Analyses a;
    std::string err;
    if (!xstream::analyse(bs, z->meta, w, a, {}, &err) || !a.at(longEdge)) return nullptr;
    session->source.linear = std::move(a.at(longEdge)->linear);
    stageLog("session_open %s quality=full streamed to long edge %d: %dx%d in %.0f ms (%s; %d pass, %s demosaic)", pathToUtf8(p.filename()).c_str(), int(longEdge),
             session->source.linear.width, session->source.linear.height, msSince(t0), policy.c_str(), a.passes, bs.gpu() ? "GPU" : "CPU");
    return session.release();
  }
  const int W = z->width, H = z->height;
  session->lazy = std::make_unique<LazyFull>(std::move(*z));
  session->lazyOpen = true;
  stageLog("session_open %s quality=full lazy %dx%d (mosaic kept, %s) in %.0f ms (footprint %.0f MB, available %.0f MB)", pathToUtf8(p.filename()).c_str(), W, H,
           policy.c_str(), msSince(t0), double(mem::footprint()) / 1048576.0, double(mem::available()) / 1048576.0);
  return session.release();
}

DappleSession* dapple_session_open(const char* path, int32_t quality, int32_t long_edge) {
#if defined(__APPLE__)
  sys::OpenQos openQos;
#endif
  return guarded(
      [&]() -> DappleSession* {
        const fs::path p = toPath(path);
        const ImageDecoder* decoder = decoders().find(p);
        if (!decoder) {
          setError("Unsupported file type: " + pathToUtf8(p));
          return nullptr;
        }
        DecodeOptions opts;
        opts.quality = quality == DAPPLE_QUALITY_FULL ? DecodeQuality::Full : DecodeQuality::Preview;
        opts.wantLongEdge = long_edge;
#ifdef DAPPLE_HAVE_GPU
        // the decoder's Demosaic node uses the GPU when the runtime switch is on: Full opens, and the preview opens above 2048 (the 2x view of an X-Trans file runs the
        // half-resolution node, which has its own measured verdict). The backend creates its device lazily on the first node run, so a preview that never runs a
        // node (a Bayer file binned for 2048) pays nothing here.
        if (opts.quality == DecodeQuality::Full || long_edge > 2048) dapple::demosaic::installGpuBackend();
#endif
        const auto t0 = std::chrono::steady_clock::now();
        // streaming export (section 42): a FULL open of a large mosaic RAW keeps the mosaic, not the float picture
        bool mosaicPath = false;
        if (opts.quality == DecodeQuality::Full && std::strcmp(decoder->name(), "libraw") == 0 && xstream::envMode() != xstream::Mode::Off)
          if (DappleSession* lazy = openLazySession(p, opts, long_edge, &mosaicPath)) return lazy;
        // a RAW the band path cannot read (linear / JPEG XL DNG, sRAW, an OpcodeList3 warp) decodes whole: refuse cleanly what cannot fit
        // (the LibRaw decode peaks near 30 B/px: a 48 MP linear DNG ~1.4 GB) instead of being killed by the OS (section 42.7)
        if (!mosaicPath && opts.quality == DecodeQuality::Full && std::strcmp(decoder->name(), "libraw") == 0 && xstream::envMode() == xstream::Mode::Auto) {
          if (auto meta = decoder->probe(p); meta && meta->sensorWidth > 0 && meta->sensorHeight > 0) {
            const std::size_t need = xstream::predictWholeDecodePeak(meta->sensorWidth, meta->sensorHeight);
            std::string how;
            const std::size_t budget = xstream::budgetBytes(&how);
            if (need > budget) {
              char b[300];
              std::snprintf(b, sizeof b, "this %dx%d RAW cannot be read in bands and its full decode needs about %.0f MB; the device has about %.0f MB (%s)",
                            meta->sensorWidth, meta->sensorHeight, double(need) / 1048576.0, double(budget) / 1048576.0, how.c_str());
              stageLog("session_open %s refused: %s", pathToUtf8(p.filename()).c_str(), b);
              setError(b);
              return nullptr;
            }
          }
        }
        auto src = decoder->decode(p, opts);
        if (!src) {
          setError(src.error().message);
          return nullptr;
        }
        auto session = std::make_unique<DappleSession>();
        session->source = std::move(*src);
        if (mosaicPath && long_edge <= 0) session->fullRawPath = p;
        const double decodeMs = msSince(t0);
        if (long_edge > 0) session->source.linear = resizeToLongEdge(session->source.linear, long_edge);
        stageLog("session_open %s quality=%s decoded %dx%d in %.0f ms, long_edge %d total %.0f ms (footprint %.0f MB, available %.0f MB)", pathToUtf8(p.filename()).c_str(),
                 opts.quality == DecodeQuality::Full ? "full" : "preview", session->source.linear.width, session->source.linear.height, decodeMs, long_edge, msSince(t0),
                 double(mem::footprint()) / 1048576.0, double(mem::available()) / 1048576.0);
        session->cameraOwned = dapple::effectiveCamera(cameras(), session->source.meta);
        session->camera = &session->cameraOwned;
        return session.release();
      },
      nullptr);
}

static std::atomic<std::uint64_t> g_regionGen{0};  // latest wins: a new region open (or dapple_region_cancel) cancels the one in flight

void dapple_region_cancel(void) { g_regionGen.fetch_add(1); }

DappleSession* dapple_session_open_region(const char* path, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
#if defined(__APPLE__)
  sys::OpenQos openQos;
#endif
  const std::uint64_t ticket = g_regionGen.fetch_add(1) + 1;
  return guarded(
      [&]() -> DappleSession* {
        const fs::path p = toPath(path);
#ifdef DAPPLE_HAVE_GPU
        dapple::demosaic::installGpuBackend();
#endif
        std::string why;
        const auto t0 = std::chrono::steady_clock::now();
        auto src = dapple::decodeRawRegion(p, x0, y0, x1, y1, DecodeOptions{}, &why, &g_regionGen, ticket);
        stageLog("session_open_region %s %dx%d at %d,%d: %s in %.0f ms", pathToUtf8(p.filename()).c_str(), x1 - x0, y1 - y0, x0, y0,
                 src ? "ok" : why.c_str(), msSince(t0));
        if (!src) {
          setError("region open: " + why);
          return nullptr;
        }
        auto session = std::make_unique<DappleSession>();
        session->source = std::move(*src);
        session->cameraOwned = dapple::effectiveCamera(cameras(), session->source.meta);
        session->camera = &session->cameraOwned;
        return session.release();
      },
      nullptr);
}

void dapple_session_close(DappleSession* session) { delete session; }

int32_t dapple_session_is_streaming(DappleSession* session) { return session && session->lazyOpen.load() ? 1 : 0; }

int32_t dapple_export_intent(const char* params_json) {
  return guarded(
      [&]() -> int32_t {
        tlExportFilm = -1;
        EditParams ep;
        if (!params_json || !parseParams(params_json, ep)) return DAPPLE_ERROR;
        tlExportFilm = engine::point::filmActive(ep) ? 1 : 0;
        return DAPPLE_OK;
      },
      DAPPLE_ERROR);
}
