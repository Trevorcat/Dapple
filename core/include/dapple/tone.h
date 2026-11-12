#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "dapple/camera.h"
#include "dapple/decoder.h"
#include "dapple/edit_params.h"
#include "dapple/image.h"

namespace dapple {

// Tonal classification of a photo: key (where the median sits) x range (how far apart the
// shadows and highlights are). All numbers live in display-referred, sRGB-encoded luminance
// 0..1 of the *rendered* image (camera look included), i.e. what the viewer sees.
//
//   key   low / mid / high      by the median p50
//   range short / mid / long    by the spread p95 - p5
//
// The nine classes are prototypes in (p5, p50, p95); a photo belongs to the closest one.
struct ToneClassInfo {
  const char* name;   // "low-short", ..., "high-long"
  const char* key;    // "low" | "mid" | "high"
  const char* range;  // "short" | "mid" | "long"
  float p5, p50, p95;
};

const std::array<ToneClassInfo, 9>& toneClasses();

// Index into toneClasses() for "key-range", or -1 when the name is not one of the nine.
int toneClassIndex(const std::string& name);

struct ToneAnalysis {
  static constexpr int kBins = 64;
  std::vector<float> histogram;  // kBins fractions (sum 1) over encoded luminance 0..1
  float p5 = 0.f, p50 = 0.f, p95 = 0.f;
  std::string key;      // "low" | "mid" | "high"
  std::string range;    // "short" | "mid" | "long"
  std::string nearest;  // "<key>-<range>"
  float distance = 0.f; // distance to the nearest prototype (same units as p5/p50/p95)
  float margin = 0.f;   // distance to the second-nearest minus `distance` (0 = on a class border)
};

// From a display-referred, sRGB-encoded image (values 0..1).
ToneAnalysis analyzeToneImage(const ImageF& srgbEncoded);

// Renders `source` with `look` (toneStrength and any film look are ignored) at a small proxy size and classifies it.
ToneAnalysis analyzeTone(const SourceImage& source, const CameraProfile& camera, const EditParams& look = {});

// With a film look active, replaces the histogram and percentiles of `a` (the develop render's analysis) by those of
// the film render of the same proxy, so the displayed histogram follows the film (incl. film exposure). The class
// fields (key / range / nearest / distance / margin) stay those of the develop render. No-op without film.
void applyFilmHistogram(ToneAnalysis& a, const SourceImage& source, const CameraProfile& camera,
                        const EditParams& look);

// Same replacement from a finished 8-bit RGBA frame (sub-sampled to ~256 px): the histogram of the picture that is on
// screen, which costs next to nothing, unlike a second film render of the proxy. Used for interactive film frames.
void applyFrameHistogram(ToneAnalysis& a, const std::uint8_t* rgba, int w, int h);

// Control points (0..255 both axes, like EditParams::toneCurve) of the tone curve that moves an
// image with analysis `a` towards class `target` ("" = its own nearest class) by `strength`
// (0..1). The curve works on encoded luminance, keeps 0 and 1 fixed, never lifts true blacks or
// pushes clipped highlights, and is limited to gentle moves of the three anchors. With an empty
// target only the part of the deviation beyond the class tolerance is corrected.
// Empty result = identity.
// `reach` > 1 widens the limits of the move (used for previews that must show the class
// difference clearly; edits use 1).
std::vector<std::pair<float, float>> toneTargetCurve(const ToneAnalysis& a, const std::string& target,
                                                     float strength, float reach = 1.f);

}  // namespace dapple
