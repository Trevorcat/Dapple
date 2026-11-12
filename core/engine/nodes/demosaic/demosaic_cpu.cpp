// CPU Demosaic (see demosaic.h). The GPU kernels (shaders/demosaic_*.comp) mirror this arithmetic statement by statement.
#include "engine/nodes/demosaic/demosaic.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

namespace dapple::demosaic {

namespace {
constexpr float kEps = 0.002f;    // discriminator floor of the Bayer weights (1 = white)
constexpr float kEpsX = 0.004f;   // guide floor of the X-Trans chroma weights
constexpr float kEpsG = 0.002f;   // discriminator floor of the X-Trans green weights
constexpr float kXLap = 0.25f;    // weight of the Laplacian term of the X-Trans green (0.5 is the Hamilton-Adams value; 0.25 measured better, 31.x)
constexpr int kXR = 2;            // radius of the X-Trans chroma neighbourhood
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
            const float gd = kEpsX + std::fabs(G[q] - g);
            const float w = 1.f / (float(u * u + v * v) * gd * gd);
            num += w * (S[q] - G[q]);
            den += w;
          }
        dd[t] = den > 0.f ? num / den : 0.f;
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

}  // namespace

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

float scaleSample(const Desc& d, int x, int y) {
  const std::size_t i = std::size_t(y) * d.width + x;
  const int ph = (y % d.period) * d.period + (x % d.period);
  const float blk = d.black[(y % d.blackRows) * d.blackCols + (x % d.blackCols)];
  const float v = std::max(float(d.data[i]) - blk, 0.f);
  return std::min(v * d.wb[d.cfa4[ph]] * d.invSpan, 1.f);
}

void demosaicWindow(const Desc& d, const float* S, Rect roi, float* out, std::size_t strideFloats) {
  if (d.cfa == Cfa::Bayer) bayerTile(d, S, roi, out, strideFloats);
  else xtransTile(d, S, roi, out, strideFloats);
}

namespace {
void buildWindow(const Desc& d, Rect roi, std::vector<float>& S) {
  const int H = haloOf(d), ww = roi.w() + 2 * H, wh = roi.h() + 2 * H;
  S.resize(std::size_t(ww) * wh);
  std::vector<int> xs(ww);
  for (int i = 0; i < ww; ++i) xs[i] = mapCoord(roi.x0 - H + i, d.width, d.cfa);
  const int P = d.period;
  for (int j = 0; j < wh; ++j) {
    const int ay = roi.y0 - H + j, y = mapCoord(ay, d.height, d.cfa);
    float* row = S.data() + std::size_t(j) * ww;
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

void demosaic(const Desc& d, Rect roi, float* out, std::size_t strideFloats, int threads) {
  if (roi.empty()) return;
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
