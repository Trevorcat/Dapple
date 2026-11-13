#pragma once
// The arithmetic of resizeToLongEdge (image.cpp) one output row at a time, so that a streaming export (core/src/export_stream.cpp) can build the
// same reduced copies band by band from a picture that never exists whole: the same footprint, the same weights and the same summation order,
// hence bit-identical results (test `export_stream.reductions`). Internal header.
#include <algorithm>
#include <cmath>

#include "dapple/image.h"

namespace dapple::resize {

struct Geom {
  int srcW = 0, srcH = 0, dw = 0, dh = 0;
  double sx = 1, sy = 1;
  bool copy = true;  // resizeToLongEdge returns the source unchanged
};

inline Geom geom(int w, int h, int longEdge) {
  Geom g;
  g.srcW = w, g.srcH = h, g.dw = w, g.dh = h;
  const int srcLong = std::max(w, h);
  if (longEdge <= 0 || srcLong <= longEdge || w <= 0 || h <= 0) return g;
  const double scale = double(longEdge) / srcLong;
  g.dw = std::max(1, int(std::lround(w * scale)));
  g.dh = std::max(1, int(std::lround(h * scale)));
  g.sx = double(w) / g.dw;
  g.sy = double(h) / g.dh;
  g.copy = false;
  return g;
}

// Source rows [y0, y1) output row y reads.
inline void rowSpan(const Geom& g, int y, int& y0, int& y1) {
  const double a = y * g.sy, b = (y + 1) * g.sy;
  y0 = int(a);
  y1 = std::min(g.srcH, int(std::ceil(b)));
}

// Output row y into out (dw * 3 floats). rowAt(yy) returns source row yy (srcW * 3 floats).
template <class RowAt>
inline void row(const Geom& g, int y, RowAt&& rowAt, float* out) {
  const double y0 = y * g.sy, y1 = (y + 1) * g.sy;
  const int iy0 = int(y0), iy1 = std::min(g.srcH, int(std::ceil(y1)));
  for (int x = 0; x < g.dw; ++x) {
    const double x0 = x * g.sx, x1 = (x + 1) * g.sx;
    const int ix0 = int(x0), ix1 = std::min(g.srcW, int(std::ceil(x1)));
    double acc[3] = {0, 0, 0}, wsum = 0;
    for (int yy = iy0; yy < iy1; ++yy) {
      const double wy = std::min<double>(yy + 1, y1) - std::max<double>(yy, y0);
      const float* in = rowAt(yy);
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
}

}  // namespace dapple::resize
