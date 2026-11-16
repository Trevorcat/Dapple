#include "session.h"
#include "../json_lite.h"

namespace capi {

// 1 when the export options ask for Display P3 ("colorSpace":"p3"), else 0.
int exportSpaceOf(const char* optionsJson) {
  return optionsJson && optionsJson[0] && jsonlite::text(optionsJson, "colorSpace", "srgb") == "p3" ? 1 : 0;
}

// Export options of the *_ex calls: {"metaSource":"<original file>","stripLocation":true,"colorSpace":"srgb","software":"Dapple 1.0"}. Writes the Exif and ICC
// segments into `jo`: the camera facts of the session (decoder metadata), the capture date, sub-second, offset and GPS read from metaSource (the file the user
// imported: HEIC / PNG conversions lose them), then the gaps filled from the session. Location only when the file has it and stripLocation is false (default
// true). False (error set) for a colour space this build cannot produce.
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
    engine::point::fastSize(engine::point::resolve(ep, *s->camera, nullptr), xstream::sizeOnly(s->lazy->meta, s->lazy->width, s->lazy->height), RenderRegion{}, fw, fh);
    if (outLongEdge > 0 && std::max(fw, fh) > outLongEdge) {
      const double k = double(outLongEdge) / std::max(fw, fh);
      fw = int(std::lround(fw * k)), fh = int(std::lround(fh * k));
    }
    quarter = std::max(1, (std::max(fw, fh) + 3) / 4);
    extra.push_back(quarter);
  }
  const double preMs = msSince(t0);
  const auto tr0 = std::chrono::steady_clock::now();
  const int r = streamRender(s, ep, lutPath, outLongEdge, onSize, sink, extra);
  if (r <= 0) return r;
  if (!metaOk) return -1;
  const double renderMs = msSince(tr0), encWaitMs = enc ? enc->waitedMs() : 0.0;
  mem::mark("encode");
  const auto tf0 = std::chrono::steady_clock::now();
  auto bytes = enc->finish();
  enc.reset();
  const double finishMs = msSince(tf0);
  if (!bytes) {
    setError(bytes.error().message);
    return -1;
  }
  bool written = false;
  double writeMs = 0;
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
    const auto tw0 = std::chrono::steady_clock::now();
    auto w = writeJpegBytes(*bytes, toPath(outPath));
    if (!w) {
      setError(w.error().message);
      return -1;
    }
    writeMs = msSince(tw0);
  }
  s->bands.reset();  // the row cache goes with the export
  smp.finish();
  stageLog("export_stream stages: before the render %.0f + render (analyses, windows, GPU, hand-off) %.0f [encoder waits inside it: %.0f] + encoder finish %.0f + file write %.0f + other %.0f = %.0f ms", preMs,
           renderMs, encWaitMs, finishMs, writeMs, msSince(t0) - preMs - renderMs - finishMs - writeMs, msSince(t0));
  stageLog("export_stream %s %dx%d%s in %.0f ms: peak footprint %.0f MB (+%.0f over the start, available at the start %.0f MB): %s", outPath ? pathToUtf8(toPath(outPath).filename()).c_str() : "",
           dims.width, dims.height, hdr ? (written ? " Ultra HDR" : " (plain)") : "", msSince(t0), double(smp.peak()) / 1048576.0, double(smp.peak() - std::min(smp.peak(), smp.base())) / 1048576.0,
           double(avail0) / 1048576.0, smp.summary().c_str());
  return 1;
}

}  // namespace capi

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
          const ExportCallScope exportCall;
          if (!renderSessionParams(session, ep, lut_path, out_long_edge, img)) {
            if (!retryInBands(session, ep)) return DAPPLE_ERROR;
            const int r = streamExportJpeg(session, ep, lut_path, out_long_edge, jpeg_quality, out_path, export_json, false, 0.f, nullptr);  // 42.9
            return r > 0 ? DAPPLE_OK : DAPPLE_ERROR;
          }
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
        {
          const ExportCallScope exportCall;
          if (!renderSessionParams(session, params, lut_path, out_long_edge, img)) {
            if (!retryInBands(session, params)) return DAPPLE_ERROR;
            const int r = streamExportJpeg(session, params, lut_path, out_long_edge, jpeg_quality, out_path, export_json, true, headroom_stops, info);  // 42.9
            return r > 0 ? DAPPLE_OK : DAPPLE_ERROR;
          }
        }
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

int32_t dapple_session_export_jpeg(DappleSession* session, const char* params_json, const char* lut_path,
                                   int32_t out_long_edge, int32_t jpeg_quality, const char* out_path) {
  return exportJpegImpl(session, params_json, lut_path, out_long_edge, jpeg_quality, out_path, nullptr);
}

int32_t dapple_session_export_jpeg_ex(DappleSession* session, const char* params_json, const char* lut_path, int32_t out_long_edge,
                                      int32_t jpeg_quality, const char* out_path, const char* export_json) {
  return exportJpegImpl(session, params_json, lut_path, out_long_edge, jpeg_quality, out_path, export_json ? export_json : "{}");
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

int32_t dapple_session_set_export_progress(DappleSession* session, DappleProgressFn fn, void* user) {
  if (!session) return DAPPLE_ERROR;
  session->exportProgress = fn;
  session->exportProgressUser = user;
  return DAPPLE_OK;
}
