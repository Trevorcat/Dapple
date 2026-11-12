#pragma once
// Per-file exposure anchor: the EV that moves this file's linear raw data to the exposure the camera itself chose to show, estimated without
// any maker or model knowledge (docs/engine/pipeline-architecture.md section 29).
//   tier 1  the embedded JPEG preview of the very frame: median over mid-tone pixels of log2(preview luminance / raw luminance), both linear, on a
//           common framing. Only this one scalar is taken from the preview (no curve, no colour).
//   tier 2  the DNG BaselineExposure tag of the file.
//   tier 3  scene statistics of the raw luminance (median, highlight protected), low confidence.
//   tier 4  nothing usable: 0 EV, confidence 0.
#include <filesystem>
#include <string>

#include "dapple/image.h"

namespace dapple {

struct ExposureAnchor {
  int tier = 4;
  float ev = 0.f;
  float confidence = 0.f;  // 0..1
  std::string note;        // e.g. "preview 1616x1080, 4210 mid-tone px, spread 0.21 EV"
  // diagnostics (filled whenever computable, whatever tier won)
  float previewEv = 0.f, previewConfidence = 0.f;
  int previewPixels = 0;
  float previewSpread = 0.f;
  float statsEv = 0.f;
  std::string source;  // "embedded-preview", "reference-image" or "" (tag / statistics)
};

// `linear`: scene-referred linear ProPhoto after white balance (the decoder output, any decode quality), `dngBaselineEv`: NaN = no tag.
// `reference`: an upright 8-bit sRGB rendering of this frame to use instead of the embedded preview (paired camera JPEG / sidecar), or null.
// `monochrome`: single-channel sensor. The camera's black-and-white rendering has a much steeper tone curve than sRGB (a Pentax K-3 III Monochrome
// preview spans 0.018 .. 0.86 linear where the raw spans 0.46 .. 0.67), so the per-pixel mid-tone ratio of tier 1 is meaningless; the witness is the
// ratio of the MEDIAN luminances of the two images (one number from the camera, the same on both sample files: -1.02 / -1.06 EV), accepted when
// the two images are rank-correlated (Spearman >= 0.5, i.e. they show the same picture).
// `embedded`: the embedded preview when the caller already extracted it (the decoder does this on a second thread while LibRaw works); null: extracted here.
ExposureAnchor computeExposureAnchor(const std::filesystem::path& raw, const ImageF& linear, float dngBaselineEv, const Image8* reference = nullptr, const Image8* embedded = nullptr,
                                  bool monochrome = false);

}  // namespace dapple
