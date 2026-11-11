#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dapple {

// Interleaved RGB float image. The colour space is defined by where the image
// sits in the pipeline (see docs/architecture.md); decoders always hand out
// linear ProPhoto RGB (ROMM, D50 white).
struct ImageF {
  int width = 0;
  int height = 0;
  std::vector<float> data;  // width * height * 3

  ImageF() = default;
  ImageF(int w, int h) : width(w), height(h), data(std::size_t(w) * h * 3, 0.f) {}

  bool empty() const { return width <= 0 || height <= 0; }
  float* row(int y) { return data.data() + std::size_t(y) * width * 3; }
  const float* row(int y) const { return data.data() + std::size_t(y) * width * 3; }
};

// 8-bit sRGB output image.
struct Image8 {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> data;  // width * height * 3
};

// Area-average downscale so the longer edge equals `longEdge`.
// Returns a copy when the image is already small enough or longEdge <= 0.
ImageF resizeToLongEdge(const ImageF& src, int longEdge);

}  // namespace dapple
