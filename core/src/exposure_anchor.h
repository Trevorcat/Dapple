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
};

// `linear`: scene-referred linear ProPhoto after white balance (the decoder output, any decode quality), `dngBaselineEv`: NaN = no tag.
ExposureAnchor computeExposureAnchor(const std::filesystem::path& raw, const ImageF& linear, float dngBaselineEv);

}  // namespace dapple
