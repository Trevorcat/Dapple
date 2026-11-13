// Implementation of the frozen C API (dapple/dapple_c.h).

#include "dapple/dapple_c.h"
#include "file_util.h"

#include <array>
#include <atomic>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <chrono>
#include <string>
#include <vector>

#include "dapple/auto_adjust.h"
#include "dapple/camera.h"
#include "dapple/decoder.h"
#include "dapple/mosaic.h"
#include "engine/nodes/demosaic/demosaic.h"
#include "dapple/edit_params.h"
#include "dapple/encode.h"
#include "exif_writer.h"
#include "dapple/film.h"
#include "dapple/film_match.h"
#include "dapple/look.h"
#include "dapple/lut.h"
#include "dapple/renderer.h"
#include "dng_thumbnail.h"
#include "dapple/tone.h"
#include "dapple/ultrahdr.h"
#include "parallel.h"
#include "determinism.h"
#include "sysperf.h"
#include "dapple/xmp_preset.h"
#include "engine/exec/engine_session.h"  // optional frame engine (DAPPLE_ENGINE), default off
#include "frame_kernels.h"
#include "local_eval.h"
#include "ai_masks.h"
#include "engine/nodes/film/film_async.h"
#include "engine/nodes/film/frame_trace.h"
#include "color.h"
#include "export_stream.h"  // streaming (band) export, docs/engine/pipeline-architecture.md section 42
#include "mem_probe.h"
#if defined(__ANDROID__)
#include <android/log.h>
#elif defined(__APPLE__)
#include <os/log.h>
#endif
#ifdef DAPPLE_HAVE_GPU
#include "engine/exec/gpu_runtime.h"  // optional GPU executor (core/gpu): runtime flag, status; frames go through EngineSession
#include "engine/nodes/demosaic/demosaic_backend.h"  // GPU Demosaic backend for the decoder (own device, docs 32)
#endif

#ifndef DAPPLE_VERSION_STRING
#define DAPPLE_VERSION_STRING "0.1.0"
#endif

static void stageLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));  // defined below (streaming export logs, section 42)
static double msSince(std::chrono::steady_clock::time_point t0);

struct DappleSession {
  dapple::SourceImage source;
  const dapple::CameraProfile* camera = nullptr;  // &cameraOwned: the profile with this file's exposure anchor
  dapple::CameraProfile cameraOwned;

  // ---- interactive frame caches (see dapple_session_render_frame) ----
  std::mutex mtx;
  std::shared_ptr<dapple::SourceImage> proxy;                  // 256 px copy for the tonal analysis
  std::vector<std::shared_ptr<const dapple::BakedLut>> bakes;  // newest last (three slots)
  std::shared_ptr<const dapple::Lut3D> lutKeep;
  dapple::EditParams lastParams;
  bool haveLast = false;
  dapple::engine::EngineSession engine;  // used only when DAPPLE_ENGINE is set (docs/engine/pipeline-architecture.md)
  dapple::engine::EngineSession exportEngine;  // DAPPLE_ENGINE=full: export / reference renders (serialised by exportMtx)
  std::mutex exportMtx;
  // film exposure that keeps the photo's key (film_match.h): own 256 px proxy, results cached by filmMatchKey()
  std::mutex keyMtx;
  std::shared_ptr<dapple::SourceImage> keyProxy;
  struct FilmKeyEntry {
    std::string key;
    dapple::FilmExposure ev;
    bool exact;
  };
  std::vector<FilmKeyEntry> filmKeys;
  std::string lastMatchStock;  // stock of the newest filmKeys entry (keyMtx)
  // background refresh of the match while a slider moves (queueFilmMatch)
  std::mutex matchMtx;  // filmCache + keyProxy: the frame thread (exact solves) and the refresh worker
  std::mutex asyncMtx;
  dapple::EditParams asyncParams;
  std::string asyncLut, asyncKey;
  bool asyncHave = false, asyncRunning = false;
  std::atomic<int> matchBusy{0};
  // touch hint (dapple_session_touch_hint): the newest interactive frame request, rendered again in a short burst on touch-down
  std::mutex hintMtx;
  std::string hintJson, hintLut, hintActive;
  DappleFrameRequest hintRq{};
  bool hintHave = false;
  std::atomic<std::uint64_t> frameSeq{0};
  std::atomic<bool> hintRunning{false};
  std::atomic<std::uint64_t> swatchSeq{0};  // dapple_session_base_previews: the newest request (or cancel) wins
  ~DappleSession() {
    dapple::aimask::cancelOwner(this);  // AI mask jobs hold a pointer to this session
    dapple::aimask::waitOwner(this);
    for (int i = 0; i < 5000 && matchBusy.load() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  dapple::FilmMatchCache filmCache;  // image statistics + remembered medians of the match (film_match.h)
  // Where the executors read the full-resolution source from (the GPU keeps a device copy per store identity)
  std::once_flag storeOnce;
  std::unique_ptr<dapple::engine::MemorySourceStore> store;
  // ---- streaming export (docs/engine/pipeline-architecture.md section 42) ----
  // A FULL open of a large mosaic RAW keeps the cached mosaic instead of the float picture: source.linear stays empty (meta and scene flag are
  // set) until a call needs the whole picture (ensureSource); exports stream from the mosaic in bands (streamRender).
  std::unique_ptr<dapple::LazyFull> lazy;
  std::atomic<bool> lazyOpen{false};
  std::recursive_mutex lazyMtx;
  std::shared_ptr<dapple::xstream::Analyses> streamA;  // exact reductions of the lazy picture (first streamed render)
  std::shared_ptr<dapple::xstream::BandSource> bands;  // the band source of the export in flight: its row cache carries the analysis pass's rows to the render
  bool streamFilm = false;                             // the export in flight is a film edit (its bands need more memory: a smaller row cache)
  std::filesystem::path fullRawPath;                   // a FULL open of a RAW at natural size (an export may switch it to bands: demoteToStream)
  DappleProgressFn exportProgress = nullptr;           // dapple_session_set_export_progress
  void* exportProgressUser = nullptr;
};

namespace {

using namespace dapple;
namespace fs = std::filesystem;

thread_local std::string g_lastError;
thread_local bool tlHintFrame = false;  // set on the touch-hint worker: its frames are not real frames

void setError(std::string message) { g_lastError = std::move(message); }

// Streaming export (section 42): a lazy FULL session gets its float picture here when a call needs the whole of it (no-op otherwise).
bool ensureSource(DappleSession* s);

// UTF-8 C string -> path (std::filesystem handles non-ASCII on Windows).
fs::path toPath(const char* utf8) {
  const std::string s = utf8 ? utf8 : "";
  return fs::path(std::u8string(s.begin(), s.end()));
}

const CameraRegistry& cameras() {
  static const CameraRegistry registry = CameraRegistry::withBuiltins();
  return registry;
}
const DecoderRegistry& decoders() {
  static const DecoderRegistry registry = DecoderRegistry::withBuiltins();
  return registry;
}

char* dupString(const std::string& s) {
  char* out = static_cast<char*>(std::malloc(s.size() + 1));
  if (!out) return nullptr;
  std::memcpy(out, s.c_str(), s.size() + 1);
  return out;
}

// ---- tiny JSON writing helpers ----
std::string esc(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (static_cast<unsigned char>(c) < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
      out += buf;
    } else {
      out += c;
    }
  }
  return out;
}
std::string q(const std::string& s) { return "\"" + esc(s) + "\""; }
std::string n(double v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.9g", v);
  return buf;
}
const char* supportName(Support s) {
  switch (s) {
    case Support::Yes: return "yes";
    case Support::No: return "no";
    default: return "unverified";
  }
}

std::string metaJson(const ShotMetadata& m, const CameraProfile& cam) {
  std::string j = "{";
  j += "\"make\":" + q(m.make) + ",\"model\":" + q(m.model) + ",\"lens\":" + q(m.lens);
  j += ",\"iso\":" + n(m.iso) + ",\"shutter\":" + n(m.shutterSeconds) + ",\"aperture\":" + n(m.aperture);
  j += ",\"focalLength\":" + n(m.focalLengthMm) + ",\"timestamp\":" + std::to_string(m.timestamp);
  j += ",\"width\":" + std::to_string(m.sensorWidth) + ",\"height\":" + std::to_string(m.sensorHeight);
  j += ",\"cfa\":" + q(toString(m.cfa == CfaKind::Unknown ? cam.cfa : m.cfa)) + ",\"rawFormat\":" + q(m.rawFormat);
  j += ",\"inCameraLook\":" + q(m.inCameraLook);
  j += ",\"camera\":{\"id\":" + q(cam.id) + ",\"displayName\":" + q(cam.displayName);
  j += ",\"ftp\":" + q(supportName(cam.transfer.ftpPush));
  j += ",\"usbPtp\":" + q(supportName(cam.transfer.usbPtp));
  j += ",\"vendorAppAutoSend\":" + q(supportName(cam.transfer.vendorAppAutoSend));
  j += ",\"vendorApp\":" + q(cam.transfer.vendorApp) + "}}";
  return j;
}

// ---- LUT cache (by path; reloaded when the file's mtime changes) ----
struct CachedLut {
  std::shared_ptr<const Lut3D> lut;
  fs::file_time_type mtime{};
};
std::mutex g_lutMutex;
std::map<std::string, CachedLut> g_luts;

std::shared_ptr<const Lut3D> loadLut(const std::string& pathUtf8) {
  const fs::path path = toPath(pathUtf8.c_str());
  std::error_code ec;
  const auto mtime = fs::last_write_time(path, ec);
  std::lock_guard<std::mutex> lock(g_lutMutex);
  auto it = g_luts.find(pathUtf8);
  if (it != g_luts.end() && (ec || it->second.mtime == mtime)) return it->second.lut;
  auto lut = Lut3D::loadCube(path);
  if (!lut) {
    setError(lut.error().message);
    return nullptr;
  }
  CachedLut entry{std::make_shared<const Lut3D>(std::move(*lut)), ec ? fs::file_time_type{} : mtime};
  if (g_luts.size() > 16) g_luts.clear();
  return (g_luts[pathUtf8] = std::move(entry)).lut;
}

bool parseParams(const char* paramsJson, EditParams& params) {
  params = EditParams{};
  if (paramsJson && paramsJson[0]) {
    auto parsed = fromJson(paramsJson);
    if (!parsed) {
      setError("params: " + parsed.error().message);
      return false;
    }
    params = std::move(*parsed);
  }
  return true;
}

// Computes (and stores in the session's key list) the film match of `params`. The caller holds s->matchMtx (filmCache and the proxy belong to it).
FilmExposure computeFilmMatch(DappleSession* s, const EditParams& params, const std::string& lutFile, const std::string& key, bool interactive, bool uncached) {
  if (!s->keyProxy) {
    auto kp = std::make_shared<SourceImage>();
    kp->sceneReferred = s->source.sceneReferred;
    kp->linear = resizeToLongEdge(s->source.linear, 256);
    s->keyProxy = std::move(kp);
  }
  std::shared_ptr<const Lut3D> lut;
  if (!lutFile.empty()) lut = loadLut(lutFile);
  s->filmCache.proxyOwner = s->keyProxy;
  s->filmCache.lutOwner = lut;
  const FilmExposure ev = uncached ? matchFilmExposure(params, *s->camera, lut.get(), *s->keyProxy)
                                   : matchFilmExposureCached(params, *s->camera, lut.get(), lutFile, *s->keyProxy, s->filmCache, !interactive);
  const bool exact = uncached || !s->filmCache.lastApprox;
  std::lock_guard<std::mutex> lock(s->keyMtx);
  for (auto it = s->filmKeys.begin(); it != s->filmKeys.end(); ++it)
    if (it->key == key) {
      s->filmKeys.erase(it);
      break;
    }
  if (s->filmKeys.size() >= 12) s->filmKeys.erase(s->filmKeys.begin());
  s->filmKeys.push_back({key, ev, exact});
  s->lastMatchStock = params.filmStock;
  return ev;
}

// Latest-wins background refresh of the match while a slider moves (wave 8): an interactive frame whose match is not cached uses the previous result
// of the same stock and queues the recompute; the repaint listeners are told when it lands. The settled frame is not interactive and computes the
// exact match itself (it waits for a running refresh at most one match long).
void queueFilmMatch(DappleSession* s, const EditParams& params, const std::string& lutFile, const std::string& key) {
  {
    std::lock_guard<std::mutex> lock(s->asyncMtx);
    s->asyncParams = params;
    s->asyncLut = lutFile;
    s->asyncKey = key;
    s->asyncHave = true;
    if (s->asyncRunning) return;
    s->asyncRunning = true;
  }
  s->matchBusy.fetch_add(1);
  std::thread([s] {
    dapple::ThreadPool::tlUse() = &dapple::ThreadPool::background();
    sys::utilityQos();
    for (;;) {
      EditParams p;
      std::string lf, k;
      {
        std::lock_guard<std::mutex> lock(s->asyncMtx);
        if (!s->asyncHave) {
          s->asyncRunning = false;
          break;
        }
        p = s->asyncParams;
        lf = s->asyncLut;
        k = s->asyncKey;
        s->asyncHave = false;
      }
      try {
        dapple::ftrace::Busy traceBusy(dapple::ftrace::S().bgMatch);
        std::lock_guard<std::mutex> ml(s->matchMtx);
        bool hit = false;
        {
          std::lock_guard<std::mutex> kl(s->keyMtx);
          for (auto& e : s->filmKeys) hit |= e.key == k;
        }
        if (!hit) computeFilmMatch(s, p, lf, k, true, false);
      } catch (...) {
      }
      bool more;
      {
        std::lock_guard<std::mutex> lock(s->asyncMtx);
        more = s->asyncHave;
      }
      if (!more) dapple::engine::filmnode::notifyRepaintIfMatchStale();
    }
    s->matchBusy.fetch_sub(1);
  }).detach();
}

// `params` with EditParams::filmKeyEv set: the EV that puts the film render's median lightness where the normal develop
// render of the same edit has it (film_match.h). A few film evaluations of the 256 px proxy, cached by key.
EditParams withFilmKey(DappleSession* s, EditParams params, const char* lutPath = nullptr, bool interactive = false) {
  params.filmKeyEv = 0.f;
  params.filmPrintEv = 0.f;
  params.filmSceneX = s->source.meta.asShotIlluminantXy[0];  // film white balance: the scene's as-shot illuminant (0 = unknown)
  params.filmSceneY = s->source.meta.asShotIlluminantXy[1];
  if (params.filmStock.empty() || isModernId(params.filmStock) || std::getenv("DAPPLE_FILM_MATCH_OFF")) return params;
  const std::string lutFile = lutPath && lutPath[0] ? lutPath : params.lutPath;
  // DAPPLE_FILM_MATCH_UNCACHED=1: the match as before section 26 (full key, every solve from scratch, no calibration warm-up): the A/B switch of the drag bench
  const char* ue = std::getenv("DAPPLE_FILM_MATCH_UNCACHED");
  const bool uncached = ue && ue[0];
  const std::string key = (uncached ? filmMatchKey(params) : filmMatchKeyLean(params)) + "|" + lutFile + "|" + std::to_string(params.filmSceneX) + "," + std::to_string(params.filmSceneY);
  if (!uncached) prewarmFilmCalibration(params, [] { sys::setBackground(true); });
  static const bool asyncOn = [] {
    const char* e = std::getenv("DAPPLE_FILM_MATCH_ASYNC");
    return !(e && e[0] == '0') && !dapple::deterministicMode();
  }();
  {
    std::lock_guard<std::mutex> lock(s->keyMtx);
    // A result that interpolated remembered medians (a print solve while a slider moves) serves interactive frames only; the frame after the
    // drag solves for real (docs/engine/pipeline-architecture.md section 26).
    for (auto& e : s->filmKeys)
      if (e.key == key && (e.exact || interactive)) {
        params.filmKeyEv = e.ev.filmEv;
        params.filmPrintEv = e.ev.printEv;
        dapple::ftrace::with([&](dapple::ftrace::Rec& x) {
          x.matchKind = e.exact ? dapple::ftrace::kMatchHit : dapple::ftrace::kMatchInterp;
          x.filmEv = e.ev.filmEv;
          std::snprintf(x.stock, sizeof x.stock, "%s", params.filmStock.c_str());
        });
        return params;
      }
    if (asyncOn && interactive && !uncached && !s->filmKeys.empty()) {  // any stock: while another stock is selected the frame shows the previous stock too
      // previous result of the same stock now, the new one on the background pool
      const FilmExposure& prev = s->filmKeys.back().ev;
      params.filmKeyEv = prev.filmEv;
      // a stock that has no match of its own yet (the film map slides across stocks): the print exposure is the stock's neutral 0, the value of its prebaked
      // coarse table (a hit costs nothing, a bake ~25 ms on the A11) and not the previous stock's solved print EV; the repaint hook refines after the match lands
      params.filmPrintEv = s->lastMatchStock != params.filmStock ? 0.f : prev.printEv;
      dapple::ftrace::with([&](dapple::ftrace::Rec& x) {
        x.matchKind = dapple::ftrace::kMatchStalePrev;
        x.filmEv = prev.filmEv;
        std::snprintf(x.stock, sizeof x.stock, "%s", params.filmStock.c_str());
      });
      dapple::engine::filmnode::markMatchStale();
      queueFilmMatch(s, params, lutFile, key);
      return params;
    }
  }
  std::lock_guard<std::mutex> ml(s->matchMtx);
  double traceMatchMs = 0;
  FilmExposure ev;
  {
    dapple::ftrace::Timer traceTimer(&traceMatchMs);
    ev = computeFilmMatch(s, params, lutFile, key, interactive, uncached);
  }
  dapple::ftrace::with([&](dapple::ftrace::Rec& x) {
    x.matchKind = dapple::ftrace::kMatchComputed;
    x.matchMs += traceMatchMs;
    x.filmEv = ev.filmEv;
    std::snprintf(x.stock, sizeof x.stock, "%s", params.filmStock.c_str());
  });
  params.filmKeyEv = ev.filmEv;
  params.filmPrintEv = ev.printEv;
  return params;
}

// Renders the session with already parsed params. Returns false (with last error set) on failure.
bool renderSessionParams(DappleSession* s, const EditParams& params, const char* lutPath, int32_t outLongEdge,
                         Image8& out, float toneReach = 1.f);

// In place: 8-bit sRGB -> 8-bit Display P3 (colorimetric, D65, same transfer curve).
void srgb8ToDisplayP3(Image8& img) {
  static const std::array<float, 256> dec = [] {
    std::array<float, 256> t{};
    for (int i = 0; i < 256; ++i) t[std::size_t(i)] = dapple::color::srgbDecode(float(i) / 255.f);
    return t;
  }();
  const dapple::color::Mat3& m = dapple::color::srgbToP3();
  const std::size_t n = std::size_t(img.width) * img.height;
  for (std::size_t i = 0; i < n; ++i) {
    std::uint8_t* px = img.data.data() + 3 * i;
    const float in[3] = {dec[px[0]], dec[px[1]], dec[px[2]]};
    float o[3];
    m.apply(in, o);
    for (int c = 0; c < 3; ++c) px[c] = std::uint8_t(dapple::color::srgbEncode(o[c]) * 255.f + 0.5f);
  }
}

bool fillImage(const Image8& img, DappleImage* out) {
  const std::size_t pixels = std::size_t(img.width) * img.height;
  auto* rgba = static_cast<uint8_t*>(std::malloc(pixels * 4));
  if (!rgba) {
    setError("out of memory");
    return false;
  }
  for (std::size_t i = 0; i < pixels; ++i) {
    rgba[4 * i] = img.data[3 * i];
    rgba[4 * i + 1] = img.data[3 * i + 1];
    rgba[4 * i + 2] = img.data[3 * i + 2];
    rgba[4 * i + 3] = 255;
  }
  out->width = img.width;
  out->height = img.height;
  out->rgba = rgba;
  return true;
}

// DAPPLE_ENGINE=full: the exact (reference / export) variant through the Planner and the CpuExecutor. Frames of more than
// 16 MP are split into tiles (TiledExport, halo + frame-global sweeps) so the float frame never exists; DAPPLE_ENGINE_TILE=N
// forces tile size N for every size, =0 never tiles.
bool engineRenderExact(DappleSession* s, const EditParams& params, const Renderer& renderer, const SourceImage& src,
                       Image8& out) {
  dapple::engine::EngineSession::ExactOptions o;
  o.hasProxy = s->proxy != nullptr;
  int forced = -1;
  if (const char* e = std::getenv("DAPPLE_ENGINE_TILE")) forced = std::atoi(e);
  const std::size_t px = std::size_t(src.linear.width) * src.linear.height;
  if (forced > 0) {
    o.mode = dapple::engine::ExecMode::TiledExport;
    o.tile = forced;
  } else if (forced < 0 && px > 16000000u) {
    o.mode = dapple::engine::ExecMode::TiledExport;
  }
  std::lock_guard<std::mutex> lock(s->exportMtx);
  std::string err;
  if (!s->exportEngine.renderExact(params, renderer, src, o, out, &err)) {
    setError(err.empty() ? "engine failure" : err);
    return false;
  }
  return true;
}

[[maybe_unused]] dapple::engine::SourceStore* sourceStoreOf(DappleSession* s) {
  std::call_once(s->storeOnce, [s] { s->store = std::make_unique<dapple::engine::MemorySourceStore>(s->source); });
  return s->store.get();
}

#ifdef DAPPLE_HAVE_GPU
// Full-picture export on the GPU executor (tiled; lattice 65, Lanczos + a little sharpening when downscaled). False = the CPU renders it.
bool gpuExport(DappleSession* s, const EditParams& params, Renderer& renderer, const Lut3D* lut, int32_t outLongEdge, Image8& out) {
  std::lock_guard<std::mutex> lock(s->exportMtx);
  dapple::engine::EngineSession::GpuExport e{params, renderer, s->source, *sourceStoreOf(s), *s->camera, lut};
  e.outLongEdge = outLongEdge;
  int w = 0;
  e.onSize = [&](int ow, int oh) {
    w = ow;
    out.width = ow;
    out.height = oh;
    out.data.assign(std::size_t(ow) * oh * 3, 0);
  };
  e.target.sink = [&](int x, int y, int tw, int th, const std::uint8_t* rgba, std::size_t stride) {
    for (int j = 0; j < th; ++j) {
      const std::uint8_t* src = rgba + std::size_t(j) * stride;
      std::uint8_t* dst = out.data.data() + (std::size_t(y + j) * w + x) * 3;
      for (int i = 0; i < tw; ++i) {
        dst[3 * i] = src[4 * i];
        dst[3 * i + 1] = src[4 * i + 1];
        dst[3 * i + 2] = src[4 * i + 2];
      }
    }
    return true;
  };
  return s->exportEngine.exportGpu(e, nullptr, nullptr) == 0;
}
#endif

// Renders the session into 8-bit RGB. Returns false (with last error set) on failure.
bool renderSession(DappleSession* s, const char* paramsJson, const char* lutPath, int32_t outLongEdge, Image8& out) {
  if (!s) {
    setError("null session");
    return false;
  }
  EditParams params;
  if (!parseParams(paramsJson, params)) return false;
  return renderSessionParams(s, params, lutPath, outLongEdge, out);
}

// Streaming export (section 42), defined below: the render of a lazy session in bands. 1 = done, 0 = this edit cannot stream, -1 = failed.
using RowSink = std::function<bool(int y0, int n, const std::uint8_t* rgb)>;
int streamRender(DappleSession* s, const EditParams& paramsIn, const char* lutPath, int32_t outLongEdge, const std::function<void(int, int)>& onSize,
                 const RowSink& sink, const std::vector<int>& extraLongEdges = {});

bool renderSessionParams(DappleSession* s, const EditParams& paramsIn, const char* lutPath, int32_t outLongEdge,
                         Image8& out, float toneReach) {
  if (s->lazyOpen.load() && toneReach == 1.f && outLongEdge > 0 && s->lazy && outLongEdge < std::max(s->lazy->width, s->lazy->height)) {
    // a reduced render of a lazy session (Ultra HDR quarter renders, headroom probe): from the exact reduction / the streamed GPU export
    const int r = streamRender(
        s, paramsIn, lutPath, outLongEdge,
        [&](int w, int h) {
          out.width = w, out.height = h;
          out.data.assign(std::size_t(w) * h * 3, 0);
        },
        [&](int y0, int n, const std::uint8_t* rgb) {
          std::memcpy(out.data.data() + std::size_t(y0) * out.width * 3, rgb, std::size_t(n) * out.width * 3);
          return true;
        });
    if (r != 0) return r > 0;
  }
  if (!ensureSource(s)) return false;
  const EditParams params = withFilmKey(s, paramsIn, lutPath);
  if (params.outputSpace == 1 && Renderer(params, *s->camera, nullptr).filmActive()) {
    // Film stocks (and their blend with the normal render) are authored for sRGB output: rendered as sRGB, then converted to Display P3 (no colour is added).
    EditParams q = paramsIn;
    q.outputSpace = 0;
    if (!renderSessionParams(s, q, lutPath, outLongEdge, out, toneReach)) return false;
    srgb8ToDisplayP3(out);
    return true;
  }
  std::string lutFile = lutPath && lutPath[0] ? lutPath : params.lutPath;
  std::shared_ptr<const Lut3D> lut;
  if (!lutFile.empty()) {
    lut = loadLut(lutFile);
    if (!lut) return false;
  }
  Renderer renderer(params, *s->camera, lut.get());
  renderer.setToneReach(toneReach);
#ifdef DAPPLE_HAVE_GPU
  dapple::engine::gpu::installProvider();
  if (dapple::engine::gpu::runtimeEnabled() && toneReach == 1.f && (outLongEdge <= 0 || outLongEdge >= 1500) && gpuExport(s, params, renderer, lut.get(), outLongEdge, out)) return true;
#endif
  if (outLongEdge > 0 && std::max(s->source.linear.width, s->source.linear.height) > outLongEdge) {
    // the Resize node of the engine (exports and previews do not keep large copies)
    const std::shared_ptr<const SourceImage> scaled = s->exportEngine.resizedSource(s->source, outLongEdge, false);
    if (!scaled) {
      setError("resize failed");
      return false;
    }
    if (dapple::engine::mode() == dapple::engine::Mode::Full) return engineRenderExact(s, params, renderer, *scaled, out);
    out = renderer.render(*scaled);  // Mode::Off: the golden oracle
  } else {
    if (dapple::engine::mode() == dapple::engine::Mode::Full) return engineRenderExact(s, params, renderer, s->source, out);
    out = renderer.render(s->source);
  }
  return true;
}

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
    const std::size_t budget = xstream::budgetBytes();
    const std::size_t machinery = std::size_t(s->lazy->width) * (s->streamFilm ? 4100u : 2700u) * 12u;
    std::size_t cb = budget > machinery ? std::min(budget - machinery, budget / 2) : 0;
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
  stageLog("export_stream analyses %dx%d: %d pass(es) %.0f ms, shadow floor %d, %d reductions, guide grids %d, %s demosaic, row cache %.0f MB (%d rows)", s->lazy->width,
           s->lazy->height, a->passes, a->ms, a->shadow ? 1 : 0, int(a->reduced.size()), a->guide ? int(a->guide->grids.size()) : 0, bs.gpu() ? "GPU" : "CPU",
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

int streamRender(DappleSession* s, const EditParams& paramsIn, const char* lutPath, int32_t outLongEdge, const std::function<void(int, int)>& onSize,
                 const RowSink& sink, const std::vector<int>& extraLongEdges) {
  if (!s->lazyOpen.load() || !s->lazy) return 0;
  if (dapple::engine::mode() != dapple::engine::Mode::Full) return 0;  // the golden oracle (DAPPLE_ENGINE=off) renders whole pictures only
  const int W = s->lazy->width, H = s->lazy->height, own = std::max(W, H);
  const bool reduced = outLongEdge > 0 && outLongEdge < own;
  s->streamFilm = Renderer(paramsIn, *s->camera, nullptr).filmActive();
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
  const bool film = Renderer(paramsIn, *s->camera, nullptr).filmActive();
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
  Renderer renderer(params, *s->camera, lut.get());
  renderer.setAnalysisSources(a->p256.get(), a->grid.get(), a->p512.get());
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
    xstream::BandSourceStore store(bs);
    dapple::engine::EngineSession::GpuExport e{params, renderer, virt, store, *s->camera, lut.get()};
    e.outLongEdge = outLongEdge;
    e.forceStream = true;
    // tiles of 512: the device pool (shared memory on phones) stays small; the GPU executor is tile-invariant (same pixels at any tile size)
    e.tile = 512;
    if (const char* te = std::getenv("DAPPLE_EXPORT_STREAM_TILE")) e.tile = std::max(64, std::atoi(te) / 4 * 4);
    std::vector<std::uint8_t> band;
    int ow = 0, bandY = -1;
    bool aborted = false;
    e.onSize = [&](int w, int h) {
      ow = w;
      sized(w, h);
    };
    e.target.sink = [&](int x, int y, int tw, int th, const std::uint8_t* rgba, std::size_t stride) {
      if (y != bandY) {  // tiles arrive in raster order: a new band row
        bandY = y;
        band.assign(std::size_t(th) * ow * 3, 0);
      }
      for (int j = 0; j < th; ++j) {
        const std::uint8_t* src = rgba + std::size_t(j) * stride;
        std::uint8_t* dst = band.data() + (std::size_t(j) * ow + x) * 3;
        for (int i = 0; i < tw; ++i) dst[3 * i] = src[4 * i], dst[3 * i + 1] = src[4 * i + 1], dst[3 * i + 2] = src[4 * i + 2];
      }
      if (x + tw < ow) return true;
      bandY = -1;
      if (!progressSink(y, th, band.data())) return !(aborted = true);
      return true;
    };
    std::string err;
    const int r = s->exportEngine.exportGpu(e, nullptr, &err);
    stageLog("export_stream GPU %dx%d -> long edge %d: %s (%llu windows, demosaic %.0f ms %s%s)", W, H, int(outLongEdge), r == 0 ? "ok" : r > 0 ? "GPU declined" : err.c_str(),
             (unsigned long long)bs.windows(), bs.demosaicMs(), bs.gpu() ? "GPU" : "CPU", bs.fellBack() ? ", fell back" : "");
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
    if (!engineRenderExact(s, params, renderer, *R, img)) return -1;
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
  const bool ok = xstream::renderBands(s->exportEngine, params, renderer, virt, bs, tile, s->proxy != nullptr, [&](int w, int h) { fw = w, fh = h, sized(w, h); },
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

template <class Fn>
auto guarded(Fn&& fn, decltype(fn()) failure) -> decltype(fn()) {
  try {
    return fn();
  } catch (const std::exception& e) {
    setError(std::string("exception: ") + e.what());
  } catch (...) {
    setError("unknown exception");
  }
  return failure;
}

}  // namespace

// ---- merge / motion photo helpers (phase 3) ----
#include "dapple/merge.h"
#include "dapple/motion_photo.h"
#include "json_lite.h"

namespace {

MergeControl makeControl(DappleProgressFn progress, void* user, const volatile int32_t* cancelFlag) {
  MergeControl c;
  auto cancelled = std::make_shared<std::atomic<bool>>(false);
  if (progress)
    c.progress = [progress, user, cancelled](float f, const char* stage) {
      if (progress(f, stage, user) != 0) cancelled->store(true);
    };
  c.shouldCancel = [cancelFlag, cancelled]() { return cancelled->load() || (cancelFlag && *cancelFlag != 0); };
  return c;
}

std::vector<fs::path> toPaths(const char* const* paths, int32_t count) {
  std::vector<fs::path> out;
  for (int32_t i = 0; i < count; ++i) out.push_back(toPath(paths[i]));
  return out;
}

DecodeQuality qualityOf(const std::string& opts) {
  return jsonlite::text(opts, "quality", "full") == "preview" ? DecodeQuality::Preview : DecodeQuality::Full;
}

std::string hdrReportJson(const HdrReport& r) {
  std::string j = "{\"kind\":\"hdr\",\"base\":" + std::to_string(r.baseIndex) + ",\"fusion\":" + (r.exposureFusion ? "true" : "false");
  j += ",\"maxValue\":" + n(r.maxValue) + ",\"seconds\":{\"load\":" + n(r.secLoad) + ",\"align\":" + n(r.secAlign) +
       ",\"merge\":" + n(r.secMerge) + "},\"frames\":[";
  for (std::size_t i = 0; i < r.frames.size(); ++i) {
    const auto& f = r.frames[i];
    j += (i ? "," : "");
    j += "{\"path\":" + q(f.path) + ",\"evExif\":" + (std::isfinite(f.evExif) ? n(f.evExif) : "null") + ",\"evUsed\":" + n(f.evUsed) +
         ",\"evFromData\":" + (f.evFromData ? "true" : "false") + ",\"aligned\":" + (f.aligned ? "true" : "false") +
         ",\"alignMethod\":" + n(f.alignMethod) + ",\"alignScore\":" + n(f.alignScore) + ",\"shiftPx\":" + n(f.shiftPx) +
         ",\"ghostFraction\":" + n(f.ghostFraction) + "}";
  }
  j += "],\"warnings\":[";
  for (std::size_t i = 0; i < r.warnings.size(); ++i) j += (i ? "," : "") + q(r.warnings[i]);
  return j + "]}";
}

std::string panoReportJson(const PanoReport& r) {
  std::string j = "{\"kind\":\"pano\",\"projection\":" + q(toString(r.projection)) + ",\"framesUsed\":" + std::to_string(r.framesUsed) + ",\"used\":[";
  for (std::size_t i = 0; i < r.usedIndices.size(); ++i) j += (i ? "," : "") + std::to_string(r.usedIndices[i]);
  j += "],\"canvas\":[" + std::to_string(r.canvasWidth) + "," + std::to_string(r.canvasHeight) + "],\"crop\":[" + std::to_string(r.cropX) +
       "," + std::to_string(r.cropY) + "],\"validFraction\":" + n(r.validFraction) + ",\"seconds\":{\"load\":" + n(r.secLoad) +
       ",\"features\":" + n(r.secFeatures) + ",\"estimate\":" + n(r.secEstimate) + ",\"compose\":" + n(r.secCompose) + "},\"frames\":[";
  for (std::size_t i = 0; i < r.frames.size(); ++i) {
    const auto& f = r.frames[i];
    j += (i ? "," : "");
    j += "{\"path\":" + q(f.path) + ",\"focalPx\":" + n(f.focalPx) + ",\"yaw\":" + n(f.yawDeg) + ",\"pitch\":" + n(f.pitchDeg) +
         ",\"roll\":" + n(f.rollDeg) + ",\"inliers\":" + std::to_string(f.inliers) + "}";
  }
  j += "],\"warnings\":[";
  for (std::size_t i = 0; i < r.warnings.size(); ++i) j += (i ? "," : "") + q(r.warnings[i]);
  return j + "]}";
}

// Opens the object; the caller appends further members and the closing brace.
std::string motionInfoJson(const MotionPhotoInfo& i) {
  std::string j = std::string("{\"valid\":") + (i.valid ? "true" : "false") + ",\"fileSize\":" + std::to_string(i.fileSize) +
                  ",\"jpegLength\":" + std::to_string(i.jpegLength) + ",\"videoOffset\":" + std::to_string(i.videoOffset) +
                  ",\"videoLength\":" + std::to_string(i.videoLength) + ",\"presentationTimestampUs\":" + std::to_string(i.presentationTimestampUs) +
                  ",\"videoMime\":" + q(i.videoMime) + ",\"duration\":" + n(i.videoDurationSec) + ",\"width\":" + std::to_string(i.videoWidth) +
                  ",\"height\":" + std::to_string(i.videoHeight) + ",\"videoCodec\":" + q(i.videoCodec) + ",\"audioCodec\":" + q(i.audioCodec) +
                  ",\"gainMap\":" + (i.hasGainMap ? "true" : "false") + ",\"gainMapOffset\":" + std::to_string(i.gainMapOffset) +
                  ",\"gainMapLength\":" + std::to_string(i.gainMapLength) + ",\"problems\":[";
  for (std::size_t k = 0; k < i.problems.size(); ++k) j += (k ? "," : "") + q(i.problems[k]);
  j += "],\"notes\":[";
  for (std::size_t k = 0; k < i.notes.size(); ++k) j += (k ? "," : "") + q(i.notes[k]);
  return j + "]";
}

}  // namespace

extern "C" {

const char* dapple_version(void) { return DAPPLE_VERSION_STRING; }

const char* dapple_last_error(void) { return g_lastError.c_str(); }

void dapple_free_string(char* s) { std::free(s); }

// ---- optional GPU backend (core/gpu): runtime flag + status ----
int32_t dapple_gpu_set_enabled(int32_t mode) {
#ifdef DAPPLE_HAVE_GPU
  dapple::engine::gpu::installProvider();
  dapple::engine::gpu::setRuntimeEnabled(mode);
  return dapple::engine::gpu::runtimeEnabled() ? 1 : 0;
#else
  (void)mode;
  return 0;
#endif
}

void dapple_gpu_configure(const char* cache_dir, const char* app_version, const char* fingerprint) {
#ifdef DAPPLE_HAVE_GPU
  dapple::engine::gpu::configure(cache_dir ? cache_dir : "", app_version ? app_version : "", fingerprint ? fingerprint : "");
#else
  // CPU-only build: the same cache directory still holds the film tables and the print-exposure constants (the prebake persists them there)
  if (cache_dir && cache_dir[0])
    dapple::engine::filmnode::setFilmTableDisk((fs::path(cache_dir) / "film_tables").string(), std::string(app_version ? app_version : "") + "|" + (fingerprint ? fingerprint : ""));
#endif
}

void dapple_gpu_warmup(void) {
#ifdef DAPPLE_HAVE_GPU
  dapple::engine::gpu::startWarmup();
#endif
}

char* dapple_engine_stats_json(void) { return dupString(dapple::engine::executorStatsJson()); }

char* dapple_gpu_status_json(void) {
#ifdef DAPPLE_HAVE_GPU
  dapple::engine::gpu::installProvider();
  return dupString(dapple::engine::gpu::statusJson());
#else
  return dupString("{\"compiled\":false,\"enabled\":false,\"available\":false}");
#endif
}

// ---- local adjustments (docs/engine/pipeline-architecture.md section 40) ----------------------------------------------------------------
int32_t dapple_mask_raster_set(const char* id, int32_t w, int32_t h, const uint8_t* bytes, int32_t stride) {
  return guarded(
      [&]() -> int32_t {
        if (!id || !bytes || !local::rasterSet(id, w, h, bytes, stride)) {
          setError("mask raster: bad arguments (id, 1..8192 px, bytes)");
          return DAPPLE_ERROR;
        }
        return DAPPLE_OK;
      },
      DAPPLE_ERROR);
}
int32_t dapple_mask_raster_remove(const char* id) { return id && local::rasterRemove(id) ? DAPPLE_OK : DAPPLE_ERROR; }
int32_t dapple_mask_raster_has(const char* id) { return id && local::rasterHas(id) ? 1 : 0; }

namespace {
// The source of a mask query at long_edge (the session's scaled copy, kept by the Resize cache) and the edit's kernels on it (exact path:
// the same geometry, gain and HDR as a frame).
struct MaskView {
  std::shared_ptr<const SourceImage> hold;
  const SourceImage* src = nullptr;
  std::unique_ptr<Renderer> renderer;
  std::unique_ptr<FrameKernels> fk;
};
bool maskView(DappleSession* s, const EditParams& params, int longEdge, MaskView& mv) {
  if (!ensureSource(s)) return false;
  if (longEdge <= 0) longEdge = 1024;
  mv.hold = s->engine.resizedSource(s->source, longEdge, false);
  mv.src = mv.hold ? mv.hold.get() : &s->source;
  if (mv.src->linear.empty()) return false;
  mv.renderer = std::make_unique<Renderer>(params, *s->camera, nullptr);
  FastOptions fo;
  fo.spatial = SpatialQuality::Off;
  mv.fk = std::make_unique<FrameKernels>(*mv.renderer, *mv.src, nullptr, fo);
  return mv.fk->valid();
}
}  // namespace

int32_t dapple_session_mask_preview(DappleSession* s, const char* params_json, int32_t index, int32_t long_edge, int32_t flags, DappleImage* out) {
  return guarded(
      [&]() -> int32_t {
        if (!s || !out) {
          setError("null argument");
          return DAPPLE_ERROR;
        }
        *out = DappleImage{0, 0, nullptr};
        EditParams params;
        if (!parseParams(params_json, params)) return DAPPLE_ERROR;
        if (index < 0 || index >= int(params.local.size())) {
          setError("mask preview: no local adjustment at that index");
          return DAPPLE_ERROR;
        }
        if (flags & 1) {  // the photo in its own orientation
          params.crop = "orig";
          params.rotate = 0;
          params.straighten = 0.f;
          params.flip = false;
        }
        std::lock_guard<std::mutex> lk(s->mtx);
        MaskView mv;
        if (!maskView(s, params, long_edge, mv)) {
          setError("mask preview: no source");
          return DAPPLE_ERROR;
        }
        const std::shared_ptr<const local::Frame> f = local::bind(params.local, *mv.src, false, true);
        const int w = mv.fk->width(), h = mv.fk->height();
        auto* px = static_cast<std::uint8_t*>(std::malloc(std::size_t(w) * h * 4));
        if (!px) {
          setError("out of memory");
          return DAPPLE_ERROR;
        }
        parallelFor(h, [&](int y) {
          std::vector<std::uint8_t> row(std::size_t(w), 0);
          if (f) local::maskRow(*f, *mv.fk, index, y, row.data());
          std::uint8_t* o = px + std::size_t(y) * w * 4;
          for (int x = 0; x < w; ++x) o[4 * x] = o[4 * x + 1] = o[4 * x + 2] = row[std::size_t(x)], o[4 * x + 3] = 255;
        });
        out->width = w;
        out->height = h;
        out->rgba = px;
        return DAPPLE_OK;
      },
      DAPPLE_ERROR);
}

int32_t dapple_session_map_point(DappleSession* s, const char* params_json, int32_t long_edge, float x, float y, int32_t direction, float* u, float* v) {
  ensureSource(s);
  return guarded(
      [&]() -> int32_t {
        if (!s || !u || !v) {
          setError("null argument");
          return DAPPLE_ERROR;
        }
        EditParams params;
        if (!parseParams(params_json, params)) return DAPPLE_ERROR;
        params.local.clear();
        std::lock_guard<std::mutex> lk(s->mtx);
        MaskView mv;
        if (!maskView(s, params, long_edge, mv)) {
          setError("map point: no source");
          return DAPPLE_ERROR;
        }
        // the frame -> photo mapping is affine (rotate 90, straighten, flip, crop): from three probes of srcUV at pixel centres
        float u0, v0, ux, vx, uy, vy;
        mv.fk->srcUV(0, 0, u0, v0);
        mv.fk->srcUV(1, 0, ux, vx);
        mv.fk->srcUV(0, 1, uy, vy);
        const double a = ux - u0, b = uy - u0, c = vx - v0, d = vy - v0;  // (u, v) = (u0, v0) + [a b; c d] (px - 0.5, py - 0.5)
        if (direction == 0) {
          *u = float(u0 + a * (x - 0.5) + b * (y - 0.5));
          *v = float(v0 + c * (x - 0.5) + d * (y - 0.5));
        } else {
          const double det = a * d - b * c;
          if (std::fabs(det) < 1e-20) {
            setError("map point: degenerate geometry");
            return DAPPLE_ERROR;
          }
          const double du = x - u0, dv = y - v0;
          *u = float((d * du - b * dv) / det + 0.5);
          *v = float((-c * du + a * dv) / det + 0.5);
        }
        return DAPPLE_OK;
      },
      DAPPLE_ERROR);
}

char* dapple_session_mask_sample_json(DappleSession* s, const char* params_json, float u, float v) {
  ensureSource(s);
  return guarded(
      [&]() -> char* {
        if (!s) {
          setError("null session");
          return nullptr;
        }
        EditParams params;
        if (!parseParams(params_json, params)) return nullptr;
        params.crop = "orig";
        params.rotate = 0;
        params.straighten = 0.f;
        params.flip = false;
        params.local.clear();
        std::lock_guard<std::mutex> lk(s->mtx);
        MaskView mv;
        if (!maskView(s, params, 1024, mv)) {
          setError("mask sample: no source");
          return nullptr;
        }
        const int x = std::clamp(int(u * float(mv.fk->width())), 0, mv.fk->width() - 1), y = std::clamp(int(v * float(mv.fk->height())), 0, mv.fk->height() - 1);
        float px[3];
        mv.fk->sourceRow(px, 1, x, y);
        mv.fk->gain(px, 1);
        mv.fk->hdrBlock(px, 1, x, y);
        float L = 0, hue = 0, sat = 0;
        local::sampleRange(px, L, hue, sat);
        char buf[160];
        std::snprintf(buf, sizeof buf, "{\"L\":%.2f,\"hue\":%.1f,\"sat\":%.2f}", L, hue, std::min(sat / 0.25f * 100.f, 400.f));
        return dupString(buf);
      },
      static_cast<char*>(nullptr));
}

// ---- AI masks (docs/engine/pipeline-architecture.md section 41) -------------------------------------------------------------------------------
extern "C++" {
namespace {
// What the AI service needs of a session: the photo's identity (hash of the decoded scene-linear picture) and its neutral rendition: default
// params (white balance as shot, exposure anchor, standard base, no look, no local adjustments), geometry "orig", sRGB, <= 1024 px. It never depends
// on the user's edit, so a plane is valid for every edit of the photo.
aimask::Source aiSource(DappleSession* s) {
  aimask::Source src;
  src.owner = s;
  src.photoId = aimask::photoIdOf(s->source.linear.data.data(), s->source.linear.width, s->source.linear.height);
  src.render = [s](aimask::Rendition& r, std::string& err) {
    EditParams p;
    p.crop = "orig", p.rotate = 0, p.straighten = 0.f, p.flip = false, p.outputSpace = 0;
    p.local.clear();
    Image8 img;
    if (!renderSessionParams(s, p, nullptr, aimask::kPlaneLongEdge, img) || img.data.empty()) {
      err = g_lastError.empty() ? "render failed" : g_lastError;
      return false;
    }
    r.w = img.width, r.h = img.height, r.rgb = std::move(img.data);
    return true;
  };
  return src;
}
std::vector<std::string> localRasterIds(const EditParams& p) {
  std::vector<std::string> ids;
  for (const LocalAdjustment& a : p.local) {
    if (!a.enabled || a.strength <= 0.f || !(a.ops.anyLinear() || a.ops.anyWindow())) continue;
    for (const MaskComponent& m : a.mask)
      if (m.kind == MaskKind::Raster && !m.ref.empty() && std::find(ids.begin(), ids.end(), m.ref) == ids.end()) ids.push_back(m.ref);
  }
  return ids;
}
std::vector<std::string> missingRasterIds(const EditParams& p) {
  std::vector<std::string> out;
  for (const std::string& id : localRasterIds(p))
    if (!local::rasterHas(id)) out.push_back(id);
  return out;
}
// Export rule: every raster plane the edit uses must exist, else it is re-created (disk cache, then recomputation); if that fails the export fails.
bool ensureLocalRasters(DappleSession* s, const EditParams& p) {
  const std::vector<std::string> missing = missingRasterIds(p);
  if (missing.empty()) return true;
  aimask::Control ctl;
  ctl.yield = false;
  std::string why;
  const std::vector<std::string> still = aimask::ensure(aiSource(s), missing, ctl, &why);
  if (still.empty()) return true;
  setError("export: local mask plane '" + still[0] + "' is missing and could not be re-created (" + why + "); nothing was written");
  return false;
}
std::string jsonEsc(const std::string& in) {
  std::string o;
  for (char c : in) {
    if (c == '"' || c == '\\') o += '\\', o += c;
    else if (static_cast<unsigned char>(c) < 0x20) o += ' ';
    else o += c;
  }
  return o;
}
}  // namespace
}  // extern "C++"

int32_t dapple_ai_configure(const char* model_dir, const char* cache_dir) {
  aimask::configure(model_dir ? model_dir : "", cache_dir ? cache_dir : "");
  return DAPPLE_OK;
}
char* dapple_ai_status_json(void) { return dupString(aimask::availabilityJson()); }
char* dapple_session_ai_photo_id(DappleSession* s) {
  if (!s) return nullptr;
  return dupString(aiSource(s).photoId);
}
int32_t dapple_session_ai_start(DappleSession* s, int32_t tasks, int32_t flags, int32_t max_faces) {
  return guarded(
      [&]() -> int32_t {
        if (!s) {
          setError("null session");
          return -1;
        }
        const aimask::rt::Avail a = aimask::rt::availability(aimask::modelDir());
        if (!(a.person || a.faces || a.depth || a.tap || aimask::rt::compiled())) {
          setError("AI masks unavailable: " + a.reason);
          return -1;
        }
        return aimask::startJob(aiSource(s), tasks & 31, max_faces > 0 ? max_faces : aimask::kMaxFaces, (flags & 1) != 0);
      },
      -1);
}
char* dapple_ai_job_json(int32_t job) { return dupString(aimask::jobJson(job)); }
void dapple_ai_job_cancel(int32_t job) { aimask::cancelJob(job); }

char* dapple_session_ai_tap_json(DappleSession* s, float u, float v, int32_t op) {
  return guarded(
      [&]() -> char* {
        if (!s) {
          setError("null session");
          return nullptr;
        }
        aimask::Control ctl;
        const aimask::TapResult r = aimask::tap(aiSource(s), u, v, op, ctl);
        char b[96];
        std::string j = std::string("{\"ok\":") + (r.ok ? "true" : "false") + ",\"message\":\"" + jsonEsc(r.message) + "\",\"id\":\"" + jsonEsc(r.id) + "\",\"w\":" +
                        std::to_string(r.w) + ",\"h\":" + std::to_string(r.h) + ",\"taps\":[";
        for (std::size_t i = 0; i < r.taps.size(); ++i) {
          std::snprintf(b, sizeof b, "%s[%.4f,%.4f,%d]", i ? "," : "", r.taps[i].x, r.taps[i].y, r.taps[i].add ? 1 : 0);
          j += b;
        }
        std::snprintf(b, sizeof b, "],\"ms\":%.1f,\"prepare_ms\":%.1f}", r.ms, r.prepareMs);
        return dupString(j + b);
      },
      nullptr);
}

int32_t dapple_ai_plane_preview(const char* id, int32_t long_edge, DappleImage* out) {
  return guarded(
      [&]() -> int32_t {
        if (!id || !out) {
          setError("null argument");
          return DAPPLE_ERROR;
        }
        *out = DappleImage{0, 0, nullptr};
        const std::shared_ptr<const local::Raster> r = local::rasterGet(id);
        if (!r) {
          setError("plane preview: id not in the raster store");
          return DAPPLE_ERROR;
        }
        const int L = long_edge > 0 ? long_edge : 128, m = std::max(r->w, r->h);
        const int w = std::max(1, int(std::lround(double(r->w) * L / m))), h = std::max(1, int(std::lround(double(r->h) * L / m)));
        const aimask::Plane p = aimask::resizePlane(r->px.data(), r->w, r->h, w, h);
        auto* px = static_cast<std::uint8_t*>(std::malloc(std::size_t(w) * h * 4));
        if (!px) {
          setError("out of memory");
          return DAPPLE_ERROR;
        }
        for (std::size_t i = 0; i < std::size_t(w) * h; ++i) px[4 * i] = px[4 * i + 1] = px[4 * i + 2] = p.px[i], px[4 * i + 3] = 255;
        out->width = w, out->height = h, out->rgba = px;
        return DAPPLE_OK;
      },
      DAPPLE_ERROR);
}

char* dapple_session_ai_missing_json(DappleSession* s, const char* params_json) {
  return guarded(
      [&]() -> char* {
        if (!s) {
          setError("null session");
          return nullptr;
        }
        EditParams params;
        if (!parseParams(params_json, params)) return nullptr;
        const std::string photo = aimask::photoIdOf(s->source.linear.data.data(), s->source.linear.width, s->source.linear.height);
        std::string j = "{\"missing\":[";
        bool first = true;
        for (const std::string& id : missingRasterIds(params)) {
          const aimask::ParsedId p = aimask::parseId(id);
          j += std::string(first ? "" : ",") + "{\"id\":\"" + jsonEsc(id) + "\",\"ai\":" + (p.ok ? "true" : "false") + ",\"photo\":" + (p.ok && p.photo == photo ? "true" : "false") + "}";
          first = false;
        }
        return dupString(j + "]}");
      },
      nullptr);
}

int32_t dapple_session_ai_ensure(DappleSession* s, const char* params_json, int32_t flags) {
  return guarded(
      [&]() -> int32_t {
        if (!s) {
          setError("null session");
          return -1;
        }
        EditParams params;
        if (!parseParams(params_json, params)) return -1;
        const std::vector<std::string> missing = missingRasterIds(params);
        if (missing.empty()) return 0;
        if (!(flags & 1)) {
          aimask::Control ctl;
          ctl.yield = false;
          std::string why;
          const auto still = aimask::ensure(aiSource(s), missing, ctl, &why);
          if (!still.empty()) setError(why);
          return int32_t(still.size());
        }
        int tasks = 0;
        std::vector<std::string> taps;
        for (const std::string& id : missing) {
          const aimask::ParsedId p = aimask::parseId(id);
          if (!p.ok) continue;
          if (p.group == "person") tasks |= aimask::kTaskPerson;
          else if (p.group == "faces") tasks |= aimask::kTaskFaces;
          else if (p.group == "depth") tasks |= aimask::kTaskDepth;
          else if (p.group == "sky") tasks |= aimask::kTaskSky;
          else taps.push_back(id);
        }
        if (!taps.empty()) {  // tap planes: the disk cache is read, else the taps are replayed (encoder + decoder, a second or two)
          aimask::Control ctl;
          ctl.yield = false;
          std::string why;
          aimask::ensure(aiSource(s), taps, ctl, &why);
        }
        return tasks ? aimask::startJob(aiSource(s), tasks, aimask::kMaxFaces, false) : 0;
      },
      -1);
}
void dapple_ai_release(int32_t all) {
  if (all) aimask::releaseAll();
  else aimask::releaseIdle();
}

void dapple_free_image(DappleImage* image) {
  if (!image) return;
  std::free(image->rgba);
  image->rgba = nullptr;
  image->width = 0;
  image->height = 0;
}

int32_t dapple_dng_embed_thumbnail(const char* dng_path, const char* jpeg_path) {
  return guarded(
      [&]() -> int32_t {
        if (!dng_path || !jpeg_path) {
          setError("null path");
          return DAPPLE_ERROR;
        }
        std::string err;
        if (!embedDngThumbnailFile(toPath(dng_path), toPath(jpeg_path), &err)) {
          setError(err);
          return DAPPLE_ERROR;
        }
        return DAPPLE_OK;
      },
      DAPPLE_ERROR);
}

int32_t dapple_extract_preview(const char* path, int32_t long_edge, DappleImage* out) {
  return guarded(
      [&]() -> int32_t {
        if (!out) {
          setError("null output");
          return DAPPLE_ERROR;
        }
        *out = DappleImage{0, 0, nullptr};
        auto r = extractEmbeddedPreview(toPath(path), long_edge);
        if (!r) {
          setError(r.error().message);
          return DAPPLE_ERROR;
        }
        return fillImage(r.value(), out) ? DAPPLE_OK : DAPPLE_ERROR;
      },
      DAPPLE_ERROR);
}

char* dapple_probe_json(const char* path) {
  return guarded(
      [&]() -> char* {
        const fs::path p = toPath(path);
        const ImageDecoder* decoder = decoders().find(p);
        if (!decoder) {
          setError("Unsupported file type: " + pathToUtf8(p));
          return nullptr;
        }
        auto meta = decoder->probe(p);
        if (!meta) {
          setError(meta.error().message);
          return nullptr;
        }
        return dupString(metaJson(*meta, cameras().resolve(meta->make, meta->model)));
      },
      nullptr);
}

char* dapple_xmp_to_json(const char* xmp_path) {
  return guarded(
      [&]() -> char* {
        auto preset = loadXmpPreset(toPath(xmp_path));
        if (!preset) {
          setError(preset.error().message);
          return nullptr;
        }
        std::string j = "{\"name\":" + q(preset->name) + ",\"params\":" + toJson(preset->params) + ",\"unsupported\":[";
        for (std::size_t i = 0; i < preset->unsupported.size(); ++i) j += (i ? "," : "") + q(preset->unsupported[i]);
        j += "]}";
        return dupString(j);
      },
      nullptr);
}

int32_t dapple_is_supported_file(const char* path) {
  return guarded([&]() -> int32_t { return decoders().find(toPath(path)) != nullptr ? 1 : 0; }, 0);
}

// One-line stage log (field diagnostics): tag dapple_engine on Android, os_log on Apple, stderr elsewhere.
static void stageLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void stageLog(const char* fmt, ...) {
  char buf[640];  // the export lines carry the memory figures at the end: keep them whole
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
#if defined(__ANDROID__)
  __android_log_write(ANDROID_LOG_INFO, "dapple_engine", buf);
#elif defined(__APPLE__)
  os_log(OS_LOG_DEFAULT, "dapple_engine: %{public}s", buf);
#else
  std::fprintf(stderr, "dapple_engine: %s\n", buf);
#endif
}
static double msSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

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
  dapple_ai_release(0);
  const std::size_t foot1 = mem::footprint();
  std::string policy;
  const bool lazy = xstream::openLazy(z->width, z->height, &policy);
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

char* dapple_session_info_json(DappleSession* session) {
  return guarded(
      [&]() -> char* {
        if (!session) {
          setError("null session");
          return nullptr;
        }
        return dupString(metaJson(session->source.meta, *session->camera));
      },
      nullptr);
}

char* dapple_session_auto_json(DappleSession* session, float strength) {
  ensureSource(session);
  return guarded(
      [&]() -> char* {
        if (!session) {
          setError("null session");
          return nullptr;
        }
        AutoOptions opt;
        opt.strength = strength;
        const AutoResult r = computeAutoAdjust(session->source, *session->camera, opt);
        const ImageStats& st = r.stats;
        std::string j = "{\"correction\":" + toJson(r.correction) + ",\"stats\":{";
        j += "\"medianLuma\":" + n(st.medianLuma) + ",\"p01Luma\":" + n(st.p01Luma) + ",\"p99Luma\":" + n(st.p99Luma);
        j += ",\"clippedFraction\":" + n(st.clippedFraction) + ",\"neutralCastA\":" + n(st.neutralCastA);
        j += ",\"neutralCastB\":" + n(st.neutralCastB) + ",\"neutralCoverage\":" + n(st.neutralCoverage);
        j += "},\"notes\":[";
        for (std::size_t i = 0; i < r.notes.size(); ++i) j += (i ? "," : "") + q(r.notes[i]);
        j += "]}";
        return dupString(j);
      },
      nullptr);
}

static std::string toneJson(const ToneAnalysis& a) {
  std::string j = "{\"histogram\":[";
  for (std::size_t i = 0; i < a.histogram.size(); ++i) j += (i ? "," : "") + n(a.histogram[i]);
  j += "],\"p5\":" + n(a.p5) + ",\"p50\":" + n(a.p50) + ",\"p95\":" + n(a.p95);
  j += ",\"nearest\":" + q(a.nearest) + ",\"key\":" + q(a.key) + ",\"range\":" + q(a.range);
  j += ",\"distance\":" + n(a.distance) + ",\"margin\":" + n(a.margin) + "}";
  return j;
}


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
  Renderer renderer(params, *s->camera, lut.get());
  renderer.setToneProxy(s->proxy.get());
  const double rendererMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tRen0).count();
  // Grain (14 Q8): a drag of a grain-family slider keeps grain in interactive frames; a frame smaller than the full
  // render previews it with grain statistically matched to the downscaled full-resolution grain.
  const std::string activeParam = rq.active_param ? rq.active_param : "";
  const bool keepGrain = interactive && (activeParam == "grain" || activeParam == "film_grain");
  const int fullLongEdge = (!hasRegion && srcLong < own && !std::getenv("DAPPLE_GRAIN_SCALE_OFF")) ? own : 0;  // diagnostics switch

  if (rq.flags & DAPPLE_FRAME_EXACT) {
    // reference pipeline (no region)
    Image8 img;
    if (dapple::engine::mode() == dapple::engine::Mode::Full) {
      if (!engineRenderExact(s, params, renderer, src, img)) return false;
    } else {
      img = renderer.render(src);
    }
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
  if (dapple::engine::engineInUse()) {
    // Frame engine: graph + table nodes + ResultCache replace the bakes[3] lookup (stage a2/a3). Same inputs, same outputs.
    dapple::engine::FrameJob job{params, renderer, src, s->proxy.get(), *s->camera, lut.get(), {}, {}, {}};
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
      if (renderer.filmActive() && res.rgba) applyFrameHistogram(a, res.rgba, res.w, res.h);
      *analysisOut = dupString(toneJson(a));
    }
    return true;
  }
  const bool fastAn = interactive && !std::getenv("DAPPLE_EXACT_ANALYSIS");
  const std::string pk = renderer.pointKey() + (fastAn ? "#f" : "#x");
  std::shared_ptr<const BakedLut> bake;
  for (auto& b : s->bakes)  // same parameters, at least as fine a lattice as asked for
    if (b->n >= std::clamp(n, 17, 65) && b->key.size() > pk.size() && b->key.compare(0, pk.size(), pk) == 0 &&
        (!bake || b->n > bake->n))
      bake = b;
  if (bake) {  // the finest lattice of this colour stage wins (as in the engine's table path)
    const std::string ck0 = renderer.colourKey();
    for (auto& b : s->bakes)
      if (b->colourKey == ck0 && b->n >= std::clamp(n, 17, 65) && b->n > bake->n) bake = nullptr;
  }
  if (!bake) {
    // A lattice with the same colour stage (only tone / white balance moved: every 光线 drag) is reused, the
    // largest one available (the final frame's 65^3 is the most accurate): just the 1D tone table is rebuilt.
    const std::string ck = renderer.colourKey();
    const BakedLut* from = nullptr;
    for (auto& b : s->bakes)
      if (b->colourKey == ck && b->n >= std::clamp(n, 17, 65) && (!from || b->n > from->n)) from = b.get();
    bake = renderer.bake(src, n, from, fastAn);
    s->bakes.push_back(bake);
    if (s->bakes.size() > 3) s->bakes.erase(s->bakes.begin());
    s->lutKeep = lut;
  }

  FastOptions fo;
  fo.spatial = interactive ? SpatialQuality::Interactive : SpatialQuality::Full;
  fo.noise = !interactive || !s->haveLast || !nearEq(s->lastParams.nrLuma, params.nrLuma) ||
             !nearEq(s->lastParams.nrColor, params.nrColor);
  fo.region = region;
  fo.keepGrain = keepGrain;
  fo.fullLongEdge = fullLongEdge;
  s->lastParams = params;
  s->haveLast = true;

  int w = 0, h = 0;
  std::uint8_t* rgba = nullptr;
  if (!rgba) rgba = renderer.renderBakedRgba(src, *bake, fo, w, h);
  if (!rgba) {
    setError("out of memory");
    return false;
  }
  out->width = w;
  out->height = h;
  out->rgba = rgba;
  if (analysisOut && (rq.flags & DAPPLE_FRAME_ANALYSIS)) {
    ToneAnalysis a = bake->analysis;  // the lattice (and its class analysis) is shared across film exposure / warmth...
    if (renderer.filmActive()) applyFrameHistogram(a, rgba, w, h);  // ...the histogram of a film edit is that of the frame
    *analysisOut = dupString(toneJson(a));
  }
  return true;
}


char* dapple_session_analyze_json(DappleSession* session) { return dapple_session_analyze_params_json(session, nullptr); }

char* dapple_session_analyze_params_json(DappleSession* session, const char* params_json) {
  ensureSource(session);
  return guarded(
      [&]() -> char* {
        if (!session) {
          setError("null session");
          return nullptr;
        }
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
int32_t dapple_session_touch_hint(DappleSession* session, int32_t burst_ms) {
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
          // low priority work (detail session...): this thread, efficiency cores, never the shared pool
          sys::setBackground(true);
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

char* dapple_engine_info_json(void) { return dupString(dapple::engine::infoJson()); }

int32_t dapple_session_cancel(DappleSession* session) {
  if (!session) return DAPPLE_ERROR;
  session->engine.cancel();  // lock-free: the render in flight (any thread) polls the ticket
  session->exportEngine.cancel();
  return DAPPLE_OK;
}

void dapple_thread_background(int32_t on) { sys::setBackground(on != 0); }

void dapple_frame_trace_enable(int32_t on) {
  dapple::ftrace::S().on.store(on != 0);
  if (on) dapple::ftrace::clear();
}
int32_t dapple_frame_trace_dump(const char* path) { return path && dapple::ftrace::dump(path) ? DAPPLE_OK : DAPPLE_ERROR; }
void dapple_frame_trace_clear(void) { dapple::ftrace::clear(); }

void dapple_set_interaction(int32_t active) {
  dapple::demosaic::setInteraction(active != 0);
  sys::setInteraction(active != 0);
}

void dapple_perf_configure(int32_t thread_limit, int32_t target_frame_ms) {
  ThreadPool::instance().setLimit(thread_limit);
  sys::adpf().setTarget(int64_t(std::max(0, target_frame_ms)) * 1000000);
}

float dapple_thermal_headroom(int32_t forecast_seconds) { return sys::adpf().headroom(forecast_seconds); }

int32_t dapple_set_repaint_callback(DappleRepaintFn fn, void* user) {
  static DappleRepaintFn cur = nullptr;
  static void* curUser = nullptr;
  using namespace dapple::engine::filmnode;
  if (cur) removeTableLandedListener(reinterpret_cast<TableLandedFn>(cur), curUser);
  cur = nullptr;
  curUser = nullptr;
  if (!fn) return DAPPLE_OK;
  cur = fn;
  curUser = user;
  addTableLandedListener(reinterpret_cast<TableLandedFn>(fn), user);
  return DAPPLE_OK;
}

char* dapple_perf_info_json(void) {
  const sys::Topology& t = sys::topology();
  ThreadPool& pool = ThreadPool::instance();
  std::string j = "{\"cpus\":" + std::to_string(t.total) + ",\"perf\":" + std::to_string(t.perf.size()) +
                  ",\"efficient\":" + std::to_string(t.efficient.size()) + ",\"pool\":" + std::to_string(pool.size()) +
                  ",\"adpf\":" + (sys::adpf().hintAvailable() ? "true" : "false") +
                  ",\"adpf_session\":" + (sys::adpf().sessionActive() ? "true" : "false") +
                  ",\"thermal_status\":" + std::to_string(sys::adpf().status()) + "}";
  return dupString(j);
}

int32_t dapple_film_load_pack(const char* path) {
  return guarded(
      [&]() -> int32_t {
        std::string err;
        if (!path || !film::setLibraryPath(toPath(path), &err)) {
          setError("film pack: " + (err.empty() ? std::string("bad path") : err));
          return DAPPLE_ERROR;
        }
        return DAPPLE_OK;
      },
      DAPPLE_ERROR);
}

int32_t dapple_film_load_pack_memory(const void* data, size_t size) {
  return guarded(
      [&]() -> int32_t {
        std::string err;
        auto lib = data ? film::FilmLibrary::loadPackMemory(data, size, &err) : nullptr;
        if (!lib) {
          setError("film pack: " + (err.empty() ? std::string("no data") : err));
          return DAPPLE_ERROR;
        }
        film::setLibrary(std::move(lib));
        return DAPPLE_OK;
      },
      DAPPLE_ERROR);
}

void dapple_film_set_pack_path(const char* path) { film::setLibraryPathLazy(toPath(path)); }

void dapple_film_prebake_start(void) { dapple::engine::filmnode::startFilmPrebake(); }
char* dapple_film_prebake_status(void) {
  char b[256];
  dapple::engine::filmnode::filmPrebakeStatus(b, int(sizeof b));
  return dupString(b);
}

char* dapple_film_list_json(void) {
  return guarded(
      [&]() -> char* {
        const auto lib = film::defaultLibrary();
        std::string j = std::string("{\"loaded\":") + (lib ? "true" : "false") + ",\"stocks\":[";
        if (lib) {
          bool first = true;
          for (const film::StockInfo& s : film::listStocks(*lib)) {
            j += std::string(first ? "" : ",") + "{\"id\":\"" + esc(s.id) + "\",\"name\":\"" + esc(s.name) +
                 "\",\"type\":\"" + s.kind + "\"" + (s.paper.empty() ? std::string() : ",\"paper\":\"" + esc(s.paper) + "\"") + "}";
            first = false;
          }
        }
        return dupString(j + "]}");
      },
      nullptr);
}

char* dapple_film_attribution(void) {
  return guarded(
      [&]() -> char* {
        const auto lib = film::defaultLibrary();
        return dupString(lib ? lib->attribution() : std::string());
      },
      nullptr);
}

// Fixed test patches (linear sRGB scene reflectances: 18 % grey, skin, sky) rendered through every camera stock with the
// default paper; the stock's swatch in the UI. The result is a pure function of the pack: callers cache it.
char* dapple_film_swatches_json(void) {
  return guarded(
      [&]() -> char* {
        const auto lib = film::defaultLibrary();
        std::string j = "{";
        if (lib) {
          static const float kPatch[3][3] = {{0.18f, 0.18f, 0.18f}, {0.50f, 0.30f, 0.22f}, {0.15f, 0.28f, 0.55f}};
          constexpr int kCell = 16;  // pixels per patch edge: the centre pixel is read, away from any edge effect
          SourceImage src;
          src.sceneReferred = true;
          src.linear = ImageF(3 * kCell, kCell);
          for (int y = 0; y < kCell; ++y)
            for (int x = 0; x < 3 * kCell; ++x) color::srgbToProPhoto().apply(kPatch[x / kCell], src.linear.row(y) + 3 * x);
          const CameraProfile& cam = cameras().resolve("", "");
          bool first = true;
          for (const film::StockInfo& si : film::listStocks(*lib)) {
            if (si.kind == "paper") continue;
            EditParams p;
            p.filmStock = si.id;
            p.filmStrength = 100.f;
            ImageF out;
            if (!dapple::engine::renderImageF(p, cam, nullptr, src, false, out) || out.width != 3 * kCell) continue;
            j += std::string(first ? "" : ",") + "\"" + esc(si.id) + "\":[";
            for (int k = 0; k < 3; ++k) {
              const float* px = out.row(kCell / 2) + 3 * (k * kCell + kCell / 2);
              char hex[16];
              std::snprintf(hex, sizeof hex, "%s\"%02x%02x%02x\"", k ? "," : "",
                            int(std::lround(std::clamp(px[0], 0.f, 1.f) * 255.f)),
                            int(std::lround(std::clamp(px[1], 0.f, 1.f) * 255.f)),
                            int(std::lround(std::clamp(px[2], 0.f, 1.f) * 255.f)));
              j += hex;
            }
            j += "]";
            first = false;
          }
        }
        return dupString(j + "}");
      },
      nullptr);
}

char* dapple_looks_json(void) { return dupString(looksJson()); }

char* dapple_params_schema_json(void) { return dupString(paramsSchemaJson()); }

char* dapple_session_film_pad_json(DappleSession* session, const char* params_json) {
  ensureSource(session);
  return guarded(
      [&]() -> char* {
        if (!session) {
          setError("null session");
          return nullptr;
        }
        EditParams params;
        if (!parseParams(params_json, params)) return nullptr;
        const std::string lutFile = params.lutPath;
        FilmPadInfo pad;
        {
          std::lock_guard<std::mutex> lock(session->keyMtx);
          if (!session->keyProxy) {
            session->keyProxy = std::make_shared<SourceImage>();
            session->keyProxy->sceneReferred = session->source.sceneReferred;
            session->keyProxy->linear = resizeToLongEdge(session->source.linear, 256);
          }
          std::shared_ptr<const Lut3D> lut;
          if (!lutFile.empty()) lut = loadLut(lutFile);
          pad = filmPadFor(params, *session->camera, lut.get(), *session->keyProxy);
        }
        char buf[1800];
        if (!pad.ok) return dupString("{\"ok\":false}");
        std::snprintf(buf, sizeof buf,
                      "{\"ok\":true,\"shared_ev\":%.4f,\"final_ev\":%.4f,\"fraction\":%.4f,\"shoot_ev\":[%.4f,%.4f,%.4f],"
                      "\"print_l\":[%.3f,%.3f,%.3f],\"black_l\":[%.3f,%.3f,%.3f],\"latitude\":[%.3f,%.3f,%.3f],\"white_l\":%.3f}",
                      double(pad.sharedEv), double(pad.finalEv), double(pad.fractionAtFinal), double(pad.shootEv[0]), double(pad.shootEv[1]),
                      double(pad.shootEv[2]), double(pad.printL[0]), double(pad.printL[1]), double(pad.printL[2]), double(pad.blackL[0]),
                      double(pad.blackL[1]), double(pad.blackL[2]), double(pad.latitude[0]), double(pad.latitude[1]), double(pad.latitude[2]),
                      double(pad.whiteL));
        std::string js = buf;
        js.pop_back();  // the closing brace: more axes follow
        char b2[1100];
        auto tf = [](bool v) { return v ? "true" : "false"; };
        std::snprintf(b2, sizeof b2,
                      ",\"scene_cct\":%.0f,\"reference_cct\":%.0f,"
                      "\"effective_lo\":{\"film_shoot\":%s,\"film_print\":%s,\"film_black\":%s,\"film_wb\":%s,\"film_latitude\":%s},\"effective_hi\":{\"film_shoot\":%s,\"film_print\":%s,\"film_black\":%s,\"film_wb\":%s,\"film_latitude\":%s}}",
                      double(pad.sceneCct), double(pad.referenceCct), tf(pad.effectiveLo[0]), tf(pad.effectiveLo[1]), tf(pad.effectiveLo[2]),
                      tf(pad.effectiveLo[3]), tf(pad.effectiveLo[4]), tf(pad.effectiveHi[0]), tf(pad.effectiveHi[1]), tf(pad.effectiveHi[2]),
                      tf(pad.effectiveHi[3]), tf(pad.effectiveHi[4]));
        return dupString(js + b2);
      },
      static_cast<char*>(nullptr));
}

int32_t dapple_session_render_look_previews(DappleSession* session, const char* params_json, int32_t out_long_edge,
                                            DappleImage* out) {
  ensureSource(session);
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
  ensureSource(session);
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
  ensureSource(session);
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

namespace {
// Export options of the *_ex calls: {"metaSource":"<original file>","stripLocation":true,"colorSpace":"srgb","software":"Dapple 1.0"}. Writes the Exif and ICC
// segments into `jo`: the camera facts of the session (decoder metadata), the capture date, sub-second, offset and GPS read from metaSource (the file the user
// imported: HEIC / PNG conversions lose them), then the gaps filled from the session. Location only when the file has it and stripLocation is false (default
// true). False (error set) for a colour space this build cannot produce.
// 1 when the export options ask for Display P3 ("colorSpace":"p3"), else 0.
int exportSpaceOf(const char* optionsJson) {
  return optionsJson && optionsJson[0] && jsonlite::text(optionsJson, "colorSpace", "srgb") == "p3" ? 1 : 0;
}

bool applyExportMeta(DappleSession* s, const char* optionsJson, const Image8& img, JpegOptions& jo) {
  if (!optionsJson || !optionsJson[0]) return true;  // the plain calls: the bare stream
  const std::string o = optionsJson;
  const std::string cs = jsonlite::text(o, "colorSpace", "srgb");
  if (cs != "srgb" && cs != "p3") {
    setError("unsupported export colour space: " + cs);
    return false;
  }
  ExportMeta m;
  const std::string src = jsonlite::text(o, "metaSource", "");
  if (!src.empty()) readSourceExif(toPath(src.c_str()), m);
  if (s) {
    const ShotMetadata& sm = s->source.meta;
    if (m.make.empty()) m.make = sm.make;
    if (m.model.empty()) m.model = sm.model;
    if (m.lens.empty()) m.lens = sm.lens;
    if (m.iso <= 0) m.iso = sm.iso;
    if (m.shutterSeconds <= 0) m.shutterSeconds = sm.shutterSeconds;
    if (m.aperture <= 0) m.aperture = sm.aperture;
    if (m.focalLengthMm <= 0) m.focalLengthMm = sm.focalLengthMm;
    if (m.dateTimeOriginal.empty() && sm.timestamp > 0) {
      const std::time_t t = std::time_t(sm.timestamp);
      std::tm tmv{};
#if defined(_WIN32)
      gmtime_s(&tmv, &t);
#else
      gmtime_r(&t, &tmv);
#endif
      char buf[24];
      std::snprintf(buf, sizeof buf, "%04d:%02d:%02d %02d:%02d:%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
      m.dateTimeOriginal = buf;
    }
  }
  if (jsonlite::boolean(o, "stripLocation", true)) {
    m.hasGps = false;
    m.hasAltitude = false;
    m.hasGpsTime = false;
    m.gpsDate.clear();
  }
  m.software = jsonlite::text(o, "software", "Dapple " DAPPLE_VERSION_STRING);
  m.width = img.width;
  m.height = img.height;
  if (cs == "p3") m.colorSpace = 0xFFFF;  // Exif: uncalibrated, the ICC profile tells
  jo.exifSegment = buildExifSegment(m);
  jo.iccSegment = buildIccSegment(cs == "p3" ? IccSpace::DisplayP3 : IccSpace::Srgb);
  return true;
}

int32_t exportJpegImpl(DappleSession* session, const char* params_json, const char* lut_path, int32_t out_long_edge, int32_t jpeg_quality,
                       const char* out_path, const char* export_json);
// Streaming export of a lazy session (section 42), defined with the Ultra HDR helpers: 1 = written, 0 = this edit cannot stream, -1 = failed.
int streamExportJpeg(DappleSession* s, const EditParams& ep, const char* lutPath, int32_t outLongEdge, int32_t quality, const char* outPath,
                     const char* exportJson, bool hdr, float headroom, DappleHdrInfo* info);
// An edit that cannot stream on a lazy session: the whole frame if it fits the device, else a clean error (never a kill by the OS).
bool wholeFrameFits(DappleSession* s, const EditParams& ep);
void demoteToStream(DappleSession* s, const EditParams& ep);
}  // namespace

int32_t dapple_session_export_jpeg(DappleSession* session, const char* params_json, const char* lut_path,
                                   int32_t out_long_edge, int32_t jpeg_quality, const char* out_path) {
  return exportJpegImpl(session, params_json, lut_path, out_long_edge, jpeg_quality, out_path, nullptr);
}

int32_t dapple_session_export_jpeg_ex(DappleSession* session, const char* params_json, const char* lut_path, int32_t out_long_edge,
                                      int32_t jpeg_quality, const char* out_path, const char* export_json) {
  return exportJpegImpl(session, params_json, lut_path, out_long_edge, jpeg_quality, out_path, export_json ? export_json : "{}");
}

namespace {
int32_t exportJpegImpl(DappleSession* session, const char* params_json, const char* lut_path, int32_t out_long_edge, int32_t jpeg_quality,
                       const char* out_path, const char* export_json) {
  return guarded(
      [&]() -> int32_t {
        Image8 img;
        const auto t0 = std::chrono::steady_clock::now();
        mem::Sampler memSmp;  // peak footprint of the export for the stage log (section 42: the whole-frame path, for comparison)
        const std::size_t memAvail0 = mem::available();
        {
          EditParams ep;
          if (!session) {
            setError("null session");
            return DAPPLE_ERROR;
          }
          if (!parseParams(params_json, ep)) return DAPPLE_ERROR;
          ep.outputSpace = exportSpaceOf(export_json);
          if (session->lazyOpen.load() && !missingRasterIds(ep).empty() && !ensureSource(session)) return DAPPLE_ERROR;  // re-creating a plane needs the picture
          if (!ensureLocalRasters(session, ep)) return DAPPLE_ERROR;  // a plane the edit needs must exist: never export a silently empty mask
          demoteToStream(session, ep);
          if (session->lazyOpen.load()) {  // streaming export (section 42)
            const int r = streamExportJpeg(session, ep, lut_path, out_long_edge, jpeg_quality, out_path, export_json, false, 0.f, nullptr);
            if (r != 0) return r > 0 ? DAPPLE_OK : DAPPLE_ERROR;
            if (!wholeFrameFits(session, ep)) return DAPPLE_ERROR;
          }
          if (!renderSessionParams(session, ep, lut_path, out_long_edge, img)) return DAPPLE_ERROR;
        }
        const double renderMs = msSince(t0);
        JpegOptions jo;
        if (jpeg_quality > 0) jo.quality = jpeg_quality > 100 ? 100 : jpeg_quality;
        if (!applyExportMeta(session, export_json, img, jo)) return DAPPLE_ERROR;
        auto r = writeJpeg(img, toPath(out_path), jo);
        memSmp.finish();
        stageLog("export_jpeg %dx%d render %.0f ms, encode+write %.0f ms; peak footprint %.0f MB (+%.0f over the start, available at the start %.0f MB)", img.width,
                 img.height, renderMs, msSince(t0) - renderMs, double(memSmp.peak()) / 1048576.0,
                 double(memSmp.peak() - std::min(memSmp.peak(), memSmp.base())) / 1048576.0, double(memAvail0) / 1048576.0);
        if (!r) {
          setError(r.error().message);
          return DAPPLE_ERROR;
        }
        return DAPPLE_OK;
      },
      DAPPLE_ERROR);
}
}  // namespace

// ---- Ultra HDR (docs/engine/pipeline-architecture.md section 34) ----
namespace {
// The bright rendition of an edit: the same params with the exposure lowered by H (the Renderer clamps exposure to -5, so H is cut to what is left).
bool renderBright(DappleSession* s, const EditParams& params, const char* lutPath, int longEdge, float H, Image8& out) {
  EditParams p = params;
  p.exposure -= H;
  return renderSessionParams(s, p, lutPath, longEdge, out);
}

// EV of the modern rendering's white point against the standard curve's 1.0 (0 for any other base): the exposure the bright rendition is lowered by
// on top of H, so that the picture's own white point sits at SDR white.
float modernWhiteEv(DappleSession* s, const EditParams& params) {
  if (!isModernId(params.filmStock) || !(params.filmStrength > 0.f)) return 0.f;
  if (!s->proxy) {
    s->proxy = std::make_shared<SourceImage>();
    s->proxy->sceneReferred = s->source.sceneReferred;
    s->proxy->linear = resizeToLongEdge(s->source.linear, 256);
  }
  Renderer r(params, *s->camera, nullptr);
  r.setToneProxy(s->proxy.get());
  return std::log2(std::max(1e-3f, r.modernSdrWhite(s->source)));
}

struct HdrDecision {
  float H = 0.f, peak = 0.f;
  int reason = 0;
  Image8 bright;  // at H
};

// Decides H (requested 0 = from the scene) and renders the bright rendition at `longEdge`.
bool hdrDecide(DappleSession* s, const EditParams& params, const char* lutPath, int longEdge, float requested, HdrDecision& d) {
  d = HdrDecision{};
  if (!params.filmStock.empty() && !isModernId(params.filmStock)) {
    d.reason = 2;
    return true;
  }
  // A modern rendering (section 37): display white is the scene's own white point, not the standard curve's. The bright rendition is the
  // standard rendering of the same edit with the exposure lowered so that the modern white point sits at 1.0 (and by H more): headroom is
  // what lies above the white point of THIS picture.
  EditParams bp = params;
  if (isModernId(params.filmStock)) {
    bp.filmStock.clear();
    bp.filmStrength = 100.f;
    bp.exposure -= modernWhiteEv(s, params);
  }
  const float room = std::max(0.f, bp.exposure + 5.f);  // exposure is clamped to -5 by the Renderer
  const float probeH = std::min(dapple::uhdr::kMaxHeadroomStops, room);
  if (probeH < 0.5f) {
    d.reason = 1;
    return true;
  }
  Image8 probe;
  if (!renderBright(s, bp, lutPath, longEdge, probeH, probe)) return false;
  d.peak = dapple::uhdr::brightPeak(probe, probeH, params.outputSpace == 1);
  float H = requested > 0.f ? std::min(dapple::uhdr::kMaxHeadroomStops, std::max(0.5f, requested)) : dapple::uhdr::autoHeadroom(d.peak);
  H = std::min(H, probeH);
  if (H < 0.5f || d.peak < 1.0001f) {
    d.reason = 1;
    return true;
  }
  d.H = H;
  if (H == probeH) d.bright = std::move(probe);
  else if (!renderBright(s, bp, lutPath, longEdge, H, d.bright)) return false;
  return true;
}

void fillHdrInfo(DappleHdrInfo* info, const HdrDecision& d, bool written, float gainMax, float capMax) {
  if (!info) return;
  info->written = written ? 1 : 0;
  info->reason = d.reason;
  info->headroom_stops = d.H;
  info->peak = d.peak;
  info->gain_max = gainMax;
  info->capacity_max = capMax;
}

bool writeBytesAtomic(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
  fs::path tmp = path;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    out.flush();
    if (!out) return false;
  }
  std::error_code ec;
  fs::rename(tmp, path, ec);
  if (ec) {
    fs::remove(path, ec);
    std::error_code ec2;
    fs::rename(tmp, path, ec2);
    if (ec2) {
      fs::remove(tmp, ec);
      return false;
    }
  }
  return true;
}
// ---- streaming export (section 42): the JPEG / Ultra HDR export of a lazy session -------------------------------------------------------------
// An edit the open did not foresee (a film stock: ~55 B/px more than a plain export) on a session that holds the whole float picture: when the
// whole-frame export would not fit, the session gives its picture back and exports in bands from the cached mosaic (same pixels).
void demoteToStream(DappleSession* s, const EditParams& ep) {
  if (s->lazyOpen.load() || s->fullRawPath.empty() || xstream::envMode() != xstream::Mode::Auto || dapple::engine::mode() != dapple::engine::Mode::Full) return;
  const int W = s->source.linear.width, H = s->source.linear.height;
  const std::size_t held = std::size_t(W) * H * 12;  // the float picture is already there
  const std::size_t need = xstream::predictWholePeak(W, H, Renderer(ep, *s->camera, nullptr).filmActive());
  std::string how;
  const std::size_t budget = xstream::budgetBytes(&how);
  if (need <= held + budget) return;
  std::string why;
  auto z = openFullLazy(s->fullRawPath, DecodeOptions{}, &why);
  if (!z || z->width != W || z->height != H) return;
  std::lock_guard<std::recursive_mutex> lk(s->lazyMtx);
  ImageF().data.swap(s->source.linear.data);
  s->source.linear = ImageF();
  s->lazy = std::make_unique<LazyFull>(std::move(*z));
  s->lazyOpen = true;
  stageLog("export: whole frame needs %.0f MB more, the device has %.0f MB (%s): the session switches to bands (picture released)", double(need - held) / 1048576.0,
           double(budget) / 1048576.0, how.c_str());
}

bool wholeFrameFits(DappleSession* s, const EditParams& ep) {
  if (!s->lazy) return true;
  const std::size_t need = xstream::predictWholePeak(s->lazy->width, s->lazy->height, Renderer(ep, *s->camera, nullptr).filmActive());
  const std::size_t budget = xstream::budgetBytes();
  if (xstream::envMode() == xstream::Mode::On || need <= budget) return true;
  char b[256];
  std::snprintf(b, sizeof b, "export: this edit needs the whole %dx%d picture in memory (about %.0f MB) and the device has about %.0f MB for it; nothing was written",
                s->lazy->width, s->lazy->height, double(need) / 1048576.0, double(budget) / 1048576.0);
  setError(b);
  stageLog("%s", b);
  return false;
}

int streamExportJpeg(DappleSession* s, const EditParams& ep, const char* lutPath, int32_t outLongEdge, int32_t quality, const char* outPath,
                     const char* exportJson, bool hdr, float headroom, DappleHdrInfo* info) {
  const auto t0 = std::chrono::steady_clock::now();
  struct DropBands {  // the band source and its row cache go with the export, whatever its outcome
    DappleSession* s;
    ~DropBands() { s->bands.reset(); }
  } dropBands{s};
  mem::Sampler smp;
  struct Mark {
    explicit Mark(mem::Sampler* m) { mem::current() = m; }
    ~Mark() { mem::current() = nullptr; }
  } mark(&smp);
  const std::size_t avail0 = mem::available();
  JpegOptions jo;
  if (quality > 0) jo.quality = quality > 100 ? 100 : quality;
  std::unique_ptr<JpegStripEncoder> enc;
  Image8 dims;
  bool metaOk = true;
  auto onSize = [&](int w, int h) {
    dims.width = w, dims.height = h;
    jo.exifSegment.clear(), jo.iccSegment.clear();
    metaOk = applyExportMeta(s, exportJson, dims, jo);
    enc = std::make_unique<JpegStripEncoder>(w, h, jo);
  };
  auto sink = [&](int, int n, const std::uint8_t* rgb) { return metaOk && enc && enc->push(rgb, n); };
  int quarter = 0;
  std::vector<int> extra;
  if (hdr) {  // the gain map's renders are at a quarter of the output's long edge: build that reduction in the same pass
    int fw = 0, fh = 0;
    Renderer(ep, *s->camera, nullptr).fastSize(xstream::sizeOnly(s->lazy->meta, s->lazy->width, s->lazy->height), RenderRegion{}, fw, fh);
    if (outLongEdge > 0 && std::max(fw, fh) > outLongEdge) {
      const double k = double(outLongEdge) / std::max(fw, fh);
      fw = int(std::lround(fw * k)), fh = int(std::lround(fh * k));
    }
    quarter = std::max(1, (std::max(fw, fh) + 3) / 4);
    extra.push_back(quarter);
  }
  const int r = streamRender(s, ep, lutPath, outLongEdge, onSize, sink, extra);
  if (r <= 0) return r;
  if (!metaOk) return -1;
  mem::mark("encode");
  auto bytes = enc->finish();
  enc.reset();
  if (!bytes) {
    setError(bytes.error().message);
    return -1;
  }
  bool written = false;
  if (hdr) {
    mem::mark("gainmap");
    auto plain = [&](const HdrDecision& d) -> int {
      auto w = writeJpegBytes(*bytes, toPath(outPath));
      if (!w) {
        setError(w.error().message);
        return -1;
      }
      fillHdrInfo(info, d, false, 0.f, 0.f);
      return 1;
    };
    // the output size the quarter is taken from: the size of the streamed picture (as the whole-frame export takes img's)
    quarter = std::max(1, (std::max(dims.width, dims.height) + 3) / 4);
    HdrDecision d;
    if (!hdrDecide(s, ep, lutPath, quarter, headroom, d)) return -1;
    if (d.reason != 0) {
      if (plain(d) < 0) return -1;
    } else {
      Image8 sdr4;
      if (!renderSessionParams(s, ep, lutPath, quarter, sdr4)) return -1;
      if (sdr4.width != d.bright.width || sdr4.height != d.bright.height) {
        d.reason = 3;
        if (plain(d) < 0) return -1;
      } else {
        const dapple::uhdr::GainMap gm = dapple::uhdr::makeGainMap(sdr4, d.bright, d.H, ep.outputSpace == 1);
        if (gm.meta.gainMax <= 0.f) {
          d.reason = 1;
          if (plain(d) < 0) return -1;
        } else {
          auto file = dapple::uhdr::assemble(*bytes, gm);
          if (!file) {
            setError(file.error().message);
            return -1;
          }
          if (!writeBytesAtomic(toPath(outPath), *file)) {
            setError("Cannot write " + std::string(outPath ? outPath : ""));
            return -1;
          }
          fillHdrInfo(info, d, true, gm.meta.gainMax, gm.meta.capacityMax);
          written = true;
        }
      }
    }
  } else {
    auto w = writeJpegBytes(*bytes, toPath(outPath));
    if (!w) {
      setError(w.error().message);
      return -1;
    }
  }
  s->bands.reset();  // the row cache goes with the export
  smp.finish();
  stageLog("export_stream %s %dx%d%s in %.0f ms: peak footprint %.0f MB (+%.0f over the start, available at the start %.0f MB): %s", outPath ? pathToUtf8(toPath(outPath).filename()).c_str() : "",
           dims.width, dims.height, hdr ? (written ? " Ultra HDR" : " (plain)") : "", msSince(t0), double(smp.peak()) / 1048576.0, double(smp.peak() - std::min(smp.peak(), smp.base())) / 1048576.0,
           double(avail0) / 1048576.0, smp.summary().c_str());
  return 1;
}
}  // namespace

int32_t dapple_session_hdr_headroom(DappleSession* session, const char* params_json, const char* lut_path, DappleHdrInfo* info) {
  return guarded(
      [&]() -> int32_t {
        if (!session) {
          setError("null session");
          return DAPPLE_ERROR;
        }
        EditParams params;
        if (!parseParams(params_json, params)) return DAPPLE_ERROR;
        HdrDecision d;
        if (!hdrDecide(session, params, lut_path, 384, 0.f, d)) return DAPPLE_ERROR;
        fillHdrInfo(info, d, d.reason == 0, 0.f, 0.f);
        return DAPPLE_OK;
      },
      DAPPLE_ERROR);
}

// Share of the picture (0..1) whose colour after the edit lies outside sRGB by more than 0.03 in linear light (a Display P3 render of a 384 px
// proxy, converted back to linear sRGB). Film edits are authored for sRGB and report 0.
int32_t dapple_session_wide_gamut_fraction(DappleSession* session, const char* params_json, const char* lut_path, float* fraction) {
  return guarded(
      [&]() -> int32_t {
        if (!session || !fraction) {
          setError("null argument");
          return DAPPLE_ERROR;
        }
        EditParams params;
        if (!parseParams(params_json, params)) return DAPPLE_ERROR;
        params.outputSpace = 1;
        Image8 img;
        if (!renderSessionParams(session, params, lut_path, 384, img)) return DAPPLE_ERROR;
        static const std::array<float, 256> dec = [] {
          std::array<float, 256> t{};
          for (int i = 0; i < 256; ++i) t[std::size_t(i)] = dapple::color::srgbDecode(float(i) / 255.f);
          return t;
        }();
        const std::size_t n = std::size_t(img.width) * img.height;
        std::size_t out = 0;
        for (std::size_t i = 0; i < n; ++i) {
          const std::uint8_t* px = img.data.data() + 3 * i;
          const float q[3] = {dec[px[0]], dec[px[1]], dec[px[2]]};
          float s[3];
          dapple::color::p3ToSrgb().apply(q, s);
          if (std::min({s[0], s[1], s[2]}) < -0.03f || std::max({s[0], s[1], s[2]}) > 1.03f) ++out;
        }
        *fraction = n ? float(double(out) / double(n)) : 0.f;
        return DAPPLE_OK;
      },
      DAPPLE_ERROR);
}

namespace {
int32_t exportJpegHdrImpl(DappleSession* session, const char* params_json, const char* lut_path, int32_t out_long_edge, int32_t jpeg_quality,
                          const char* out_path, float headroom_stops, DappleHdrInfo* info, const char* export_json);
}
int32_t dapple_session_export_jpeg_hdr(DappleSession* session, const char* params_json, const char* lut_path, int32_t out_long_edge,
                                       int32_t jpeg_quality, const char* out_path, float headroom_stops, DappleHdrInfo* info) {
  return exportJpegHdrImpl(session, params_json, lut_path, out_long_edge, jpeg_quality, out_path, headroom_stops, info, nullptr);
}
int32_t dapple_session_export_jpeg_hdr_ex(DappleSession* session, const char* params_json, const char* lut_path, int32_t out_long_edge,
                                          int32_t jpeg_quality, const char* out_path, float headroom_stops, DappleHdrInfo* info,
                                          const char* export_json) {
  return exportJpegHdrImpl(session, params_json, lut_path, out_long_edge, jpeg_quality, out_path, headroom_stops, info, export_json ? export_json : "{}");
}
namespace {
int32_t exportJpegHdrImpl(DappleSession* session, const char* params_json, const char* lut_path, int32_t out_long_edge, int32_t jpeg_quality,
                          const char* out_path, float headroom_stops, DappleHdrInfo* info, const char* export_json) {
  return guarded(
      [&]() -> int32_t {
        if (!session) {
          setError("null session");
          return DAPPLE_ERROR;
        }
        EditParams params;
        if (!parseParams(params_json, params)) return DAPPLE_ERROR;
        params.outputSpace = exportSpaceOf(export_json);
        if (session->lazyOpen.load() && !missingRasterIds(params).empty() && !ensureSource(session)) return DAPPLE_ERROR;
        if (!ensureLocalRasters(session, params)) return DAPPLE_ERROR;
        demoteToStream(session, params);
        if (session->lazyOpen.load()) {  // streaming export (section 42)
          const int r = streamExportJpeg(session, params, lut_path, out_long_edge, jpeg_quality, out_path, export_json, true, headroom_stops, info);
          if (r != 0) return r > 0 ? DAPPLE_OK : DAPPLE_ERROR;
          if (!wholeFrameFits(session, params)) return DAPPLE_ERROR;
        }
        Image8 img;
        if (!renderSessionParams(session, params, lut_path, out_long_edge, img)) return DAPPLE_ERROR;
        JpegOptions jo;
        if (jpeg_quality > 0) jo.quality = jpeg_quality > 100 ? 100 : jpeg_quality;
        if (!applyExportMeta(session, export_json, img, jo)) return DAPPLE_ERROR;
        auto plain = [&](const HdrDecision& d) -> int32_t {
          auto r = writeJpeg(img, toPath(out_path), jo);
          if (!r) {
            setError(r.error().message);
            return DAPPLE_ERROR;
          }
          fillHdrInfo(info, d, false, 0.f, 0.f);
          return DAPPLE_OK;
        };
        const int quarter = std::max(1, (std::max(img.width, img.height) + 3) / 4);
        HdrDecision d;
        if (!hdrDecide(session, params, lut_path, quarter, headroom_stops, d)) return DAPPLE_ERROR;
        if (d.reason != 0) return plain(d);
        Image8 sdr4;
        if (!renderSessionParams(session, params, lut_path, quarter, sdr4)) return DAPPLE_ERROR;
        if (sdr4.width != d.bright.width || sdr4.height != d.bright.height) {
          d.reason = 3;
          return plain(d);
        }
        const dapple::uhdr::GainMap gm = dapple::uhdr::makeGainMap(sdr4, d.bright, d.H, params.outputSpace == 1);
        if (gm.meta.gainMax <= 0.f) {
          d.reason = 1;
          return plain(d);
        }
        auto base = encodeJpeg(img, jo);
        if (!base) {
          setError(base.error().message);
          return DAPPLE_ERROR;
        }
        auto file = dapple::uhdr::assemble(*base, gm);
        if (!file) {
          setError(file.error().message);
          return DAPPLE_ERROR;
        }
        if (!writeBytesAtomic(toPath(out_path), *file)) {
          setError("Cannot write " + std::string(out_path ? out_path : ""));
          return DAPPLE_ERROR;
        }
        fillHdrInfo(info, d, true, gm.meta.gainMax, gm.meta.capacityMax);
        return DAPPLE_OK;
      },
      DAPPLE_ERROR);
}
}  // namespace

// ---------------------------------------------------------------------------------------------------------
// HDR merge, panorama, detection, Motion Photo
// ---------------------------------------------------------------------------------------------------------
int32_t dapple_progress_to_cell(float fraction, const char* stage, void* user) {
  auto* cell = static_cast<DappleProgressCell*>(user);
  if (!cell) return 0;
  static const char* const kStages[] = {"", "load", "align", "deghost", "merge", "features", "match",
                                        "estimate", "warp", "seams", "blend", "crop", "detect", "done"};
  int32_t code = 14;
  if (stage)
    for (int32_t i = 1; i < 14; ++i)
      if (std::strcmp(stage, kStages[i]) == 0) code = i;
  cell->stage = code;
  cell->fraction = fraction;
  return 0;
}

int32_t dapple_merge_available(void) { return mergeAvailable() ? 1 : 0; }

DappleSession* dapple_session_open_merged(const char* const* paths, int32_t count, const char* kind, const char* options_json,
                                          DappleProgressFn progress, void* user, const volatile int32_t* cancel_flag,
                                          char** report_json_out) {
  if (report_json_out) *report_json_out = nullptr;
  return guarded(
      [&]() -> DappleSession* {
        if (!paths || count < 2) {
          setError("merge needs at least two files");
          return nullptr;
        }
        const std::string k = kind ? kind : "";
        const std::string opts = options_json ? options_json : "";
        const auto files = toPaths(paths, count);
        const MergeControl ctl = makeControl(progress, user, cancel_flag);
        auto session = std::make_unique<DappleSession>();
        if (k == "hdr") {
          HdrOptions o;
          o.baseIndex = int(jsonlite::number(opts, "base", -1));
          o.deghost = deghostFromString(jsonlite::text(opts, "deghost", "mid"));
          o.align = jsonlite::boolean(opts, "align", true);
          o.quality = qualityOf(opts);
          o.longEdge = int(jsonlite::number(opts, "longEdge", 0));
          auto r = mergeHdr(files, o, ctl);
          if (!r) {
            setError(r.error().message);
            return nullptr;
          }
          if (report_json_out) *report_json_out = dupString(hdrReportJson(r->report));
          session->source = std::move(r->image);
        } else if (k == "pano") {
          PanoOptions o;
          o.projection = panoProjectionFromString(jsonlite::text(opts, "projection", "cyl"));
          o.crop = jsonlite::boolean(opts, "crop", true);
          o.fill = jsonlite::boolean(opts, "fill", false);
          o.baseIndex = int(jsonlite::number(opts, "base", -1));
          o.exposureCompensation = jsonlite::boolean(opts, "exposureComp", true);
          o.quality = qualityOf(opts);
          o.longEdge = int(jsonlite::number(opts, "longEdge", 0));
          auto r = stitchPanorama(files, o, ctl);
          if (!r) {
            setError(r.error().message);
            return nullptr;
          }
          if (report_json_out) *report_json_out = dupString(panoReportJson(r->report));
          session->source = std::move(r->image);
        } else {
          setError("unknown merge kind '" + k + "' (expected \"hdr\" or \"pano\")");
          return nullptr;
        }
        session->cameraOwned = dapple::effectiveCamera(cameras(), session->source.meta);
        session->camera = &session->cameraOwned;
        return session.release();
      },
      nullptr);
}

int32_t dapple_session_save_linear(DappleSession* session, const char* out_path, const char* format) {
  ensureSource(session);
  return guarded(
      [&]() -> int32_t {
        if (!session) {
          setError("null session");
          return DAPPLE_ERROR;
        }
        const std::string f = format ? format : "dng";
        Result<void> r = f == "tiff" || f == "tif" ? writeLinearTiff16(session->source.linear, toPath(out_path))
                         : f == "dng"              ? writeLinearDng(session->source, toPath(out_path))
                                                   : fail("unknown format '" + f + "' (expected \"dng\" or \"tiff\")");
        if (!r) {
          setError(r.error().message);
          return DAPPLE_ERROR;
        }
        return DAPPLE_OK;
      },
      DAPPLE_ERROR);
}

char* dapple_detect_sequences_json(const char* const* paths, int32_t count, const char* options_json, DappleProgressFn progress,
                                   void* user, const volatile int32_t* cancel_flag) {
  return guarded(
      [&]() -> char* {
        if (!paths || count < 0) {
          setError("bad arguments");
          return nullptr;
        }
        const std::string opts = options_json ? options_json : "";
        DetectOptions d;
        d.maxGapSeconds = jsonlite::number(opts, "maxGapSeconds", d.maxGapSeconds);
        d.panoGapSeconds = jsonlite::number(opts, "panoGapSeconds", d.panoGapSeconds);
        d.minEvStep = jsonlite::number(opts, "minEvStep", d.minEvStep);
        d.minEvSpread = jsonlite::number(opts, "minEvSpread", d.minEvSpread);
        d.verifyContent = jsonlite::boolean(opts, "verifyContent", true);
        const MergeControl ctl = makeControl(progress, user, cancel_flag);
        std::vector<FrameInfo> frames;
        std::vector<int> original;  // index into `paths` of each probed frame
        std::vector<int> unreadable;
        for (int32_t i = 0; i < count; ++i) {
          if (ctl.cancelled()) {
            setError("cancelled");
            return nullptr;
          }
          auto fi = probeFrame(toPath(paths[i]));
          if (fi) {
            frames.push_back(std::move(*fi));
            original.push_back(i);
          } else {
            unreadable.push_back(i);
          }
          ctl.report(0.5f * float(i + 1) / float(std::max<int32_t>(1, count)), "detect");
        }
        MergeControl inner = ctl;
        inner.progress = nullptr;
        const auto groups = detectSequences(frames, d, inner);
        ctl.report(1.f, "detect");
        std::string j = "{\"frames\":[";
        for (std::size_t i = 0; i < frames.size(); ++i) {
          const FrameInfo& f = frames[i];
          j += (i ? "," : "");
          j += "{\"index\":" + std::to_string(original[i]) + ",\"path\":" + q(pathToUtf8(f.path)) + ",\"timestamp\":" +
               std::to_string(f.meta.timestamp) + ",\"ev\":" + (f.exposureKnown ? n(f.exposureStops) : "null") + ",\"iso\":" + n(f.meta.iso) +
               ",\"shutter\":" + n(f.meta.shutterSeconds) + ",\"aperture\":" + n(f.meta.aperture) + ",\"focalLength\":" + n(f.meta.focalLengthMm) +
               ",\"sequence\":" +
               (f.hasSequence ? "{\"length\":" + std::to_string(f.sequenceLength) + ",\"number\":" + std::to_string(f.sequenceImageNumber) +
                                    ",\"file\":" + std::to_string(f.sequenceFileNumber) + ",\"releaseMode\":" + std::to_string(f.releaseMode) + "}"
                              : std::string("null")) + "}";
        }
        j += "],\"groups\":[";
        for (std::size_t g = 0; g < groups.size(); ++g) {
          const auto& gr = groups[g];
          j += (g ? "," : "");
          j += std::string("{\"kind\":") + (gr.kind == SequenceGroup::Kind::Bracket ? "\"bracket\"" : "\"panorama\"") +
               ",\"confidence\":" + n(gr.confidence) + ",\"reason\":" + q(gr.reason) + ",\"start\":" + std::to_string(gr.startTime) +
               ",\"end\":" + std::to_string(gr.endTime) + ",\"frames\":[";
          for (std::size_t i = 0; i < gr.indices.size(); ++i)
            j += (i ? "," : "") + std::string("{\"index\":") + std::to_string(original[gr.indices[i]]) + ",\"path\":" + q(gr.paths[i]) +
                 ",\"evStep\":" + n(gr.evSteps[i]) + "}";
          j += "]}";
        }
        j += "],\"unreadable\":[";
        for (std::size_t i = 0; i < unreadable.size(); ++i) j += (i ? "," : "") + std::to_string(unreadable[i]);
        j += "]}";
        return dupString(j);
      },
      nullptr);
}

char* dapple_video_probe_json(const char* mp4_path) {
  return guarded(
      [&]() -> char* {
        auto v = probeVideo(toPath(mp4_path));
        if (!v) {
          setError(v.error().message);
          return nullptr;
        }
        std::string j = "{\"duration\":" + n(v->durationSec) + ",\"width\":" + std::to_string(v->width) + ",\"height\":" + std::to_string(v->height) +
                        ",\"rotation\":" + std::to_string(v->rotationDeg) + ",\"fps\":" + n(v->fps) + ",\"videoCodec\":" + q(v->videoCodec) +
                        ",\"audioCodec\":" + q(v->audioCodec) + ",\"streamCopyOk\":" + (v->streamCopyOk ? "true" : "false") + ",\"keyframes\":[";
        for (std::size_t i = 0; i < v->keyframesSec.size(); ++i) j += (i ? "," : "") + n(v->keyframesSec[i]);
        return dupString(j + "]}");
      },
      nullptr);
}

int32_t dapple_motion_photo_from_video(const char* video_path, const char* jpeg_path, const char* out_path, const char* options_json,
                                       char** info_json_out) {
  if (info_json_out) *info_json_out = nullptr;
  return guarded(
      [&]() -> int32_t {
        const std::string opts = options_json ? options_json : "";
        MotionFromVideoRequest rq;
        rq.video = toPath(video_path);
        rq.coverJpeg = toPath(jpeg_path);
        rq.output = toPath(out_path);
        rq.startSec = jsonlite::number(opts, "start", 0.0);
        rq.durationSec = jsonlite::number(opts, "duration", 3.0);
        rq.coverSec = jsonlite::number(opts, "coverTime", -1.0);
        rq.keepAudio = jsonlite::boolean(opts, "keepAudio", true);
        rq.legacyMicroVideo = jsonlite::boolean(opts, "legacyMicroVideo", false);
        auto r = createMotionPhotoFromVideo(rq);
        if (!r) {
          setError(r.error().message);
          return DAPPLE_ERROR;
        }
        if (info_json_out) {
          std::string j = motionInfoJson(r->info);
          j += ",\"clip\":{\"start\":" + n(r->trim.actualStartSec) + ",\"duration\":" + n(r->trim.actualDurationSec) +
               ",\"frames\":" + std::to_string(r->trim.videoFrames) + ",\"audioKept\":" + (r->trim.audioKept ? "true" : "false") +
               ",\"coverInClip\":" + n(r->coverInClipSec) + ",\"note\":" + q(r->trim.note) + "}}";
          *info_json_out = dupString(j);
        }
        return DAPPLE_OK;
      },
      DAPPLE_ERROR);
}

int32_t dapple_motion_photo_write(const char* jpeg_path, const char* mp4_path, const char* out_path, double cover_time_sec,
                                  char** info_json_out) {
  if (info_json_out) *info_json_out = nullptr;
  return guarded(
      [&]() -> int32_t {
        MotionPhotoOptions o;
        o.presentationTimestampUs = cover_time_sec >= 0 ? std::int64_t(cover_time_sec * 1e6) : -1;
        auto r = writeMotionPhoto(toPath(jpeg_path), toPath(mp4_path), toPath(out_path), o);
        if (!r) {
          setError(r.error().message);
          return DAPPLE_ERROR;
        }
        if (info_json_out) *info_json_out = dupString(motionInfoJson(*r) + "}");
        return DAPPLE_OK;
      },
      DAPPLE_ERROR);
}

char* dapple_motion_photo_validate_json(const char* path) {
  return guarded(
      [&]() -> char* {
        auto r = validateMotionPhotoFile(toPath(path));
        if (!r) {
          setError(r.error().message);
          return nullptr;
        }
        return dupString(motionInfoJson(*r) + "}");
      },
      nullptr);
}

}  // extern "C"

// ---- streaming export (section 42): progress of an export, memory probe -----------------------------------------------------------------
int32_t dapple_session_set_export_progress(DappleSession* session, DappleProgressFn fn, void* user) {
  if (!session) return DAPPLE_ERROR;
  session->exportProgress = fn;
  session->exportProgressUser = user;
  return DAPPLE_OK;
}

char* dapple_memory_json(void) {
  char b[192];
  std::snprintf(b, sizeof b, "{\"footprint\":%llu,\"resident\":%llu,\"available\":%llu,\"exportBudget\":%llu,\"stream\":\"%s\"}",
                (unsigned long long)mem::footprint(), (unsigned long long)mem::resident(), (unsigned long long)mem::available(),
                (unsigned long long)xstream::budgetBytes(),
                xstream::envMode() == xstream::Mode::On ? "on" : xstream::envMode() == xstream::Mode::Off ? "off" : "auto");
  return dupString(b);
}

int32_t dapple_session_is_streaming(DappleSession* session) { return session && session->lazyOpen.load() ? 1 : 0; }

// Not part of the C API (not in dapple_c.h): the test suite reads the bright-rendition shift of a modern rendering (EV, see modernWhiteEv).
extern "C" float dapple_internal_modern_white_ev(DappleSession* session, const char* params_json) {
  EditParams params;
  if (!session || !parseParams(params_json, params) || !ensureSource(session)) return 0.f;
  return modernWhiteEv(session, params);
}
