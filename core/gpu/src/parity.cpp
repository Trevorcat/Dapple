#include "parity.h"

#include <algorithm>
#include <cmath>
#include <thread>

#include "colour_metrics.h"

namespace dapple::gpu {

namespace {
constexpr int kBins = 20000;  // 0.001 dE
}

DeStats compareRgba8(const std::uint8_t* a, const std::uint8_t* b, std::size_t npx) {
  const unsigned nt = npx < 200000 ? 1u : std::max(1u, std::thread::hardware_concurrency());
  std::vector<std::vector<std::uint64_t>> hist(nt, std::vector<std::uint64_t>(kBins + 1, 0));
  std::vector<double> sum(nt, 0), mx(nt, 0);
  std::vector<std::size_t> mxAt(nt, 0);
  std::vector<std::uint64_t> diff(nt, 0);
  std::vector<int> mc(nt, 0);
  auto work = [&](unsigned t, std::size_t b0, std::size_t e0) {
    for (std::size_t i = b0; i < e0; ++i) {
      const std::uint8_t* p = a + 4 * i;
      const std::uint8_t* q = b + 4 * i;
      const int d = std::max({std::abs(p[0] - q[0]), std::abs(p[1] - q[1]), std::abs(p[2] - q[2])});
      double de = 0;
      if (d) {
        const float pa[3] = {p[0] / 255.f, p[1] / 255.f, p[2] / 255.f}, qa[3] = {q[0] / 255.f, q[1] / 255.f, q[2] / 255.f};
        de = metrics::deltaE2000(metrics::srgbToLab(pa), metrics::srgbToLab(qa));
        ++diff[t];
        mc[t] = std::max(mc[t], d);
      }
      sum[t] += de;
      if (de > mx[t]) {
        mx[t] = de;
        mxAt[t] = i;
      }
      hist[t][std::min<int>(kBins, int(de * 1000.0))]++;
    }
  };
  const std::size_t per = (npx + nt - 1) / nt;
  std::vector<std::thread> th;
  for (unsigned t = 1; t < nt; ++t) th.emplace_back(work, t, t * per, std::min(npx, (t + 1) * per));
  work(0, 0, std::min(npx, per));
  for (auto& x : th) x.join();
  DeStats s;
  std::vector<std::uint64_t> h(kBins + 1, 0);
  std::uint64_t dsum = 0;
  for (unsigned t = 0; t < nt; ++t) {
    s.mean += sum[t];
    if (mx[t] > s.max) {
      s.max = mx[t];
      s.maxAt = mxAt[t];
    }
    dsum += diff[t];
    s.maxChan = std::max(s.maxChan, mc[t]);
    for (int i = 0; i <= kBins; ++i) h[i] += hist[t][i];
  }
  if (!npx) return s;
  s.mean /= double(npx);
  s.differing = double(dsum) / double(npx);
  const std::uint64_t target = std::uint64_t(std::ceil(0.99 * double(npx)));
  std::uint64_t acc = 0;
  for (int i = 0; i <= kBins; ++i) {
    acc += h[i];
    if (acc >= target) {
      s.p99 = (i + 1) * 0.001;
      break;
    }
  }
  return s;
}

std::vector<std::uint8_t> toRgba8(const ImageF& f) {
  std::vector<std::uint8_t> o(std::size_t(f.width) * f.height * 4);
  const std::size_t n = std::size_t(f.width) * f.height;
  for (std::size_t i = 0; i < n; ++i) {
    for (int c = 0; c < 3; ++c) o[4 * i + c] = std::uint8_t(std::clamp(f.data[3 * i + c], 0.f, 1.f) * 255.f + 0.5f);
    o[4 * i + 3] = 255;
  }
  return o;
}

namespace {
inline std::uint32_t hash(std::uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}
inline float noise(int x, int y, std::uint32_t seed) { return float(hash(std::uint32_t(x) * 73856093u ^ std::uint32_t(y) * 19349663u ^ seed) & 0xFFFF) / 65535.f - 0.5f; }
}  // namespace

SourceImage makeTestImage(int w, int h) {
  SourceImage s;
  s.sceneReferred = true;
  s.linear = ImageF(w, h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const float u = float(x) / float(std::max(1, w - 1)), v = float(y) / float(std::max(1, h - 1));
      // smooth background: sky-like gradient with a warm-to-cool shift
      float r = 0.05f + 0.55f * u * u, g = 0.06f + 0.5f * (1.f - v) * u, b = 0.1f + 0.7f * (1.f - u) * (0.4f + 0.6f * v);
      // colour patches along the bottom
      if (v > 0.78f) {
        const int k = std::min(7, int(u * 8.f));
        static const float pal[8][3] = {{0.8f, 0.1f, 0.1f}, {0.7f, 0.5f, 0.05f}, {0.1f, 0.6f, 0.1f}, {0.05f, 0.5f, 0.6f},
                                        {0.1f, 0.1f, 0.8f}, {0.5f, 0.1f, 0.5f}, {0.3f, 0.3f, 0.3f}, {0.9f, 0.9f, 0.9f}};
        r = pal[k][0];
        g = pal[k][1];
        b = pal[k][2];
      }
      // an edge-rich block (checker + diagonal line) for the sharpening / clarity / noise reduction stages
      if (u > 0.55f && u < 0.9f && v > 0.15f && v < 0.55f) {
        const bool c = ((x / 6) + (y / 6)) & 1;
        r = g = b = c ? 0.55f : 0.12f;
        if (std::abs((x - y) % 23) < 2) r = 0.9f;
      }
      // a dim, noisy, hazy region
      if (u < 0.35f && v > 0.35f && v < 0.7f) {
        const float t = 0.25f + 0.1f * std::sin(u * 40.f);
        r = g = b = t;
      }
      // sensor-like noise (stronger in the dark)
      const float n0 = noise(x, y, 11u), n1 = noise(x, y, 23u), n2 = noise(x, y, 37u);
      const float amp = 0.012f + 0.03f * (1.f - std::min(1.f, (r + g + b) / 1.5f));
      r = std::max(0.f, r + amp * n0 * 2.f);
      g = std::max(0.f, g + amp * (0.7f * n0 + 0.7f * n1));
      b = std::max(0.f, b + amp * n2 * 2.f);
      // a few bright, strongly saturated spots: tone-mapped colour beyond the first lattice
      const float dx = u - 0.2f, dy = v - 0.2f;
      if (dx * dx + dy * dy < 0.0025f) {
        r = 3.0f;
        g = 0.4f;
        b = 0.05f;
      }
      const float ex = u - 0.45f, ey = v - 0.12f;
      if (ex * ex + ey * ey < 0.0016f) {
        r = 0.1f;
        g = 2.5f;
        b = 0.3f;
      }
      float* o = s.linear.row(y) + 3 * x;
      o[0] = r;
      o[1] = g;
      o[2] = b;
    }
  return s;
}

}  // namespace dapple::gpu
