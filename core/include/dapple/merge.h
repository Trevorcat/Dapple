#pragma once
// Multi-frame merging: HDR bracket merge, panorama stitching, and the helpers
// that find bracket / panorama candidates in a list of files.
//
// All merge outputs are ordinary SourceImage objects (linear ProPhoto float,
// scene-referred for RAW input), so the normal develop pipeline (tone, styles,
// every edit parameter) applies to them unchanged.
//
// HDR alignment and panorama stitching need OpenCV (Apache-2.0, optional at
// build time: CMake option DAPPLE_WITH_OPENCV, macro DAPPLE_HAVE_OPENCV).
// Without it, mergeHdr still merges (no alignment) and stitchPanorama returns
// an error. `mergeAvailable()` tells which.

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "dapple/decoder.h"
#include "dapple/image.h"
#include "dapple/metadata.h"
#include "dapple/result.h"

namespace dapple {

// ---------------------------------------------------------------------------
// Progress / cancellation
// ---------------------------------------------------------------------------
struct MergeControl {
  // fraction in [0,1]; stage is a short stable key: "load", "align", "merge",
  // "deghost", "features", "match", "estimate", "warp", "seams", "blend", "crop", "done".
  std::function<void(float fraction, const char* stage)> progress;
  // Set to true from another thread to abort; the call then fails with "cancelled".
  const std::atomic<bool>* cancel = nullptr;
  // Optional second source (the C API bridges its callback / int flag through this). Polled often: keep it cheap.
  std::function<bool()> shouldCancel;

  bool cancelled() const {
    return (cancel && cancel->load(std::memory_order_relaxed)) || (shouldCancel && shouldCancel());
  }
  void report(float f, const char* stage) const {
    if (progress) progress(f < 0 ? 0 : f > 1 ? 1 : f, stage);
  }
};

// True when this build can align HDR frames and stitch panoramas (OpenCV linked).
bool mergeAvailable();

// ---------------------------------------------------------------------------
// HDR merge
// ---------------------------------------------------------------------------
enum class Deghost { Off, Low, Mid, High };
const char* toString(Deghost d);
Deghost deghostFromString(const std::string& s, Deghost fallback = Deghost::Mid);

struct HdrOptions {
  int baseIndex = -1;                 // frame whose pixels/geometry win; -1 = middle exposure
  Deghost deghost = Deghost::Mid;
  bool align = true;                  // false for tripod shots (saves time)
  DecodeQuality quality = DecodeQuality::Full;
  int longEdge = 0;                   // >0: downscale every frame (preview); 0 = native
};

struct HdrFrameReport {
  std::string path;
  double evExif = 0;        // stops relative to the base frame from shutter*ISO/aperture^2; NaN if unknown
  double evUsed = 0;        // stops relative to the base used for normalisation
  bool evFromData = false;  // evUsed was measured from pixels (EXIF absent or inconsistent)
  bool aligned = false;     // a non-identity warp was applied
  double alignMethod = 0;   // 0 none, 1 identity-ECC, 2 features, 3 features+ECC
  double alignScore = 0;    // ECC correlation (0..1) or feature inlier count
  double shiftPx = 0;       // mean displacement of the 4 image corners (full-res px)
  double ghostFraction = 0; // fraction of pixels replaced by the base frame
};

struct HdrReport {
  int baseIndex = 0;
  bool exposureFusion = false;      // JPEG/display-referred inputs: Mertens fusion fallback
  std::vector<HdrFrameReport> frames;
  double maxValue = 0;              // largest merged value (1.0 = base-frame clipping point)
  double secLoad = 0, secAlign = 0, secMerge = 0;
  std::vector<std::string> warnings;
};

struct HdrResult {
  SourceImage image;  // linear float, 1.0 == base-frame sensor clipping point, values may exceed 1
  HdrReport report;
};

// 2..9 files. Order does not matter (the base is chosen by exposure).
Result<HdrResult> mergeHdr(const std::vector<std::filesystem::path>& files, const HdrOptions& options = {},
                           const MergeControl& control = {});

// Same, for frames that are already decoded (tests, apps that cache decodes).
Result<HdrResult> mergeHdrImages(std::vector<SourceImage> frames, const HdrOptions& options = {},
                                 const MergeControl& control = {});

// ---------------------------------------------------------------------------
// Panorama
// ---------------------------------------------------------------------------
enum class PanoProjection { Spherical, Cylindrical, Perspective };
const char* toString(PanoProjection p);
PanoProjection panoProjectionFromString(const std::string& s, PanoProjection fallback = PanoProjection::Cylindrical);

struct PanoOptions {
  PanoProjection projection = PanoProjection::Cylindrical;
  bool crop = true;            // crop to the largest inner axis-aligned rectangle
  bool fill = false;           // inpaint (Telea) the empty edges instead of leaving black; ignored if crop
  int baseIndex = -1;          // frame placed at the projection centre; -1 = automatic (wave correction)
  bool exposureCompensation = true;
  DecodeQuality quality = DecodeQuality::Full;
  int longEdge = 0;            // >0: cap the long edge of the *output* (preview); 0 = native resolution
  double matchConfidence = 0.3;  // feature matcher confidence; lower accepts weaker overlaps
};

struct PanoFrameReport {
  std::string path;
  double focalPx = 0;          // estimated focal length in registration-scale pixels (same for all)
  double yawDeg = 0, pitchDeg = 0, rollDeg = 0;  // rotation relative to frame 0 / base
  int inliers = 0;             // inliers of the best match with another frame
};

struct PanoReport {
  PanoProjection projection = PanoProjection::Cylindrical;
  int framesUsed = 0;
  std::vector<int> usedIndices;  // indices into the input list that made it into the panorama
  std::vector<PanoFrameReport> frames;
  int canvasWidth = 0, canvasHeight = 0;  // before cropping
  int cropX = 0, cropY = 0;               // crop rectangle in canvas pixels (0,0,canvasW,canvasH when not cropped)
  double validFraction = 0;               // fraction of the output covered by image data
  double secLoad = 0, secFeatures = 0, secEstimate = 0, secCompose = 0;
  std::vector<std::string> warnings;
};

struct PanoResult {
  SourceImage image;  // linear float
  PanoReport report;
};

Result<PanoResult> stitchPanorama(const std::vector<std::filesystem::path>& files, const PanoOptions& options = {},
                                  const MergeControl& control = {});
Result<PanoResult> stitchPanoramaImages(std::vector<SourceImage> frames, const PanoOptions& options = {},
                                        const MergeControl& control = {});

// ---------------------------------------------------------------------------
// Archival output of merged results
// ---------------------------------------------------------------------------
// 16-bit linear RGB TIFF (ProPhoto primaries, linear light). Values are divided by `headroom`
// (power of two >= the maximum, chosen automatically when 0) before quantisation; the headroom is
// stored in the ImageDescription so readers can restore it.
Result<void> writeLinearTiff16(const ImageF& image, const std::filesystem::path& path, float headroom = 0.f);

// Linear DNG (LinearRaw, 16-bit, ProPhoto D50 as the "camera" space, BaselineExposure = log2(headroom)).
// Opens in Lightroom/Camera Raw and in Dapple's own decoder.
Result<void> writeLinearDng(const SourceImage& image, const std::filesystem::path& path, float headroom = 0.f);

// ---------------------------------------------------------------------------
// Sequence detection
// ---------------------------------------------------------------------------
struct FrameInfo {
  std::filesystem::path path;
  ShotMetadata meta;
  double exposureStops = 0;   // log2(shutter * iso / aperture^2): larger = brighter capture
  bool exposureKnown = false;
  // Sony maker-note sequence data (LibRaw), when present.
  bool hasSequence = false;
  long long sequenceImageNumber = 0;  // running number of the shooting sequence
  int sequenceLength = 0;             // shots in the sequence
  long long sequenceFileNumber = 0;
  int releaseMode = -1;               // Sony ReleaseMode2: 2/23 exposure bracketing, 9 HDR, 7/15 sweep panorama
  std::string orientationKey;         // sensor w x h
};

Result<FrameInfo> probeFrame(const std::filesystem::path& path);

struct SequenceGroup {
  enum class Kind { Bracket, Panorama } kind = Kind::Bracket;
  std::vector<int> indices;            // indices into the input list, capture order
  std::vector<std::string> paths;
  std::vector<double> evSteps;         // per frame, stops relative to the group's first frame
  long long startTime = 0, endTime = 0;
  double confidence = 0;               // 0..1
  std::string reason;                  // human readable evidence
};

struct DetectOptions {
  double maxGapSeconds = 4.0;          // brackets: max time between consecutive frames
  double panoGapSeconds = 10.0;        // panoramas: max time between consecutive frames
  double minEvStep = 0.7;              // brackets: smallest step between neighbouring exposures (without camera confirmation)
  double minEvSpread = 1.0;            // brackets: smallest total exposure range (without camera confirmation)
  double minPanoOverlap = 0.10;        // panoramas: overlap of neighbouring frames (fraction of the frame)
  double maxPanoOverlap = 0.72;        // higher = a re-framed burst, not a pan
  bool verifyContent = true;           // use embedded previews: bracket = same view, pano = partial overlap
};

// `files` may be any mix of supported images in any order; frames are sorted by capture time internally.
// Groups never share frames; brackets win over panoramas for the same frames.
std::vector<SequenceGroup> detectSequences(const std::vector<FrameInfo>& frames, const DetectOptions& options = {},
                                           const MergeControl& control = {});

}  // namespace dapple
