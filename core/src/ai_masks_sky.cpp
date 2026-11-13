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
  const float frac = float(area) / float(N);
  if (area == 0 || frac < 0.04f || float(blueCells) < 0.25f * float(area) || cy / double(area) > 0.6 * gh) return Plane{};
  // soft mask on the grid (one gaussian pass), then up to the frame
  std::vector<float> m(N);
  for (std::size_t k = 0; k < N; ++k) m[k] = in[k] ? std::max(0.5f, cand[k]) : 0.f;
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
  if (confidence) *confidence = frac * float(blueCells) / float(area);
  return resizePlaneF(m.data(), gw, gh, w, h, 0.f, 1.f);
}

}  // namespace dapple::aimask
