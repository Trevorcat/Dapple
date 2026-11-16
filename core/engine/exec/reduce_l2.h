#pragma once
// The level-2 box reduction of a linear picture (docs/engine/pipeline-architecture.md section 59): every output pixel is the mean of a 4 x 4 block of the
// picture (block grid aligned to the picture's origin, the last partial block averaged over its real pixels), float RGB, ceil(W / 4) x ceil(H / 4).
// ONE function makes it for every path (a float picture in memory, the analysis pass of a streamed export that sees the rows band by band), so all of
// them get the same bytes: a full block is ((row 0 + row 1) + (row 2 + row 3)) / 16 with each row ((x0 + x1) + (x2 + x3)) in float, a partial block (the
// picture's right / bottom edge) is summed in double and divided by its pixel count; a block never depends on the bands the rows arrive in as long as a
// band starts on a multiple of 4 rows, and disjoint row ranges may be reduced by different threads.
#include <algorithm>
#include <cstddef>
#include <vector>

namespace dapple::engine {

struct ReducedL2 {
  int w = 0, h = 0;  // ceil(W / 4) x ceil(H / 4)
  std::vector<float> rgb;
  void init(int W, int H) {
    w = (W + 3) / 4;
    h = (H + 3) / 4;
    rgb.assign(std::size_t(w) * std::size_t(h) * 3, 0.f);
  }
};

// Rows [y0, y1) of a picture W wide (`p` = row y0, `pitch` floats between rows) into `out` (init(W, H) done). y0 must be a multiple of 4; y1 a multiple of 4
// or the picture's last row + 1.
inline void reduceL2Rows(const float* p, std::size_t pitch, int W, int y0, int y1, ReducedL2& out) {
  const int fullBx = W / 4;
  for (int by = y0; by < y1; by += 4) {
    float* o = out.rgb.data() + std::size_t(by / 4) * std::size_t(out.w) * 3;
    const float* r0 = p + std::size_t(by - y0) * pitch;
    int bx = 0;
    if (by + 4 <= y1) {
      const float* r1 = r0 + pitch;
      const float* r2 = r1 + pitch;
      const float* r3 = r2 + pitch;
      for (; bx < fullBx; ++bx) {
        const std::size_t k = std::size_t(bx) * 12;
        for (int c = 0; c < 3; ++c) {
          const float t0 = (r0[k + c] + r0[k + 3 + c]) + (r0[k + 6 + c] + r0[k + 9 + c]);
          const float t1 = (r1[k + c] + r1[k + 3 + c]) + (r1[k + 6 + c] + r1[k + 9 + c]);
          const float t2 = (r2[k + c] + r2[k + 3 + c]) + (r2[k + 6 + c] + r2[k + 9 + c]);
          const float t3 = (r3[k + c] + r3[k + 3 + c]) + (r3[k + 6 + c] + r3[k + 9 + c]);
          o[std::size_t(bx) * 3 + std::size_t(c)] = ((t0 + t1) + (t2 + t3)) * 0.0625f;
        }
      }
    }
    const int ye = by + 4 < y1 ? by + 4 : y1;
    for (; bx < out.w; ++bx) {  // partial blocks: the right edge, or all of a last row of blocks that is not 4 rows high
      const int xe = bx * 4 + 4 < W ? bx * 4 + 4 : W;
      double a[3] = {0, 0, 0};
      for (int y = by; y < ye; ++y) {
        const float* r = r0 + std::size_t(y - by) * pitch;
        for (int x = bx * 4; x < xe; ++x)
          for (int c = 0; c < 3; ++c) a[c] += r[3 * std::size_t(x) + std::size_t(c)];
      }
      const double n = double((ye - by) * (xe - bx * 4));
      for (int c = 0; c < 3; ++c) o[std::size_t(bx) * 3 + std::size_t(c)] = float(a[c] / n);
    }
  }
}

}  // namespace dapple::engine
