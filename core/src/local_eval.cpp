// Evaluation of local adjustments (docs/engine/pipeline-architecture.md section 40): resolution of the list for a frame, the
// low-resolution photo-space planes (brush coverage, fast-guided-filter coefficients of raster masks) and their cache, the raster
// store, and the CPU kernels of LocalLinear / LocalDetail. GPU twins: core/engine/shaders/local.glsl, local_detail.comp.
#include "local_eval.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <list>
#include <mutex>
#include <unordered_map>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

#include "frame_kernels.h"
#include "parallel.h"

namespace dapple::local {

namespace {

constexpr float kLumaPP[3] = {0.2880402f, 0.7118741f, 0.0000857f};   // ProPhoto Y (renderer / point.comp)
constexpr float kLumaS[3] = {0.2126729f, 0.7151522f, 0.0721750f};    // sRGB luma (spatial.cpp)

inline float sstep(float e0, float e1, float x) {
  const float t = std::clamp((x - e0) / (e1 - e0), 0.f, 1.f);
  return t * t * (3.f - 2.f * t);
}

std::uint64_t mix64(std::uint64_t h, std::uint64_t x) {
  h ^= x + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  h *= 0x100000001B3ull;
  return h ^ (h >> 29);
}
std::uint64_t mixf(std::uint64_t h, float v) {
  if (v == 0.f) v = 0.f;
  std::uint32_t b;
  std::memcpy(&b, &v, 4);
  return mix64(h, b);
}

// ---- counters + plane cache ----------------------------------------------------------------------------------------------------
std::atomic<std::uint64_t> g_brushBuilds{0}, g_brushPrefix{0}, g_rasterBuilds{0}, g_guideBuilds{0}, g_planeHits{0}, g_binds{0};

class PlaneCache {
 public:
  std::shared_ptr<const Plane> find(std::uint64_t key) {
    std::lock_guard<std::mutex> lk(m_);
    auto it = map_.find(key);
    if (it == map_.end()) return nullptr;
    lru_.splice(lru_.begin(), lru_, it->second.pos);
    return it->second.plane;
  }
  void put(std::uint64_t key, std::shared_ptr<const Plane> p) {
    std::lock_guard<std::mutex> lk(m_);
    if (map_.count(key)) return;
    lru_.push_front(key);
    bytes_ += p->bytes();
    map_[key] = Entry{std::move(p), lru_.begin()};
    while (bytes_ > kBudget && lru_.size() > 1) {
      const std::uint64_t k = lru_.back();
      lru_.pop_back();
      auto it = map_.find(k);
      bytes_ -= it->second.plane->bytes();
      map_.erase(it);
    }
  }
  void clear() {
    std::lock_guard<std::mutex> lk(m_);
    map_.clear();
    lru_.clear();
    bytes_ = 0;
  }

 private:
  static constexpr std::size_t kBudget = 96u << 20;
  struct Entry {
    std::shared_ptr<const Plane> plane;
    std::list<std::uint64_t>::iterator pos;
  };
  std::mutex m_;
  std::unordered_map<std::uint64_t, Entry> map_;
  std::list<std::uint64_t> lru_;
  std::size_t bytes_ = 0;
};
PlaneCache& cache() {
  static PlaneCache c;
  return c;
}

// ---- raster store ----------------------------------------------------------------------------------------------------------------
struct RasterStore {
  std::mutex m;
  std::unordered_map<std::string, std::shared_ptr<const Raster>> map;
  std::list<std::string> order;  // oldest first (cap 64 planes / 96 MB)
  std::size_t bytes = 0;
};
RasterStore& rasters() {
  static RasterStore s;
  return s;
}

void logOncePerKey(const std::string& s) {
  static std::mutex m;
  static std::vector<std::string> seen;
  std::lock_guard<std::mutex> lk(m);
  if (std::find(seen.begin(), seen.end(), s) != seen.end()) return;
  if (seen.size() < 64) seen.push_back(s);
  logLine(s);
}

// ---- separable box filters ---------------------------------------------------------------------------------------------------------
// Mean over the window clipped to the plane (count-normalised edges), double running sums.
void boxMeanN(std::vector<float>& d, int w, int h, int r) {
  std::vector<float> t(d.size());
  parallelFor(h, [&](int y) {
    const float* s = d.data() + std::size_t(y) * w;
    float* o = t.data() + std::size_t(y) * w;
    double sum = 0;
    int a = 0, b = -1;  // window [a, b]
    for (int x = 0; x < w; ++x) {
      const int na = std::max(0, x - r), nb = std::min(w - 1, x + r);
      while (b < nb) sum += s[++b];
      while (a < na) sum -= s[a++];
      o[x] = float(sum / double(b - a + 1));
    }
  });
  parallelFor(w, [&](int x) {
    double sum = 0;
    int a = 0, b = -1;
    for (int y = 0; y < h; ++y) {
      const int na = std::max(0, y - r), nb = std::min(h - 1, y + r);
      while (b < nb) sum += t[std::size_t(++b) * w + x];
      while (a < na) sum -= t[std::size_t(a++) * w + x];
      d[std::size_t(y) * w + x] = float(sum / double(b - a + 1));
    }
  });
}

// Box (radius r, replicated edges, direct sums in the GPU's order of magnitude: double accumulation) of `ch` interleaved channels.
// Mirrors sepconv.comp OP 0 (replicated edges, divide by 2r + 1).
void boxRep(std::vector<float>& d, int w, int h, int ch, int r) {
  if (r <= 0) return;
  std::vector<float> t(d.size());
  const double inv = 1.0 / double(2 * r + 1);
  parallelFor(h, [&](int y) {
    const float* s = d.data() + std::size_t(y) * w * ch;
    float* o = t.data() + std::size_t(y) * w * ch;
    for (int c = 0; c < ch; ++c) {
      double sum = 0;
      for (int i = -r; i <= r; ++i) sum += s[std::size_t(std::clamp(i, 0, w - 1)) * ch + c];
      for (int x = 0; x < w; ++x) {
        o[std::size_t(x) * ch + c] = float(sum * inv);
        sum += double(s[std::size_t(std::min(x + r + 1, w - 1)) * ch + c]) - double(s[std::size_t(std::max(x - r, 0)) * ch + c]);
      }
    }
  });
  parallelFor(w, [&](int x) {
    for (int c = 0; c < ch; ++c) {
      double sum = 0;
      for (int i = -r; i <= r; ++i) sum += t[(std::size_t(std::clamp(i, 0, h - 1)) * w + x) * ch + c];
      for (int y = 0; y < h; ++y) {
        d[(std::size_t(y) * w + x) * ch + c] = float(sum * inv);
        sum += double(t[(std::size_t(std::min(y + r + 1, h - 1)) * w + x) * ch + c]) - double(t[(std::size_t(std::max(y - r, 0)) * w + x) * ch + c]);
      }
    }
  });
}

// ---- guide of the raster refinement ------------------------------------------------------------------------------------------------
struct GuideKey {
  std::uint64_t key = 0;
  float ref = 1.f;  // 99th percentile luminance of the sample
};
thread_local const GuideCapture* tlGuide = nullptr;  // streaming export (section 42): the captured inputs replace the pixels of the source
const GuideCapture* guideCaptureFor(const ImageF& im) { return tlGuide && tlGuide->W == im.width && tlGuide->H == im.height ? tlGuide : nullptr; }

GuideKey guideKeyOf(const SourceImage& src) {
  const ImageF& im = src.linear;
  const GuideCapture* cap = guideCaptureFor(im);
  GuideKey g;
  std::uint64_t h = mix64(0x51DE, std::uint64_t(im.width) << 32 | std::uint32_t(im.height));
  std::vector<float> Y;
  Y.reserve(64 * 64);
  for (int j = 0; j < 64; ++j)
    for (int i = 0; i < 64; ++i) {
      int x, y;
      guideSamplePos(im.width, im.height, i, j, x, y);
      const float* p = cap ? cap->samples.data() + 3 * std::size_t(j * 64 + i) : im.row(y) + 3 * std::size_t(x);
      h = mixf(mixf(mixf(h, p[0]), p[1]), p[2]);
      Y.push_back(p[0] * kLumaPP[0] + p[1] * kLumaPP[1] + p[2] * kLumaPP[2]);
    }
  const std::size_t k = Y.size() * 99 / 100;
  std::nth_element(Y.begin(), Y.begin() + long(k), Y.end());
  g.ref = std::max(1e-4f, Y[k]);
  g.key = h;
  return g;
}

inline float guideCh(float v, float ref) { return std::sqrt(std::clamp(v / ref, 0.f, 4.f)); }

// The photo area-averaged to gw x gh (pre-gain source), encoded as the guide (3 channels).
std::shared_ptr<const Plane> guideGrid(const SourceImage& src, const GuideKey& gk, int gw, int gh) {
  const std::uint64_t key = mix64(mix64(gk.key, 0x6A1DE), std::uint64_t(gw) << 32 | std::uint32_t(gh));
  if (auto p = cache().find(key)) return p;
  ++g_guideBuilds;
  auto pl = std::make_shared<Plane>();
  pl->w = gw, pl->h = gh, pl->ch = 3, pl->key = key;
  pl->data.assign(std::size_t(gw) * gh * 3, 0.f);
  const ImageF& im = src.linear;
  const int W = im.width, H = im.height;
  if (const GuideCapture* cap = guideCaptureFor(im)) {  // streaming export: the means captured band by band (same sums, same order)
    for (const GuideCapture::Grid& gr : cap->grids)
      if (gr.gw == gw && gr.gh == gh && gr.mean.size() == std::size_t(gw) * gh * 3) {
        for (std::size_t i = 0; i < std::size_t(gw) * gh * 3; ++i) pl->data[i] = guideCh(gr.mean[i], gk.ref);
        std::shared_ptr<const Plane> out = pl;
        cache().put(key, out);
        return out;
      }
  }
  parallelFor(gh, [&](int j) {
    const int y0 = int(std::floor(double(j) * H / gh)), y1 = std::max(y0 + 1, int(std::floor(double(j + 1) * H / gh)));
    for (int i = 0; i < gw; ++i) {
      const int x0 = int(std::floor(double(i) * W / gw)), x1 = std::max(x0 + 1, int(std::floor(double(i + 1) * W / gw)));
      double acc[3] = {0, 0, 0};
      for (int y = y0; y < std::min(y1, H); ++y) {
        const float* r = im.row(y);
        for (int x = x0; x < std::min(x1, W); ++x)
          for (int c = 0; c < 3; ++c) acc[c] += r[3 * x + c];
      }
      const double n = double((std::min(y1, H) - y0) * (std::min(x1, W) - x0));
      float* o = pl->data.data() + (std::size_t(j) * gw + i) * 3;
      for (int c = 0; c < 3; ++c) o[c] = guideCh(float(acc[c] / n), gk.ref);
    }
  });
  std::shared_ptr<const Plane> out = pl;
  cache().put(key, out);
  return out;
}

// Fast guided filter with a colour guide (He, Sun, Tang 2010/2013; He, Sun 2015): coefficients (a.rgb, b) of q = a . I + b on the grid.
std::shared_ptr<const Plane> rasterPlane(const Raster& ra, const SourceImage& src, const GuideKey& gk, float sx, float sy, bool interactive,
                                         float refine, const MaskComponent& mc) {
  const int cap = interactive ? 512 : 1024;
  const int gl = std::clamp(std::max(ra.w, ra.h), 64, cap);
  const int gw = std::max(8, int(std::lround(gl * sx))), gh = std::max(8, int(std::lround(gl * sy)));
  std::uint64_t key = mix64(mix64(ra.hash, gk.key), std::uint64_t(gw) << 32 | std::uint32_t(gh));
  key = mixf(mix64(key, 0x4A57E4), refine);
  const bool ranged = mc.hasRange();
  if (ranged) key = mixf(mixf(mixf(mix64(key, 0x5A4E6E), mc.rangeLo), mc.rangeHi), mc.rangeFeather);
  if (auto p = cache().find(key)) {
    ++g_planeHits;
    return p;
  }
  ++g_rasterBuilds;
  const std::size_t N = std::size_t(gw) * gh;
  // the mask on the grid (bilinear from the raster, whatever its own aspect)
  std::vector<float> P(N);
  parallelFor(gh, [&](int j) {
    for (int i = 0; i < gw; ++i) {
      const float fx = std::clamp((float(i) + 0.5f) / float(gw) * float(ra.w) - 0.5f, 0.f, float(ra.w - 1));
      const float fy = std::clamp((float(j) + 0.5f) / float(gh) * float(ra.h) - 0.5f, 0.f, float(ra.h - 1));
      const int x0 = std::min(int(fx), ra.w - 1), y0 = std::min(int(fy), ra.h - 1);
      const int x1 = std::min(x0 + 1, ra.w - 1), y1 = std::min(y0 + 1, ra.h - 1);
      const float tx = fx - float(x0), ty = fy - float(y0);
      auto at = [&](int x, int y) { return float(ra.px[std::size_t(y) * ra.w + x]) * (1.f / 255.f); };
      const float a = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * tx, b = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * tx;
      float v = a + (b - a) * ty;
      if (ranged) {  // range of the plane value (depth band ...) before the edge-aware refinement
        const float f = std::max(mc.rangeFeather, 0.01f), p = v * 100.f;
        v = sstep(mc.rangeLo - f, mc.rangeLo, p) * (1.f - sstep(mc.rangeHi, mc.rangeHi + f, p));
      }
      P[std::size_t(j) * gw + i] = v;
    }
  });
  auto pl = std::make_shared<Plane>();
  pl->w = gw, pl->h = gh, pl->ch = 4, pl->key = key;
  pl->data.assign(N * 4, 0.f);
  if (refine <= 0.f) {
    for (std::size_t i = 0; i < N; ++i) pl->data[i * 4 + 3] = P[i];
  } else {
    const std::shared_ptr<const Plane> G = guideGrid(src, gk, gw, gh);
    // box radius 1.2 % .. 4.2 % of the grid's long edge (the research note's 2 - 4 %), eps in guide units (sqrt-encoded, about 0..1.4):
    // tuned on the step-edge test (local_tests raster: a 256 px mask up to 3 low-res px off lands within 0.5 px of the image edge)
    const int r = std::max(1, int(std::lround(float(gl) * (0.012f + 0.03f * refine / 100.f))));
    const float eps = 5e-4f;
    // 13 means: I (3), p, I p (3), I I^T (6)
    std::vector<std::vector<float>> m(13, std::vector<float>(N));
    parallelFor(int(gh), [&](int j) {
      for (int i = 0; i < gw; ++i) {
        const std::size_t k = std::size_t(j) * gw + i;
        const float* I = G->data.data() + k * 3;
        const float p = P[k];
        m[0][k] = I[0], m[1][k] = I[1], m[2][k] = I[2], m[3][k] = p;
        m[4][k] = I[0] * p, m[5][k] = I[1] * p, m[6][k] = I[2] * p;
        m[7][k] = I[0] * I[0], m[8][k] = I[0] * I[1], m[9][k] = I[0] * I[2], m[10][k] = I[1] * I[1], m[11][k] = I[1] * I[2], m[12][k] = I[2] * I[2];
      }
    });
    for (auto& pln : m) boxMeanN(pln, gw, gh, r);
    std::vector<std::vector<float>> ab(4, std::vector<float>(N));
    parallelFor(int(gh), [&](int j) {
      for (int i = 0; i < gw; ++i) {
        const std::size_t k = std::size_t(j) * gw + i;
        const double mI[3] = {m[0][k], m[1][k], m[2][k]}, mp = m[3][k];
        const double cIp[3] = {m[4][k] - mI[0] * mp, m[5][k] - mI[1] * mp, m[6][k] - mI[2] * mp};
        double S[3][3] = {{m[7][k] - mI[0] * mI[0] + eps, m[8][k] - mI[0] * mI[1], m[9][k] - mI[0] * mI[2]},
                          {0, m[10][k] - mI[1] * mI[1] + eps, m[11][k] - mI[1] * mI[2]},
                          {0, 0, m[12][k] - mI[2] * mI[2] + eps}};
        S[1][0] = S[0][1], S[2][0] = S[0][2], S[2][1] = S[1][2];
        // inverse of the symmetric 3x3
        const double c00 = S[1][1] * S[2][2] - S[1][2] * S[2][1], c01 = S[0][2] * S[2][1] - S[0][1] * S[2][2],
                     c02 = S[0][1] * S[1][2] - S[0][2] * S[1][1], c11 = S[0][0] * S[2][2] - S[0][2] * S[2][0],
                     c12 = S[0][2] * S[1][0] - S[0][0] * S[1][2], c22 = S[0][0] * S[1][1] - S[0][1] * S[1][0];
        const double det = S[0][0] * c00 + S[0][1] * (S[1][2] * S[2][0] - S[1][0] * S[2][2]) + S[0][2] * (S[1][0] * S[2][1] - S[1][1] * S[2][0]);
        double a[3] = {0, 0, 0};
        if (std::fabs(det) > 1e-18) {
          const double id = 1.0 / det;
          a[0] = (c00 * cIp[0] + c01 * cIp[1] + c02 * cIp[2]) * id;
          a[1] = (c01 * cIp[0] + c11 * cIp[1] + c12 * cIp[2]) * id;
          a[2] = (c02 * cIp[0] + c12 * cIp[1] + c22 * cIp[2]) * id;
        }
        ab[0][k] = float(a[0]), ab[1][k] = float(a[1]), ab[2][k] = float(a[2]);
        ab[3][k] = float(mp - a[0] * mI[0] - a[1] * mI[1] - a[2] * mI[2]);
      }
    });
    for (auto& pln : ab) boxMeanN(pln, gw, gh, r);
    for (std::size_t i = 0; i < N; ++i)
      for (int c = 0; c < 4; ++c) pl->data[i * 4 + std::size_t(c)] = ab[std::size_t(c)][i];
  }
  std::shared_ptr<const Plane> out = pl;
  cache().put(key, out);
  return out;
}

// ---- brush planes ------------------------------------------------------------------------------------------------------------------
std::uint64_t strokeHash(std::uint64_t h, const BrushStroke& s) {
  h = mixf(mixf(mixf(h, s.size), s.hardness), s.flow);
  h = mix64(h, s.erase ? 0xE5 : 0x95);
  h = mix64(h, s.pts.size());
  for (float p : s.pts) h = mixf(h, p);
  return h;
}

// Draws one stroke into the coverage plane: stroke coverage S = max over its segments of flow * falloff(distance), then
// paint m += S (1 - m) or erase m *= 1 - S. Distances in the photo metric (units of the long edge).
void drawStroke(Plane& pl, const BrushStroke& s, float sx, float sy) {
  const int w = pl.w, h = pl.h;
  const float r = s.size, hard = std::min(s.hardness / 100.f, 0.999f), flow = s.flow / 100.f;
  const float inner = hard * r;
  // plane pixel (i, j) centre -> metric ((i + .5) / w * sx, (j + .5) / h * sy)
  const float pxm = sx / float(w), pym = sy / float(h);
  const std::size_t np = s.pts.size() / 2;
  float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
  for (std::size_t k = 0; k < np; ++k) {
    bx0 = std::min(bx0, s.pts[2 * k] * sx), bx1 = std::max(bx1, s.pts[2 * k] * sx);
    by0 = std::min(by0, s.pts[2 * k + 1] * sy), by1 = std::max(by1, s.pts[2 * k + 1] * sy);
  }
  const int i0 = std::max(0, int(std::floor((bx0 - r) / pxm))), i1 = std::min(w - 1, int(std::ceil((bx1 + r) / pxm)));
  const int j0 = std::max(0, int(std::floor((by0 - r) / pym))), j1 = std::min(h - 1, int(std::ceil((by1 + r) / pym)));
  if (i0 > i1 || j0 > j1) return;
  const int bw = i1 - i0 + 1, bh = j1 - j0 + 1;
  std::vector<float> S(std::size_t(bw) * bh, 0.f);
  for (std::size_t k = 0; k < std::max<std::size_t>(1, np - 1); ++k) {
    const float ax = s.pts[2 * k] * sx, ay = s.pts[2 * k + 1] * sy;
    const std::size_t k1 = np > 1 ? k + 1 : k;
    const float bxp = s.pts[2 * k1] * sx, byp = s.pts[2 * k1 + 1] * sy;
    const float dx = bxp - ax, dy = byp - ay, len2 = dx * dx + dy * dy;
    const int si0 = std::max(i0, int(std::floor((std::min(ax, bxp) - r) / pxm))), si1 = std::min(i1, int(std::ceil((std::max(ax, bxp) + r) / pxm)));
    const int sj0 = std::max(j0, int(std::floor((std::min(ay, byp) - r) / pym))), sj1 = std::min(j1, int(std::ceil((std::max(ay, byp) + r) / pym)));
    for (int j = sj0; j <= sj1; ++j) {
      const float y = (float(j) + 0.5f) * pym;
      float* row = S.data() + std::size_t(j - j0) * bw;
      for (int i = si0; i <= si1; ++i) {
        const float x = (float(i) + 0.5f) * pxm;
        float t = len2 > 0.f ? ((x - ax) * dx + (y - ay) * dy) / len2 : 0.f;
        t = std::clamp(t, 0.f, 1.f);
        const float ex = x - (ax + t * dx), ey = y - (ay + t * dy);
        const float d = std::sqrt(ex * ex + ey * ey);
        if (d >= r) continue;
        const float c = flow * (1.f - sstep(inner, r, d));
        float& o = row[i - i0];
        o = std::max(o, c);
      }
    }
  }
  for (int j = 0; j < bh; ++j) {
    float* m = pl.data.data() + std::size_t(j + j0) * w + i0;
    const float* sr = S.data() + std::size_t(j) * bw;
    for (int i = 0; i < bw; ++i) m[i] = s.erase ? m[i] * (1.f - sr[i]) : m[i] + sr[i] * (1.f - m[i]);
  }
}

std::shared_ptr<const Plane> brushPlane(const MaskComponent& mc, float sx, float sy, bool interactive) {
  float rmin = 0.5f;
  for (const BrushStroke& s : mc.strokes) rmin = std::min(rmin, s.size);
  const int cap = interactive ? 1024 : 2048;
  const int L = std::clamp(int(std::lround(8.f / rmin)), 256, cap);
  const int w = std::max(8, int(std::lround(L * sx))), h = std::max(8, int(std::lround(L * sy)));
  const std::size_t n = mc.strokes.size();
  std::vector<std::uint64_t> keys(n + 1);
  keys[0] = mix64(0xB2B5, std::uint64_t(w) << 32 | std::uint32_t(h));
  for (std::size_t k = 0; k < n; ++k) keys[k + 1] = strokeHash(keys[k], mc.strokes[k]);
  if (auto p = cache().find(keys[n])) {
    ++g_planeHits;
    return p;
  }
  // the plane of a prefix of the stroke list (live painting appends to the end): copy it and draw the rest
  std::shared_ptr<const Plane> base;
  std::size_t from = 0;
  for (std::size_t k = n; k-- > 0 && n - k <= 8;) {
    if (k == 0) break;
    if ((base = cache().find(keys[k]))) {
      from = k;
      break;
    }
  }
  ++g_brushBuilds;
  if (base) ++g_brushPrefix;
  auto pl = std::make_shared<Plane>();
  pl->w = w, pl->h = h, pl->ch = 1;
  if (base) pl->data = base->data;
  else pl->data.assign(std::size_t(w) * h, 0.f);
  for (std::size_t k = from; k < n; ++k) {
    drawStroke(*pl, mc.strokes[k], sx, sy);
    if (k + 2 == n && n >= 2 && from + 1 < n) {  // keep the plane before the last stroke: the next frames of a growing stroke start from it
      auto pre = std::make_shared<Plane>(*pl);
      pre->key = keys[n - 1];
      cache().put(keys[n - 1], pre);
    }
  }
  pl->key = keys[n];
  std::shared_ptr<const Plane> out = pl;
  cache().put(keys[n], out);
  return out;
}

inline float samplePlane(const Plane& p, int c, float u, float v) {
  const float fx = std::clamp(u * float(p.w) - 0.5f, 0.f, float(p.w - 1)), fy = std::clamp(v * float(p.h) - 0.5f, 0.f, float(p.h - 1));
  const int x0 = std::min(int(fx), p.w - 1), y0 = std::min(int(fy), p.h - 1);
  const int x1 = std::min(x0 + 1, p.w - 1), y1 = std::min(y0 + 1, p.h - 1);
  const float tx = fx - float(x0), ty = fy - float(y0);
  const float* d = p.data.data();
  const int ch = p.ch;
  const float a = d[(std::size_t(y0) * p.w + x0) * ch + c], b = d[(std::size_t(y0) * p.w + x1) * ch + c];
  const float e = d[(std::size_t(y1) * p.w + x0) * ch + c], g = d[(std::size_t(y1) * p.w + x1) * ch + c];
  const float t0 = a + (b - a) * tx, t1 = e + (g - e) * tx;
  return t0 + (t1 - t0) * ty;
}

// OKLab via ProPhoto (D50) -> XYZ -> Bradford D65 -> LMS: the combined matrix
struct Lab3 {
  float M[9];
};
const Lab3& labMatrix() {
  static const Lab3 m = [] {
    const double pp[9] = {0.7976749, 0.1351917, 0.0313534, 0.2880402, 0.7118741, 0.0000857, 0.0, 0.0, 0.8252100};
    const double br[9] = {0.9555766, -0.0230393, 0.0631636, -0.0282895, 1.0099416, 0.0210077, 0.0122982, -0.0204830, 1.3299098};
    const double m1[9] = {0.8189330101, 0.3618667424, -0.1288597137, 0.0329845436, 0.9293118715, 0.0361456387, 0.0482003018, 0.2643662691, 0.6338517070};
    auto mul = [](const double* a, const double* b, double* o) {
      for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) o[3 * i + j] = a[3 * i] * b[j] + a[3 * i + 1] * b[3 + j] + a[3 * i + 2] * b[6 + j];
    };
    double t[9], r[9];
    mul(br, pp, t);
    mul(m1, t, r);
    Lab3 o;
    for (int i = 0; i < 9; ++i) o.M[i] = float(r[i]);
    return o;
  }();
  return m;
}

inline void rangeOf(const float lin[3], float& L, float& hue, float& sat) {
  const float Y = std::clamp(lin[0] * kLumaPP[0] + lin[1] * kLumaPP[1] + lin[2] * kLumaPP[2], 0.f, 1.f);
  L = Y > 0.008856f ? 1.16f * std::cbrt(Y) - 0.16f : 9.033f * Y;
  const float* M = labMatrix().M;
  const float r = std::max(lin[0], 0.f), g = std::max(lin[1], 0.f), b = std::max(lin[2], 0.f);
  const float l = std::cbrt(M[0] * r + M[1] * g + M[2] * b), m = std::cbrt(M[3] * r + M[4] * g + M[5] * b),
              s = std::cbrt(M[6] * r + M[7] * g + M[8] * b);
  const float Lo = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
  const float A = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
  const float B = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
  hue = std::atan2(B, A) * 57.29577951f;
  if (hue < 0.f) hue += 360.f;
  sat = std::sqrt(A * A + B * B) / std::max(Lo, 0.05f);
}

inline float compValue(const Frame& f, const Comp& c, float u, float v, float x, float y, const float lin[3], const float src[3], bool& rangeDone,
                       float& L, float& hue, float& sat) {
  switch (c.kind) {
    case kCLinear: {
      const float t = ((x - c.p[0]) * c.p[2] + (y - c.p[1]) * c.p[3]) * c.p[4];
      return 1.f - sstep(0.f, 1.f, t);
    }
    case kCRadial: {
      const float dx = x - c.p[0], dy = y - c.p[1];
      const float xr = (dx * c.p[2] + dy * c.p[3]) * c.p[4], yr = (-dx * c.p[3] + dy * c.p[2]) * c.p[5];
      const float e = std::sqrt(xr * xr + yr * yr);
      return c.p[6] < 1e-3f ? (e <= 1.f ? 1.f : 0.f) : 1.f - sstep(1.f - c.p[6], 1.f, e);
    }
    case kCLum:
    case kCColour: {
      if (!rangeDone) {
        rangeOf(lin, L, hue, sat);
        rangeDone = true;
      }
      if (c.kind == kCLum) return sstep(c.p[0] - c.p[2], c.p[0], L) * (1.f - sstep(c.p[1], c.p[1] + c.p[2], L));
      float dh = std::fabs(hue - c.p[0]);
      dh = std::min(dh, 360.f - dh);
      const float ch = 1.f - sstep(c.p[1], c.p[1] + c.p[2], dh);
      const float cs = sstep(0.5f * c.p[3], c.p[3] + 0.005f, sat);
      return ch * cs;
    }
    case kCBrush: return c.plane ? samplePlane(*c.plane, 0, u, v) : 0.f;
    case kCRaster: {
      if (!c.plane) return 0.f;
      const float a0 = samplePlane(*c.plane, 0, u, v), a1 = samplePlane(*c.plane, 1, u, v), a2 = samplePlane(*c.plane, 2, u, v),
                  b = samplePlane(*c.plane, 3, u, v);
      const float q = a0 * guideCh(src[0], f.guideRef) + a1 * guideCh(src[1], f.guideRef) + a2 * guideCh(src[2], f.guideRef) + b;
      return std::clamp(q, 0.f, 1.f);
    }
    default: return 0.f;
  }
}

void writeDesc(Frame& f) {
  // vec4 layout (local.glsl): [0] (nAdj, anyWindow, sx, sy) [1] (guideRef) [2..4] OKLab matrix rows; adjustments at 8 (16 vec4 each, up to
  // 16), components at 8 + 256 (4 vec4 each)
  const int nAdj = int(f.adj.size());
  f.desc.assign(std::size_t(8 + 256 + 4 * f.comp.size()) * 4, 0.f);
  auto V = [&](int i) { return f.desc.data() + std::size_t(i) * 4; };
  V(0)[0] = float(nAdj), V(0)[1] = f.anyWindow ? 1.f : 0.f, V(0)[2] = f.sx, V(0)[3] = f.sy;
  V(1)[0] = f.guideRef;
  const float* M = labMatrix().M;
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c) V(2 + r)[c] = M[3 * r + c];
  for (int a = 0; a < nAdj; ++a) {
    const Adj& A = f.adj[std::size_t(a)];
    float* b = V(8 + 16 * a);
    b[0] = float(A.nc), b[1] = A.invert ? 1.f : 0.f, b[2] = A.weight, b[3] = float(A.c0);
    for (int k = 0; k < 12; ++k) b[4 + k] = A.lin[k];
    b[16] = A.tintRgb[0], b[17] = A.tintRgb[1], b[18] = A.tintRgb[2], b[19] = A.hasLinear ? 1.f : 0.f;
    for (int k = 0; k < kLocalWindowSlots; ++k) b[20 + k] = A.win[k];
  }
  for (std::size_t c = 0; c < f.comp.size(); ++c) {
    const Comp& C = f.comp[c];
    float* b = V(8 + 256 + 4 * int(c));
    b[0] = float(C.kind), b[1] = float(C.op), b[2] = C.invert ? 1.f : 0.f, b[3] = float(C.planeOffset);
    for (int k = 0; k < 8; ++k) b[4 + k] = C.p[k];
    if (C.plane) b[12] = float(C.plane->w), b[13] = float(C.plane->h), b[14] = float(C.plane->ch);
  }
  std::uint64_t h = 0xDE5C;
  for (float x : f.desc) h = mixf(h, x);
  f.descKey = h;
}

// tint colour: hue (degrees) at full saturation in sRGB, to linear ProPhoto, luminance 1
void tintColour(float hueDeg, float out[3]) {
  const float hh = std::fmod(hueDeg / 60.f, 6.f);
  const float x = 1.f - std::fabs(std::fmod(hh, 2.f) - 1.f);
  float r = 0, g = 0, b = 0;
  if (hh < 1) r = 1, g = x;
  else if (hh < 2) r = x, g = 1;
  else if (hh < 3) g = 1, b = x;
  else if (hh < 4) g = x, b = 1;
  else if (hh < 5) r = x, b = 1;
  else r = 1, b = x;
  // a gentle colour: halfway to white before linearising (a full primary as a filter is too strong)
  float s[3] = {0.5f + 0.5f * r, 0.5f + 0.5f * g, 0.5f + 0.5f * b};
  for (float& c : s) c = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
  // linear sRGB (D65) -> ProPhoto (D50): Bradford-adapted matrix
  const float m[9] = {0.5293459f, 0.3300607f, 0.1405933f, 0.0983699f, 0.8734079f, 0.0282222f, 0.0168832f, 0.1176741f, 0.8654427f};
  for (int i = 0; i < 3; ++i) out[i] = m[3 * i] * s[0] + m[3 * i + 1] * s[1] + m[3 * i + 2] * s[2];
  const float Y = out[0] * kLumaPP[0] + out[1] * kLumaPP[1] + out[2] * kLumaPP[2];
  for (int i = 0; i < 3; ++i) out[i] /= std::max(Y, 1e-6f);
}

}  // namespace

void logLine(const std::string& s) {
#if defined(__ANDROID__)
  __android_log_write(ANDROID_LOG_INFO, "dapple_engine", s.c_str());
#else
  std::fprintf(stderr, "dapple_engine: %s\n", s.c_str());
#endif
}

// ---- raster store --------------------------------------------------------------------------------------------------------------------
bool rasterSet(const std::string& id, int w, int h, const std::uint8_t* bytes, int stride) {
  if (id.empty() || id.size() > 128 || w <= 0 || h <= 0 || w > 8192 || h > 8192 || !bytes) return false;
  if (stride <= 0) stride = w;
  auto r = std::make_shared<Raster>();
  const int L = std::max(w, h);
  if (L <= 1024) {
    r->w = w, r->h = h;
    r->px.resize(std::size_t(w) * h);
    for (int y = 0; y < h; ++y) std::memcpy(r->px.data() + std::size_t(y) * w, bytes + std::size_t(y) * stride, std::size_t(w));
  } else {  // area average to 1024 px long edge
    r->w = std::max(1, int(std::lround(double(w) * 1024 / L))), r->h = std::max(1, int(std::lround(double(h) * 1024 / L)));
    r->px.resize(std::size_t(r->w) * r->h);
    for (int j = 0; j < r->h; ++j) {
      const int y0 = j * h / r->h, y1 = std::max(y0 + 1, (j + 1) * h / r->h);
      for (int i = 0; i < r->w; ++i) {
        const int x0 = i * w / r->w, x1 = std::max(x0 + 1, (i + 1) * w / r->w);
        unsigned acc = 0, n = 0;
        for (int y = y0; y < std::min(y1, h); ++y)
          for (int x = x0; x < std::min(x1, w); ++x) acc += bytes[std::size_t(y) * stride + x], ++n;
        r->px[std::size_t(j) * r->w + i] = std::uint8_t((acc + n / 2) / std::max(1u, n));
      }
    }
  }
  std::uint64_t hsh = mix64(0x7A57E7, std::uint64_t(r->w) << 32 | std::uint32_t(r->h));
  std::size_t i = 0;
  for (; i + 8 <= r->px.size(); i += 8) {
    std::uint64_t q;
    std::memcpy(&q, r->px.data() + i, 8);
    hsh = mix64(hsh, q);
  }
  for (; i < r->px.size(); ++i) hsh = mix64(hsh, r->px[i]);
  r->hash = hsh | 1;
  RasterStore& s = rasters();
  std::lock_guard<std::mutex> lk(s.m);
  if (auto old = s.map.find(id); old != s.map.end()) {
    s.bytes -= old->second->px.size();
  } else {
    s.order.push_back(id);
  }
  s.bytes += r->px.size();
  s.map[id] = std::move(r);
  while (s.order.size() > 1 && (s.map.size() > 64 || s.bytes > (96u << 20))) {
    logLine("local adjustments: raster store full (64 planes / 96 MB), dropped '" + s.order.front().substr(0, 40) + "'");
    s.bytes -= s.map[s.order.front()]->px.size();
    s.map.erase(s.order.front());
    s.order.pop_front();
  }
  return true;
}
bool rasterRemove(const std::string& id) {
  RasterStore& s = rasters();
  std::lock_guard<std::mutex> lk(s.m);
  auto it = s.map.find(id);
  if (it == s.map.end()) return false;
  s.bytes -= it->second->px.size();
  s.order.remove(id);
  s.map.erase(it);
  return true;
}
bool rasterHas(const std::string& id) { return rasterGet(id) != nullptr; }
std::shared_ptr<const Raster> rasterGet(const std::string& id) {
  RasterStore& s = rasters();
  std::lock_guard<std::mutex> lk(s.m);
  auto it = s.map.find(id);
  return it == s.map.end() ? nullptr : it->second;
}
std::uint64_t rasterContentHash(const std::string& id) {
  auto r = rasterGet(id);
  return r ? r->hash : 0;
}

Counters counters() {
  Counters c;
  c.brushBuilds = g_brushBuilds.load(), c.brushPrefixHits = g_brushPrefix.load(), c.rasterBuilds = g_rasterBuilds.load();
  c.guideBuilds = g_guideBuilds.load(), c.planeHits = g_planeHits.load(), c.binds = g_binds.load();
  return c;
}
void resetPlaneCache() { cache().clear(); }

// ---- streaming export capture (section 42) -------------------------------------------------------------------------------------------
void guideSamplePos(int W, int H, int i, int j, int& x, int& y) {
  x = std::min(W - 1, (2 * i + 1) * W / 128);
  y = std::min(H - 1, (2 * j + 1) * H / 128);
}
void guideCellSpan(int j, int n, int N, int& a, int& b) {
  a = int(std::floor(double(j) * N / n));
  b = std::min(N, std::max(a + 1, int(std::floor(double(j + 1) * N / n))));
}
std::vector<std::pair<int, int>> guideGridSizes(const std::vector<LocalAdjustment>& list, int W, int H, bool interactive) {
  std::vector<std::pair<int, int>> out;
  if (W <= 0 || H <= 0) return out;
  const int L = std::max(W, H);
  const float sx = float(W) / float(L), sy = float(H) / float(L);
  for (const LocalAdjustment& a : list)
    for (const MaskComponent& m : a.mask) {
      if (m.kind != MaskKind::Raster || m.refine <= 0.f) continue;
      const std::shared_ptr<const Raster> ra = rasterGet(m.ref);
      if (!ra) continue;
      const int gl = std::clamp(std::max(ra->w, ra->h), 64, interactive ? 512 : 1024);  // rasterPlane's grid
      const std::pair<int, int> s{std::max(8, int(std::lround(gl * sx))), std::max(8, int(std::lround(gl * sy)))};
      if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
    }
  return out;
}
void setGuideCapture(const GuideCapture* c) { tlGuide = c; }

// ---- bind ----------------------------------------------------------------------------------------------------------------------------
std::shared_ptr<const Frame> bind(const std::vector<LocalAdjustment>& list, const SourceImage& src, bool interactive, bool preview) {
  if (list.empty() || src.linear.empty()) return nullptr;
  auto f = std::make_shared<Frame>();
  const int W = src.linear.width, H = src.linear.height, L = std::max(W, H);
  f->sx = float(W) / float(L);
  f->sy = float(H) / float(L);
  GuideKey gk;
  bool gkDone = false;
  int nRaster = 0, nBrush = 0;
  std::uint64_t pk = 0x91A4E5;
  for (std::size_t ai = 0; ai < list.size(); ++ai) {
    const LocalAdjustment& a = list[ai];
    if (a.mask.empty()) continue;
    if (!preview && (!a.enabled || a.strength <= 0.f)) continue;
    const bool lin = a.ops.anyLinear() && !preview, win = a.ops.anyWindow() && !preview;
    if (!lin && !win && !preview) continue;
    Adj A;
    A.srcIndex = int(ai);
    A.invert = a.invert;
    A.weight = preview ? 1.f : a.strength / 100.f;
    A.c0 = int(f->comp.size());
    const auto& v = a.ops.v;
    A.lin[0] = v[kLoExposure];
    A.lin[1] = v[kLoContrast] / 100.f, A.lin[2] = v[kLoHighlights] / 100.f, A.lin[3] = v[kLoShadows] / 100.f;
    A.lin[4] = v[kLoWhites] / 100.f, A.lin[5] = v[kLoBlacks] / 100.f;
    A.lin[6] = v[kLoTemp], A.lin[7] = v[kLoTint];
    A.lin[8] = v[kLoSaturation] / 100.f, A.lin[9] = v[kLoVibrance] / 100.f, A.lin[10] = v[kLoColorSat] / 100.f;
    if (A.lin[10] > 0.f) tintColour(v[kLoColorHue], A.tintRgb);
    A.hasLinear = lin;
    A.win[0] = v[kLoClarity] / 100.f, A.win[1] = v[kLoTexture] / 100.f, A.win[2] = v[kLoSharpen] / 100.f;
    A.win[3] = v[kLoNoise] / 100.f, A.win[4] = v[kLoDehaze] / 100.f;
    for (const MaskComponent& m : a.mask) {
      Comp c;
      c.op = int(m.op);
      c.invert = m.invert;
      switch (m.kind) {
        case MaskKind::Linear: {
          c.kind = kCLinear;
          const float x0 = m.x0 * f->sx, y0 = m.y0 * f->sy, dx = m.x1 * f->sx - x0, dy = m.y1 * f->sy - y0;
          c.p[0] = x0, c.p[1] = y0, c.p[2] = dx, c.p[3] = dy, c.p[4] = 1.f / std::max(dx * dx + dy * dy, 1e-12f);
          break;
        }
        case MaskKind::Radial: {
          c.kind = kCRadial;
          const float th = m.angle * 0.01745329252f;
          c.p[0] = m.cx * f->sx, c.p[1] = m.cy * f->sy, c.p[2] = std::cos(th), c.p[3] = std::sin(th);
          c.p[4] = 1.f / m.rx, c.p[5] = 1.f / m.ry, c.p[6] = m.feather / 100.f;
          break;
        }
        case MaskKind::Luminance:
          c.kind = kCLum;
          c.p[0] = m.lo / 100.f, c.p[1] = m.hi / 100.f, c.p[2] = std::max(m.lumFeather / 100.f, 1e-4f);
          break;
        case MaskKind::Colour:
          c.kind = kCColour;
          c.p[0] = m.hue, c.p[1] = m.hueRange, c.p[2] = std::max(m.hueFeather, 0.5f), c.p[3] = m.satMin / 100.f * 0.25f;
          break;
        case MaskKind::Brush:
          c.kind = kCBrush;
          if (m.strokes.empty()) break;
          if (++nBrush > kMaxBrushComponents) {
            logOncePerKey("local adjustments: more than 8 brush components in one edit, the extra ones are ignored");
            break;
          }
          c.plane = brushPlane(m, f->sx, f->sy, interactive);
          break;
        case MaskKind::Raster: {
          c.kind = kCRaster;
          if (++nRaster > kMaxRasterComponents) {
            logOncePerKey("local adjustments: more than 4 raster components in one edit, the extra ones are ignored");
            break;
          }
          const std::shared_ptr<const Raster> ra = rasterGet(m.ref);
          if (!ra) {
            logOncePerKey("local adjustments: raster mask '" + m.ref.substr(0, 40) + "' not supplied (dapple_mask_raster_set): treated as empty");
            break;
          }
          if (!gkDone) {
            gk = guideKeyOf(src);
            gkDone = true;
          }
          c.plane = rasterPlane(*ra, src, gk, f->sx, f->sy, interactive, m.refine, m);
          f->anyRaster = true;
          break;
        }
        default: continue;  // unknown kinds evaluate to nothing
      }
      if (c.plane) {
        c.planeOffset = f->planeFloats;
        f->planeFloats += int(c.plane->data.size());
        pk = mix64(pk, c.plane->key);
      }
      f->comp.push_back(c);
    }
    A.nc = int(f->comp.size()) - A.c0;
    if (A.nc == 0) continue;
    f->anyLinear |= lin;
    if (win)
      for (int k = 0; k < kLocalWindowSlots; ++k)
        if (A.win[k] != 0.f) f->winUsed[k] = true;
    f->anyWindow |= win;
    f->needGuided |= A.win[0] != 0.f || A.win[4] != 0.f;
    f->adj.push_back(A);
  }
  if (f->adj.empty()) return nullptr;
  if (f->anyRaster && gkDone) f->guideRef = gk.ref;
  f->planesKey = mix64(pk, std::uint64_t(f->planeFloats));
  writeDesc(*f);
  ++g_binds;
  return f;
}

std::vector<float> planeStore(const Frame& f) {
  std::vector<float> out(std::size_t(std::max(1, f.planeFloats)), 0.f);
  for (const Comp& c : f.comp)
    if (c.plane) std::memcpy(out.data() + c.planeOffset, c.plane->data.data(), c.plane->bytes());
  return out;
}

// ---- per pixel -------------------------------------------------------------------------------------------------------------------------
float maskOf(const Frame& f, int a, float u, float v, const float lin[3], const float src[3]) {
  const Adj& A = f.adj[std::size_t(a)];
  const float x = u * f.sx, y = v * f.sy;
  bool rangeDone = false;
  float L = 0, hue = 0, sat = 0;
  float m = 0.f;
  for (int j = 0; j < A.nc; ++j) {
    const Comp& c = f.comp[std::size_t(A.c0 + j)];
    float cv = compValue(f, c, u, v, x, y, lin, src, rangeDone, L, hue, sat);
    if (c.invert) cv = 1.f - cv;
    if (j == 0 || c.op == 0) m = j == 0 ? cv : std::max(m, cv);
    else if (c.op == 1) m *= 1.f - cv;
    else m *= cv;
  }
  if (A.invert) m = 1.f - m;
  return m * A.weight;
}

void applyLinear(const Frame& f, float u, float v, const float src[3], float px[3], float* K) {
  const int n = int(f.adj.size());
  float w[kMaxLocalAdjustments];
  const float in[3] = {px[0], px[1], px[2]};
  for (int a = 0; a < n; ++a) w[a] = maskOf(f, a, u, v, in, src);
  if (K)
    for (int k = 0; k < kLocalWindowSlots; ++k) K[k] = 0.f;
  for (int a = 0; a < n; ++a) {
    const float wa = w[a];
    if (wa <= 0.f) continue;
    const Adj& A = f.adj[std::size_t(a)];
    if (K)
      for (int k = 0; k < kLocalWindowSlots; ++k) K[k] += wa * A.win[k];
    if (!A.hasLinear) continue;
    const float* l = A.lin;
    if (l[6] != 0.f || l[7] != 0.f) {  // white balance (Renderer::whiteBalanceGain with the weighted sliders)
      float g[3] = {std::exp2(0.006f * l[6] * wa), std::exp2(-0.004f * l[7] * wa), std::exp2(-0.006f * l[6] * wa)};
      const float nrm = g[0] * kLumaPP[0] + g[1] * kLumaPP[1] + g[2] * kLumaPP[2];
      for (int c = 0; c < 3; ++c) px[c] *= g[c] / nrm;
    }
    if (l[0] != 0.f) {
      const float e = std::exp2(l[0] * wa);
      for (int c = 0; c < 3; ++c) px[c] *= e;
    }
    if (l[1] != 0.f || l[2] != 0.f || l[3] != 0.f || l[4] != 0.f || l[5] != 0.f) {
      const float Y = px[0] * kLumaPP[0] + px[1] * kLumaPP[1] + px[2] * kLumaPP[2];
      if (Y > 1e-9f) {
        const float Lg = std::log2(Y / 0.18f);
        const float d = l[1] * 0.875f * std::tanh(Lg / 2.5f) + l[2] * 1.25f * sstep(-0.5f, 2.f, Lg) +
                        l[3] * 1.25f * (1.f - sstep(-4.5f, -0.5f, Lg)) * sstep(-11.f, -8.f, Lg) + l[4] * 0.8f * sstep(0.5f, 2.5f, Lg) +
                        l[5] * 0.8f * (1.f - sstep(-8.f, -4.f, Lg)) * sstep(-13.f, -10.f, Lg);
        const float e = std::exp2(wa * d);
        for (int c = 0; c < 3; ++c) px[c] *= e;
      }
    }
    if (l[8] != 0.f || l[9] != 0.f) {
      const float Y = px[0] * kLumaPP[0] + px[1] * kLumaPP[1] + px[2] * kLumaPP[2];
      const float mx = std::max(px[0], std::max(px[1], px[2])), mn = std::min(px[0], std::min(px[1], px[2]));
      const float sv = mx > 1e-9f ? (mx - mn) / mx : 0.f;
      const float s = (1.f + l[8] * wa) * (1.f + l[9] * wa * (1.f - sv));
      for (int c = 0; c < 3; ++c) px[c] = std::max(Y + (px[c] - Y) * s, 0.f);
    }
    if (l[10] > 0.f) {
      const float t = l[10] * wa;
      for (int c = 0; c < 3; ++c) px[c] *= 1.f + (A.tintRgb[c] - 1.f) * t;
    }
  }
}

void linearBlock(const Frame& f, const FrameKernels& fk, float* px, int n, int x0, int y, const AuxTarget* aux) {
  float srcBuf[256 * 3];
  if (f.anyRaster) fk.sourceRow(srcBuf, n, x0, y);
  const bool wantK = aux && aux->data && f.anyWindow;
  float K[kLocalWindowSlots];
  for (int i = 0; i < n; ++i) {
    float u, v;
    fk.srcUV(x0 + i, y, u, v);
    const float* s = f.anyRaster ? srcBuf + 3 * i : px + 3 * i;
    applyLinear(f, u, v, s, px + 3 * i, wantK ? K : nullptr);
    if (wantK) {
      const std::size_t o = std::size_t(y - aux->oy) * aux->w + std::size_t(x0 + i - aux->ox);
      for (int k = 0; k < kLocalWindowSlots; ++k)
        if (aux->slot[k] >= 0) aux->data[std::size_t(aux->slot[k]) * aux->plane + o] = K[k];
    }
  }
}

int auxLayout(const Frame& f, AuxTarget& t) {
  int n = 0;
  for (int k = 0; k < kLocalWindowSlots; ++k) t.slot[k] = f.winUsed[k] ? n++ : -1;
  return n;
}

// ---- LocalDetail -------------------------------------------------------------------------------------------------------------------------
DetailRadii detailRadii(const Frame& f, float scale) {
  DetailRadii r;
  r.rt = std::max(1, int(std::lround(2.f * scale)));
  r.rs = std::max(1, int(std::lround(0.75f * scale)));
  r.rg = std::max(2, int(std::lround(0.02f * 2048.f * scale / 4.f)));
  r.guided = f.needGuided;
  int h = std::max(2 * r.rt + 2, r.rs + 1);
  if (r.guided) h = std::max(h, 4 * (2 * r.rg + 2) + 4);
  r.halo = (h + 3) / 4 * 4;
  return r;
}

void detailWindow(const Frame& f, ImageF& img, const AuxTarget& aux, int fw, int fh, float scale) {
  (void)fw, (void)fh;
  const int w = img.width, h = img.height;
  const std::size_t N = std::size_t(w) * h;
  const DetailRadii R = detailRadii(f, scale);
  std::vector<float> Y(N), bf, bs, base;
  parallelFor(h, [&](int y) {
    const float* p = img.row(y);
    for (int x = 0; x < w; ++x) Y[std::size_t(y) * w + x] = kLumaS[0] * p[3 * x] + kLumaS[1] * p[3 * x + 1] + kLumaS[2] * p[3 * x + 2];
  });
  const bool fine = f.winUsed[1] || f.winUsed[3], vfine = f.winUsed[2];
  if (fine) {
    bf = Y;
    boxRep(bf, w, h, 1, R.rt);
    boxRep(bf, w, h, 1, R.rt);
  }
  if (vfine) {
    bs = Y;
    boxRep(bs, w, h, 1, R.rs);
  }
  if (R.guided) {  // the clarity base: guided filter on the window's 1/4 grid (the window origin is on the frame's grid)
    const int lw = (w + 3) / 4, lh = (h + 3) / 4;
    std::vector<float> g(std::size_t(lw) * lh * 2);
    parallelFor(lh, [&](int ly) {
      for (int lx = 0; lx < lw; ++lx) {
        const int x0 = lx * 4, y0 = ly * 4, x1 = std::min(w, x0 + 4), y1 = std::min(h, y0 + 4);
        float acc[3] = {0, 0, 0};
        for (int y = y0; y < y1; ++y) {
          const float* p = img.row(y);
          for (int x = x0; x < x1; ++x)
            for (int c = 0; c < 3; ++c) acc[c] += p[3 * x + c];
        }
        const float inv = 1.f / float((y1 - y0) * (x1 - x0));
        const float yv = (acc[0] * inv) * kLumaS[0] + (acc[1] * inv) * kLumaS[1] + (acc[2] * inv) * kLumaS[2];
        g[(std::size_t(ly) * lw + lx) * 2] = yv;
        g[(std::size_t(ly) * lw + lx) * 2 + 1] = yv * yv;
      }
    });
    boxRep(g, lw, lh, 2, R.rg);
    const float eps = 0.045f * 0.045f;
    for (std::size_t i = 0; i < std::size_t(lw) * lh; ++i) {
      const float m = g[2 * i], v = std::max(0.f, g[2 * i + 1] - m * m);
      const float a = v / (v + eps);
      g[2 * i] = a;
      g[2 * i + 1] = m - a * m;
    }
    boxRep(g, lw, lh, 2, R.rg);
    base.resize(N);
    parallelFor(h, [&](int y) {
      const float py = std::clamp((float(y) + 0.5f) / 4.f - 0.5f, 0.f, float(lh - 1));
      const int y0 = std::min(int(py), lh - 1), y1 = std::min(y0 + 1, lh - 1);
      const float ty = py - float(y0);
      for (int x = 0; x < w; ++x) {
        const float pxf = std::clamp((float(x) + 0.5f) / 4.f - 0.5f, 0.f, float(lw - 1));
        const int x0 = std::min(int(pxf), lw - 1), x1 = std::min(x0 + 1, lw - 1);
        const float tx = pxf - float(x0);
        float ab[2];
        for (int c = 0; c < 2; ++c) {
          const float a0 = g[(std::size_t(y0) * lw + x0) * 2 + c], a1 = g[(std::size_t(y0) * lw + x1) * 2 + c];
          const float b0 = g[(std::size_t(y1) * lw + x0) * 2 + c], b1 = g[(std::size_t(y1) * lw + x1) * 2 + c];
          const float t0 = a0 + (a1 - a0) * tx, t1 = b0 + (b1 - b0) * tx;
          ab[c] = t0 + (t1 - t0) * ty;
        }
        base[std::size_t(y) * w + x] = ab[0] * Y[std::size_t(y) * w + x] + ab[1];
      }
    });
  }
  parallelFor(h, [&](int y) {
    float* p = img.row(y);
    for (int x = 0; x < w; ++x) {
      const std::size_t i = std::size_t(y) * w + x;
      float K[kLocalWindowSlots];
      for (int k = 0; k < kLocalWindowSlots; ++k) K[k] = aux.slot[k] >= 0 ? aux.data[std::size_t(aux.slot[k]) * aux.plane + i] : 0.f;
      const float Yv = Y[i];
      float dY = 0.f;
      if (K[0] != 0.f) {
        const float g = std::clamp(K[0], -2.f, 2.f);
        const float gg = g > 0.f ? g * 1.1f : g * 0.85f;
        const float mid = 0.4f + 0.6f * (1.f - (2.f * Yv - 1.f) * (2.f * Yv - 1.f));
        dY += gg * (Yv - base[i]) * mid;
      }
      if (K[1] != 0.f) {
        const float t = std::clamp(K[1], -2.f, 2.f), d = Yv - bf[i];
        dY += t > 0.f ? t * 1.5f * d / (1.f + 10.f * std::fabs(d)) : t * 0.9f * d;
      }
      if (K[2] != 0.f) {
        const float s = std::clamp(K[2], -1.f, 2.f), d = Yv - bs[i];
        if (s > 0.f) {
          const float a = std::fabs(d);
          dY += s * 1.5f * d * (0.15f / (0.15f + a)) * (a / (a + 0.0025f));
        } else {
          dY += s * d;
        }
      }
      if (K[3] > 0.f) {
        const float kn = std::min(K[3], 1.f), d = Yv - bf[i];
        const float s2 = (0.004f + 0.02f * kn) * (0.004f + 0.02f * kn);
        dY -= kn * d * s2 / (s2 + d * d);
      }
      float c[3] = {p[3 * x] + dY, p[3 * x + 1] + dY, p[3 * x + 2] + dY};
      if (K[4] != 0.f) {
        const float kd = std::clamp(K[4], -1.f, 1.f);
        if (kd > 0.f) {
          const float mn = std::max(0.f, std::min(c[0], std::min(c[1], c[2])));
          const float V = std::clamp(mn * (base[i] / std::max(Yv, 1e-3f)), 0.f, 1.f);
          const float a = std::min(0.9f * kd, 0.95f) * V;
          for (float& cc : c) cc = (cc - a) / (1.f - a);
        } else {
          const float veil = 0.6f * base[i] + 0.35f, t = -0.6f * kd;
          for (float& cc : c) cc += (veil - cc) * t;
        }
      }
      for (int k = 0; k < 3; ++k) p[3 * x + k] = std::clamp(c[k], 0.f, 1.f);
    }
  });
}

void maskRow(const Frame& f, const FrameKernels& fk, int index, int y, std::uint8_t* out) {
  const int w = fk.width();
  int ai = -1;
  for (std::size_t a = 0; a < f.adj.size(); ++a)
    if (f.adj[a].srcIndex == index) ai = int(a);
  float src[256 * 3], lin[256 * 3];
  for (int x0 = 0; x0 < w; x0 += 256) {
    const int n = std::min(256, w - x0);
    fk.sourceRow(src, n, x0, y);
    std::memcpy(lin, src, std::size_t(n) * 3 * sizeof(float));
    fk.gain(lin, n);
    fk.hdrBlock(lin, n, x0, y);
    for (int i = 0; i < n; ++i) {
      float u, v;
      fk.srcUV(x0 + i, y, u, v);
      const float m = ai >= 0 ? maskOf(f, ai, u, v, lin + 3 * i, src + 3 * i) : 0.f;
      out[x0 + i] = std::uint8_t(std::clamp(m, 0.f, 1.f) * 255.f + 0.5f);
    }
  }
}

void sampleRange(const float lin[3], float& Lstar, float& hue, float& sat) {
  float L = 0;
  rangeOf(lin, L, hue, sat);
  Lstar = L * 100.f;
}

}  // namespace dapple::local
