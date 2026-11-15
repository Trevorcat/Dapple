#include "session.h"
#include "dapple/merge.h"
#include "dapple/motion_photo.h"
#include "../json_lite.h"

namespace {

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
