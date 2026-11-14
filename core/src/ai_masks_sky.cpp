// Classical sky prior (docs section 41.6): no licensed sky model exists, so this is colour + texture + connectivity only. It only claims a
// sky when it finds a smooth, blue-ish or bright region connected to the top edge; it never invents a sky on overcast / white scenes.
#include <algorithm>
#include <cmath>
#include <deque>

#include "ai_masks.h"

namespace dapple::aimask {
namespace {
inline float sstep(float a, float b, float x) {
  const float t = std::clamp((x - a) / (b - a), 0.f, 1.f);
  return t * t * (3.f - 2.f * t);
}
}  // namespace

Plane skyPrior(const std::uint8_t* rgb, int w, int h, int stride, const Plane* farDepth, float* confidence) {
  if (confidence) *confidence = 0.f;
  if (w < 32 || h < 32) return Plane{};
  const int L = std::max(w, h);
  const int gw = std::max(8, int(std::lround(320.0 * w / L))), gh = std::max(8, int(std::lround(320.0 * h / L)));
  const std::size_t N = std::size_t(gw) * gh;
  std::vector<float> R(N), G(N), B(N), Y(N), far(N, 1.f);
  for (int j = 0; j < gh; ++j) {
    const int y0 = j * h / gh, y1 = std::max(y0 + 1, (j + 1) * h / gh);
    for (int i = 0; i < gw; ++i) {
      const int x0 = i * w / gw, x1 = std::max(x0 + 1, (i + 1) * w / gw);
      double a[3] = {0, 0, 0}, d = 0;
      int n = 0;
      for (int y = y0; y < y1 && y < h; ++y)
        for (int x = x0; x < x1 && x < w; ++x) {
          const std::uint8_t* p = rgb + std::size_t(y) * stride + std::size_t(x) * 3;
          a[0] += p[0], a[1] += p[1], a[2] += p[2];
          if (farDepth && !farDepth->empty()) d += farDepth->px[std::size_t(y) * farDepth->w + x];
          ++n;
        }
      const std::size_t k = std::size_t(j) * gw + i;
      R[k] = float(a[0] / n / 255.0), G[k] = float(a[1] / n / 255.0), B[k] = float(a[2] / n / 255.0);
      Y[k] = 0.2126f * R[k] + 0.7152f * G[k] + 0.0722f * B[k];
      if (farDepth && !farDepth->empty()) far[k] = float(d / n / 255.0);
    }
  }
  // local texture: standard deviation of the luma in a 5 x 5 window
  std::vector<float> tex(N, 0.f);
  for (int j = 0; j < gh; ++j)
    for (int i = 0; i < gw; ++i) {
      double s = 0, s2 = 0;
      int n = 0;
      for (int dj = -2; dj <= 2; ++dj)
        for (int di = -2; di <= 2; ++di) {
          const int jj = std::clamp(j + dj, 0, gh - 1), ii = std::clamp(i + di, 0, gw - 1);
          const double v = Y[std::size_t(jj) * gw + ii];
          s += v, s2 += v * v, ++n;
        }
      const double m = s / n;
      tex[std::size_t(j) * gw + i] = float(std::sqrt(std::max(0.0, s2 / n - m * m)));
    }
  std::vector<float> blue(N), cand(N);
  for (std::size_t k = 0; k < N; ++k) {
    const float mx = std::max({R[k], G[k], B[k]}), mn = std::min({R[k], G[k], B[k]});
    const float sat = mx > 1e-4f ? (mx - mn) / mx : 0.f;
    float hue = 0.f;
    if (mx - mn > 1e-5f) {
      if (mx == R[k]) hue = std::fmod((G[k] - B[k]) / (mx - mn), 6.f);
      else if (mx == G[k]) hue = (B[k] - R[k]) / (mx - mn) + 2.f;
      else hue = (R[k] - G[k]) / (mx - mn) + 4.f;
      hue *= 60.f;
      if (hue < 0) hue += 360.f;
    }
    blue[k] = sstep(165.f, 195.f, hue) * (1.f - sstep(255.f, 280.f, hue)) * sstep(0.07f, 0.2f, sat) * sstep(0.25f, 0.45f, mx);
    const float cloud = (1.f - sstep(0.10f, 0.18f, sat)) * sstep(0.62f, 0.8f, mx);  // bright, nearly grey: cloud / haze
    // blue needs little smoothness (blue seen between branches is sky; the branch pixels themselves are not blue and stay out); grey / white needs a smooth region
    const float smoothBlue = 1.f - sstep(0.12f, 0.22f, tex[k]), smoothCloud = 1.f - sstep(0.035f, 0.07f, tex[k]);
    cand[k] = std::max(blue[k] * smoothBlue, cloud * 0.9f * smoothCloud) * sstep(0.2f, 0.5f, far[k]);
  }
  // region growing from blue seeds in the top 20 % of the frame
  std::vector<std::uint8_t> in(N, 0);
  std::deque<std::size_t> q;
  for (int j = 0; j < std::max(2, gh / 5); ++j)
    for (int i = 0; i < gw; ++i) {
      const std::size_t k = std::size_t(j) * gw + i;
      if (blue[k] > 0.5f && cand[k] > 0.5f) in[k] = 1, q.push_back(k);
    }
  while (!q.empty()) {
    const std::size_t k = q.front();
    q.pop_front();
    const int i = int(k % gw), j = int(k / gw);
    const int nb[4][2] = {{i - 1, j}, {i + 1, j}, {i, j - 1}, {i, j + 1}};
    for (auto& n : nb) {
      if (n[0] < 0 || n[1] < 0 || n[0] >= gw || n[1] >= gh) continue;
      const std::size_t kk = std::size_t(n[1]) * gw + n[0];
      if (!in[kk] && cand[kk] > 0.5f) in[kk] = 1, q.push_back(kk);
    }
  }
  std::size_t area = 0, blueCells = 0;
  double cy = 0;
  for (std::size_t k = 0; k < N; ++k)
    if (in[k]) ++area, cy += double(k / gw), blueCells += blue[k] > 0.5f;
  float frac = float(area) / float(N);
  float conf = frac * (area ? float(blueCells) / float(area) : 0.f);
  const bool blueOk = !(area == 0 || frac < 0.04f || float(blueCells) < 0.25f * float(area) || cy / double(area) > 0.6 * gh);
  if (!blueOk) {
    if (!(farDepth && !farDepth->empty())) return Plane{};  // without the depth far plane a bright smooth region could be a wall: no overcast sky
    // Overcast / hazy sky (no blue): a bright, smooth, slightly cool region that spans a good part of the top edge. A neutral or warm wall never
    // passes the cool-tint test, and the far plane has to agree (far > 0.5 over most of the region), which keeps indoor walls and backdrops out.
    std::fill(in.begin(), in.end(), 0);
    q.clear();
    std::vector<float> ccand(N);
    for (std::size_t k = 0; k < N; ++k) {
      const float mx = std::max({R[k], G[k], B[k]}), mn = std::min({R[k], G[k], B[k]});
      const float sat = mx > 1e-4f ? (mx - mn) / mx : 0.f;
      ccand[k] = (1.f - sstep(0.14f, 0.24f, sat)) * sstep(0.55f, 0.72f, Y[k]) * (1.f - sstep(0.03f, 0.06f, tex[k])) * sstep(0.45f, 0.6f, far[k]);
    }
    for (int j = 0; j < std::max(2, gh / 6); ++j)
      for (int i = 0; i < gw; ++i) {
        const std::size_t k = std::size_t(j) * gw + i;
        if (ccand[k] > 0.5f && B[k] - R[k] > 0.004f) in[k] = 1, q.push_back(k);
      }
    while (!q.empty()) {
      const std::size_t k = q.front();
      q.pop_front();
      const int i = int(k % gw), j = int(k / gw);
      const int nb[4][2] = {{i - 1, j}, {i + 1, j}, {i, j - 1}, {i, j + 1}};
      for (auto& n : nb) {
        if (n[0] < 0 || n[1] < 0 || n[0] >= gw || n[1] >= gh) continue;
        const std::size_t kk = std::size_t(n[1]) * gw + n[0];
        if (!in[kk] && ccand[kk] > 0.5f) in[kk] = 1, q.push_back(kk);
      }
    }
    area = 0, cy = 0;
    double cool = 0, ym = 0, y2 = 0;
    int top = 0;
    for (std::size_t k = 0; k < N; ++k)
      if (in[k]) ++area, cy += double(k / gw), cool += B[k] - R[k], ym += Y[k], y2 += double(Y[k]) * Y[k];
    for (int i = 0; i < gw; ++i) top += in[i];
    frac = float(area) / float(N);
    if (area == 0 || frac < 0.06f || cy / double(area) > 0.45 * gh || float(top) < 0.35f * float(gw) || cool / double(area) < 0.012 || ym / double(area) < 0.6)
      return Plane{};
    // cloud structure: a flat, evenly lit region is a wall or a backdrop, an overcast sky has soft tonal variation
    const double ymean = ym / double(area);
    if (std::sqrt(std::max(0.0, y2 / double(area) - ymean * ymean)) < 0.05) return Plane{};
    cand = ccand;
    conf = 0.5f * frac;
  }

  // sky colour model per row from the accepted cells: haze and cloud bands near the horizon join when they look like the sky above them
  // (brighter than the model is never a reason to refuse; darker, greener or warmer is)
  std::vector<float> rY(gh, -1.f), rS(gh, 0.f), rBR(gh, 0.f);
  int maxRow = 0;
  for (int j = 0; j < gh; ++j) {
    double y = 0, sat = 0, br = 0;
    int n = 0;
    for (int i = 0; i < gw; ++i) {
      const std::size_t k = std::size_t(j) * gw + i;
      if (!in[k]) continue;
      const float mx = std::max({R[k], G[k], B[k]}), mn = std::min({R[k], G[k], B[k]});
      y += Y[k], sat += mx > 1e-4f ? (mx - mn) / mx : 0.f, br += B[k] - R[k], ++n;
    }
    if (n >= 2) rY[j] = float(y / n), rS[j] = float(sat / n), rBR[j] = float(br / n), maxRow = j;
  }
  for (int j = 0, last = -1; j < gh; ++j) {  // rows without cells: nearest filled row above
    if (rY[j] >= 0) last = j;
    else if (last >= 0) rY[j] = rY[last], rS[j] = rS[last], rBR[j] = rBR[last];
  }
  for (int j = gh - 1, nxt = -1; j >= 0; --j) {  // rows above the first filled row: nearest below
    if (rY[j] >= 0) nxt = j;
    else if (nxt >= 0) rY[j] = rY[nxt], rS[j] = rS[nxt], rBR[j] = rBR[nxt];
  }
  if (rY[0] < 0) return Plane{};
  const std::vector<std::uint8_t> in0 = in;  // cells accepted before the colour-model growth
  {
    std::vector<std::uint8_t> ext = in;
    q.clear();
    for (std::size_t k = 0; k < N; ++k)
      if (in[k]) q.push_back(k);
    const int limit = std::min(gh - 1, maxRow + 2);  // sky does not continue below its own lowest rows (water reflections stay out)
    while (!q.empty()) {
      const std::size_t k = q.front();
      q.pop_front();
      const int i = int(k % gw), j = int(k / gw);
      const int nb[4][2] = {{i - 1, j}, {i + 1, j}, {i, j - 1}, {i, j + 1}};
      for (auto& n : nb) {
        if (n[0] < 0 || n[1] < 0 || n[0] >= gw || n[1] > limit) continue;
        const std::size_t kk = std::size_t(n[1]) * gw + n[0];
        if (ext[kk]) continue;
        const int r = n[1];
        const float mx = std::max({R[kk], G[kk], B[kk]}), mn = std::min({R[kk], G[kk], B[kk]});
        const float sat = mx > 1e-4f ? (mx - mn) / mx : 0.f;
        const bool like = tex[kk] < 0.06f && Y[kk] > 0.74f * rY[r] - 0.02f && sat < rS[r] + 0.07f && (B[kk] - R[kk]) > rBR[r] - 0.07f &&
                          (G[kk] - B[kk]) < 0.02f && far[kk] > 0.3f;
        if (like) ext[kk] = 1, q.push_back(kk);
      }
    }
    in.swap(ext);
  }
  // soft mask on the grid (one gaussian pass)
  std::vector<float> m(N);
  for (std::size_t k = 0; k < N; ++k) m[k] = in[k] ? (in0[k] ? std::max(0.5f, cand[k]) : 0.85f) : 0.f;
  std::vector<float> t(N);
  for (int j = 0; j < gh; ++j)
    for (int i = 0; i < gw; ++i) {
      float a = 0;
      for (int d = -1; d <= 1; ++d) a += m[std::size_t(j) * gw + std::clamp(i + d, 0, gw - 1)] * (d == 0 ? 0.5f : 0.25f);
      t[std::size_t(j) * gw + i] = a;
    }
  for (int j = 0; j < gh; ++j)
    for (int i = 0; i < gw; ++i) {
      float a = 0;
      for (int d = -1; d <= 1; ++d) a += t[std::size_t(std::clamp(j + d, 0, gh - 1)) * gw + i] * (d == 0 ? 0.5f : 0.25f);
      m[std::size_t(j) * gw + i] = a;
    }
  if (confidence) *confidence = conf;
  Plane out = resizePlaneF(m.data(), gw, gh, w, h, 0.f, 1.f);

  // Full-resolution refinement in the edge band only: pixels darker, greener or warmer than the sky model of their row leave the mask (branches,
  // leaves); strongly sky-like pixels inside the band join it (gaps between leaves); then a guided filter on the frame luma restores the edge.
  std::vector<std::uint8_t> band(N, 0), cnt(N, 0);
  {  // 7 x 7 count of accepted cells (separable); the band is where it is neither 0 nor 49, and it fades the joining of new pixels
    std::vector<std::uint8_t> tc(N);
    for (int j = 0; j < gh; ++j)
      for (int i = 0; i < gw; ++i) {
        int c = 0;
        for (int d = -3; d <= 3; ++d) c += in[std::size_t(j) * gw + std::clamp(i + d, 0, gw - 1)];
        tc[std::size_t(j) * gw + i] = std::uint8_t(c);
      }
    for (int j = 0; j < gh; ++j)
      for (int i = 0; i < gw; ++i) {
        int c = 0;
        for (int d = -3; d <= 3; ++d) c += tc[std::size_t(std::clamp(j + d, 0, gh - 1)) * gw + i];
        cnt[std::size_t(j) * gw + i] = std::uint8_t(c);
        band[std::size_t(j) * gw + i] = c != 0 && c != 49;
      }
  }
  int bandMaxJ = -1;
  for (int j = 0; j < gh; ++j)
    for (int i = 0; i < gw; ++i)
      if (band[std::size_t(j) * gw + i]) bandMaxJ = j;
  if (bandMaxJ < 0) return out;
  const int hb = std::min(h, (bandMaxJ + 1) * h / gh + 2);  // rows below the lowest band cell keep the coarse value
  std::vector<float> rowY(h), rowS(h), rowBR(h);
  for (int y = 0; y < h; ++y) {
    const float fy = std::clamp((float(y) + 0.5f) * float(gh) / float(h) - 0.5f, 0.f, float(gh - 1));
    const int j0 = int(fy), j1 = std::min(j0 + 1, gh - 1);
    const float ty = fy - float(j0);
    rowY[y] = rY[j0] + (rY[j1] - rY[j0]) * ty, rowS[y] = rS[j0] + (rS[j1] - rS[j0]) * ty, rowBR[y] = rBR[j0] + (rBR[j1] - rBR[j0]) * ty;
  }
  std::vector<int> gxOf(w);
  for (int x = 0; x < w; ++x) gxOf[x] = std::min(gw - 1, x * gw / w);
  std::vector<float> P(std::size_t(w) * hb), Yf(std::size_t(w) * hb);
  std::vector<std::uint8_t> inBand(std::size_t(w) * hb);
  for (int y = 0; y < hb; ++y) {
    const int gj = std::min(gh - 1, y * gh / h);
    for (int x = 0; x < w; ++x) {
      const std::size_t o = std::size_t(y) * w + x;
      const std::uint8_t* px = rgb + std::size_t(y) * stride + std::size_t(x) * 3;
      const float r = px[0] * (1.f / 255.f), g = px[1] * (1.f / 255.f), b = px[2] * (1.f / 255.f);
      const float yy = 0.2126f * r + 0.7152f * g + 0.0722f * b;
      Yf[o] = yy;
      const float cm = out.px[o] * (1.f / 255.f);
      const std::size_t gk = std::size_t(gj) * gw + gxOf[x];
      inBand[o] = band[gk];
      if (!inBand[o]) {
        P[o] = cm;
        continue;
      }
      const float mx = std::max({r, g, b}), mn = std::min({r, g, b});
      const float sat = mx > 1e-4f ? (mx - mn) / mx : 0.f;
      auto ramp = [](float lo, float inv, float x) { return std::clamp((x - lo) * inv, 0.f, 1.f); };  // linear: cheaper than smoothstep on the hot path
      const float dark = 1.f - ramp(0.f, 1.f / (0.14f * rowY[y] + 0.02f), 0.80f * rowY[y] - yy);  // darker than the sky: not sky
      const float green = 1.f - ramp(0.015f, 1.f / 0.035f, g - b);
      const float warm = 1.f - ramp(0.05f, 1.f / 0.07f, (r - b) + rowBR[y]);  // redder than the sky's own cast
      const float chroma = 1.f - ramp(0.f, 1.f / 0.12f, sat - rowS[y] - 0.08f);
      const float like = dark * green * warm * chroma;
      // inside the coarse mask a pixel leaves when unlike; outside it joins only when strongly like
      P[o] = cm > 0.5f ? std::min(cm, like) : std::max(cm, like > 0.9f ? like * ramp(0.f, 1.f / 0.5f, float(cnt[gk]) * (1.f / 49.f)) : 0.f);
    }
  }
  // fast guided filter: statistics on a 3x sub-sampled grid, box radius 2 (about 6 px at full size), applied with the full-size luma
  constexpr int SS = 3;  // guided-filter sub-sampling
  const int sw = (w + SS - 1) / SS, sh = (hb + SS - 1) / SS;
  const std::size_t SN = std::size_t(sw) * sh;
  std::vector<float> I(SN), Pp(SN), a(SN), bb(SN), tmp(SN);
  for (int y = 0; y < sh; ++y)
    for (int x = 0; x < sw; ++x) {  // box average of the SS x SS block
      float ai = 0, ap = 0;
      int n = 0;
      for (int dy = 0; dy < SS; ++dy)
        for (int dx = 0; dx < SS; ++dx) {
          const int xx = x * SS + dx, yy = y * SS + dy;
          if (xx >= w || yy >= hb) continue;
          ai += Yf[std::size_t(yy) * w + xx], ap += P[std::size_t(yy) * w + xx], ++n;
        }
      I[std::size_t(y) * sw + x] = ai / float(n), Pp[std::size_t(y) * sw + x] = ap / float(n);
    }
  auto box = [&](std::vector<float>& v) {  // separable running-sum box filter, clamped edges
    const int rr = 2;
    for (int y = 0; y < sh; ++y) {
      float acc = 0;
      for (int d = -rr; d <= rr; ++d) acc += v[std::size_t(y) * sw + std::clamp(d, 0, sw - 1)];
      for (int x = 0; x < sw; ++x) {
        tmp[std::size_t(y) * sw + x] = acc * (1.f / (2 * rr + 1));
        acc += v[std::size_t(y) * sw + std::clamp(x + rr + 1, 0, sw - 1)] - v[std::size_t(y) * sw + std::clamp(x - rr, 0, sw - 1)];
      }
    }
    for (int x = 0; x < sw; ++x) {
      float acc = 0;
      for (int d = -rr; d <= rr; ++d) acc += tmp[std::size_t(std::clamp(d, 0, sh - 1)) * sw + x];
      for (int y = 0; y < sh; ++y) {
        v[std::size_t(y) * sw + x] = acc * (1.f / (2 * rr + 1));
        acc += tmp[std::size_t(std::clamp(y + rr + 1, 0, sh - 1)) * sw + x] - tmp[std::size_t(std::clamp(y - rr, 0, sh - 1)) * sw + x];
      }
    }
  };
  std::vector<float> mI = I, mP = Pp, IP(SN), II(SN);
  for (std::size_t k = 0; k < SN; ++k) IP[k] = I[k] * Pp[k], II[k] = I[k] * I[k];
  box(mI), box(mP), box(IP), box(II);
  for (std::size_t k = 0; k < SN; ++k) {
    const float var = II[k] - mI[k] * mI[k], cov = IP[k] - mI[k] * mP[k];
    a[k] = cov / (var + 5e-4f), bb[k] = mP[k] - a[k] * mI[k];
  }
  box(a), box(bb);
  for (int y = 0; y < hb; ++y) {
    const float fy = std::clamp((float(y) + 0.5f) / float(SS) - 0.5f, 0.f, float(sh - 1));
    const int y0 = int(fy), y1 = std::min(y0 + 1, sh - 1);
    const float ty = fy - float(y0);
    for (int x = 0; x < w; ++x) {
      const std::size_t o = std::size_t(y) * w + x;
      if (!inBand[o]) continue;  // outside the band the coarse value stands
      const float fx = std::clamp((float(x) + 0.5f) / float(SS) - 0.5f, 0.f, float(sw - 1));
      const int x0 = int(fx), x1 = std::min(x0 + 1, sw - 1);
      const float tx = fx - float(x0);
      auto lerp = [&](const std::vector<float>& v) {
        const float p0 = v[std::size_t(y0) * sw + x0] + (v[std::size_t(y0) * sw + x1] - v[std::size_t(y0) * sw + x0]) * tx;
        const float p1 = v[std::size_t(y1) * sw + x0] + (v[std::size_t(y1) * sw + x1] - v[std::size_t(y1) * sw + x0]) * tx;
        return p0 + (p1 - p0) * ty;
      };
      out.px[o] = std::uint8_t(std::clamp(lerp(a) * Yf[o] + lerp(bb), 0.f, 1.f) * 255.f + 0.5f);
    }
  }
  return out;
}

}  // namespace dapple::aimask
