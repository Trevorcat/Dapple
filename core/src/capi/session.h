// Internal header of the C API implementation (core/src/capi/*.cpp): the session object and the helpers the translation units share.
// Not part of the public API (that is dapple/dapple_c.h).
#pragma once

#include "dapple/dapple_c.h"
#include "../file_util.h"

#include <algorithm>
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
#include <condition_variable>
#include <filesystem>
#include <functional>

#include "dapple/auto_adjust.h"
#include "dapple/camera.h"
#include "dapple/decoder.h"
#include "dapple/mosaic.h"
#include "engine/nodes/demosaic/demosaic.h"
#include "dapple/edit_params.h"
#include "dapple/encode.h"
#include "../exif_writer.h"
#include "dapple/film.h"
#include "dapple/film_match.h"
#include "dapple/look.h"
#include "dapple/lut.h"
#include "engine/nodes/point/point.h"
#include "../dng_thumbnail.h"
#include "dapple/tone.h"
#include "dapple/ultrahdr.h"
#include "../parallel.h"
#include "../determinism.h"
#include "../sysperf.h"
#include "dapple/xmp_preset.h"
#include "engine/exec/engine_session.h"  // the frame engine: every frame and export
#include "engine/nodes/point/cpu.h"
#include "../local_eval.h"
#include "../warp_field.h"
#include "../ai_masks.h"
#include "engine/nodes/film/film_async.h"
#include "engine/nodes/film/frame_trace.h"
#include "../color.h"
#include "../export_stream.h"  // streaming (band) export, docs/engine/pipeline-architecture.md section 42
#include "../mem_probe.h"
#include "engine/nodes/film/film_nodes.h"  // releaseFrameWorkspaces (dapple_memory_trim)
#ifdef DAPPLE_HAVE_GPU
#include "engine/exec/gpu_runtime.h"  // optional GPU executor (core/gpu): runtime flag, status; frames go through EngineSession
#include "engine/nodes/demosaic/demosaic_backend.h"  // GPU Demosaic backend for the decoder (own device, docs 32)
#include "engine/hal/hal.h"  // live device memory (memory report)
#endif

#ifndef DAPPLE_VERSION_STRING
#define DAPPLE_VERSION_STRING "0.1.0"
#endif

struct DappleSession {
  DappleSession();
  DappleSession(const DappleSession&) = delete;
  DappleSession& operator=(const DappleSession&) = delete;
  dapple::SourceImage source;
  std::once_flag photoIdOnce;  // photoId: the 16 hex digits of the AI plane ids (aimask::photoIdOf), computed on first use
  std::string photoId;
  const dapple::CameraProfile* camera = nullptr;  // &cameraOwned: the profile with this file's exposure anchor
  dapple::CameraProfile cameraOwned;

  // ---- interactive frame caches (see dapple_session_render_frame) ----
  std::mutex mtx;
  std::shared_ptr<dapple::SourceImage> proxy;                  // 256 px copy for the tonal analysis
  std::shared_ptr<const dapple::Lut3D> lutKeep;
  dapple::EditParams lastParams;
  bool haveLast = false;
  dapple::engine::EngineSession engine;  // interactive / final frames (docs/engine/pipeline-architecture.md)
  dapple::engine::EngineSession exportEngine;  // export / reference renders (serialised by exportMtx)
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
  // The match values an interactive film frame actually used (keyMtx): they ease toward the latest landed match instead of stepping when a background solve lands
  // (docs/engine/pipeline-architecture.md section 57). stockLast = the newest landed match of each stock (the film map's remembered match).
  struct FilmUsed {
    bool valid = false;
    std::string stock;
    float ev = 0.f, pev = 0.f;
    double t = 0.0;
  } filmUsed;
  std::map<std::string, dapple::FilmExposure> stockLast;
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
  ~DappleSession();
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
  bool gpuOom = false;                                 // the last GPU export of this session failed to allocate on the device (42.9)
  bool retryBands = false;                             // ... and the whole-frame render gave up for the export to switch to bands
  std::filesystem::path fullRawPath;                   // a FULL open of a RAW at natural size (an export may switch it to bands: demoteToStream)
  DappleProgressFn exportProgress = nullptr;           // dapple_session_set_export_progress
  void* exportProgressUser = nullptr;
};

using namespace dapple;
namespace fs = std::filesystem;

namespace capi {

// Live sessions (dapple_memory_report_json, pipeline-architecture.md 42.9): registered for their whole lifetime.
std::mutex& sessionRegistryMutex();
std::vector<DappleSession*>& sessionRegistry();

// ---- error channel, strings, JSON writing, LUT cache (session.cpp) ----
const std::string& lastError();
void setError(std::string message);
char* dupString(const std::string& s);
// UTF-8 C string -> path (std::filesystem handles non-ASCII on Windows).
fs::path toPath(const char* utf8);
const CameraRegistry& cameras();
const DecoderRegistry& decoders();
std::string esc(const std::string& s);
std::string q(const std::string& s);
std::string n(double v);
std::shared_ptr<const Lut3D> loadLut(const std::string& pathUtf8);
bool parseParams(const char* paramsJson, EditParams& params);
// In place: 8-bit sRGB -> 8-bit Display P3 (colorimetric, D65, same transfer curve).
void srgb8ToDisplayP3(Image8& img);
bool fillImage(const Image8& img, DappleImage* out);
// One-line stage log (field diagnostics): tag dapple_engine on Android, os_log on Apple, stderr elsewhere.
void stageLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
double msSince(std::chrono::steady_clock::time_point t0);

// Inside dapple_session_export_jpeg(_hdr)(_ex): a GPU allocation failure may switch the export to bands (42.9).
extern thread_local bool tlExportCall;
struct ExportCallScope {
  bool prev = tlExportCall;
  ExportCallScope() { tlExportCall = true; }
  ~ExportCallScope() { tlExportCall = prev; }
};

// Runs `fn`; an exception becomes the error message and `failure`.
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

// The same for entry points that return nothing.
template <class Fn>
void guardedVoid(Fn&& fn) {
  try {
    fn();
  } catch (const std::exception& e) {
    setError(std::string("exception: ") + e.what());
  } catch (...) {
    setError("unknown exception");
  }
}

// ---- film match (film.cpp) ----
// `params` with EditParams::filmKeyEv set: the EV that puts the film render's median lightness where the normal develop
// render of the same edit has it (film_match.h). A few film evaluations of the 256 px proxy, cached by key.
EditParams withFilmKey(DappleSession* s, EditParams params, const char* lutPath = nullptr, bool interactive = false);
// Clock of the match easing in seconds (tests inject one; null = steady_clock). Not part of the C API.
void setFilmClockForTest(double (*fn)());

// ---- whole-frame rendering (render.cpp) ----
// The exact (reference / export) variant through the Planner and the CpuExecutor. Frames of more than 16 MP are split into tiles.
bool engineRenderExact(DappleSession* s, const EditParams& params, const engine::point::Params& point, const SourceImage& src, Image8& out);
// Renders the session with already parsed params. Returns false (with last error set) on failure.
bool renderSessionParams(DappleSession* s, const EditParams& params, const char* lutPath, int32_t outLongEdge, Image8& out, float toneReach = 1.f);
// The same from a params JSON.
bool renderSession(DappleSession* s, const char* paramsJson, const char* lutPath, int32_t outLongEdge, Image8& out);
#ifdef DAPPLE_HAVE_GPU
// Where the executors read the full-resolution source from (the GPU keeps a device copy per store identity).
dapple::engine::SourceStore* sourceStoreOf(DappleSession* s);
// What a whole-frame GPU export of a W x H picture adds on this device (42.9). `extra` counts on unified memory (the GPU's images come out of the RAM
// the CPU budget is about); `fits` = false when the driver's budget (VK_EXT_memory_budget) cannot hold the resident source, the film images and the
// pool. No active device: nothing to add, fits.
struct GpuWhole {
  bool active = false, unified = false, fits = true;
  std::size_t extra = 0, need = 0, free = 0;
};
GpuWhole gpuWholeCheck(int W, int H, bool film);
#endif

// ---- streaming export of a lazy session (stream.cpp), section 42 ----
// A lazy FULL session gets its float picture here when a call needs the whole of it (no-op otherwise).
bool ensureSource(DappleSession* s);
using RowSink = std::function<bool(int y0, int n, const std::uint8_t* rgb)>;
// The render of a lazy session in bands. 1 = done, 0 = this edit cannot stream, -1 = failed.
int streamRender(DappleSession* s, const EditParams& paramsIn, const char* lutPath, int32_t outLongEdge, const std::function<void(int, int)>& onSize,
                 const RowSink& sink, const std::vector<int>& extraLongEdges = {});
// An edit that cannot stream on a lazy session: the whole frame if it fits the device, else a clean error (never a kill by the OS).
bool wholeFrameFits(DappleSession* s, const EditParams& ep);
void demoteToStream(DappleSession* s, const EditParams& ep, bool force = false);
bool retryInBands(DappleSession* s, const EditParams& ep);

// ---- export (export.cpp, hdr.cpp) ----
int exportSpaceOf(const char* optionsJson);
bool applyExportMeta(DappleSession* s, const char* optionsJson, const Image8& img, JpegOptions& jo);
// Streaming export of a lazy session: 1 = written, 0 = this edit cannot stream, -1 = failed.
int streamExportJpeg(DappleSession* s, const EditParams& ep, const char* lutPath, int32_t outLongEdge, int32_t quality, const char* outPath,
                     const char* exportJson, bool hdr, float headroom, DappleHdrInfo* info);
struct HdrDecision {
  float H = 0.f, peak = 0.f;
  int reason = 0;
  Image8 bright;  // at H
};
// Decides H (requested 0 = from the scene) and renders the bright rendition at `longEdge`.
bool hdrDecide(DappleSession* s, const EditParams& params, const char* lutPath, int longEdge, float requested, HdrDecision& d);
void fillHdrInfo(DappleHdrInfo* info, const HdrDecision& d, bool written, float gainMax, float capMax);
bool writeBytesAtomic(const fs::path& path, const std::vector<std::uint8_t>& bytes);

// ---- local rasters and AI masks (ai.cpp) ----
std::vector<std::string> missingRasterIds(const EditParams& p);
// Export rule: every raster plane the edit uses must exist, else it is re-created (disk cache, then recomputation); if that fails the export fails.
bool ensureLocalRasters(DappleSession* s, const EditParams& p);
// A foreground exact frame never shows a silently empty AI mask: a plane of this photo that the edit references and the store no longer holds is
// re-created first. Failures are not errors of the frame.
void recreateLocalRastersSoft(DappleSession* s, const EditParams& p);

}  // namespace capi
using namespace capi;
