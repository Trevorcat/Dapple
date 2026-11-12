#pragma once
// Shared by the start-up self-test, the parity tests and `dapple gpu-bench`: a synthetic scene-referred test picture and
// the Delta E 2000 comparison of two RGBA8 buffers.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "dapple/decoder.h"

namespace dapple::gpu {

struct DeStats {
  double mean = 0, p99 = 0, max = 0;
  double differing = 0;  // fraction of pixels with any channel different
  int maxChan = 0;
  std::size_t maxAt = 0;  // pixel index of the largest difference
};

// RGBA8 vs RGBA8 (alpha ignored); multi-threaded for large images.
DeStats compareRgba8(const std::uint8_t* a, const std::uint8_t* b, std::size_t npx);

// Gradients, colour patches, edges, fine texture and a few bright saturated spots (they leave the first lattice).
// Linear ProPhoto, scene referred (1.0 = clipping). Deterministic.
SourceImage makeTestImage(int w, int h);

// RGBA8 from a float RGB image (0..1).
std::vector<std::uint8_t> toRgba8(const ImageF& f);

}  // namespace dapple::gpu
