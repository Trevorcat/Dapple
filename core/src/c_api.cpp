// Implementation of the frozen C API (dapple/dapple_c.h).

#include "dapple/dapple_c.h"
#include "file_util.h"

#include <atomic>
#include <cstdarg>
#include <cmath>
#include <cstdio>
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
#include "engine/nodes/film/film_async.h"
#include "color.h"
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
  ~DappleSession() {
    for (int i = 0; i < 5000 && matchBusy.load() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  dapple::FilmMatchCache filmCache;  // image statistics + remembered medians of the match (film_match.h)
  // Where the executors read the full-resolution source from (the GPU keeps a device copy per store identity)
  std::once_flag storeOnce;
  std::unique_ptr<dapple::engine::MemorySourceStore> store;
};

namespace {

using namespace dapple;
namespace fs = std::filesystem;

thread_local std::string g_lastError;
thread_local bool tlHintFrame = false;  // set on the touch-hint worker: its frames are not real frames

void setError(std::string message) { g_lastError = std::move(message); }

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
  if (params.filmStock.empty() || std::getenv("DAPPLE_FILM_MATCH_OFF")) return params;
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
        return params;
      }
    if (asyncOn && interactive && !uncached && !s->filmKeys.empty()) {  // any stock: while another stock is selected the frame shows the previous stock too
      // previous result of the same stock now, the new one on the background pool
      const FilmExposure& prev = s->filmKeys.back().ev;
      params.filmKeyEv = prev.filmEv;
      params.filmPrintEv = prev.printEv;
      dapple::engine::filmnode::markMatchStale();
      queueFilmMatch(s, params, lutFile, key);
      return params;
    }
  }
  std::lock_guard<std::mutex> ml(s->matchMtx);
  const FilmExposure ev = computeFilmMatch(s, params, lutFile, key, interactive, uncached);
  params.filmKeyEv = ev.filmEv;
  params.filmPrintEv = ev.printEv;
  return params;
}

// Renders the session with already parsed params. Returns false (with last error set) on failure.
bool renderSessionParams(DappleSession* s, const EditParams& params, const char* lutPath, int32_t outLongEdge,
                         Image8& out, float toneReach = 1.f);

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

bool renderSessionParams(DappleSession* s, const EditParams& paramsIn, const char* lutPath, int32_t outLongEdge,
                         Image8& out, float toneReach) {
  const EditParams params = withFilmKey(s, paramsIn, lutPath);
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
                  ",\"problems\":[";
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
  (void)cache_dir;
  (void)app_version;
  (void)fingerprint;
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
  char buf[256];
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

DappleSession* dapple_session_open(const char* path, int32_t quality, int32_t long_edge) {
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
        if (opts.quality == DecodeQuality::Full) dapple::demosaic::installGpuBackend();  // the decoder's Demosaic node uses the GPU when the runtime switch is on
#endif
        const auto t0 = std::chrono::steady_clock::now();
        auto src = decoder->decode(p, opts);
        if (!src) {
          setError(src.error().message);
          return nullptr;
        }
        auto session = std::make_unique<DappleSession>();
        session->source = std::move(*src);
        const double decodeMs = msSince(t0);
        if (long_edge > 0) session->source.linear = resizeToLongEdge(session->source.linear, long_edge);
        stageLog("session_open %s quality=%s decoded %dx%d in %.0f ms, long_edge %d total %.0f ms", pathToUtf8(p.filename()).c_str(),
                 opts.quality == DecodeQuality::Full ? "full" : "preview", session->source.linear.width, session->source.linear.height, decodeMs, long_edge, msSince(t0));
        session->cameraOwned = dapple::effectiveCamera(cameras(), session->source.meta);
        session->camera = &session->cameraOwned;
        return session.release();
      },
      nullptr);
}

static std::atomic<std::uint64_t> g_regionGen{0};  // latest wins: a new region open (or dapple_region_cancel) cancels the one in flight

void dapple_region_cancel(void) { g_regionGen.fetch_add(1); }

DappleSession* dapple_session_open_region(const char* path, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
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

static bool renderFrame(DappleSession* s, const char* paramsJson, const char* lutPath, const DappleFrameRequest& rq,
                 DappleImage* out, char** analysisOut) {
  std::lock_guard<std::mutex> lock(s->mtx);
  EditParams params;
  if (!parseParams(paramsJson, params)) return false;
  const auto tMatch0 = std::chrono::steady_clock::now();
  params = withFilmKey(s, params, lutPath, (rq.flags & DAPPLE_FRAME_INTERACTIVE) != 0);
  const double matchMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tMatch0).count();
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
    job.request.noise = !interactive || !s->haveLast || !nearEq(s->lastParams.nrLuma, params.nrLuma) ||
                        !nearEq(s->lastParams.nrColor, params.nrColor);
    job.fastOptions.spatial = interactive ? SpatialQuality::Interactive : SpatialQuality::Full;
    job.fastOptions.noise = job.request.noise;
    job.fastOptions.region = region;
    job.fastOptions.keepGrain = keepGrain;
    // a table slider moves: a print table that is not cached is baked coarsely, the frame after the drag bakes the exact one (section 26)
    job.fastOptions.coarseFilm = interactive && !activeParam.empty() && activeParam != "grain" && activeParam != "film_grain" && activeParam != "film_strength" &&
                                 activeParam != "film_halation" && !std::getenv("DAPPLE_FILM_COARSE_OFF");
    job.fastOptions.fullLongEdge = fullLongEdge;
    s->lutKeep = lut;
    s->lastParams = params;
    s->haveLast = true;
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
int32_t dapple_session_touch_hint(DappleSession* session, int32_t burst_ms) {
  if (!session || burst_ms == 0) return DAPPLE_OK;
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
        const std::function<void()> job = [&] {
          const auto t0 = std::chrono::steady_clock::now();
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

void dapple_set_interaction(int32_t active) { dapple::demosaic::setInteraction(active != 0); }

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

int32_t dapple_session_export_jpeg(DappleSession* session, const char* params_json, const char* lut_path,
                                   int32_t out_long_edge, int32_t jpeg_quality, const char* out_path) {
  return guarded(
      [&]() -> int32_t {
        Image8 img;
        const auto t0 = std::chrono::steady_clock::now();
        if (!renderSession(session, params_json, lut_path, out_long_edge, img)) return DAPPLE_ERROR;
        const double renderMs = msSince(t0);
        JpegOptions jo;
        if (jpeg_quality > 0) jo.quality = jpeg_quality > 100 ? 100 : jpeg_quality;
        auto r = writeJpeg(img, toPath(out_path), jo);
        stageLog("export_jpeg %dx%d render %.0f ms, encode+write %.0f ms", img.width, img.height, renderMs, msSince(t0) - renderMs);
        if (!r) {
          setError(r.error().message);
          return DAPPLE_ERROR;
        }
        return DAPPLE_OK;
      },
      DAPPLE_ERROR);
}

// ---- Ultra HDR (docs/engine/pipeline-architecture.md section 34) ----
namespace {
// The bright rendition of an edit: the same params with the exposure lowered by H (the Renderer clamps exposure to -5, so H is cut to what is left).
bool renderBright(DappleSession* s, const EditParams& params, const char* lutPath, int longEdge, float H, Image8& out) {
  EditParams p = params;
  p.exposure -= H;
  return renderSessionParams(s, p, lutPath, longEdge, out);
}

struct HdrDecision {
  float H = 0.f, peak = 0.f;
  int reason = 0;
  Image8 bright;  // at H
};

// Decides H (requested 0 = from the scene) and renders the bright rendition at `longEdge`.
bool hdrDecide(DappleSession* s, const EditParams& params, const char* lutPath, int longEdge, float requested, HdrDecision& d) {
  d = HdrDecision{};
  if (!params.filmStock.empty()) {
    d.reason = 2;
    return true;
  }
  const float room = std::max(0.f, params.exposure + 5.f);  // exposure is clamped to -5 by the Renderer
  const float probeH = std::min(dapple::uhdr::kMaxHeadroomStops, room);
  if (probeH < 0.5f) {
    d.reason = 1;
    return true;
  }
  Image8 probe;
  if (!renderBright(s, params, lutPath, longEdge, probeH, probe)) return false;
  d.peak = dapple::uhdr::brightPeak(probe, probeH);
  float H = requested > 0.f ? std::min(dapple::uhdr::kMaxHeadroomStops, std::max(0.5f, requested)) : dapple::uhdr::autoHeadroom(d.peak);
  H = std::min(H, probeH);
  if (H < 0.5f || d.peak < 1.0001f) {
    d.reason = 1;
    return true;
  }
  d.H = H;
  if (H == probeH) d.bright = std::move(probe);
  else if (!renderBright(s, params, lutPath, longEdge, H, d.bright)) return false;
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

int32_t dapple_session_export_jpeg_hdr(DappleSession* session, const char* params_json, const char* lut_path, int32_t out_long_edge,
                                       int32_t jpeg_quality, const char* out_path, float headroom_stops, DappleHdrInfo* info) {
  return guarded(
      [&]() -> int32_t {
        if (!session) {
          setError("null session");
          return DAPPLE_ERROR;
        }
        EditParams params;
        if (!parseParams(params_json, params)) return DAPPLE_ERROR;
        Image8 img;
        if (!renderSessionParams(session, params, lut_path, out_long_edge, img)) return DAPPLE_ERROR;
        JpegOptions jo;
        if (jpeg_quality > 0) jo.quality = jpeg_quality > 100 ? 100 : jpeg_quality;
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
        const dapple::uhdr::GainMap gm = dapple::uhdr::makeGainMap(sdr4, d.bright, d.H);
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
