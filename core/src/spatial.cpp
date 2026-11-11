#include "spatial.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "parallel.h"

namespace dapple {

namespace {

using Plane = std::vector<float>;

// Reusable full-size planes (page-faulting fresh 10 MB buffers costs more than the filters).
Plane& arena(int slot) {
  static thread_local Plane planes[6];
  return planes[slot];
}

constexpr float kR = 0.2126729f, kG = 0.7151522f, kB = 0.0721750f;

inline float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

// --------------------------------------------------------------------------- box blur ---

// Horizontal running-sum blur with replicated edges.
void boxH(const float* src, float* dst, int w, int h, int r) {
  const float inv = 1.f / float(2 * r + 1);
  parallelFor(
      h,
      [&](int y) {
        const float* s = src + std::size_t(y) * w;
        float* d = dst + std::size_t(y) * w;
        float sum = float(r + 1) * s[0];
        for (int i = 1; i <= r; ++i) sum += s[std::min(i, w - 1)];
        for (int x = 0; x < w; ++x) {
          d[x] = sum * inv;
          sum += s[std::min(x + r + 1, w - 1)] - s[std::max(x - r, 0)];
        }
      },
      4);
}

// Vertical running-sum blur, processing strips of columns so memory access stays contiguous.
void boxV(const float* src, float* dst, int w, int h, int r) {
  constexpr int kStrip = 64;
  const float inv = 1.f / float(2 * r + 1);
  const int strips = (w + kStrip - 1) / kStrip;
  parallelFor(strips, [&](int sIdx) {
    const int x0 = sIdx * kStrip, n = std::min(kStrip, w - x0);
    float sum[kStrip];
    const float* top = src + x0;
    for (int j = 0; j < n; ++j) sum[j] = float(r + 1) * top[j];
    for (int i = 1; i <= r; ++i) {
      const float* row = src + std::size_t(std::min(i, h - 1)) * w + x0;
      for (int j = 0; j < n; ++j) sum[j] += row[j];
    }
    for (int y = 0; y < h; ++y) {
      float* d = dst + std::size_t(y) * w + x0;
      const float* add = src + std::size_t(std::min(y + r + 1, h - 1)) * w + x0;
      const float* sub = src + std::size_t(std::max(y - r, 0)) * w + x0;
      for (int j = 0; j < n; ++j) {
        d[j] = sum[j] * inv;
        sum[j] += add[j] - sub[j];
      }
    }
  });
}

// In-place blur: `iters` passes of box(r) in both directions (2 passes ~ triangle, 3 ~ gaussian).
void boxBlur(Plane& data, Plane& tmp, int w, int h, int r, int iters) {
  r = std::max(1, r);
  tmp.resize(data.size());
  for (int i = 0; i < iters; ++i) {
    boxH(data.data(), tmp.data(), w, h, r);
    boxV(tmp.data(), data.data(), w, h, r);
  }
}

// Gaussian blur src -> dst (tmp is scratch). Direct kernel for small sigma, 3 boxes otherwise.
void gaussBlur(const Plane& src, Plane& dst, Plane& tmp, int w, int h, float sigma) {
  dst.resize(src.size());
  tmp.resize(src.size());
  if (sigma > 2.6f) {
    const int r = std::max(1, int(std::lround(std::sqrt(sigma * sigma * 4.f / 3.f + 0.25f) - 0.5f)));
    dst = src;
    boxBlur(dst, tmp, w, h, r, 3);
    return;
  }
  const int R = std::max(1, int(std::ceil(sigma * 3.f)));
  std::vector<float> k(2 * R + 1);
  float s = 0.f;
  for (int i = -R; i <= R; ++i) s += (k[i + R] = std::exp(-0.5f * float(i * i) / (sigma * sigma)));
  for (float& v : k) v /= s;
  parallelFor(
      h,
      [&](int y) {
        const float* in = src.data() + std::size_t(y) * w;
        float* out = tmp.data() + std::size_t(y) * w;
        for (int x = 0; x < w; ++x) {
          float acc = 0.f;
          if (x >= R && x < w - R) {
            for (int i = -R; i <= R; ++i) acc += k[i + R] * in[x + i];
          } else {
            for (int i = -R; i <= R; ++i) acc += k[i + R] * in[std::clamp(x + i, 0, w - 1)];
          }
          out[x] = acc;
        }
      },
      4);
  parallelFor(
      h,
      [&](int y) {
        float* out = dst.data() + std::size_t(y) * w;
        for (int x = 0; x < w; ++x) out[x] = 0.f;
        for (int i = -R; i <= R; ++i) {
          const float* in = tmp.data() + std::size_t(std::clamp(y + i, 0, h - 1)) * w;
          const float kk = k[i + R];
          for (int x = 0; x < w; ++x) out[x] += kk * in[x];
        }
      },
      4);
}

// -------------------------------------------------------------- low resolution helpers ---

struct Axis {
  std::vector<int> i0, i1;
  std::vector<float> t;
};

// Bilinear sampling positions of a low-res grid (factor f) for every full-res coordinate.
Axis makeAxis(int n, int ln, int f) {
  Axis a;
  a.i0.resize(n);
  a.i1.resize(n);
  a.t.resize(n);
  for (int i = 0; i < n; ++i) {
    const float p = std::clamp((float(i) + 0.5f) / float(f) - 0.5f, 0.f, float(ln - 1));
    const int i0 = std::min(int(p), ln - 1);
    a.i0[i] = i0;
    a.i1[i] = std::min(i0 + 1, ln - 1);
    a.t[i] = p - float(i0);
  }
  return a;
}

inline float sampleLow(const float* low, int lw, const Axis& ax, const Axis& ay, int x, int y) {
  const float* r0 = low + std::size_t(ay.i0[y]) * lw;
  const float* r1 = low + std::size_t(ay.i1[y]) * lw;
  const float tx = ax.t[x], ty = ay.t[y];
  const float a = r0[ax.i0[x]] + (r0[ax.i1[x]] - r0[ax.i0[x]]) * tx;
  const float b = r1[ax.i0[x]] + (r1[ax.i1[x]] - r1[ax.i0[x]]) * tx;
  return a + (b - a) * ty;
}

// Area average of `ch` interleaved channels by integer factor f.
void downscale(const float* src, int ch, int w, int h, int f, std::vector<float>& dst, int& lw, int& lh) {
  lw = (w + f - 1) / f;
  lh = (h + f - 1) / f;
  dst.assign(std::size_t(lw) * lh * ch, 0.f);
  parallelFor(lh, [&](int ly) {
    const int y0 = ly * f, y1 = std::min(h, y0 + f);
    for (int lx = 0; lx < lw; ++lx) {
      const int x0 = lx * f, x1 = std::min(w, x0 + f);
      float acc[3] = {0.f, 0.f, 0.f};
      for (int y = y0; y < y1; ++y) {
        const float* row = src + (std::size_t(y) * w + x0) * ch;
        for (int x = x0; x < x1; ++x)
          for (int c = 0; c < ch; ++c) acc[c] += row[(x - x0) * ch + c];
      }
      const float inv = 1.f / float((y1 - y0) * (x1 - x0));
      for (int c = 0; c < ch; ++c) dst[(std::size_t(ly) * lw + lx) * ch + c] = acc[c] * inv;
    }
  });
}

void lumaOf(const ImageF& img, Plane& Y) {
  const int w = img.width, h = img.height;
  Y.resize(std::size_t(w) * h);
  parallelFor(
      h,
      [&](int y) {
        const float* p = img.row(y);
        float* o = Y.data() + std::size_t(y) * w;
        for (int x = 0; x < w; ++x) o[x] = kR * p[3 * x] + kG * p[3 * x + 1] + kB * p[3 * x + 2];
      },
      8);
}

// --------------------------------------------------------------------- noise reduction ---

// Edge-stopped blend towards a blurred copy. Strong where the pixel agrees with its
// neighbourhood (flat, noisy), weak where it differs (edges).
void denoiseLuma(ImageF& img, Plane& Y, Plane& tmp, float amount, float scale) {
  const int w = img.width, h = img.height;
  Plane& Yb = arena(3);
  Yb = Y;
  const int r = std::max(1, int(std::lround((0.8f + 1.8f * amount) * scale)));
  boxBlur(Yb, tmp, w, h, r, 2);
  const float blend = std::min(1.f, std::pow(amount, 0.75f) * 1.15f);
  const float sigma = 0.006f + 0.05f * amount;
  const float inv2 = 1.f / (sigma * sigma);
  parallelFor(
      h,
      [&](int y) {
        float* p = img.row(y);
        float* yy = Y.data() + std::size_t(y) * w;
        const float* yb = Yb.data() + std::size_t(y) * w;
        for (int x = 0; x < w; ++x) {
          const float e = yy[x] - yb[x];
          const float k = blend / (1.f + e * e * inv2);
          const float d = -e * k;
          p[3 * x] = clamp01(p[3 * x] + d);
          p[3 * x + 1] = clamp01(p[3 * x + 1] + d);
          p[3 * x + 2] = clamp01(p[3 * x + 2] + d);
          yy[x] += d;
        }
      },
      8);
}

void denoiseChroma(ImageF& img, const Plane& Y, float amount, float scale) {
  const int w = img.width, h = img.height;
  const float rFull = (1.5f + 5.f * amount) * scale;
  const int f = rFull >= 6.f ? 4 : 2;  // chroma noise is low-frequency: work on a reduced copy
  std::vector<float> low;
  int lw = 0, lh = 0;
  downscale(img.data.data(), 3, w, h, f, low, lw, lh);
  const std::size_t LN = std::size_t(lw) * lh;
  Plane u(LN), v(LN), yb(LN), tmp;
  for (std::size_t i = 0; i < LN; ++i) {
    const float yy = kR * low[3 * i] + kG * low[3 * i + 1] + kB * low[3 * i + 2];
    yb[i] = yy;
    u[i] = low[3 * i] - yy;
    v[i] = low[3 * i + 2] - yy;
  }
  const int rl = std::max(1, int(std::lround(rFull / float(f))));
  boxBlur(u, tmp, lw, lh, rl, 2);
  boxBlur(v, tmp, lw, lh, rl, 2);
  boxBlur(yb, tmp, lw, lh, std::max(1, rl / 2), 2);
  const Axis ax = makeAxis(w, lw, f), ay = makeAxis(h, lh, f);
  const float blend = std::min(1.f, amount * 1.4f);
  const float inv2 = 1.f / (0.05f * 0.05f);
  parallelFor(
      h,
      [&](int y) {
        float* p = img.row(y);
        const float* yy = Y.data() + std::size_t(y) * w;
        for (int x = 0; x < w; ++x) {
          const float Yv = yy[x];
          const float e = Yv - sampleLow(yb.data(), lw, ax, ay, x, y);
          const float k = blend / (1.f + e * e * inv2);
          const float R0 = p[3 * x], B0 = p[3 * x + 2];
          const float un = (R0 - Yv) + (sampleLow(u.data(), lw, ax, ay, x, y) - (R0 - Yv)) * k;
          const float vn = (B0 - Yv) + (sampleLow(v.data(), lw, ax, ay, x, y) - (B0 - Yv)) * k;
          const float R = Yv + un, B = Yv + vn;
          p[3 * x] = clamp01(R);
          p[3 * x + 2] = clamp01(B);
          p[3 * x + 1] = clamp01((Yv - kR * R - kB * B) / kG);
        }
      },
      8);
}

// --------------------------------------------------------------------------- dehaze ---

void dehazeImage(ImageF& img, float amount, float scale) {
  const int w = img.width, h = img.height;
  if (amount < 0.f) {  // add haze: veil of light grey
    const float k = 0.5f * -amount, air = 0.88f;
    parallelFor(
        h,
        [&](int y) {
          float* p = img.row(y);
          for (int i = 0; i < 3 * w; ++i) p[i] = p[i] * (1.f - k) + air * k;
        },
        8);
    return;
  }
  constexpr int f = 4;
  std::vector<float> low;
  int lw = 0, lh = 0;
  downscale(img.data.data(), 3, w, h, f, low, lw, lh);
  const std::size_t LN = std::size_t(lw) * lh;
  // dark channel: per-pixel min over RGB, min filter, smoothing
  Plane dc(LN), tmp(LN);
  for (std::size_t i = 0; i < LN; ++i) dc[i] = std::min({low[3 * i], low[3 * i + 1], low[3 * i + 2]});
  const int rm = std::max(1, int(std::lround(2.5f * scale)));
  for (int y = 0; y < lh; ++y)
    for (int x = 0; x < lw; ++x) {
      float m = 1.f;
      for (int i = -rm; i <= rm; ++i) m = std::min(m, dc[std::size_t(y) * lw + std::clamp(x + i, 0, lw - 1)]);
      tmp[std::size_t(y) * lw + x] = m;
    }
  for (int y = 0; y < lh; ++y)
    for (int x = 0; x < lw; ++x) {
      float m = 1.f;
      for (int i = -rm; i <= rm; ++i) m = std::min(m, tmp[std::size_t(std::clamp(y + i, 0, lh - 1)) * lw + x]);
      dc[std::size_t(y) * lw + x] = m;
    }
  boxBlur(dc, tmp, lw, lh, std::max(1, int(std::lround(3.f * scale))), 2);
  // airlight: mean colour of the pixels with the highest dark channel
  float mx = 0.f;
  for (float v : dc) mx = std::max(mx, v);
  double air[3] = {0, 0, 0};
  int cnt = 0;
  for (std::size_t i = 0; i < LN; ++i)
    if (dc[i] >= 0.96f * mx) {
      for (int c = 0; c < 3; ++c) air[c] += low[3 * i + c];
      ++cnt;
    }
  float A[3];
  for (int c = 0; c < 3; ++c) A[c] = std::clamp(cnt ? float(air[c] / cnt) : 1.f, 0.7f, 1.f);
  const float Amin = std::min({A[0], A[1], A[2]});
  const float omega = 0.7f * amount;
  Plane t(LN);
  for (std::size_t i = 0; i < LN; ++i) t[i] = std::clamp(1.f - omega * dc[i] / Amin, 0.25f, 1.f);
  const Axis ax = makeAxis(w, lw, f), ay = makeAxis(h, lh, f);
  parallelFor(
      h,
      [&](int y) {
        float* p = img.row(y);
        for (int x = 0; x < w; ++x) {
          const float inv = 1.f / sampleLow(t.data(), lw, ax, ay, x, y);
          for (int c = 0; c < 3; ++c) p[3 * x + c] = clamp01((p[3 * x + c] - A[c]) * inv + A[c]);
        }
      },
      8);
}

// ------------------------------------------------------- clarity / texture / sharpening ---

// Edge-aware base layer of the luminance: fast guided filter (He & Sun) run on a 1/4 size copy,
// its linear coefficients upsampled bilinearly and applied at full resolution.
void guidedBase(const Plane& Y, int w, int h, float scale, Plane& base) {
  constexpr int f = 4;
  std::vector<float> low;
  int lw = 0, lh = 0;
  downscale(Y.data(), 1, w, h, f, low, lw, lh);
  const std::size_t LN = std::size_t(lw) * lh;
  const int r = std::max(2, int(std::lround(0.02f * 2048.f * scale / f)));
  const float eps = 0.045f * 0.045f;
  Plane mI = low, mII(LN), tmp(LN);
  for (std::size_t i = 0; i < LN; ++i) mII[i] = low[i] * low[i];
  boxBlur(mI, tmp, lw, lh, r, 1);
  boxBlur(mII, tmp, lw, lh, r, 1);
  Plane a(LN), b(LN);
  for (std::size_t i = 0; i < LN; ++i) {
    const float var = std::max(0.f, mII[i] - mI[i] * mI[i]);
    a[i] = var / (var + eps);
    b[i] = mI[i] - a[i] * mI[i];
  }
  boxBlur(a, tmp, lw, lh, r, 1);
  boxBlur(b, tmp, lw, lh, r, 1);
  const Axis ax = makeAxis(w, lw, f), ay = makeAxis(h, lh, f);
  base.resize(Y.size());
  parallelFor(
      h,
      [&](int y) {
        const std::size_t o = std::size_t(y) * w;
        for (int x = 0; x < w; ++x)
          base[o + x] = sampleLow(a.data(), lw, ax, ay, x, y) * Y[o + x] + sampleLow(b.data(), lw, ax, ay, x, y);
      },
      8);
}

void detailPass(ImageF& img, const Plane& Y, const SpatialParams& p, float scale) {
  const int w = img.width, h = img.height;
  const std::size_t N = std::size_t(w) * h;
  Plane& dY = arena(2);
  dY.assign(N, 0.f);
  Plane& tmp = arena(1);
  Plane& work = arena(3);
  Plane& work2 = arena(4);

  if (p.clarity != 0.f) {
    guidedBase(Y, w, h, scale, work);
    const float g = p.clarity > 0.f ? p.clarity * 1.1f : p.clarity * 0.85f;
    parallelFor(
        h,
        [&](int y) {
          const std::size_t o = std::size_t(y) * w;
          for (int x = 0; x < w; ++x) {
            const float v = Y[o + x];
            const float mid = 0.4f + 0.6f * (1.f - (2.f * v - 1.f) * (2.f * v - 1.f));
            dY[o + x] += g * (v - work[o + x]) * mid;
          }
        },
        8);
  }
  if (p.texture != 0.f) {
    work = Y;
    boxBlur(work, tmp, w, h, std::max(1, int(std::lround(2.f * scale))), 2);
    const bool pos = p.texture > 0.f;
    const float g = pos ? p.texture * 1.5f : p.texture * 0.9f;
    parallelFor(
        h,
        [&](int y) {
          const std::size_t o = std::size_t(y) * w;
          for (int x = 0; x < w; ++x) {
            const float d = Y[o + x] - work[o + x];
            // positive: compress big edges so only fine texture is boosted (no halos)
            const float dd = pos ? d / (1.f + 10.f * std::fabs(d)) : d;
            dY[o + x] += g * dd;
          }
        },
        8);
  }
  if (p.sharpen > 0.f) {
    const float sigma = std::max(0.4f, p.sharpRadius * scale * 0.5f);
    gaussBlur(Y, work2, tmp, w, h, sigma);
    const float k = p.sharpen;
    constexpr float lim = 0.15f, gate = 0.0025f;
    parallelFor(
        h,
        [&](int y) {
          const std::size_t o = std::size_t(y) * w;
          for (int x = 0; x < w; ++x) {
            const float d = Y[o + x] - work2[o + x];
            const float a = std::fabs(d);
            // soft limit (no big halos), and a gate that leaves sensor-noise-sized detail alone
            dY[o + x] += k * d * (lim / (lim + a)) * (a / (a + gate));
          }
        },
        8);
  }
  parallelFor(
      h,
      [&](int y) {
        float* px = img.row(y);
        const std::size_t o = std::size_t(y) * w;
        for (int x = 0; x < w; ++x) {
          const float d = dY[o + x];
          px[3 * x] = clamp01(px[3 * x] + d);
          px[3 * x + 1] = clamp01(px[3 * x + 1] + d);
          px[3 * x + 2] = clamp01(px[3 * x + 2] + d);
        }
      },
      8);
}

// ---------------------------------------------------------------------------- grain ---

inline std::uint32_t hash2(std::uint32_t x, std::uint32_t y) {
  std::uint32_t h = x * 374761393u + y * 668265263u + 0x9E3779B9u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}
// Triangular noise in [-1, 1] at an integer lattice point.
inline float lattice(int ix, int iy) {
  const std::uint32_t a = hash2(std::uint32_t(ix), std::uint32_t(iy));
  const std::uint32_t b = hash2(std::uint32_t(ix) + 7919u, std::uint32_t(iy) + 104729u);
  return float((a & 0xFFFF) + (b & 0xFFFF)) * (1.f / 65535.f) - 1.f;
}

void addGrain(ImageF& img, float amount, float scale) {
  const int w = img.width, h = img.height;
  const float period = std::max(1.f, 1.6f * scale);
  const float inv = 1.f / period;
  const float amp = amount * 0.13f * (period > 1.01f ? 1.5f : 1.f);
  parallelFor(
      h,
      [&](int y) {
        float* p = img.row(y);
        const float fy = float(y) * inv;
        const int iy = int(fy);
        const float ty0 = fy - float(iy);
        const float ty = ty0 * ty0 * (3.f - 2.f * ty0);
        for (int x = 0; x < w; ++x) {
          float n;
          if (period <= 1.01f) {
            n = lattice(x, y);
          } else {
            const float fx = float(x) * inv;
            const int ix = int(fx);
            const float tx0 = fx - float(ix);
            const float tx = tx0 * tx0 * (3.f - 2.f * tx0);
            const float a = lattice(ix, iy) + (lattice(ix + 1, iy) - lattice(ix, iy)) * tx;
            const float b = lattice(ix, iy + 1) + (lattice(ix + 1, iy + 1) - lattice(ix, iy + 1)) * tx;
            n = a + (b - a) * ty;
          }
          const float Yv = kR * p[3 * x] + kG * p[3 * x + 1] + kB * p[3 * x + 2];
          const float wgt = 0.25f + 0.75f * 4.f * Yv * (1.f - Yv);  // strongest in the mid-tones
          const float d = n * amp * wgt;
          p[3 * x] = clamp01(p[3 * x] + d);
          p[3 * x + 1] = clamp01(p[3 * x + 1] + d);
          p[3 * x + 2] = clamp01(p[3 * x + 2] + d);
        }
      },
      8);
}

}  // namespace

void applySpatial(ImageF& img, const SpatialParams& p) {
  if (img.empty() || !p.any()) return;
  const int w = img.width, h = img.height;
  const float scale = float(std::max(w, h)) / 2048.f;
  Plane Y, tmp;
  const bool needY = p.nrLuma > 0 || p.nrColor > 0 || p.clarity != 0 || p.texture != 0 || p.sharpen > 0;
  if (needY) lumaOf(img, Y);
  if (p.nrLuma > 0) denoiseLuma(img, Y, tmp, p.nrLuma, scale);
  if (p.nrColor > 0) denoiseChroma(img, Y, p.nrColor, scale);
  if (p.dehaze != 0) {
    dehazeImage(img, p.dehaze, scale);
    if (p.clarity != 0 || p.texture != 0 || p.sharpen > 0) lumaOf(img, Y);
  }
  if (p.clarity != 0 || p.texture != 0 || p.sharpen > 0) detailPass(img, Y, p, scale);
  if (p.grain > 0) addGrain(img, p.grain, scale);
}

}  // namespace dapple
