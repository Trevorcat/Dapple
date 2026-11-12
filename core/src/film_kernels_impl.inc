// Included once per instruction-set variant (see film_kernels_base.cpp / film_kernels_avx2.cpp).
// 4-wide channel-in-lane float vectors (clang ext_vector_type: SSE/AVX/NEON), scalar struct elsewhere.
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "film_kernels.h"

namespace dapple::film::detail {
namespace DAPPLE_KNS {

#if defined(__clang__)
typedef float f4 __attribute__((ext_vector_type(4)));
#define K_INLINE __attribute__((always_inline)) inline
#else
#define K_INLINE inline
struct f4 {
  float v[4];
  float& operator[](int i) { return v[i]; }
  float operator[](int i) const { return v[i]; }
};
static K_INLINE f4 operator+(f4 a, f4 b) { return {{a.v[0] + b.v[0], a.v[1] + b.v[1], a.v[2] + b.v[2], a.v[3] + b.v[3]}}; }
static K_INLINE f4 operator-(f4 a, f4 b) { return {{a.v[0] - b.v[0], a.v[1] - b.v[1], a.v[2] - b.v[2], a.v[3] - b.v[3]}}; }
static K_INLINE f4 operator*(f4 a, f4 b) { return {{a.v[0] * b.v[0], a.v[1] * b.v[1], a.v[2] * b.v[2], a.v[3] * b.v[3]}}; }
static K_INLINE f4 operator*(f4 a, float s) { return {{a.v[0] * s, a.v[1] * s, a.v[2] * s, a.v[3] * s}}; }
static K_INLINE f4 operator*(float s, f4 a) { return a * s; }
static K_INLINE f4 operator+(f4 a, float s) { return {{a.v[0] + s, a.v[1] + s, a.v[2] + s, a.v[3] + s}}; }
static K_INLINE f4 operator-(f4 a, float s) { return {{a.v[0] - s, a.v[1] - s, a.v[2] - s, a.v[3] - s}}; }
#endif

static K_INLINE f4 ld4(const float* p) {
  f4 v;
  std::memcpy(&v, p, 16);
  return v;
}
static K_INLINE f4 splat(float s) { return f4{s, s, s, s}; }
static K_INLINE f4 ld3(const float* p) { return f4{p[0], p[1], p[2], 0.f}; }
static K_INLINE void st3(float* p, f4 v) {
  p[0] = v[0];
  p[1] = v[1];
  p[2] = v[2];
}
static K_INLINE f4 vmax(f4 a, f4 b) {
  return f4{a[0] > b[0] ? a[0] : b[0], a[1] > b[1] ? a[1] : b[1], a[2] > b[2] ? a[2] : b[2], a[3] > b[3] ? a[3] : b[3]};
}
static K_INLINE float fmaxs(float a, float b) { return a > b ? a : b; }
static K_INLINE float fmins(float a, float b) { return a < b ? a : b; }

// Mitchell-Netravali (B = C = 1/3) weights for taps ia-1 .. ia+2.
static K_INLINE float mitK0(float t) { return ((7.f * t - 12.f) * t * t + 16.f / 3.f) * (1.f / 6.f); }
static K_INLINE float mitK1(float t) { return (((-7.f / 3.f * t + 12.f) * t - 20.f) * t + 32.f / 3.f) * (1.f / 6.f); }

// Linear exposure from linear ProPhoto RGB: matrix, chromaticity -> tc, bicubic raw LUT, brightness.
static K_INLINE f4 pxExposure(const KView& k, const float* rgb, float scale) {
  const f4 xyz = ld4(k.mIn) * rgb[0] + ld4(k.mIn + 4) * rgb[1] + ld4(k.mIn + 8) * rgb[2];
  const float b = xyz[0] + xyz[1] + xyz[2];
  // x = X/b, 1-x = (Y+Z)/b, tc1 = y/(1-x) = Y/(Y+Z): one division for both
  const float bb = fmaxs(b, 1e-10f), yz = fmaxs(xyz[1] + xyz[2], 1e-10f);
  const float r = 1.f / (bb * yz);
  const float dd = yz * (yz * r);
  const float tc0 = fmins(fmaxs(dd * dd, 0.f), 1.f);
  const float tc1 = fmins(fmaxs(xyz[1] * (bb * r), 0.f), 1.f);
  const float top = float(k.rawN - 1);
  const float a = tc0 * top, c = tc1 * top;
  int ia = int(a), ic = int(c);
  ia = ia < k.rawN - 2 ? ia : k.rawN - 2;
  ic = ic < k.rawN - 2 ? ic : k.rawN - 2;
  const float fa = a - float(ia), fc = c - float(ic);
  const float wa[4] = {mitK1(fa + 1.f), mitK0(fa), mitK0(1.f - fa), mitK1(2.f - fa)};
  const float wc[4] = {mitK1(fc + 1.f), mitK0(fc), mitK0(1.f - fc), mitK1(2.f - fc)};
  f4 acc = splat(0.f);
  for (int dj = 0; dj < 4; ++dj) {
    const float* row = k.raw + (std::size_t(ic + dj) * std::size_t(k.rawStride) + std::size_t(ia)) * 4;
    const f4 r = ld4(row) * wa[0] + ld4(row + 4) * wa[1] + ld4(row + 8) * wa[2] + ld4(row + 12) * wa[3];
    acc = acc + r * wc[dj];
  }
  return acc * (b * scale);
}

// log10(max(e,0) + 1e-10): exponent extraction + odd series in t = (m-1)/(m+1), |error| < 1e-7.
// log10(max(e,0) + 1e-10): exponent extraction + odd series in t = (m-1)/(m+1), |error| < 1e-7.
// Clang vector path: all three channels at once, one vector division.
#if defined(__clang__)
typedef int i4 __attribute__((ext_vector_type(4)));
static K_INLINE f4 pxLogE(f4 e) {
  const f4 z = vmax(e, splat(0.f)) + 1e-10f;
  const i4 bits = __builtin_bit_cast(i4, z);
  i4 ex = (bits >> 23) - 127;
  const i4 mb = (bits & 0x7FFFFF) | 0x3F800000;
  f4 m = __builtin_bit_cast(f4, mb);
  const i4 hi = m > 1.41421356f;  // -1 where the mantissa is in the upper half
  const f4 mh = m * 0.5f;
  m = __builtin_bit_cast(f4, (__builtin_bit_cast(i4, mh) & hi) | (__builtin_bit_cast(i4, m) & ~hi));
  ex = ex - hi;
  const f4 t = (m - 1.f) / (m + 1.f);
  const f4 t2 = t * t;
  const f4 p = 1.f + t2 * (1.f / 3.f + t2 * (1.f / 5.f + t2 * (1.f / 7.f + t2 * (1.f / 9.f))));
  return (__builtin_convertvector(ex, f4) + 2.88539008f * t * p) * 0.30102999566f;
}
#else
static K_INLINE f4 pxLogE(f4 e) {
  const f4 z = vmax(e, splat(0.f)) + 1e-10f;
  f4 res = splat(0.f);
  for (int c = 0; c < 3; ++c) {
    std::uint32_t bits;
    const float v = z[c];
    std::memcpy(&bits, &v, 4);
    int ex = int(bits >> 23) - 127;
    const std::uint32_t mb = (bits & 0x7FFFFFu) | 0x3F800000u;
    float m;
    std::memcpy(&m, &mb, 4);
    if (m > 1.41421356f) {
      m *= 0.5f;
      ++ex;
    }
    const float t = (m - 1.f) / (m + 1.f), t2 = t * t;
    const float p = 1.f + t2 * (1.f / 3.f + t2 * (1.f / 5.f + t2 * (1.f / 7.f + t2 * (1.f / 9.f))));
    res[c] = (float(ex) + 2.88539008f * t * p) * 0.30102999566f;
  }
  return res;
}
#endif

static K_INLINE float curve(const float* tab, float idx, int n) {
  idx = idx > 0.f ? idx : 0.f;
  const float top = float(n - 1);
  idx = idx < top ? idx : top;
  const int i = int(idx);
  const float f = idx - float(i);
  return tab[2 * i] + f * tab[2 * i + 1];
}

static K_INLINE f4 pxCurves(const KView& k, const float* tab, f4 x) {
  const f4 g = ld4(k.gamma);
  const f4 idx = (g * x - k.curveX0) * k.curveInvDx;
  return f4{curve(tab, idx[0], k.curveN), curve(tab + 2 * k.curveN, idx[1], k.curveN),
            curve(tab + 4 * k.curveN, idx[2], k.curveN), 0.f};
}

static K_INLINE f4 pxQ(const KView& k, f4 d) {
  return ld4(k.dirQ) * d[0] + ld4(k.dirQ + 4) * d[1] + ld4(k.dirQ + 8) * d[2] + ld4(k.dirQ0);
}

// Tetrahedral interpolation in the baked film-density -> display LUT (C fastest, then M, then Y).
static K_INLINE f4 tetra(const float* lut, int N, const float* dminv, const float* dscalev, f4 d) {
  const float top = float(N - 1);
  int i0[3];
  float fr[3];
  for (int c = 0; c < 3; ++c) {
    float v = (d[c] - dminv[c]) * dscalev[c] * top;
    v = v > 0.f ? v : 0.f;
    v = v < top ? v : top;
    int i = int(v);
    i = i < N - 2 ? i : N - 2;
    i0[c] = i;
    fr[c] = v - float(i);
  }
  const int sx = 4, sy = 4 * N, sz = 4 * N * N;
  const float* base = lut + std::size_t(i0[2]) * std::size_t(sz) + std::size_t(i0[1]) * std::size_t(sy) +
                      std::size_t(i0[0]) * std::size_t(sx);
  const float fx = fr[0], fy = fr[1], fz = fr[2];
  const int rx = int(fy > fx) + int(fz > fx);
  const int ry = int(fx >= fy) + int(fz > fy);
  const int rz = int(fx >= fz) + int(fy >= fz);
  float fs[3];
  int ss[3];
  fs[rx] = fx;
  fs[ry] = fy;
  fs[rz] = fz;
  ss[rx] = sx;
  ss[ry] = sy;
  ss[rz] = sz;
  const int o1 = ss[0], o2 = ss[0] + ss[1], o3 = sx + sy + sz;
  return ld4(base) * (1.f - fs[0]) + ld4(base + o1) * (fs[0] - fs[1]) + ld4(base + o2) * (fs[1] - fs[2]) +
         ld4(base + o3) * fs[2];
}
static K_INLINE f4 pxPrintScan(const KView& k, f4 d) { return tetra(k.lut, k.lutN, k.dmin, k.dscale, d); }
static K_INLINE f4 pxPrintRaw(const KView& k, f4 d) { return tetra(k.lut1, k.lutN, k.dmin, k.dscale, d); }
static K_INLINE f4 pxPaperScan(const KView& k, f4 logE) {
  const f4 idx = (logE - k.paperX0) * k.paperInvDx;
  const f4 dp = f4{curve(k.paperCurve, idx[0], k.paperN), curve(k.paperCurve + 2 * k.paperN, idx[1], k.paperN),
                   curve(k.paperCurve + 4 * k.paperN, idx[2], k.paperN), 0.f};
  return tetra(k.lut3, k.lutN, k.dminS, k.dscaleS, dp);
}

static K_INLINE f4 pxFused(const KView& k, const float* rgb, float scale) {
  const f4 x = pxLogE(pxExposure(k, rgb, scale));
  const f4 d1 = pxCurves(k, k.dn, x);
  const f4 q = pxQ(k, d1);
  const f4 d = pxCurves(k, k.d0, x - q);
  return pxPrintScan(k, d);
}

static void fused(const KView& k, const float* in, float* out, int n, float scale) {
  for (int i = 0; i < n; ++i) st3(out + 3 * std::size_t(i), pxFused(k, in + 3 * std::size_t(i), scale));
}
static void exposure(const KView& k, const float* rgb, float* e, int n, float scale) {
  for (int i = 0; i < n; ++i) st3(e + 3 * std::size_t(i), pxExposure(k, rgb + 3 * std::size_t(i), scale));
}
static void logE(const KView&, const float* e, float* x, int n) {
  for (int i = 0; i < n; ++i) st3(x + 3 * std::size_t(i), pxLogE(ld3(e + 3 * std::size_t(i))));
}
static void develop1(const KView& k, const float* x, float* q, int n) {
  for (int i = 0; i < n; ++i) {
    const f4 xv = ld3(x + 3 * std::size_t(i));
    st3(q + 3 * std::size_t(i), pxQ(k, pxCurves(k, k.dn, xv)));
  }
}
static void develop2(const KView& k, const float* x, const float* q, float* d, int n) {
  for (int i = 0; i < n; ++i)
    st3(d + 3 * std::size_t(i), pxCurves(k, k.d0, ld3(x + 3 * std::size_t(i)) - ld3(q + 3 * std::size_t(i))));
}
static void printScan(const KView& k, const float* d, float* out, int n) {
  for (int i = 0; i < n; ++i) st3(out + 3 * std::size_t(i), pxPrintScan(k, ld3(d + 3 * std::size_t(i))));
}
static void printRaw(const KView& k, const float* d, float* logE, int n) {
  for (int i = 0; i < n; ++i) st3(logE + 3 * std::size_t(i), pxPrintRaw(k, ld3(d + 3 * std::size_t(i))));
}
static void paperScan(const KView& k, const float* logE, float* out, int n) {
  for (int i = 0; i < n; ++i) st3(out + 3 * std::size_t(i), pxPaperScan(k, ld3(logE + 3 * std::size_t(i))));
}
static void single(const KView& k, const float* rgb, float* out, float scale) { st3(out, pxFused(k, rgb, scale)); }

// Block-fused: the stages run back to back over L1-resident blocks (no full-frame intermediates), which keeps
// the stage loops short enough for the out-of-order core to overlap neighbouring pixels.
#ifndef DAPPLE_FILM_BLOCK
#define DAPPLE_FILM_BLOCK 256
#endif
static void block(const KView& k, const float* in, float* out, int n, float scale) {
  constexpr int B = DAPPLE_FILM_BLOCK;
  alignas(64) float e[B * 3];
  alignas(64) float q[B * 3];
  for (int i0 = 0; i0 < n; i0 += B) {
    const int m = n - i0 < B ? n - i0 : B;
    exposure(k, in + 3 * std::size_t(i0), e, m, scale);
    logE(k, e, e, m);
    develop1(k, e, q, m);
    develop2(k, e, q, q, m);
    printScan(k, q, out + 3 * std::size_t(i0), m);
  }
}

}  // namespace DAPPLE_KNS

const KernelSet DAPPLE_KSET = {DAPPLE_KNAME,         DAPPLE_KNS::fused,    DAPPLE_KNS::exposure,  DAPPLE_KNS::logE,
                               DAPPLE_KNS::develop1, DAPPLE_KNS::develop2, DAPPLE_KNS::printScan, DAPPLE_KNS::single,
                               DAPPLE_KNS::printRaw, DAPPLE_KNS::paperScan, DAPPLE_KNS::block};

}  // namespace dapple::film::detail
