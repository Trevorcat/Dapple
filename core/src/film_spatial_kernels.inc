// Spatial-effect kernels, compiled once per instruction-set variant (film_kernels_base.cpp / film_kernels_avx2.cpp,
// after film_kernels_impl.inc; the macros DAPPLE_KNS / DAPPLE_SSET are those of the including file).
// Plain loops over fixed-size blocks: the compiler vectorises them for SSE2 / AVX2+FMA / NEON; no libm call, no
// allocation and no virtual call in any loop here (math is polynomial, bit tricks).
#include <algorithm>
#include <cmath>
#include <cstring>

#include "film_spatial.h"

namespace dapple::film::detail {
namespace DAPPLE_KNS {
namespace sp {

#define L8(...)                         \
  for (int l_ = 0; l_ < 8; ++l_) {      \
    __VA_ARGS__;                        \
  }

// ---- FIR / resampling ---------------------------------------------------------------------------------------

static void firRow(const float* __restrict src, float* __restrict dst, int n, const float* w, int r) {
  constexpr int B = 32;
  int i0 = 0;
  const float w0 = w[0];
  for (; i0 + B <= n; i0 += B) {
    float acc[B];
    const float* s = src + i0;
    for (int i = 0; i < B; ++i) acc[i] = w0 * s[i];
    for (int k = 1; k <= r; ++k) {
      const float wk = w[k];
      const float* a = s - k;
      const float* b = s + k;
      for (int i = 0; i < B; ++i) acc[i] += wk * (a[i] + b[i]);
    }
    for (int i = 0; i < B; ++i) dst[i0 + i] = acc[i];
  }
  for (; i0 < n; ++i0) {
    float acc = w0 * src[i0];
    for (int k = 1; k <= r; ++k) acc += w[k] * (src[i0 - k] + src[i0 + k]);
    dst[i0] = acc;
  }
}

static void firCol(const float* __restrict mid, std::ptrdiff_t stride, float* __restrict dst, int n, const float* w, int r,
                   int accumulate) {
  constexpr int B = 32;
  int i0 = 0;
  const float w0 = w[0];
  for (; i0 + B <= n; i0 += B) {
    float acc[B];
    const float* s = mid + i0;
    if (accumulate)
      for (int i = 0; i < B; ++i) acc[i] = dst[i0 + i] + w0 * s[i];
    else
      for (int i = 0; i < B; ++i) acc[i] = w0 * s[i];
    for (int k = 1; k <= r; ++k) {
      const float wk = w[k];
      const float* a = s - std::ptrdiff_t(k) * stride;
      const float* b = s + std::ptrdiff_t(k) * stride;
      for (int i = 0; i < B; ++i) acc[i] += wk * (a[i] + b[i]);
    }
    for (int i = 0; i < B; ++i) dst[i0 + i] = acc[i];
  }
  for (; i0 < n; ++i0) {
    float acc = (accumulate ? dst[i0] : 0.f) + w0 * mid[i0];
    for (int k = 1; k <= r; ++k)
      acc += w[k] * (mid[i0 - std::ptrdiff_t(k) * stride] + mid[i0 + std::ptrdiff_t(k) * stride]);
    dst[i0] = acc;
  }
}

static void scaleRow(const float* __restrict src, float* __restrict dst, int n, float w) {
  for (int i = 0; i < n; ++i) dst[i] = w * src[i];
}

// 2x2 box average, replicate the last column when nIn is odd.
static void down2Row(const float* __restrict r0, const float* __restrict r1, float* __restrict dst, int nOut, int nIn) {
  const int full = nIn / 2;
  for (int i = 0; i < full; ++i) dst[i] = 0.25f * (r0[2 * i] + r0[2 * i + 1] + r1[2 * i] + r1[2 * i + 1]);
  if (nOut > full) dst[full] = 0.5f * (r0[2 * full] + r1[2 * full]);
}

// dst[j] = w0 a[j] + w1 b[j] + w2 c[j] + w3 d[j]
static void lerpRows(const float* __restrict a, const float* __restrict b, const float* __restrict c,
                     const float* __restrict d, const float* w4, float* __restrict dst, int n) {
  const float w0 = w4[0], w1 = w4[1], w2 = w4[2], w3 = w4[3];
  for (int i = 0; i < n; ++i) dst[i] = w0 * a[i] + w1 * b[i] + w2 * c[i] + w3 * d[i];
}

// Catmull-Rom x2 upsampling along x. tmp[-2 .. nLow+1] valid; hi[2i] sits at low position i - 1/4, hi[2i+1] at i + 1/4.
static void up2Row(const float* __restrict tmp, float* __restrict dst, int nLow, int nHi) {
  constexpr float A0 = -0.0234375f, A1 = 0.2265625f, A2 = 0.8671875f, A3 = -0.0703125f;  // t = 0.75
  constexpr float B0 = -0.0703125f, B1 = 0.8671875f, B2 = 0.2265625f, B3 = -0.0234375f;  // t = 0.25
  const int full = nHi / 2;
  for (int i = 0; i < full; ++i) {
    dst[2 * i] += A0 * tmp[i - 2] + A1 * tmp[i - 1] + A2 * tmp[i] + A3 * tmp[i + 1];
    dst[2 * i + 1] += B0 * tmp[i - 1] + B1 * tmp[i] + B2 * tmp[i + 1] + B3 * tmp[i + 2];
  }
  if (nHi & 1) {
    const int i = full;
    dst[2 * i] += A0 * tmp[i - 2] + A1 * tmp[i - 1] + A2 * tmp[i] + A3 * tmp[i + 1];
  }
  (void)nLow;
}

// ---- random numbers and vector math on blocks of 8 lanes ---------------------------------------------------

static inline void pcg3d(std::uint32_t& x, std::uint32_t& y, std::uint32_t& z) {
  x = x * 1664525u + 1013904223u;
  y = y * 1664525u + 1013904223u;
  z = z * 1664525u + 1013904223u;
  x += y * z;
  y += z * x;
  z += x * y;
  x ^= x >> 16;
  y ^= y >> 16;
  z ^= z >> 16;
  x += y * z;
  y += z * x;
  z += x * y;
}

// 2^x for |x| < 126, max rel. error 3.3e-6 (degree-5 Taylor on the fractional part in [-0.5, 0.5], measured in testSpatialKernelExp)
static inline float exp2f_(float x) {
  x = x < -125.f ? -125.f : (x > 125.f ? 125.f : x);
  const float r = (x + 12582912.f) - 12582912.f;  // round to nearest integer
  const float f = x - r;
  const float p = 1.f + f * (0.69314718f + f * (0.24022651f + f * (0.05550411f + f * (0.00961813f + f * 0.00133336f))));
  std::uint32_t b = std::uint32_t(int(r) + 127) << 23;
  float s;
  std::memcpy(&s, &b, 4);
  return p * s;
}
static inline float expf_(float x) { return exp2f_(x * 1.44269504f); }

// ln(u) for u in (0, 1]
static inline float logf_(float u) {
  std::uint32_t b;
  std::memcpy(&b, &u, 4);
  int e = int(b >> 23) - 127;
  std::uint32_t mb = (b & 0x7FFFFFu) | 0x3F800000u;
  float m;
  std::memcpy(&m, &mb, 4);
  const bool hi = m > 1.41421356f;
  m = hi ? m * 0.5f : m;
  e += hi ? 1 : 0;
  const float t = (m - 1.f) / (m + 1.f), t2 = t * t;
  const float p = 1.f + t2 * (1.f / 3.f + t2 * (1.f / 5.f + t2 * (1.f / 7.f + t2 * (1.f / 9.f))));
  return float(e) * 0.69314718f + 2.f * t * p;
}

// cos(2 pi t) for t in [0, 1)
static inline float cos2pi(float t) {
  float a = t - 0.5f;  // [-0.5, 0.5): cos(2 pi t) = -cos(2 pi a)
  const float aa = a < 0.f ? -a : a;
  const bool fold = aa > 0.25f;
  const float b = fold ? 0.5f - aa : aa;  // [0, 0.25]
  const float x = 6.28318531f * b, x2 = x * x;
  const float c = 1.f + x2 * (-0.5f + x2 * (4.16666667e-2f + x2 * (-1.38888889e-3f + x2 * (2.48015873e-5f + x2 * -2.75573192e-7f))));
  // cos(2 pi a) = fold ? -c : c ; cos(2 pi t) = -cos(2 pi a)
  return fold ? c : -c;
}

static inline float u01(std::uint32_t w) { return (float(w >> 8) + 0.5f) * (1.f / 16777216.f); }  // (0, 1)

// Standard normal from two words (Box-Muller, cosine branch).
static inline float normalFrom(std::uint32_t a, std::uint32_t b) {
  const float r = std::sqrt(-2.f * logf_(u01(a)));
  return r * cos2pi(u01(b));
}

// ---- grain ---------------------------------------------------------------------------------------------------

// Poisson(lambda) per lane: inversion (CDF walk) for lambda < 4, Cornish-Fisher normal approximation above (mean and
// variance exact, skewness 1/sqrt(lambda) like the Poisson).
static inline void poisson8(const float* __restrict lam, const std::uint32_t* __restrict wa, const std::uint32_t* __restrict wb,
                            float* __restrict K) {
  constexpr float kSwitch = 4.f;
  bool anySmall = false, anyBig = false;
  L8(anySmall |= lam[l_] < kSwitch; anyBig |= lam[l_] >= kSwitch);
  float u[8];
  L8(u[l_] = u01(wa[l_]));
  float big[8];
  if (anyBig) {
    L8(const float z = normalFrom(wa[l_], wb[l_]); const float sl = std::sqrt(lam[l_]);
       const float k = lam[l_] + sl * z + (z * z - 1.f) * (1.f / 6.f); big[l_] = k > 0.f ? k : 0.f);
    if (!anySmall) {
      L8(K[l_] = big[l_]);
      return;
    }
  } else {
    L8(big[l_] = 0.f);
  }
  float p[8], cdf[8], k[8];
  L8(p[l_] = expf_(-(lam[l_] < kSwitch ? lam[l_] : kSwitch)); cdf[l_] = p[l_]; k[l_] = 0.f);
  for (int i = 1; i < 24; ++i) {
    int any = 0;
    float inv = 1.f / float(i);
    L8(const bool m = u[l_] > cdf[l_]; any |= m; k[l_] += m ? 1.f : 0.f; p[l_] *= lam[l_] * inv; cdf[l_] += p[l_]);
    if (!any) break;
  }
  L8(K[l_] = lam[l_] < kSwitch ? k[l_] : big[l_]);
}

static void grainRow(const GrainChannelPlan& gc, const float* d, float* g0, float* g1, float* g2, int n, int x0, int y,
                     std::uint32_t streamBase, int sumOnly) {
  const float top = float(gc.tableN);
  float* gl[3] = {g0, g1, g2};
  for (int i0 = 0; i0 < n; i0 += 8) {
    const int m = n - i0 < 8 ? n - i0 : 8;
    float vv[8];
    int ii[8];
    float ff[8];
    for (int l = 0; l < 8; ++l) {
      float v = d[i0 + (l < m ? l : m - 1)] * gc.invScale;
      v = v > 0.f ? v : 0.f;
      v = v < top ? v : top;
      ii[l] = int(v);
      ff[l] = v - float(ii[l]);
      vv[l] = v;
    }
    float sum[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    float Gm[8];
    bool useMerged = false;
    if (sumOnly) {
      // Merged sampler: when the expected particle count of the three sub-layers together is large, their sum is drawn as one
      // variate with the exact mean and variance and the exact skewness (Cornish-Fisher), from a table indexed by density.
      float M[8], sV[8], gam[8], lam[8], z[8];
      bool allBig = true;
      for (int l = 0; l < 8; ++l) {
        const float* t = &gc.merged[std::size_t(ii[l]) * 4];
        M[l] = t[0] + ff[l] * (t[4] - t[0]);
        sV[l] = t[1] + ff[l] * (t[5] - t[1]);
        gam[l] = t[2] + ff[l] * (t[6] - t[2]);
        lam[l] = t[3] + ff[l] * (t[7] - t[3]);
        allBig = allBig && lam[l] >= gc.lambdaSwitch;
      }
      std::uint32_t w0[8], w1[8], w2[8];
      L8(w0[l_] = std::uint32_t(x0 + i0 + l_); w1[l_] = std::uint32_t(y); w2[l_] = streamBase ^ 0x5851F42Du; pcg3d(w0[l_], w1[l_], w2[l_]));
      L8(z[l_] = normalFrom(w0[l_], w1[l_]); const float g = M[l_] + sV[l_] * (z[l_] + gam[l_] * (z[l_] * z[l_] - 1.f) * (1.f / 6.f));
         Gm[l_] = g > 0.f ? g : 0.f);
      if (allBig) {
        for (int l = 0; l < m; ++l) g0[i0 + l] = Gm[l];
        continue;
      }
      useMerged = true;
      for (int l = 0; l < 8; ++l) sum[l] = lam[l] >= gc.lambdaSwitch ? Gm[l] : -1.f;  // -1: exact path below
    }
    // sub-layer densities (table lookup, linear interpolation)
    float D[3][8];
    for (int l = 0; l < 8; ++l) {
      const float* t = &gc.inv[std::size_t(ii[l]) * 4];
      D[0][l] = t[0] + ff[l] * (t[4] - t[0]);
      D[1][l] = t[1] + ff[l] * (t[5] - t[1]);
      D[2][l] = t[2] + ff[l] * (t[6] - t[2]);
    }
    float exact[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    (void)vv;
    // two hash calls give the six words of the three layers (a pair per layer: inversion uses one, the normal branch both)
    std::uint32_t h[6][8];
    L8(h[0][l_] = std::uint32_t(x0 + i0 + l_); h[1][l_] = std::uint32_t(y); h[2][l_] = streamBase; pcg3d(h[0][l_], h[1][l_], h[2][l_]));
    L8(h[3][l_] = std::uint32_t(x0 + i0 + l_); h[4][l_] = std::uint32_t(y); h[5][l_] = streamBase + 0x9E3779B9u; pcg3d(h[3][l_], h[4][l_], h[5][l_]));
    for (int ly = 0; ly < gc.layers; ++ly) {
      float p[8], s[8], lam[8], K[8];
      const float dmin = gc.dminL[ly], invDmax = gc.invDmaxL[ly], nl = gc.nL[ly], uu = gc.u;
      L8(float pp = (D[ly][l_] + dmin) * invDmax; pp = pp < 1e-6f ? 1e-6f : pp; pp = pp > 1.f - 1e-6f ? 1.f - 1e-6f : pp;
         p[l_] = pp; s[l_] = 1.f - pp * uu * (1.f - 1e-6f); lam[l_] = nl * pp / s[l_]);
      poisson8(lam, h[2 * ly], h[2 * ly + 1], K);
      const float sc = gc.scaleL[ly];
      if (sumOnly) {
        L8(exact[l_] += K[l_] * sc * s[l_]);
      } else {
        float g[8];
        L8(g[l_] = K[l_] * sc * s[l_]);
        for (int l = 0; l < m; ++l) gl[ly][i0 + l] = g[l];
      }
    }
    if (sumOnly) {
      for (int l = 0; l < m; ++l) g0[i0 + l] = (useMerged && sum[l] >= 0.f) ? sum[l] : exact[l];
    }
  }
}

// p[i] = 2^(k p[i]) in place (print-exposure encode of the enlarger diffusion path).
static void exp2Scale(float* p, int n, float k) {
  for (int i = 0; i < n; ++i) p[i] = exp2f_(p[i] * k);
}

// Highlight boost (spec 03 s4.1) on one row: v > x0: v + X k (exp(a d) - a d - 1), d = (v - x0) / X.
static void boostRow(float* r, int n, float fx0, float fX, float fa, float fk) {
  const float invX = 1.f / fX, XK = fX * fk;
  for (int i = 0; i < n; ++i) {
    const float v = r[i];
    const float d = (v - fx0) * invX;
    const float ad = fa * d;
    const float b = v + XK * (expf_(ad) - ad - 1.f);
    r[i] = v > fx0 ? b : v;
  }
}

static void logNormalRow(float* dst, int n, int x0, int y, std::uint32_t stream, float mu, float sigma) {
  for (int i0 = 0; i0 < n; i0 += 8) {
    const int m = n - i0 < 8 ? n - i0 : 8;
    std::uint32_t w0[8], w1[8], w2[8];
    float v[8];
    L8(w0[l_] = std::uint32_t(x0 + i0 + l_); w1[l_] = std::uint32_t(y); w2[l_] = stream; pcg3d(w0[l_], w1[l_], w2[l_]));
    L8(v[l_] = expf_(mu + sigma * normalFrom(w0[l_], w1[l_])));
    for (int l = 0; l < m; ++l) dst[i0 + l] = v[l];
  }
}

}  // namespace sp
}  // namespace DAPPLE_KNS

const SpatialKernels DAPPLE_SSET = {DAPPLE_KNAME,        DAPPLE_KNS::sp::firRow,   DAPPLE_KNS::sp::firCol,
                                    DAPPLE_KNS::sp::scaleRow, DAPPLE_KNS::sp::down2Row, DAPPLE_KNS::sp::up2Row,
                                    DAPPLE_KNS::sp::lerpRows, DAPPLE_KNS::sp::grainRow, DAPPLE_KNS::sp::logNormalRow,
                                    DAPPLE_KNS::sp::exp2Scale, DAPPLE_KNS::sp::boostRow};

}  // namespace dapple::film::detail
