#include "hdr_field.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

#include "dapple/decoder.h"
#include "parallel.h"

namespace dapple {
namespace hdr {

namespace {

constexpr int kGridLong = 64;  // cells across the long edge
constexpr int kBins = 36;
constexpr float kSigmaSpace = 3.5f, kSigmaRange = 1.5f;  // cells / bins (0.75 EV)

inline float smooth(float a, float b, float x) {
  const float t = std::clamp((x - a) / (b - a), 0.f, 1.f);
  return t * t * (3.f - 2.f * t);
}

std::vector<float> gauss(float sigma) {
  const int r = std::max(1, int(std::ceil(3.f * sigma)));
  std::vector<float> k(std::size_t(2 * r + 1));
  float s = 0.f;
  for (int i = -r; i <= r; ++i) s += k[std::size_t(i + r)] = std::exp(-0.5f * float(i * i) / (sigma * sigma));
  for (float& v : k) v /= s;
  return k;
}

// zero-padded separable blur of a (gw x gh x nb x 2) volume along one axis (0 x, 1 y, 2 z)
void blurAxis(const std::vector<float>& in, std::vector<float>& out, int gw, int gh, int nb, int axis, const std::vector<float>& k) {
  const int r = int(k.size() / 2);
  const int n[3] = {gw, gh, nb};
  const std::size_t stride[3] = {2, std::size_t(gw) * 2, std::size_t(gw) * gh * 2};
  const int outer = (axis == 0 ? gh * nb : axis == 1 ? gw * nb : gw * gh);
  parallelFor(
      outer,
      [&](int o) {
        std::size_t base;
        if (axis == 0) base = std::size_t(o) * stride[1];                                       // o = z * gh + y
        else if (axis == 1) base = std::size_t(o / gw) * stride[2] + std::size_t(o % gw) * stride[0];  // o = z * gw + x
        else base = std::size_t(o / gw) * stride[1] + std::size_t(o % gw) * stride[0];          // o = y * gw + x
        for (int i = 0; i < n[axis]; ++i) {
          float a = 0.f, b = 0.f;
          for (int t = std::max(0, i - r); t <= std::min(n[axis] - 1, i + r); ++t) {
            const float w = k[std::size_t(t - i + r)];
            const float* p = &in[base + std::size_t(t) * stride[axis]];
            a += w * p[0];
            b += w * p[1];
          }
          out[base + std::size_t(i) * stride[axis]] = a;
          out[base + std::size_t(i) * stride[axis] + 1] = b;
        }
      },
      4);
}

std::vector<float> blur3(const std::vector<float>& v, int gw, int gh, int nb, float sxy, float sz) {
  std::vector<float> a(v.size()), b(v.size());
  const std::vector<float> kxy = gauss(sxy), kz = gauss(sz);
  blurAxis(v, a, gw, gh, nb, 0, kxy);
  blurAxis(a, b, gw, gh, nb, 1, kxy);
  blurAxis(b, a, gw, gh, nb, 2, kz);
  return a;
}

std::uint64_t fnv(const void* p, std::size_t n, std::uint64_t h = 1469598103934665603ull) {
  const auto* b = static_cast<const std::uint8_t*>(p);
  for (std::size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
  return h;
}

}  // namespace

float sliceBase(const HdrField& f, float u, float v, float lc0) {
  const float gx = std::clamp(u, 0.f, 1.f) * float(f.gw - 1), gy = std::clamp(v, 0.f, 1.f) * float(f.gh - 1);
  const float zmax = float(f.nb - 1);
  const float gz = std::clamp((lc0 - f.lmin) / f.lstep, 0.f, zmax);
  const int x0 = std::min(int(gx), f.gw - 2), y0 = std::min(int(gy), f.gh - 2), z0 = std::min(int(gz), f.nb - 2);
  const float fx = gx - float(x0), fy = gy - float(y0), fz = gz - float(z0);
  float num = 0.f, den = 0.f;
  for (int dz = 0; dz < 2; ++dz)
    for (int dy = 0; dy < 2; ++dy)
      for (int dx = 0; dx < 2; ++dx) {
        const float w = (dx ? fx : 1.f - fx) * (dy ? fy : 1.f - fy) * (dz ? fz : 1.f - fz);
        const float* c = &f.g[((std::size_t(z0 + dz) * f.gh + std::size_t(y0 + dy)) * f.gw + std::size_t(x0 + dx)) * 2];
        num += w * c[0];
        den += w * c[1];
      }
  const float lcc = std::clamp(lc0, f.lmin, f.lmin + zmax * f.lstep);
  return (num + kPrior * lcc) / (den + kPrior);
}

namespace {

}  // namespace

std::shared_ptr<HdrField> buildField(const std::vector<float>& lum, int pw, int ph) {
  auto f = std::make_shared<HdrField>();
  if (pw < 1 || ph < 1 || lum.size() < std::size_t(pw) * ph) return f;
  const bool wide = pw >= ph;
  f->gw = wide ? kGridLong : std::max(8, int(std::lround(double(kGridLong - 1) * pw / ph)) + 1);
  f->gh = wide ? std::max(8, int(std::lround(double(kGridLong - 1) * ph / pw)) + 1) : kGridLong;
  f->nb = kBins;
  const int gw = f->gw, gh = f->gh, nb = f->nb;
  std::vector<float> vol(std::size_t(gw) * gh * nb * 2, 0.f);
  double sumL = 0.0;
  for (int j = 0; j < ph; ++j) {
    const float gy = (float(j) + 0.5f) / float(ph) * float(gh - 1);
    const int y0 = std::min(int(gy), gh - 2);
    const float fy = gy - float(y0);
    for (int i = 0; i < pw; ++i) {
      const float gx = (float(i) + 0.5f) / float(pw) * float(gw - 1);
      const int x0 = std::min(int(gx), gw - 2);
      const float fx = gx - float(x0);
      const float L = std::log2(std::max(lum[std::size_t(j) * pw + i], kFloor));
      sumL += L;
      const float gz = std::clamp((L - f->lmin) / f->lstep, 0.f, float(nb - 1) - 1e-4f);
      const int z0 = int(gz);
      const float fz = gz - float(z0);
      for (int dz = 0; dz < 2; ++dz)
        for (int dy = 0; dy < 2; ++dy)
          for (int dx = 0; dx < 2; ++dx) {
            const float w = (dx ? fx : 1.f - fx) * (dy ? fy : 1.f - fy) * (dz ? fz : 1.f - fz);
            float* c = &vol[((std::size_t(z0 + dz) * gh + (y0 + dy)) * gw + (x0 + dx)) * 2];
            c[0] += w * L;
            c[1] += w;
          }
    }
  }
  f->key0 = float(sumL / (double(pw) * ph));
  std::vector<float> g = blur3(vol, gw, gh, nb, kSigmaSpace, kSigmaRange);
  float dmax = 1e-20f;
  for (std::size_t i = 1; i < g.size(); i += 2) dmax = std::max(dmax, g[i]);
  const float inv = 1.f / dmax;
  for (float& v : g) v *= inv;
  f->g = std::move(g);
  {  // p1 / p99 of the base over the proxy (exposure-free EV): the range the compression is measured against
    std::vector<int> hist(512, 0);
    for (int j = 0; j < ph; ++j)
      for (int i = 0; i < pw; ++i) {
        const float b = sliceBase(*f, (float(i) + 0.5f) / float(pw), (float(j) + 0.5f) / float(ph), std::log2(std::max(lum[std::size_t(j) * pw + i], kFloor)));
        ++hist[std::size_t(std::clamp(int((b - f->lmin) * 20.f), 0, 511))];
      }
    const double total = double(pw) * ph;
    double acc = 0;
    bool gotLo = false;
    f->lo0 = f->lmin, f->hi0 = f->lmin + 25.f;
    for (int k = 0; k < 512; ++k) {
      acc += hist[std::size_t(k)];
      if (!gotLo && acc >= 0.01 * total) f->lo0 = f->lmin + (float(k) + 0.5f) / 20.f, gotLo = true;
      if (acc >= 0.99 * total) {
        f->hi0 = f->lmin + (float(k) + 0.5f) / 20.f;
        break;
      }
    }
  }
  f->id = fnv(f->g.data(), f->g.size() * sizeof(float));
  return f;
}

std::shared_ptr<const HdrField> fieldFor(const SourceImage& src, const float wb[3]) {
  const ImageF& im = src.linear;
  if (im.empty()) return nullptr;
  struct Entry {
    const float* data = nullptr;
    int w = 0, h = 0;
    std::uint64_t fp = 0;
    std::shared_ptr<const HdrField> f;
  };
  static std::mutex mu;
  static std::vector<Entry> cache;
  // identity of the source pixels: address, size and a sample of 97 pixels (an address alone can be reused by another image)
  std::uint64_t fp = fnv(wb, 3 * sizeof(float));
  const std::size_t total = im.data.size();
  for (std::size_t i = 0; i < 97; ++i) fp = fnv(&im.data[(total / 97) * i], sizeof(float), fp);
  {
    std::lock_guard<std::mutex> lk(mu);
    for (std::size_t i = 0; i < cache.size(); ++i)
      if (cache[i].data == im.data.data() && cache[i].w == im.width && cache[i].h == im.height && cache[i].fp == fp) {
        Entry e = cache[i];
        cache.erase(cache.begin() + long(i));
        cache.push_back(e);  // most recent last
        return e.f;
      }
  }
  const ImageF small = resizeToLongEdge(im, kProxyEdge);
  std::vector<float> lum(std::size_t(small.width) * small.height);
  for (std::size_t i = 0; i < lum.size(); ++i) {
    const float* p = &small.data[3 * i];
    lum[i] = kLumR * p[0] * wb[0] + kLumG * p[1] * wb[1] + kLumB * p[2] * wb[2];
  }
  std::shared_ptr<const HdrField> f = buildField(lum, small.width, small.height);
  std::lock_guard<std::mutex> lk(mu);
  cache.push_back({im.data.data(), im.width, im.height, fp, f});
  if (cache.size() > 3) cache.erase(cache.begin());
  return f;
}

HdrParams resolve(const HdrField& f, float strength01, float expoLog2, float iso, const float gain[3], float targetRange) {
  HdrParams p;
  p.s = strength01;
  p.ev = expoLog2;
  p.yn = 0.002f * std::sqrt(std::max(iso > 0.f ? iso : 400.f, 100.f) / 100.f) * std::exp2(expoLog2);
  p.yclip = std::max(1e-4f, kLumR * gain[0] + kLumG * gain[1] + kLumB * gain[2]);
  p.lo = f.lo0 + expoLog2;
  p.hi = f.hi0 + expoLog2;
  const float R = p.hi - p.lo;
  if (R < 1.f) return p;
  const float mid = std::log2(0.18f);
  p.key = std::clamp(std::clamp(f.key0 + expoLog2, mid - 3.f, mid + 1.f), p.lo + 0.2f * R, p.hi - 0.2f * R);
  const float E0 = std::max(0.f, R - targetRange);
  const float E = E0 * E0 / (E0 + 2.f);  // soft onset: a small excess is compressed far less than its size (a 1.4 EV excess acts as 0.6 EV)
  p.ls = strength01 * std::min(kLiftCap * (p.key - p.lo), kShare * E);
  p.hp = strength01 * std::min(kPullCap * (p.hi - p.key), kShare * E);
  return p;
}

float baseGain(const HdrParams& p, float b, float* slope) {
  float gl = 0.f, sl = 1.f;
  if (b < p.key) {
    const float r = p.key - p.lo, t = std::clamp((b - p.lo) / r, 0.f, 1.f), q = 1.f - t;
    gl = p.ls * 6.75f * t * q * q;
    if (b > p.lo) sl = 1.f + p.ls * 6.75f * (q * q - 2.f * t * q) / r;
  } else {
    const float r = p.hi - p.key, u = std::clamp((b - p.key) / r, 0.f, 1.f);
    gl = -p.hp * u * u;
    if (b < p.hi) sl = 1.f - p.hp * 2.f * u / r;
  }
  if (slope) *slope = sl;
  return gl;
}

float gainEv(const HdrParams& p, float L, float b) {
  float sl;
  float gl = baseGain(p, b, &sl);
  const float lyn = std::log2(p.yn);
  if (gl > 0.f) gl *= smooth(lyn, lyn + 3.f, b);
  else if (gl < 0.f) {
    const float lcl = std::log2(p.yclip);
    gl *= 1.f - smooth(lcl - 0.4f, lcl - 0.03f, L);
  }
  const float kc = std::min(0.5f * (1.f / std::max(sl, 0.4f) - 1.f), 0.35f);  // half of what restores the slope, capped
  const float det = std::clamp(L - b, -1.5f, 1.5f) * kc * smooth(lyn + 3.f, lyn + 7.f, b);
  return gl + det;
}

void apply(const HdrField& f, const HdrParams& p, float u, float v, float rgb[3]) {
  const float Y = kLumR * rgb[0] + kLumG * rgb[1] + kLumB * rgb[2];
  const float lc0 = std::log2(std::max(Y * std::exp2(-p.ev), kFloor));
  const float b = sliceBase(f, u, v, lc0) + p.ev;
  const float L = lc0 + p.ev;
  const float g = std::exp2(gainEv(p, L, b));
  if (Y <= 1e-9f) {
    for (int c = 0; c < 3; ++c) rgb[c] *= g;
    return;
  }
  // chroma in a cube-root space: a darkening keeps kKappa of the colourfulness it would lose, a lift is pulled toward grey near the noise floor
  float sc = 1.f;
  if (g < 1.f) sc = std::pow(g, (1.f - kKappa) / 3.f);
  else if (g > 1.f) sc = 1.f + (smooth(0.5f * p.yn, 12.f * p.yn, Y) - 1.f) * std::min((g - 1.f) * 2.f, 1.f);
  const float Yg = g * Y;
  const float m = std::cbrt(Yg), m0 = std::cbrt(Y);
  float out[3], lum = 0.f;
  for (int c = 0; c < 3; ++c) {
    const float w = m + (std::cbrt(std::max(rgb[c], 0.f)) - m0) * sc * std::cbrt(g);
    out[c] = std::max(w, 0.f);
    out[c] = out[c] * out[c] * out[c];
  }
  lum = kLumR * out[0] + kLumG * out[1] + kLumB * out[2];
  const float n = lum > 1e-12f ? Yg / lum : 0.f;
  for (int c = 0; c < 3; ++c) rgb[c] = out[c] * n;
}

}  // namespace hdr
}  // namespace dapple
