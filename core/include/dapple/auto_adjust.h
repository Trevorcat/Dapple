#pragma once

#include <string>
#include <vector>

#include "dapple/camera.h"
#include "dapple/decoder.h"
#include "dapple/edit_params.h"
#include "dapple/tone.h"

namespace dapple {

struct AutoOptions {
  bool exposure = true;
  bool tone = true;          // highlights / shadows protection
  bool whiteBalance = true;  // gentle neutral correction on top of as-shot
  float strength = 1.f;      // 0..1, scales every correction
};

// Image statistics the automatic correction is based on. Also the compact
// numeric context we can hand to an AI model alongside a thumbnail.
struct ImageStats {
  float medianLuma = 0.f;        // scene-linear, after camera baseline
  float p01Luma = 0.f;
  float p99Luma = 0.f;
  float clippedFraction = 0.f;   // share of pixels at/above sensor clip
  float neutralCastA = 0.f;      // OKLab mean a/b of near-neutral pixels
  float neutralCastB = 0.f;
  float neutralCoverage = 0.f;   // share of pixels used for the cast estimate
};

struct AutoResult {
  EditParams correction;           // add on top of the user's preset/look
  ImageStats stats;
  ToneAnalysis tone;               // tonal class of the untouched render (before the correction)
  std::vector<std::string> notes;  // human readable reasons
};

// Deterministic per-photo correction (no ML). Runs on a downscaled image.
AutoResult computeAutoAdjust(const SourceImage& image, const CameraProfile& camera,
                             const AutoOptions& options = {});

}  // namespace dapple
