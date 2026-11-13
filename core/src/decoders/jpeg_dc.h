// Reduced-size JPEG decode for statistics that need only a small picture (the exposure anchor): baseline / extended sequential Huffman JPEG,
// one interleaved scan, returned at 1/4 scale: each pixel is the mean of a 4x4 sub-block, rebuilt from the four lowest DCT coefficients (0,0) (1,0) (0,1) (1,1). The Huffman pass is the only
// cost: no full IDCT, no 8x8 expansion, no colour conversion of the full frame. Anything else (progressive, arithmetic, 12 bit, several scans) returns
// false and the caller decodes in full.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dapple {

struct JpegDcImage {
  int width = 0, height = 0;          // 1/4 of the JPEG size, rounded up
  int fullWidth = 0, fullHeight = 0;  // size of the JPEG itself
  std::vector<std::uint8_t> rgb;      // width * height * 3
};

bool decodeJpegDcScale(const std::uint8_t* data, std::size_t size, JpegDcImage& out);

// Size of a JPEG (SOF) without decoding it; false when it cannot be read.
bool jpegSize(const std::uint8_t* data, std::size_t size, int& width, int& height);

}  // namespace dapple
