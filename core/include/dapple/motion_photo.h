#pragma once
// Google Motion Photo (format 1.0, developer.android.com/media/platform/motion-photo-format):
// a primary JPEG whose XMP describes a Container directory, with an MP4 appended as the last item.
//
// Approach (no video re-encoding, no FFmpeg):
//  * trimMp4 cuts a window out of an MP4/MOV by *stream copy* at keyframes: the sample tables of
//    the video track (and an AAC audio track when present) are rebuilt for the selected samples
//    and written into a fresh fast-start MP4. The window therefore snaps to keyframes (GOP length
//    of the source, ~1 s for Sony XAVC S), so the clip is "about" the requested length.
//  * buildMotionPhoto takes ready JPEG bytes + ready MP4 bytes (e.g. a clip trimmed on the device by
//    MediaCodec / AVFoundation, which is the recommended path on phones) and writes the container.
//  * validateMotionPhoto checks a file against the spec and reports every problem it finds.
// Raw LPCM audio (Sony XAVC S) is not allowed in Motion Photos and is dropped; AAC is kept.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "dapple/result.h"

namespace dapple {

struct VideoInfo {
  double durationSec = 0;
  int width = 0, height = 0;
  int rotationDeg = 0;          // from the track matrix (0/90/180/270)
  double fps = 0;
  std::string videoCodec;       // fourcc of the sample entry, e.g. "avc1", "hvc1"
  std::string audioCodec;       // "" when no audio track; "mp4a" = AAC; "twos"/"sowt"/"lpcm" = PCM
  std::vector<double> keyframesSec;  // presentation times of sync samples (the possible cut points)
  bool streamCopyOk = false;    // codec is H.264 / HEVC / AV1 and tables are parseable
};

Result<VideoInfo> probeVideo(const std::filesystem::path& mp4);

struct TrimOptions {
  double startSec = 0;          // requested window start (source presentation time)
  double durationSec = 3.0;     // requested window length
  bool keepAudio = true;        // keeps AAC audio; PCM audio is always dropped
};

struct TrimResult {
  std::vector<std::uint8_t> mp4;
  double actualStartSec = 0;    // source presentation time of the first kept frame
  double actualDurationSec = 0;
  int videoFrames = 0;
  bool audioKept = false;
  std::string note;             // e.g. "PCM audio dropped"
};

Result<TrimResult> trimMp4(const std::filesystem::path& input, const TrimOptions& options = {});

struct MotionPhotoOptions {
  // Presentation time (microseconds, within the clip) of the video frame that matches the still;
  // -1 = unset (readers use the middle of the clip).
  std::int64_t presentationTimestampUs = -1;
  // Also write the legacy Camera:MicroVideo* attributes (ignored by v1 readers, helps older galleries).
  bool legacyMicroVideo = false;
};

// jpeg: a complete JPEG (EXIF kept, any existing XMP packet is replaced). mp4: a complete MP4/MOV.
Result<std::vector<std::uint8_t>> buildMotionPhoto(const std::vector<std::uint8_t>& jpeg,
                                                  const std::vector<std::uint8_t>& mp4,
                                                  const MotionPhotoOptions& options = {});

struct MotionPhotoInfo {
  bool valid = false;
  std::vector<std::string> problems;   // empty when valid
  std::vector<std::string> notes;      // non-fatal observations
  std::uint64_t fileSize = 0;
  std::uint64_t jpegLength = 0;        // up to and including EOI
  std::uint64_t videoOffset = 0, videoLength = 0;
  std::int64_t presentationTimestampUs = -1;
  int version = 0;
  std::string videoMime;
  double videoDurationSec = 0;
  int videoWidth = 0, videoHeight = 0;
  std::string videoCodec, audioCodec;
  // Ultra HDR cover (gain map between the primary and the video): checked against the XMP directory, the MPF index and the bytes
  bool hasGainMap = false;
  std::uint64_t gainMapOffset = 0, gainMapLength = 0;
};

MotionPhotoInfo validateMotionPhoto(const std::vector<std::uint8_t>& file);
Result<MotionPhotoInfo> validateMotionPhotoFile(const std::filesystem::path& file);

struct MotionFromVideoRequest {
  std::filesystem::path video;      // source MP4/MOV (stream-copied)
  std::filesystem::path coverJpeg;  // the still: developed JPEG (grab the frame on the platform or use a RAW still)
  std::filesystem::path output;     // should end in "MP.jpg"
  double startSec = 0;
  double durationSec = 3.0;
  double coverSec = -1;             // source time of the cover frame; <0 = middle of the window
  bool keepAudio = true;
  bool legacyMicroVideo = false;
};

struct MotionFromVideoResult {
  MotionPhotoInfo info;
  TrimResult trim;
  double coverInClipSec = 0;
};

Result<MotionFromVideoResult> createMotionPhotoFromVideo(const MotionFromVideoRequest& request);

// JPEG + already trimmed MP4 on disk -> Motion Photo on disk (the on-device trim path).
Result<MotionPhotoInfo> writeMotionPhoto(const std::filesystem::path& jpeg, const std::filesystem::path& mp4,
                                         const std::filesystem::path& output, const MotionPhotoOptions& options = {});

}  // namespace dapple
