#pragma once
// Image resampling that reproduces PIL bit for bit (Pillow's 8-bit RGB Image.resize(BILINEAR) with its antialiasing support scaling,
// fixed-point 22-bit coefficients, horizontal pass first; Image.transform(AFFINE, BILINEAR)), because the task wrappers must feed the models
// exactly what the desktop reference feeds them. Internal.
#include <cstdint>
#include <vector>

#include "ai/tasks.h"

namespace dapple::ai::img {

struct Rgb {
  int w = 0, h = 0;
  std::vector<uint8_t> px;  // w*h*3
};

// PIL: im.resize((dw, dh), Image.BILINEAR)
Rgb resizeBilinear(const uint8_t* src, int sw, int sh, int stride, int dw, int dh);
inline Rgb resizeBilinear(const Image& im, int dw, int dh) { return resizeBilinear(im.rgb, im.w, im.h, im.stride, dw, dh); }

// Pastes `im` centred on a black s x s square (s = max(w, h); offsets (s-w)/2, (s-h)/2 as integer floor) as PIL does. Returns the square.
Rgb letterboxSquare(const Image& im, int& ox, int& oy);

// PIL: im.transform((dw, dh), Image.AFFINE, coeff, Image.BILINEAR): output (x,y) samples the input at (a*(x+.5)+b*(y+.5)+c, d*(x+.5)+e*(y+.5)+f);
// outside the input = 0.
Rgb affineBilinear(const Image& im, const double coeff[6], int dw, int dh);

}  // namespace dapple::ai::img
