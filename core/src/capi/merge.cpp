#include "session.h"
#include "dapple/merge.h"
#include "../json_lite.h"

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

int32_t dapple_merge_available(void) { return guarded([&]() -> int32_t { return mergeAvailable() ? 1 : 0; }, 0); }

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
        if (!ensureSource(session)) return DAPPLE_ERROR;
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
