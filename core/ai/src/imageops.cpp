#include "imageops.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dapple::ai::img {
namespace {
constexpr int kPrecisionBits = 32 - 8 - 2;

struct Coeffs {
  std::vector<int> bounds;   // xmin, xcount per output
  std::vector<int32_t> k;    // kmax per output
  int kmax = 0;
};

// Pillow Resample.c precompute_coeffs + normalize_coeffs_8bpc, bilinear (support 1).
Coeffs precompute(int inSize, int outSize) {
  Coeffs c;
  const double scale = double(inSize) / outSize;
  const double filterscale = std::max(scale, 1.0);
  const double support = 1.0 * filterscale;
  c.kmax = int(std::ceil(support)) * 2 + 1;
  c.bounds.resize(size_t(outSize) * 2);
  c.k.assign(size_t(outSize) * size_t(c.kmax), 0);
  std::vector<double> kk(size_t(c.kmax));
  for (int xx = 0; xx < outSize; ++xx) {
    const double center = (xx + 0.5) * scale;
    const double ss = 1.0 / filterscale;
    int xmin = int(center - support + 0.5);
    if (xmin < 0) xmin = 0;
    int xmax = int(center + support + 0.5);
    if (xmax > inSize) xmax = inSize;
    xmax -= xmin;
    double ww = 0.0;
    std::fill(kk.begin(), kk.end(), 0.0);
    for (int x = 0; x < xmax; ++x) {
      double v = (x + xmin - center + 0.5) * ss;
      v = v < 0 ? -v : v;
      const double w = v < 1.0 ? 1.0 - v : 0.0;
      kk[size_t(x)] = w;
      ww += w;
    }
    for (int x = 0; x < xmax; ++x) {
      const double w = ww != 0.0 ? kk[size_t(x)] / ww : kk[size_t(x)];
      c.k[size_t(xx) * size_t(c.kmax) + size_t(x)] = int32_t(w < 0 ? -0.5 + w * (1 << kPrecisionBits) : 0.5 + w * (1 << kPrecisionBits));
    }
    c.bounds[size_t(xx) * 2] = xmin;
    c.bounds[size_t(xx) * 2 + 1] = xmax;
  }
  return c;
}

inline uint8_t clip8(int32_t v) {
  v >>= kPrecisionBits;
  return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v);
}
}  // namespace

Rgb resizeBilinear(const uint8_t* src, int sw, int sh, int stride, int dw, int dh) {
  Rgb out;
  out.w = dw;
  out.h = dh;
  out.px.resize(size_t(dw) * size_t(dh) * 3);
  // horizontal pass into a sw->dw intermediate (PIL: only when the width changes), then vertical.
  std::vector<uint8_t> mid;
  const uint8_t* cur = src;
  int curStride = stride;
  if (dw != sw) {
    const Coeffs ch = precompute(sw, dw);
    mid.resize(size_t(dw) * size_t(sh) * 3);
    for (int y = 0; y < sh; ++y) {
      const uint8_t* row = src + size_t(y) * size_t(stride);
      uint8_t* o = mid.data() + size_t(y) * size_t(dw) * 3;
      for (int x = 0; x < dw; ++x) {
        const int xmin = ch.bounds[size_t(x) * 2], xn = ch.bounds[size_t(x) * 2 + 1];
        const int32_t* k = ch.k.data() + size_t(x) * size_t(ch.kmax);
        int32_t s0 = 1 << (kPrecisionBits - 1), s1 = s0, s2 = s0;
        for (int i = 0; i < xn; ++i) {
          const uint8_t* p = row + size_t(xmin + i) * 3;
          s0 += p[0] * k[i];
          s1 += p[1] * k[i];
          s2 += p[2] * k[i];
        }
        o[x * 3] = clip8(s0);
        o[x * 3 + 1] = clip8(s1);
        o[x * 3 + 2] = clip8(s2);
      }
    }
    cur = mid.data();
    curStride = dw * 3;
  }
  if (dh != sh) {
    const Coeffs cv = precompute(sh, dh);
    for (int y = 0; y < dh; ++y) {
      const int ymin = cv.bounds[size_t(y) * 2], yn = cv.bounds[size_t(y) * 2 + 1];
      const int32_t* k = cv.k.data() + size_t(y) * size_t(cv.kmax);
      uint8_t* o = out.px.data() + size_t(y) * size_t(dw) * 3;
      for (int x = 0; x < dw * 3; ++x) {
        int32_t s = 1 << (kPrecisionBits - 1);
        for (int i = 0; i < yn; ++i) s += cur[size_t(ymin + i) * size_t(curStride) + size_t(x)] * k[i];
        o[x] = clip8(s);
      }
    }
  } else {
    for (int y = 0; y < dh; ++y) std::memcpy(out.px.data() + size_t(y) * size_t(dw) * 3, cur + size_t(y) * size_t(curStride), size_t(dw) * 3);
  }
  return out;
}

Rgb letterboxSquare(const Image& im, int& ox, int& oy) {
  const int s = std::max(im.w, im.h);
  ox = (s - im.w) / 2;
  oy = (s - im.h) / 2;
  Rgb sq;
  sq.w = sq.h = s;
  sq.px.assign(size_t(s) * size_t(s) * 3, 0);
  for (int y = 0; y < im.h; ++y)
    std::memcpy(sq.px.data() + (size_t(y + oy) * size_t(s) + size_t(ox)) * 3, im.rgb + size_t(y) * size_t(im.stride), size_t(im.w) * 3);
  return sq;
}

Rgb affineBilinear(const Image& im, const double a[6], int dw, int dh) {
  Rgb out;
  out.w = dw;
  out.h = dh;
  out.px.assign(size_t(dw) * size_t(dh) * 3, 0);
  auto at = [&](int x, int y, int c) -> double {
    x = x < 0 ? 0 : x >= im.w ? im.w - 1 : x;
    y = y < 0 ? 0 : y >= im.h ? im.h - 1 : y;
    return im.rgb[size_t(y) * size_t(im.stride) + size_t(x) * 3 + size_t(c)];
  };
  for (int y = 0; y < dh; ++y) {
    const double xo = a[2] + a[1] * (y + 0.5) + a[0] * 0.5;
    const double yo = a[5] + a[4] * (y + 0.5) + a[3] * 0.5;
    for (int x = 0; x < dw; ++x) {
      double xin = xo + a[0] * x, yin = yo + a[3] * x;
      if (xin < 0.0 || xin >= im.w || yin < 0.0 || yin >= im.h) continue;  // outside: 0
      xin -= 0.5;
      yin -= 0.5;
      const int x0 = int(std::floor(xin)), y0 = int(std::floor(yin));
      const double dx = xin - x0, dy = yin - y0;
      for (int c = 0; c < 3; ++c) {
        const double v1 = at(x0, y0, c) * (1.0 - dx) + at(x0 + 1, y0, c) * dx;
        const double v2 = at(x0, y0 + 1, c) * (1.0 - dx) + at(x0 + 1, y0 + 1, c) * dx;
        const double v = v1 + dy * (v2 - v1);
        out.px[(size_t(y) * size_t(dw) + size_t(x)) * 3 + size_t(c)] = uint8_t(v);  // PIL truncates (verified by the tests against PIL)
      }
    }
  }
  return out;
}

}  // namespace dapple::ai::img
