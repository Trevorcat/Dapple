// Dequantisation of an 8-bit decoded picture (docs/engine/pipeline-architecture.md, film section "8-bit sources"). A smooth gradient stored in 8 bits is a
// staircase (a JPEG's steps are also rounded by the DCT quantiser); a film's contrast and saturation make those steps bands. Where the neighbourhood of a pixel
// varies by at most about 1.5 code values the pixel is replaced by an edge-limited smooth value; texture and edges are left as decoded.
//
// Work is in the encoded 8-bit code values (0..255, one float per channel), so the thresholds are in LSBs of the source whatever its colour space:
//   1. flatness weight per pixel: range (max - min) of its 3x3 neighbourhood in every channel; 1 up to 1.5 codes, 0 from 3 codes (smoothstep-free, linear);
//   2. a coarse grid (block = long edge / 150 px, so a block covers about a JPEG step of a typical sky or wall gradient at any size): the flatness-weighted
//      block means, smoothed twice by a 3x3 average that only takes in neighbours within 1.5 codes;
//   3. per pixel: the joint-bilateral bilinear value of the coarse grid (cells further than 2.5 codes from the pixel itself do not count); it replaces the
//      pixel by weight flatness x (1 for a deviation up to 1 code, 0 from 2 codes).
// A picture below 24 px on a side, or one with no flat pixel, is returned as decoded.
#pragma once
#include <cstdint>
#include <vector>

namespace dapple {

class Deband8 {
 public:
  // `rgb` is w x h x 3 interleaved 8-bit codes and must outlive the object. prepare() runs the analysis passes (parallel).
  Deband8(const std::uint8_t* rgb, int w, int h);
  // True when at least one pixel can change.
  bool active() const { return active_; }
  // The smoothed code values (0..255, fractional) of pixel (x, y); equals the decoded codes where the weight is 0.
  void pixel(int x, int y, float out[3]) const;

 private:
  const std::uint8_t* px_;
  int w_, f_ = 1, cw_ = 0, ch_ = 0;
  bool active_ = false;
  std::vector<std::uint8_t> gate_;  // flatness weight 0..255 per pixel
  std::vector<float> grid_;         // cw x ch x 3 smoothed coarse values; < 0 = invalid
};

// Convenience for tests: the whole picture as float codes.
std::vector<float> debandCodes(const std::uint8_t* rgb, int w, int h);

}  // namespace dapple
