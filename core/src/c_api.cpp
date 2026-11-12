// Implementation of the frozen C API (dapple/dapple_c.h).

#include "dapple/dapple_c.h"
#include "file_util.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "dapple/auto_adjust.h"
#include "dapple/camera.h"
#include "dapple/decoder.h"
#include "dapple/edit_params.h"
#include "dapple/encode.h"
#include "dapple/film.h"
#include "dapple/film_match.h"
#include "dapple/look.h"
#include "dapple/lut.h"
#include "dapple/renderer.h"
#include "dapple/tone.h"
#include "parallel.h"
#include "sysperf.h"
#include "dapple/xmp_preset.h"
#include "engine/exec/engine_session.h"  // optional frame engine (DAPPLE_ENGINE), default off
#include "color.h"
#ifdef DAPPLE_HAVE_GPU
#include "engine/exec/gpu_runtime.h"  // optional GPU executor (core/gpu): runtime flag, status; frames go through EngineSession
#endif

#ifndef DAPPLE_VERSION_STRING
#define DAPPLE_VERSION_STRING "0.1.0"
#endif

struct DappleSession {
  dapple::SourceImage source;
  const dapple::CameraProfile* camera = nullptr;

  // ---- interactive frame caches (see dapple_session_render_frame) ----
  struct Scaled {
    int longEdge = 0;
    std::shared_ptr<dapple::SourceImage> img;
    std::size_t bytes = 0;
    unsigned stamp = 0;
  };
  std::mutex mtx;
  std::vector<Scaled> scaled;
  unsigned stamp = 0;
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
  std::vector<std::pair<std::string, float>> filmKeys;
  // Where the executors read the full-resolution source from (the GPU keeps a device copy per store identity)
  std::once_flag storeOnce;
  std::unique_ptr<dapple::engine::MemorySourceStore> store;
};

namespace {

using namespace dapple;
namespace fs = std::filesystem;

thread_local std::string g_lastError;

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

// `params` with EditParams::filmKeyEv set: the EV that puts the film render's median lightness where the normal develop
// render of the same edit has it (film_match.h). A few film evaluations of the 256 px proxy, cached by key.
EditParams withFilmKey(DappleSession* s, EditParams params, const char* lutPath = nullptr) {
  params.filmKeyEv = 0.f;
  if (params.filmStock.empty() || std::getenv("DAPPLE_FILM_MATCH_OFF")) return params;
  const std::string lutFile = lutPath && lutPath[0] ? lutPath : params.lutPath;
  const std::string key = filmMatchKey(params) + "|" + lutFile;
  std::lock_guard<std::mutex> lock(s->keyMtx);
  for (auto& e : s->filmKeys)
    if (e.first == key) {
      params.filmKeyEv = e.second;
      return params;
    }
  if (!s->keyProxy) {
    s->keyProxy = std::make_shared<SourceImage>();
    s->keyProxy->sceneReferred = s->source.sceneReferred;
    s->keyProxy->linear = resizeToLongEdge(s->source.linear, 256);
  }
  std::shared_ptr<const Lut3D> lut;
  if (!lutFile.empty()) lut = loadLut(lutFile);
  const float ev = matchFilmExposure(params, *s->camera, lut.get(), *s->keyProxy);
  if (s->filmKeys.size() >= 12) s->filmKeys.erase(s->filmKeys.begin());
  s->filmKeys.emplace_back(key, ev);
  params.filmKeyEv = ev;
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
    SourceImage scaled;
    scaled.sceneReferred = s->source.sceneReferred;
    scaled.linear = resizeToLongEdge(s->source.linear, outLongEdge);
    if (dapple::engine::mode() == dapple::engine::Mode::Full) return engineRenderExact(s, params, renderer, scaled, out);
    out = renderer.render(scaled);
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
        auto src = decoder->decode(p, opts);
        if (!src) {
          setError(src.error().message);
          return nullptr;
        }
        auto session = std::make_unique<DappleSession>();
        session->source = std::move(*src);
        if (long_edge > 0) session->source.linear = resizeToLongEdge(session->source.linear, long_edge);
        session->camera = &cameras().resolve(session->source.meta.make, session->source.meta.model);
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


// Source scaled to `longEdge` (cached, at most ~160 MB of scaled copies); null when the session
// source itself is not larger.
static std::shared_ptr<SourceImage> scaledSource(DappleSession* s, int longEdge) {
  const int own = std::max(s->source.linear.width, s->source.linear.height);
  if (longEdge <= 0 || longEdge >= own) return nullptr;
  ++s->stamp;
  for (auto& e : s->scaled)
    if (e.longEdge == longEdge) {
      e.stamp = s->stamp;
      return e.img;
    }
  auto img = std::make_shared<SourceImage>();
  img->sceneReferred = s->source.sceneReferred;
  img->linear = resizeToLongEdge(s->source.linear, longEdge);
  DappleSession::Scaled e;
  e.longEdge = longEdge;
  e.img = img;
  e.bytes = img->linear.data.size() * sizeof(float);
  e.stamp = s->stamp;
  s->scaled.push_back(e);
  auto total = [&] {
    std::size_t t = 0;
    for (auto& x : s->scaled) t += x.bytes;
    return t;
  };
  while (s->scaled.size() > 1 && (total() > (160u << 20) || s->scaled.size() > 4)) {
    auto oldest = s->scaled.end();
    for (auto it = s->scaled.begin(); it != s->scaled.end(); ++it)
      if (it->img != img && (oldest == s->scaled.end() || it->stamp < oldest->stamp)) oldest = it;
    if (oldest == s->scaled.end()) break;
    s->scaled.erase(oldest);
  }
  return img;
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
  params = withFilmKey(s, params, lutPath);
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
  std::shared_ptr<SourceImage> hold = scaledSource(s, srcLong);
  const SourceImage& src = hold ? *hold : s->source;

  if (!s->proxy) {
    s->proxy = std::make_shared<SourceImage>();
    s->proxy->sceneReferred = s->source.sceneReferred;
    s->proxy->linear = resizeToLongEdge(s->source.linear, 256);
  }
  Renderer renderer(params, *s->camera, lut.get());
  renderer.setToneProxy(s->proxy.get());
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

void dapple_perf_configure(int32_t thread_limit, int32_t target_frame_ms) {
  ThreadPool::instance().setLimit(thread_limit);
  sys::adpf().setTarget(int64_t(std::max(0, target_frame_ms)) * 1000000);
}

float dapple_thermal_headroom(int32_t forecast_seconds) { return sys::adpf().headroom(forecast_seconds); }

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
                 "\",\"type\":\"" + s.kind + "\"}";
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
            const ImageF out = Renderer(p, cam, nullptr).renderFloat(src, false);
            if (out.width != 3 * kCell) continue;
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
        if (!renderSession(session, params_json, lut_path, out_long_edge, img)) return DAPPLE_ERROR;
        JpegOptions jo;
        if (jpeg_quality > 0) jo.quality = jpeg_quality > 100 ? 100 : jpeg_quality;
        auto r = writeJpeg(img, toPath(out_path), jo);
        if (!r) {
          setError(r.error().message);
          return DAPPLE_ERROR;
        }
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
        session->camera = &cameras().resolve(session->source.meta.make, session->source.meta.model);
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
