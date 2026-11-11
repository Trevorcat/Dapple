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

#include "dapple/auto_adjust.h"
#include "dapple/camera.h"
#include "dapple/decoder.h"
#include "dapple/edit_params.h"
#include "dapple/encode.h"
#include "dapple/look.h"
#include "dapple/lut.h"
#include "dapple/renderer.h"
#include "dapple/tone.h"
#include "dapple/xmp_preset.h"

#ifndef DAPPLE_VERSION_STRING
#define DAPPLE_VERSION_STRING "0.1.0"
#endif

struct DappleSession {
  dapple::SourceImage source;
  const dapple::CameraProfile* camera = nullptr;
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

bool renderSessionParams(DappleSession* s, const EditParams& params, const char* lutPath, int32_t outLongEdge,
                         Image8& out, float toneReach) {
  std::string lutFile = lutPath && lutPath[0] ? lutPath : params.lutPath;
  std::shared_ptr<const Lut3D> lut;
  if (!lutFile.empty()) {
    lut = loadLut(lutFile);
    if (!lut) return false;
  }
  Renderer renderer(params, *s->camera, lut.get());
  renderer.setToneReach(toneReach);
  if (outLongEdge > 0 && std::max(s->source.linear.width, s->source.linear.height) > outLongEdge) {
    SourceImage scaled;
    scaled.sceneReferred = s->source.sceneReferred;
    scaled.linear = resizeToLongEdge(s->source.linear, outLongEdge);
    out = renderer.render(scaled);
  } else {
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
        return dupString(toneJson(analyzeTone(session->source, *session->camera, params)));
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
