#include "dapple/image.h"

#include <algorithm>
#include <cmath>

#include "parallel.h"

namespace dapple {

ImageF resizeToLongEdge(const ImageF& src, int longEdge) {
  const int srcLong = std::max(src.width, src.height);
  if (longEdge <= 0 || srcLong <= longEdge || src.empty()) return src;

  const double scale = double(longEdge) / srcLong;
  const int dw = std::max(1, int(std::lround(src.width * scale)));
  const int dh = std::max(1, int(std::lround(src.height * scale)));
  ImageF dst(dw, dh);
  const double sx = double(src.width) / dw;
  const double sy = double(src.height) / dh;

  // Box filter over the exact source footprint with fractional edge weights.
  parallelRows(dh, [&](int y) {
    const double y0 = y * sy, y1 = (y + 1) * sy;
    const int iy0 = int(y0), iy1 = std::min(src.height, int(std::ceil(y1)));
    float* out = dst.row(y);
    for (int x = 0; x < dw; ++x) {
      const double x0 = x * sx, x1 = (x + 1) * sx;
      const int ix0 = int(x0), ix1 = std::min(src.width, int(std::ceil(x1)));
      double acc[3] = {0, 0, 0}, wsum = 0;
      for (int yy = iy0; yy < iy1; ++yy) {
        const double wy = std::min<double>(yy + 1, y1) - std::max<double>(yy, y0);
        const float* in = src.row(yy);
        for (int xx = ix0; xx < ix1; ++xx) {
          const double w = wy * (std::min<double>(xx + 1, x1) - std::max<double>(xx, x0));
          acc[0] += w * in[3 * xx];
          acc[1] += w * in[3 * xx + 1];
          acc[2] += w * in[3 * xx + 2];
          wsum += w;
        }
      }
      for (int c = 0; c < 3; ++c) out[3 * x + c] = float(acc[c] / wsum);
    }
  });
  return dst;
}

}  // namespace dapple
