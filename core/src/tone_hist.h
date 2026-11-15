#pragma once
// The fine luminance histogram the tonal analysis is built from (tone.cpp), shared with the fused proxy pass of the fast analysis
// (engine/nodes/point/bake.cpp): one definition of the bin of a pixel, so both count exactly the same pixels into the same bins.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "color.h"
#include "dapple/tone.h"

namespace dapple {

constexpr int kToneFine = 512;  // fine histogram for percentiles
using ToneCounts = std::array<std::uint32_t, kToneFine>;

// The fine bin of an encoded display pixel (each channel clamped to [0, 1]): encoded luminance.
inline int toneFineBin(const float* p) {
  const float lin = 0.2126729f * color::srgbDecode(std::clamp(p[0], 0.f, 1.f)) + 0.7151522f * color::srgbDecode(std::clamp(p[1], 0.f, 1.f)) +
                    0.0721750f * color::srgbDecode(std::clamp(p[2], 0.f, 1.f));
  const float e = color::srgbEncode(lin);
  return std::min(kToneFine - 1, int(e * kToneFine));
}

// The analysis of `n` > 0 pixels counted into `fine`. Counts are integers, so any partition of the pixels summed gives the same result.
ToneAnalysis toneAnalysisFromCounts(const ToneCounts& fine, std::size_t n);

}  // namespace dapple
