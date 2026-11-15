// CPU Demosaic (see demosaic.h). The GPU kernels (shaders/demosaic_*.comp) mirror this arithmetic statement by statement.
#include "engine/nodes/demosaic/demosaic.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dapple::demosaic {

namespace {
constexpr float kEps = 0.002f;    // discriminator floor of the Bayer weights (1 = white)
constexpr float kEpsX = 0.004f;   // guide floor of the X-Trans chroma weights
constexpr float kEpsG = 0.002f;   // discriminator floor of the X-Trans green weights
constexpr float kXLap = 0.25f;    // weight of the Laplacian term of the X-Trans green (0.5 is the Hamilton-Adams value; 0.25 measured better, 31.x)
constexpr int kXR = 2;
constexpr int kClipSkip = 0;      // fast tier: 1 = ignore clipped samples in the colour differences (measured: no gain)
constexpr float kShrink = 0.f;
constexpr int kXMed = 1;          // 3 x 3 median of the colour differences (1 = on)
inline float sq(float x) { return x * x; }
inline float clamp01(float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }
inline int pmod(int a, int n) { return ((a % n) + n) % n; }

// ---- Bayer ---------------------------------------------------------------------------------------------------------------------

inline float bayerGreen(const float* p, int ww) {  // p at a red / blue sample
  const float C = p[0], l = p[-1], r = p[1], l2 = p[-2], r2 = p[2], u = p[-ww], d = p[ww], u2 = p[-2 * ww], d2 = p[2 * ww];
  float gh = 0.5f * (l + r) + 0.25f * (2.f * C - l2 - r2);
  float gv = 0.5f * (u + d) + 0.25f * (2.f * C - u2 - d2);
  gh = std::min(std::max(gh, std::min(l, r)), std::max(l, r));
  gv = std::min(std::max(gv, std::min(u, d)), std::max(u, d));
  const float dh = std::fabs(l - r) + std::fabs(2.f * C - l2 - r2) + 0.5f * (std::fabs(p[-ww - 1] - p[-ww + 1]) + std::fabs(p[ww - 1] - p[ww + 1]));
  const float dv = std::fabs(u - d) + std::fabs(2.f * C - u2 - d2) + 0.5f * (std::fabs(p[-1 - ww] - p[-1 + ww]) + std::fabs(p[1 - ww] - p[1 + ww]));
  const float wh = 1.f / sq(kEps + dh), wv = 1.f / sq(kEps + dv);
  return (wh * gh + wv * gv) / (wh + wv);
}

void bayerTile(const Desc& d, const float* S, Rect roi, float* out, std::size_t stride) {
  const int H = kHaloBayer, rw = roi.w(), rh = roi.h(), ww = rw + 2 * H, wh = rh + 2 * H;
  const int ox = pmod(roi.x0 - H, 2), oy = pmod(roi.y0 - H, 2);
  auto colAt = [&](int i, int j) { return int(d.cfa3[((j + oy) & 1) * 2 + ((i + ox) & 1)]); };
  std::vector<float> G(std::size_t(ww) * wh, 0.f);
  for (int j = 2; j < wh - 2; ++j)
    for (int i = 2; i < ww - 2; ++i) {
      const float* p = S + std::size_t(j) * ww + i;
      G[std::size_t(j) * ww + i] = colAt(i, j) == 1 ? *p : bayerGreen(p, ww);
    }
  for (int y = 0; y < rh; ++y) {
    float* o = out + std::size_t(y) * stride;
    for (int x = 0; x < rw; ++x) {
      const int i = x + H, j = y + H;
      const std::size_t k = std::size_t(j) * ww + i;
      const int c = colAt(i, j);
      const float g = G[k];
      float rgb[3];
      if (c == 1) {
        const int hc = colAt(i - 1, j);  // colour of the horizontal neighbours (0 or 2)
        const float dh = 0.5f * ((S[k - 1] - G[k - 1]) + (S[k + 1] - G[k + 1]));
        const float dv = 0.5f * ((S[k - ww] - G[k - ww]) + (S[k + ww] - G[k + ww]));
        rgb[1] = g;
        rgb[hc] = g + dh;
        rgb[2 - hc] = g + dv;
      } else {
        rgb[1] = g;
        rgb[c] = S[k];
        const float d1 = 0.5f * ((S[k - ww + 1] - G[k - ww + 1]) + (S[k + ww - 1] - G[k + ww - 1]));
        const float d2 = 0.5f * ((S[k - ww - 1] - G[k - ww - 1]) + (S[k + ww + 1] - G[k + ww + 1]));
        const float g1 = std::fabs(G[k - ww + 1] - g) + std::fabs(G[k + ww - 1] - g);
        const float g2 = std::fabs(G[k - ww - 1] - g) + std::fabs(G[k + ww + 1] - g);
        const float w1 = 1.f / sq(kEps + g1), w2 = 1.f / sq(kEps + g2);
        rgb[2 - c] = g + (w1 * d1 + w2 * d2) / (w1 + w2);
      }
      for (int q = 0; q < 3; ++q) rgb[q] = clamp01(rgb[q]);
      float* px = o + 3 * x;
      for (int r = 0; r < 3; ++r) {
        const float v = d.m[r * 3] * rgb[0] + d.m[r * 3 + 1] * rgb[1] + d.m[r * 3 + 2] * rgb[2];
        px[r] = d.clampOut ? clamp01(v) : v;
      }
    }
  }
}

// ---- X-Trans -------------------------------------------------------------------------------------------------------------------

struct XtTables {
  // per phase (y % 6) * 6 + (x % 6), direction (H, V, D1 = (1,1), D2 = (1,-1)): distance in steps to the nearest green on the - / + side (0 = none within 3)
  std::int8_t k[36][4][2];
  XtTables(const Desc& d) {
    static const int dx[4] = {1, 0, 1, 1}, dy[4] = {0, 1, 1, -1};
    for (int ph = 0; ph < 36; ++ph)
      for (int dir = 0; dir < 4; ++dir)
        for (int s = 0; s < 2; ++s) {
          int found = 0;
          const int sg = s == 0 ? -1 : 1;
          for (int q = 1; q <= 3 && !found; ++q) {
            const int x = pmod(ph % 6 + sg * q * dx[dir], 6), y = pmod(ph / 6 + sg * q * dy[dir], 6);
            if (d.cfa3[y * 6 + x] == 1) found = q;
          }
          k[ph][dir][s] = std::int8_t(found);
        }
  }
};

void xtransTile(const Desc& d, const float* S, Rect roi, float* out, std::size_t stride) {
  static const int dx[4] = {1, 0, 1, 1}, dy[4] = {0, 1, 1, -1};
  static const float len[4] = {1.f, 1.f, 1.41421356f, 1.41421356f};
  const XtTables T(d);
  const int H = kHaloXTrans, rw = roi.w(), rh = roi.h(), ww = rw + 2 * H, wh = rh + 2 * H;
  const int ox = pmod(roi.x0 - H, 6), oy = pmod(roi.y0 - H, 6);
  auto phase = [&](int i, int j) { return ((j + oy) % 6) * 6 + ((i + ox) % 6); };
  std::vector<float> G(std::size_t(ww) * wh, 0.f);
  for (int j = 3; j < wh - 3; ++j)
    for (int i = 3; i < ww - 3; ++i) {
      const int ph = phase(i, j);
      const float* p = S + std::size_t(j) * ww + i;
      if (d.cfa3[ph] == 1) {
        G[std::size_t(j) * ww + i] = *p;
        continue;
      }
      float num = 0.f, den = 0.f;
      for (int dir = 0; dir < 4; ++dir) {
        const int km = T.k[ph][dir][0], kp = T.k[ph][dir][1];
        if (!km || !kp) continue;
        const std::ptrdiff_t st = std::ptrdiff_t(dy[dir]) * ww + dx[dir];
        const float gm = p[-km * st], gp = p[kp * st];
        const float lm = float(km) * len[dir], lp = float(kp) * len[dir];
        const float est = (lp * gm + lm * gp) / (lm + lp);
        const float w = 1.f / (sq(kEpsG + std::fabs(gm - gp)) * sq(lm + lp));
        num += w * est;
        den += w;
      }
      if (den > 0.f) {
        float gest = num / den;
        if (kXLap > 0.f) {  // Laplacian term (the Hamilton-Adams correction, generalised): the sample's excess over the weighted mean of its own colour nearby
          const int c = d.cfa3[ph];
          float cs = 0.f, cw = 0.f, gmin = 1e9f, gmax = -1e9f;
          for (int v = -3; v <= 3; ++v)
            for (int u = -3; u <= 3; ++u) {
              if (!u && !v) continue;
              const int pc = d.cfa3[phase(i + u, j + v)];
              const float sv = p[v * ww + u];
              if (pc == c && u * u + v * v <= 10) {
                const float w = 1.f / float(u * u + v * v);
                cs += w * sv, cw += w;
              } else if (pc == 1 && u * u + v * v <= 8) {
                gmin = std::min(gmin, sv), gmax = std::max(gmax, sv);
              }
            }
          if (cw > 0.f && gmax >= gmin) gest = std::min(std::max(gest + kXLap * (*p - cs / cw), gmin), gmax);
        }
        G[std::size_t(j) * ww + i] = gest;
      } else {  // no direction has a green on both sides: the mean of the greens of the 3 x 3
        float s = 0.f;
        int n = 0;
        for (int v = -1; v <= 1; ++v)
          for (int u = -1; u <= 1; ++u)
            if ((u || v) && d.cfa3[phase(i + u, j + v)] == 1) s += p[v * ww + u], ++n;
        G[std::size_t(j) * ww + i] = n ? s / float(n) : *p;
      }
    }
  // colour differences of the ROI + 1 pixel ring (red - green, blue - green), then their 3 x 3 median: the isolated colour speckle of a
  // difference estimate at a one-pixel feature is a median outlier, a coloured edge is not (the dcraw / LibRaw "median passes" idea, one pass).
  const int cw = rw + 2, ch = rh + 2;
  std::vector<float> DR(std::size_t(cw) * ch), DB(std::size_t(cw) * ch);
  for (int y = -1; y <= rh; ++y)
    for (int x = -1; x <= rw; ++x) {
      const int i = x + H, j = y + H;
      const std::size_t k = std::size_t(j) * ww + i;
      const int c = d.cfa3[phase(i, j)];
      const float g = G[k];
      float dd[2];
      for (int t = 0; t < 2; ++t) {  // 0: red, 1: blue
        const int ct = t * 2;
        if (c == ct) {
          dd[t] = S[k] - g;
          continue;
        }
        float num = 0.f, den = 0.f;
        for (int v = -kXR; v <= kXR; ++v)
          for (int u = -kXR; u <= kXR; ++u) {
            if (d.cfa3[phase(i + u, j + v)] != ct) continue;
            const std::size_t q = k + std::ptrdiff_t(v) * ww + u;
            if (kClipSkip && S[q] >= 0.995f) continue;
            const float gd = kEpsX + std::fabs(G[q] - g);
            const float w = 1.f / (float(u * u + v * v) * gd * gd);
            num += w * (S[q] - G[q]);
            den += w;
          }
        dd[t] = den > 0.f ? num / den * (kShrink > 0.f ? den / (den + kShrink) : 1.f) : 0.f;
      }
      DR[std::size_t(y + 1) * cw + x + 1] = dd[0];
      DB[std::size_t(y + 1) * cw + x + 1] = dd[1];
    }
  auto med9 = [](const float* p, int stride) {
    float a[9];
    int n = 0;
    for (int v = -1; v <= 1; ++v)
      for (int u = -1; u <= 1; ++u) a[n++] = p[v * stride + u];
    std::nth_element(a, a + 4, a + 9);
    return a[4];
  };
  for (int y = 0; y < rh; ++y) {
    float* o = out + std::size_t(y) * stride;
    for (int x = 0; x < rw; ++x) {
      const int i = x + H, j = y + H;
      const std::size_t k = std::size_t(j) * ww + i;
      const std::size_t kc = std::size_t(y + 1) * cw + x + 1;
      const float g = G[k];
      float rgb[3];
      rgb[1] = g;
      rgb[0] = g + (kXMed ? med9(&DR[kc], cw) : DR[kc]);
      rgb[2] = g + (kXMed ? med9(&DB[kc], cw) : DB[kc]);
      for (int q = 0; q < 3; ++q) rgb[q] = clamp01(rgb[q]);
      float* px = o + 3 * x;
      for (int r = 0; r < 3; ++r) {
        const float v = d.m[r * 3] * rgb[0] + d.m[r * 3 + 1] * rgb[1] + d.m[r * 3 + 2] * rgb[2];
        px[r] = d.clampOut ? clamp01(v) : v;
      }
    }
  }
}

// ---- X-Trans, candidate directions + homogeneity selection (the Markesteijn idea, derived independently) -------------------------------------
// 1. per direction d (H, V, D1, D2) a green plane: the two nearest greens along d (every non-green phase has both within 3 steps), blended by
//    distance, plus a directional Laplacian term (the sample's excess over its own colour along d).
// 2. per direction the colour differences red - green, blue - green of every pixel: the guide-weighted mean of the unclipped samples of that colour
//    lying within one pixel of the line through the pixel along d (never from a clipped sample; none left: neutral).
// 3. every direction is one RGB candidate; a derivative map in a cube-root colour space (sum of squared differences to the 4 neighbours) says how smooth
//    the candidate is; the homogeneity of a candidate at p is the number of pixels of the 3 x 3 around p where its derivative is within a factor of the
//    best candidate's; the result is the mean of the candidates with the highest homogeneity.
constexpr float kThrF = 1.5f;   // a candidate is homogeneous at a pixel when its derivative is within this factor of the best candidate
constexpr float kThrA = 2e-4f;  // plus this absolute slack (cube-root colour units squared)
constexpr float kLapD = 0.25f;  // weight of the directional Laplacian term of the green candidates
constexpr float kGuide = 0.02f; // floor of the guide weight of the colour differences
constexpr int kHomTie = 1;      // candidates within this many homogeneity counts of the best are averaged
constexpr float kClip = 0.995f;
constexpr float kClipSim = 4.f;  // similarity mass (sum of guide weights) a clipped neighbourhood needs to be trusted
constexpr int kXtMed = 1;        // 3 x 3 median of the final colour differences
constexpr float kClipPen = 0.3f; // derivative penalty of a candidate that interpolated across a clipped green

// cube root of a non-negative float, 2 Newton steps from a bit-trick start (relative error ~1e-6); the GPU kernel uses pow(x, 1/3)
// (branch-free: the Newton steps run for every value and x <= 0 selects 0 at the end, so a loop over it vectorises; the same values as the early return)
inline float fastCbrt(float x) {
  const std::uint32_t i = std::bit_cast<std::uint32_t>(x) / 3 + 0x2a514067u;
  float y = std::bit_cast<float>(i);
  y = y - (y * y * y - x) / (3.f * y * y);
  y = y - (y * y * y - x) / (3.f * y * y);
  return x <= 0.f ? 0.f : y;
}

// exact median of nine by a sorting network (19 compare-exchanges; demosaic_xtrans.comp med9 is the same network). It returns the value std::nth_element
// would put in the middle, at a fraction of the cost (nth_element was 40 % of the X-Trans CPU time).
inline float median9(float a0, float a1, float a2, float a3, float a4, float a5, float a6, float a7, float a8) {
  auto s = [](float& x, float& y) {
    const float t = std::min(x, y);
    y = std::max(x, y);
    x = t;
  };
  s(a1, a2), s(a4, a5), s(a7, a8), s(a0, a1), s(a3, a4), s(a6, a7), s(a1, a2), s(a4, a5), s(a7, a8);
  s(a0, a3), s(a5, a8), s(a4, a7), s(a3, a6), s(a1, a4), s(a2, a5), s(a4, a7), s(a4, a2), s(a6, a4), s(a4, a2);
  return a4;
}


void xtransTileCand(const Desc& d, const float* S, Rect roi, float* out, std::size_t stride) {
  static const int dx[4] = {1, 0, 1, 1}, dy[4] = {0, 1, 1, -1};
  static const float len[4] = {1.f, 1.f, 1.41421356f, 1.41421356f};
  const XtTables T(d);
  const XtTaps TP(d);
  const int H = kHaloXTransCand, rw = roi.w(), rh = roi.h(), ww = rw + 2 * H, wh = rh + 2 * H;
  const int ox = pmod(roi.x0 - H, 6), oy = pmod(roi.y0 - H, 6);
  auto phase = [&](int i, int j) { return ((j + oy) % 6) * 6 + ((i + ox) % 6); };
  const std::size_t N = std::size_t(ww) * wh;
  // per-thread planes, not cleared: every stage reads only what the stage before it wrote (the rings shrink stage by stage)
  thread_local std::vector<float> G4, DD, D4, CC, SG, SR, SB, THR, MR, MB;
  thread_local std::vector<std::uint8_t> HM;
  auto atLeast = [](auto& v, std::size_t n) {
    if (v.size() < n) v.resize(n);
  };
  atLeast(G4, 4 * N), atLeast(DD, 8 * N), atLeast(D4, 4 * N), atLeast(CC, 36 * std::size_t(ww)), atLeast(THR, N);
  // 1. directional green
  for (int j = 3; j < wh - 3; ++j)
    for (int i = 3; i < ww - 3; ++i) {
      const std::size_t k = std::size_t(j) * ww + i;
      const int ph = phase(i, j), c = d.cfa3[ph];
      const float* p = S + k;
      for (int dir = 0; dir < 4; ++dir) {
        if (c == 1) {
          G4[4 * k + dir] = *p;
          continue;
        }
        const int km = T.k[ph][dir][0], kp = T.k[ph][dir][1];
        const std::ptrdiff_t st = std::ptrdiff_t(dy[dir]) * ww + dx[dir];
        const float gm = p[-km * st], gp = p[kp * st];
        const float lm = float(km) * len[dir], lp = float(kp) * len[dir];
        float g = (lp * gm + lm * gp) / (lm + lp);
        const int nl = TP.nlap[ph][dir];
        if (nl) {
          float cs = 0.f;
          for (int a = 0; a < nl; ++a) cs += p[TP.lap[ph][dir][a] * st];
          g += kLapD * (*p - cs / float(nl));
        }
        G4[4 * k + dir] = std::min(std::max(g, std::min(gm, gp)), std::max(gm, gp));
      }
    }
  // 2. directional colour differences (red, blue): [8 k + 4 t + dir]
  // One pixel: the reference statement order. Pixels i, i + 6, i + 12, i + 18 of a row share the phase, so they share the tap lists: chromaQuad runs them as
  // the four lanes of a vector, every lane doing exactly the statements of the scalar form in the same order (the per-lane results are bit-identical).
  auto chromaOne = [&](int i, int j) {
    const std::size_t k = std::size_t(j) * ww + i;
    const int ph = phase(i, j), c = d.cfa3[ph];
    for (int dir = 0; dir < 4; ++dir) {
      const float g = G4[4 * k + dir];
      for (int t = 0; t < 2; ++t) {
        float res;
        if (c == t * 2) {
          res = S[k] - g;
        } else {
          float num = 0.f, den = 0.f, numC = 0.f, denC = 0.f;  // unclipped / clipped samples
          const int nt = TP.n[ph][dir][t];
          for (int a = 0; a < nt; ++a) {
            const XtTaps::Tap& tp = TP.t[ph][dir][t][a];
            const std::size_t q = k + std::ptrdiff_t(tp.v) * ww + tp.u;
            const float gq = G4[4 * q + dir];
            const float w = tp.gw / (kGuide + std::fabs(gq - g));
            if (S[q] >= kClip) numC += w * (S[q] - gq), denC += w;
            else num += w * (S[q] - gq), den += w;
          }
          // colour differences from unclipped samples only; clipped ones count only inside a clipped region (similar green), see 32.3
          res = den > 0.f ? num / den : (denC >= kClipSim ? numC / denC : 0.f);
        }
        DD[8 * k + 4 * t + dir] = res;
      }
    }
  };
#if defined(__clang__)
  auto chromaQuad = [&](int i, int j) {  // lanes: pixels i + 6 l, l = 0..3
    typedef float F4 __attribute__((ext_vector_type(4)));
    typedef int I4 __attribute__((ext_vector_type(4)));
    const std::size_t k0 = std::size_t(j) * ww + i;
    const int ph = phase(i, j), c = d.cfa3[ph];
    auto at = [&](const float* base, std::size_t k, int step) { return F4{base[k], base[k + std::size_t(step)], base[k + 2 * std::size_t(step)], base[k + 3 * std::size_t(step)]}; };
    for (int dir = 0; dir < 4; ++dir) {
      const F4 g = at(G4.data() + dir, 4 * k0, 24);
      for (int t = 0; t < 2; ++t) {
        F4 res;
        if (c == t * 2) {
          res = at(S, k0, 6) - g;
        } else {
          F4 num = 0.f, den = 0.f, numC = 0.f, denC = 0.f;
          const int nt = TP.n[ph][dir][t];
          for (int a = 0; a < nt; ++a) {
            const XtTaps::Tap& tp = TP.t[ph][dir][t][a];
            const std::size_t q = k0 + std::ptrdiff_t(tp.v) * ww + tp.u;
            const F4 gq = at(G4.data() + dir, 4 * q, 24), sq = at(S, q, 6);
            const F4 w = tp.gw / (kGuide + __builtin_elementwise_abs(gq - g));
            const F4 n2 = num + w * (sq - gq), d2 = den + w, n2C = numC + w * (sq - gq), d2C = denC + w;  // both branches of the scalar form, then the lane's one
            const I4 clip = sq >= kClip;  // all ones where clipped
            auto pick = [](I4 m, F4 a2, F4 b2) {  // a2 where m, else b2 (bitwise: no arithmetic on the values)
              const I4 r = (m & __builtin_bit_cast(I4, a2)) | (~m & __builtin_bit_cast(I4, b2));
              return __builtin_bit_cast(F4, r);
            };
            num = pick(clip, num, n2), den = pick(clip, den, d2), numC = pick(clip, n2C, numC), denC = pick(clip, d2C, denC);
          }
          for (int l = 0; l < 4; ++l) res[l] = den[l] > 0.f ? num[l] / den[l] : (denC[l] >= kClipSim ? numC[l] / denC[l] : 0.f);
        }
        for (int l = 0; l < 4; ++l) DD[8 * (k0 + 6 * std::size_t(l)) + 4 * t + dir] = res[l];
      }
    }
  };
#endif
  for (int j = 6; j < wh - 6; ++j) {
    int i = 6;
#if defined(__clang__)
    for (; i + 24 <= ww - 6; i += 24)
      for (int r = 0; r < 6; ++r) chromaQuad(i + r, j);
#endif
    for (; i < ww - 6; ++i) chromaOne(i, j);
  }
  // candidate colours in the cube-root space, then 3. the derivative maps, row by row: the colours live in a ring of three rows (CC[12 (row % 3) ww + 12 i +
  // 3 dir + channel]), the derivatives of row j - 1 follow the colours of row j (a whole-window colour plane was 12 floats per pixel through memory)
  auto ccRow = [&](int j) { return CC.data() + 12 * std::size_t(j % 3) * ww; };
  for (int j = 6; j < wh - 6; ++j) {
    float* cr = ccRow(j);
    for (int i = 6; i < ww - 6; ++i) {
      const std::size_t k = std::size_t(j) * ww + i;
      const int c = d.cfa3[phase(i, j)];
      for (int dir = 0; dir < 4; ++dir) {
        const float g = G4[4 * k + dir];
        float rgb[3] = {c == 0 ? S[k] : g + DD[8 * k + dir], g, c == 2 ? S[k] : g + DD[8 * k + 4 + dir]};
        for (int q = 0; q < 3; ++q) rgb[q] = clamp01(rgb[q]);
        for (int r = 0; r < 3; ++r) cr[12 * i + 3 * dir + r] = d.m[r * 3] * rgb[0] + d.m[r * 3 + 1] * rgb[1] + d.m[r * 3 + 2] * rgb[2];
      }
    }
    float* row = cr + 12 * 6;  // the cube roots over the row as one contiguous run: that loop vectorises, same arithmetic per value
    const int n = 12 * (ww - 12);
    for (int a = 0; a < n; ++a) row[a] = fastCbrt(row[a]);
    const int jd = j - 1;  // derivative row: its colour rows jd - 1 .. jd + 1 are in the ring now
    if (jd < 7 || jd >= wh - 7) continue;
    const float *cu = ccRow(jd - 1), *cm = ccRow(jd), *cd = ccRow(jd + 1);
    for (int i = 7; i < ww - 7; ++i) {
      const std::size_t k = std::size_t(jd) * ww + i;
      const int ph = phase(i, jd);
      for (int dir = 0; dir < 4; ++dir) {
        const float* c0 = cm + 12 * i + 3 * dir;
        float sum = 0.f;
        const float* nb[4] = {c0 - 12, c0 + 12, cu + 12 * i + 3 * dir, cd + 12 * i + 3 * dir};  // left, right, up, down
        for (int n2 = 0; n2 < 4; ++n2) {
          const float* cn = nb[n2];
          sum += sq(c0[0] - cn[0]) + sq(c0[1] - cn[1]) + sq(c0[2] - cn[2]);
        }
        if (d.cfa3[ph] != 1) {  // a candidate that interpolated across a clipped green is a poor one
          const int km = T.k[ph][dir][0], kp = T.k[ph][dir][1];
          const std::ptrdiff_t st = std::ptrdiff_t(dy[dir]) * ww + dx[dir];
          if (S[k - km * st] >= kClip || S[k + kp * st] >= kClip) sum += kClipPen;
        }
        D4[4 * k + dir] = sum;
      }
      // the homogeneity threshold of this pixel, once (the selection below reads it for 9 pixels)
      const float m = std::min(std::min(D4[4 * k], D4[4 * k + 1]), std::min(D4[4 * k + 2], D4[4 * k + 3]));
      THR[k] = m * kThrF + kThrA;
    }
  }
  // 4. homogeneity selection on the ROI + 1 ring: the mean of the best candidates as green and two colour differences
  //    (per row: the homogeneity counts in one plain loop, which vectorises, then the selection)
  const int cw = rw + 2, ch = rh + 2;
  atLeast(SG, std::size_t(cw) * ch), atLeast(SR, std::size_t(cw) * ch), atLeast(SB, std::size_t(cw) * ch), atLeast(HM, 4 * std::size_t(cw));
  for (int y = -1; y <= rh; ++y) {
    const int j = y + H;
    {
      const float* __restrict dq = D4.data();
      const float* __restrict tq = THR.data();
      std::uint8_t* __restrict hmr = HM.data();
      for (int x = -1; x <= rw; ++x) {
        const std::size_t k = std::size_t(j) * ww + x + H;
        int h0 = 0, h1 = 0, h2 = 0, h3 = 0;
        for (int v = -1; v <= 1; ++v)
          for (int u = -1; u <= 1; ++u) {
            const std::size_t q = k + std::ptrdiff_t(v) * ww + u;
            const float thr = tq[q];
            h0 += dq[4 * q] <= thr, h1 += dq[4 * q + 1] <= thr, h2 += dq[4 * q + 2] <= thr, h3 += dq[4 * q + 3] <= thr;
          }
        std::uint8_t* hx = hmr + 4 * std::size_t(x + 1);
        hx[0] = std::uint8_t(h0), hx[1] = std::uint8_t(h1), hx[2] = std::uint8_t(h2), hx[3] = std::uint8_t(h3);
      }
    }
    for (int x = -1; x <= rw; ++x) {
      const int i = x + H;
      const std::size_t k = std::size_t(j) * ww + i;
      const std::uint8_t* hm = HM.data() + 4 * std::size_t(x + 1);
      const int hmax = std::max(std::max(hm[0], hm[1]), std::max(hm[2], hm[3]));
      float g = 0.f, r = 0.f, b2 = 0.f;
      int cnt = 0;
      const int c = d.cfa3[phase(i, j)];
      for (int dir = 0; dir < 4; ++dir) {
        if (hm[dir] < hmax - kHomTie) continue;
        const float gd = G4[4 * k + dir];
        g += gd;
        r += c == 0 ? S[k] : gd + DD[8 * k + dir];
        b2 += c == 2 ? S[k] : gd + DD[8 * k + 4 + dir];
        ++cnt;
      }
      g /= float(cnt), r /= float(cnt), b2 /= float(cnt);
      const std::size_t o2 = std::size_t(y + 1) * cw + x + 1;
      SG[o2] = g;
      SR[o2] = clamp01(r) - clamp01(g);
      SB[o2] = clamp01(b2) - clamp01(g);
    }
  }
  // 5. 3 x 3 median of the two colour differences (the isolated colour speckle of an estimate at a one-pixel feature is an outlier, an edge is not), matrix, output
  //    (the medians of a row in plain loops over planar differences: they vectorise)
  atLeast(MR, std::size_t(rw)), atLeast(MB, std::size_t(rw));
  auto medRow = [&](const float* __restrict p, float* __restrict m) {  // p: the plane at (row y + 1, column 1) of the ring layout
    const std::ptrdiff_t r = cw;
    for (int x = 0; x < rw; ++x) {
      const float* q = p + x;
      m[x] = median9(q[-r - 1], q[-r], q[-r + 1], q[-1], q[0], q[1], q[r - 1], q[r], q[r + 1]);
    }
  };
  for (int y = 0; y < rh; ++y) {
    float* o = out + std::size_t(y) * stride;
    const std::size_t o0 = std::size_t(y + 1) * cw + 1;
    if (kXtMed) {
      medRow(SR.data() + o0, MR.data());
      medRow(SB.data() + o0, MB.data());
    } else {
      std::memcpy(MR.data(), SR.data() + o0, std::size_t(rw) * 4);
      std::memcpy(MB.data(), SB.data() + o0, std::size_t(rw) * 4);
    }
    for (int x = 0; x < rw; ++x) {
      const float g = clamp01(SG[o0 + x]);
      float rgb[3] = {g + MR[x], g, g + MB[x]};
      for (int q = 0; q < 3; ++q) rgb[q] = clamp01(rgb[q]);
      float* px = o + 3 * x;
      for (int r = 0; r < 3; ++r) {
        const float v = d.m[r * 3] * rgb[0] + d.m[r * 3 + 1] * rgb[1] + d.m[r * 3 + 2] * rgb[2];
        px[r] = d.clampOut ? clamp01(v) : v;
      }
    }
  }
}

}  // namespace

XtTaps::XtTaps(const Desc& d) {
  static const int dx[4] = {1, 0, 1, 1}, dy[4] = {0, 1, 1, -1};
  static const float len[4] = {1.f, 1.f, 1.41421356f, 1.41421356f};
  for (int ph = 0; ph < 36; ++ph)
    for (int dir = 0; dir < 4; ++dir) {
      for (int c = 0; c < 2; ++c) {
        int cnt = 0;
        for (int v = -3; v <= 3; ++v)
          for (int u = -3; u <= 3; ++u) {
            const int r2 = u * u + v * v;
            if (r2 == 0 || r2 > 10) continue;
            const float perp = std::fabs(float(u * dy[dir] - v * dx[dir])) / len[dir];
            if (perp > 1.01f) continue;
            if (d.cfa3[pmod(ph / 6 + v, 6) * 6 + pmod(ph % 6 + u, 6)] != c * 2) continue;
            if (cnt < 12) t[ph][dir][c][cnt++] = {std::int8_t(u), std::int8_t(v), 1.f / (float(r2) * (1.f + perp * perp))};
          }
        n[ph][dir][c] = std::int8_t(cnt);
      }
      int nl = 0;
      const int col = d.cfa3[ph];
      for (int q = -3; q <= 3; ++q)
        if (q && d.cfa3[pmod(ph / 6 + q * dy[dir], 6) * 6 + pmod(ph % 6 + q * dx[dir], 6)] == col) lap[ph][dir][nl++] = std::int8_t(q);
      nlap[ph][dir] = std::int8_t(nl);
    }
}

int mapCoord(int i, int n, Cfa c) {
  if (i >= 0 && i < n) return i;
  if (c == Cfa::Bayer) {
    if (n < 2) return 0;
    for (int guard = 0; guard < 8 && (i < 0 || i >= n); ++guard) i = i < 0 ? -i : 2 * (n - 1) - i;
    return std::min(std::max(i, 0), n - 1);
  }
  if (n < 6) return std::min(std::max(i, 0), n - 1);
  if (i < 0) i += 6 * ((-i + 5) / 6);
  else i -= 6 * ((i - n) / 6 + 1);
  return std::min(std::max(i, 0), n - 1);
}

namespace {
std::atomic<bool> g_interaction{false};
}
void setInteraction(bool on) { g_interaction.store(on, std::memory_order_relaxed); }
bool interactionActive() { return g_interaction.load(std::memory_order_relaxed); }
void yieldWhileInteracting(const std::atomic<std::uint64_t>* cancelCur, std::uint64_t cancelMine) {
  for (int waited = 0; g_interaction.load(std::memory_order_relaxed) && waited < 500; ++waited) {  // 500 x 4 ms
    if (cancelCur && cancelCur->load(std::memory_order_relaxed) != cancelMine) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
  }
}

float scaleSample(const Desc& d, int x, int y) {
  const std::size_t i = std::size_t(y) * d.width + x;
  const int ph = (y % d.period) * d.period + (x % d.period);
  const float blk = d.black[(y % d.blackRows) * d.blackCols + (x % d.blackCols)];
  const float v = std::max(float(d.data[i]) - blk, 0.f);
  return std::min(v * d.wb[d.cfa4[ph]] * d.invSpan, 1.f);
}

void demosaicWindow(const Desc& d, const float* S, Rect roi, float* out, std::size_t strideFloats) {
  if (d.cfa == Cfa::Bayer) bayerTile(d, S, roi, out, strideFloats);
  else if (d.xtMode >= 1) xtransTileCand(d, S, roi, out, strideFloats);
  else xtransTile(d, S, roi, out, strideFloats);
}

namespace {
constexpr float kHotTau = 0.1f, kHotMaxNeighbour = 0.9f;

// Isolated hot sample: well above every same-colour neighbour within 3 pixels while those neighbours are far from clipping (a real highlight has bright neighbours).
// Replaced by the trimmed mean (without the largest and the smallest) of the neighbours.
void hotFilter(const Desc& d, const std::vector<float>& W, int ww, int wh, int E, int ox, int oy, std::vector<float>& S, int iw, int ih) {
  for (int j = 0; j < ih; ++j)
    for (int i = 0; i < iw; ++i) {
      const int wi = i + E, wj = j + E;
      const float v = W[std::size_t(wj) * ww + wi];
      float out = v;
      if (v > kHotTau) {
        const int c = d.cfa3[((wj + oy) % d.period) * d.period + ((wi + ox) % d.period)];
        float sum = 0.f, mx = -1.f, mn = 2.f;
        int n = 0;
        for (int dv = -3; dv <= 3; ++dv)
          for (int du = -3; du <= 3; ++du) {
            if ((du == 0 && dv == 0) || du * du + dv * dv > 10) continue;
            if (d.cfa3[((wj + dv + oy + 12) % d.period) * d.period + ((wi + du + ox + 12) % d.period)] != c) continue;
            const float q = W[std::size_t(wj + dv) * ww + wi + du];
            sum += q, mx = std::max(mx, q), mn = std::min(mn, q), ++n;
          }
        if (n >= 4 && v > mx + kHotTau && mx < kHotMaxNeighbour) out = (sum - mx - mn) / float(n - 2);
      }
      S[std::size_t(j) * iw + i] = out;
    }
  (void)wh;
}

void buildWindow(const Desc& d, Rect roi, std::vector<float>& S) {
  const int E = hotExtra(d), H = haloOf(d) + E, ww = roi.w() + 2 * H, wh = roi.h() + 2 * H;
  std::vector<float> Wbig;
  std::vector<float>& S0 = E ? Wbig : S;
  S0.resize(std::size_t(ww) * wh);
  std::vector<int> xs(ww);
  for (int i = 0; i < ww; ++i) xs[i] = mapCoord(roi.x0 - H + i, d.width, d.cfa);
  const int P = d.period;
  for (int j = 0; j < wh; ++j) {
    const int ay = roi.y0 - H + j, y = mapCoord(ay, d.height, d.cfa);
    float* row = S0.data() + std::size_t(j) * ww;
    const std::uint16_t* src = d.data + std::size_t(y) * d.width;
    for (int i = 0; i < ww; ++i) {
      // the sample comes from the mapped pixel; the colour phase and the black table phase from the window position (what the GPU kernel sees)
      const int ax = roi.x0 - H + i;
      const int c4 = d.cfa4[pmod(ay, P) * P + pmod(ax, P)];
      const float blk = d.black[pmod(ay, d.blackRows) * d.blackCols + pmod(ax, d.blackCols)];
      const float v = std::max(float(src[xs[i]]) - blk, 0.f) * d.wb[c4] * d.invSpan;
      row[i] = std::min(v, 1.f);
    }
  }
  if (E) {
    const int iw = roi.w() + 2 * (H - E), ih = roi.h() + 2 * (H - E);
    S.resize(std::size_t(iw) * ih);
    hotFilter(d, S0, ww, wh, E, pmod(roi.x0 - H, d.period), pmod(roi.y0 - H, d.period), S, iw, ih);
  }
}

template <class F>
void runTiles(const Desc& d, Rect roi, int threads, int tile, F&& tileFn) {
  std::vector<Rect> tiles;
  for (int y = roi.y0; y < roi.y1; y += tile)
    for (int x = roi.x0; x < roi.x1; x += tile) tiles.push_back({x, y, std::min(x + tile, roi.x1), std::min(y + tile, roi.y1)});
  int nt = threads > 0 ? threads : int(std::max(1u, std::thread::hardware_concurrency()));
  nt = std::min<int>(nt, int(tiles.size()));
  std::atomic<std::size_t> next{0};
  auto worker = [&] {
    std::vector<float> S;
    for (;;) {
      const std::size_t t = next.fetch_add(1);
      if (t >= tiles.size()) break;
      if (d.cancelCur && d.cancelCur->load(std::memory_order_relaxed) != d.cancelMine) break;
      yieldWhileInteracting(d.cancelCur, d.cancelMine);
      tileFn(tiles[t], S);
    }
  };
  if (nt <= 1) {
    worker();
    return;
  }
  std::vector<std::thread> th;
  for (int i = 1; i < nt; ++i) th.emplace_back(worker);
  worker();
  for (auto& t : th) t.join();
}
}  // namespace

#include "demosaic_half.inc"

void demosaic(const Desc& d, Rect roi, float* out, std::size_t strideFloats, int threads) {
  if (roi.empty()) return;
  if (d.half && d.cfa == Cfa::XTrans) return demosaicHalf(d, roi, out, strideFloats, threads);
  runTiles(d, roi, threads, 256, [&](Rect t, std::vector<float>& S) {
    buildWindow(d, t, S);
    demosaicWindow(d, S.data(), t, out + std::size_t(t.y0 - roi.y0) * strideFloats + std::size_t(t.x0 - roi.x0) * 3, strideFloats);
  });
}

void bilinear(const Desc& d, Rect roi, float* out, std::size_t strideFloats, int threads) {
  if (roi.empty()) return;
  runTiles(d, roi, threads, 256, [&](Rect t, std::vector<float>& S) {
    const int H = 2, ww = t.w() + 2 * H;
    std::vector<float> W(std::size_t(ww) * (t.h() + 2 * H));
    for (int j = 0; j < t.h() + 2 * H; ++j)
      for (int i = 0; i < ww; ++i) W[std::size_t(j) * ww + i] = scaleSample(d, mapCoord(t.x0 - H + i, d.width, d.cfa), mapCoord(t.y0 - H + j, d.height, d.cfa));
    (void)S;
    for (int y = 0; y < t.h(); ++y)
      for (int x = 0; x < t.w(); ++x) {
        float sum[3] = {0, 0, 0}, cnt[3] = {0, 0, 0};
        for (int v = -2; v <= 2; ++v)
          for (int u = -2; u <= 2; ++u) {
            const int ax = mapCoord(t.x0 + x + u, d.width, d.cfa), ay = mapCoord(t.y0 + y + v, d.height, d.cfa);
            const int c = d.cfa3[(ay % d.period) * d.period + (ax % d.period)];
            const float w = 1.f / float(1 + u * u + v * v);
            sum[c] += w * W[std::size_t(y + H + v) * ww + (x + H + u)];
            cnt[c] += w;
          }
        float rgb[3];
        for (int c = 0; c < 3; ++c) rgb[c] = clamp01(cnt[c] > 0 ? sum[c] / cnt[c] : 0.f);
        float* px = out + std::size_t(t.y0 - roi.y0 + y) * strideFloats + std::size_t(t.x0 - roi.x0 + x) * 3;
        for (int r = 0; r < 3; ++r) {
          const float v = d.m[r * 3] * rgb[0] + d.m[r * 3 + 1] * rgb[1] + d.m[r * 3 + 2] * rgb[2];
          px[r] = d.clampOut ? clamp01(v) : v;
        }
      }
  });
}

}  // namespace dapple::demosaic
