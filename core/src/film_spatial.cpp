// Spatial and stochastic film effects: plan (parameters -> mixtures / tables), the Gaussian-mixture engine (FIR at full
// resolution, box pyramid for wide Gaussians), and the render driver (planar passes + a fused tile pass).
#include "film_spatial.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "parallel.h"

namespace dapple::film::detail {

const SpatialKernels& selectSpatialKernels() {
#if defined(DAPPLE_FILM_HAVE_AVX2)
  if (&selectKernels() == &kAvx2) return sAvx2;
#endif
  return sBase;
}

namespace {

inline int reflectIdx(int i, int n) {  // scipy "reflect": d c b a | a b c d | d c b a
  if (n == 1) return 0;
  const int period = 2 * n;
  i %= period;
  if (i < 0) i += period;
  return i < n ? i : period - 1 - i;
}

inline double nan0(float v) { return std::isnan(v) ? 0.0 : double(v); }

}  // namespace

// ---- mixtures -----------------------------------------------------------------------------------------------

Mixture mixIdentity() { return Mixture{}; }

Mixture mixGauss(double sigma) {
  Mixture m;
  m.w0 = 0;
  m.comps.push_back({sigma, 1.0});
  return m;
}

Mixture mixExpTail(double l) {
  // Gaussian-mixture surrogate of the 2D exponential (spec 03 s3.2, n = 3)
  static const double a[3] = {0.1633, 0.6496, 0.1870}, c[3] = {0.5360, 1.5236, 2.7684};
  const double norm = a[0] + a[1] + a[2];  // the published weights sum to 0.9999: renormalise so the filter conserves energy exactly
  Mixture m;
  m.w0 = 0;
  for (int k = 0; k < 3; ++k) m.comps.push_back({c[k] * l, a[k] / norm});
  return m;
}

Mixture mixConvolve(const Mixture& a, const Mixture& b) {
  Mixture m;
  m.w0 = a.w0 * b.w0;
  for (const auto& c : b.comps) m.comps.push_back({c.sigma, c.w * a.w0});
  for (const auto& c : a.comps) m.comps.push_back({c.sigma, c.w * b.w0});
  for (const auto& ca : a.comps)
    for (const auto& cb : b.comps) m.comps.push_back({std::sqrt(ca.sigma * ca.sigma + cb.sigma * cb.sigma), ca.w * cb.w});
  return m;
}

Mixture mixAffine(const Mixture& a, double wi, double wm) {
  Mixture m;
  m.w0 = wi + wm * a.w0;
  for (const auto& c : a.comps) m.comps.push_back({c.sigma, c.w * wm});
  return m;
}

// Merges components whose widths differ by less than 3 percent (variance-weighted) and drops empty ones.
static void mixSimplify(Mixture& m) {
  std::sort(m.comps.begin(), m.comps.end(), [](const GaussComp& a, const GaussComp& b) { return a.sigma < b.sigma; });
  std::vector<GaussComp> out;
  for (const GaussComp& c : m.comps) {
    if (c.w == 0) continue;
    if (!out.empty() && c.sigma < out.back().sigma * 1.03 + 1e-9 && out.back().w * c.w > 0) {
      GaussComp& o = out.back();
      const double w = o.w + c.w;
      o.sigma = std::sqrt((o.w * o.sigma * o.sigma + c.w * c.sigma * c.sigma) / w);
      o.w = w;
    } else {
      out.push_back(c);
    }
  }
  m.comps = std::move(out);
}

// ---- diffusion filter ----------------------------------------------------------------------------------------

namespace {
struct Family {
  double gain;
  double coreL, coreS;
  int coreN;
  double haloL, haloS;
  int haloN;
  double bloomL, bloomS;
  int bloomN;
  double bloomAlpha, wc, wh, wb, warmth;
};
bool family(const std::string& name, Family& f) {
  if (name == "glimmerglass") f = {0.65, 10, 1.5, 2, 50, 2.0, 3, 260, 2.5, 4, 3.2, 0.60, 0.30, 0.10, 0.0};
  else if (name == "black_pro_mist") f = {0.75, 16, 1.5, 2, 95, 2.0, 3, 380, 2.5, 4, 3.5, 0.40, 0.47, 0.13, 0.65};
  else if (name == "pro_mist") f = {1.05, 14, 1.5, 2, 150, 2.0, 3, 650, 2.5, 4, 2.9, 0.28, 0.42, 0.30, 0.40};
  else if (name == "cinebloom") f = {1.00, 20, 1.5, 2, 200, 2.0, 3, 1000, 2.5, 4, 2.5, 0.22, 0.30, 0.48, 0.85};
  else return false;
  return true;
}
std::vector<double> spread(double l0, double s, int n) {
  std::vector<double> v;
  if (n <= 1 || s <= 1.0) {
    v.push_back(l0);
    return v;
  }
  const double lo = l0 / s, hi = l0 * s;
  for (int i = 0; i < n; ++i) v.push_back(lo * std::pow(hi / lo, double(i) / (n - 1)));
  return v;
}
}  // namespace

bool buildDiffusionMixture(const DiffusionFilterParams& f, double pix, Mixture out[3]) {
  if (!f.active || f.strength <= 0 || f.spatialScale <= 0 || pix <= 0) return false;
  Family fam;
  if (!family(f.filterFamily, fam)) return false;
  static const double xs[5] = {-3, -2, -1, 0, 1}, ys[5] = {0.10, 0.20, 0.35, 0.55, 0.75};
  const double ls = std::log2(std::max(f.strength, 1e-6));
  double base;
  if (ls <= xs[0]) base = ys[0];
  else if (ls >= xs[4]) base = ys[4];
  else {
    int i = 0;
    while (ls > xs[i + 1]) ++i;
    base = ys[i] + (ys[i + 1] - ys[i]) * (ls - xs[i]) / (xs[i + 1] - xs[i]);
  }
  const double p = std::clamp(base * fam.gain, 0.0, 0.99);
  if (p <= 0) return false;
  double wc = fam.wc * std::max(f.coreIntensity, 0.0), wh = fam.wh * std::max(f.haloIntensity, 0.0),
         wb = fam.wb * std::max(f.bloomIntensity, 0.0);
  const double tot = wc + wh + wb;
  if (tot <= 0) {
    wc = fam.wc;
    wh = fam.wh;
    wb = fam.wb;
  } else {
    wc /= tot;
    wh /= tot;
    wb /= tot;
  }
  const double sc = std::max(f.spatialScale, 1e-6);
  auto px = [&](double lum) { return std::max(lum * sc / pix, 1e-6); };
  const auto lc = spread(fam.coreL * std::max(f.coreSize, 1e-6), fam.coreS, fam.coreN);
  const auto lh = spread(fam.haloL * std::max(f.haloSize, 1e-6), fam.haloS, fam.haloN);
  const auto lb = spread(fam.bloomL * std::max(f.bloomSize, 1e-6), fam.bloomS, fam.bloomN);
  std::vector<double> bw(lb.size());
  double bs = 0;
  for (std::size_t i = 0; i < lb.size(); ++i) {
    bw[i] = std::pow(lb[i], 2.0 - fam.bloomAlpha);
    bs += bw[i];
  }
  for (double& v : bw) v /= bs;
  // halo colour
  const double W = std::clamp(fam.warmth + f.haloWarmth, -1.5, 1.5);
  const double acoef[3] = {1.30, 0.15, -1.45};
  const int nh = int(lh.size());
  for (int c = 0; c < 3; ++c) {
    std::vector<double> v(nh, wh / nh);
    if (nh >= 2) {
      std::vector<double> g(nh);
      double gm = 0;
      for (int k = 0; k < nh; ++k) g[k] = -1.0 + 2.0 * k / (nh - 1);
      double sv = 0, sg = 0;
      for (int k = 0; k < nh; ++k) {
        sv += v[k];
        sg += v[k] * g[k];
      }
      gm = sv > 0 ? sg / sv : 0;
      for (double& x : g) x -= gm;
      double s2 = 0;
      std::vector<double> v2(nh);
      for (int k = 0; k < nh; ++k) {
        v2[k] = std::max(0.0, v[k] * (1.0 + W * acoef[c] * g[k]));
        s2 += v2[k];
      }
      if (s2 > 0)
        for (int k = 0; k < nh; ++k) v[k] = v2[k] * sv / s2;
    }
    Mixture K;
    K.w0 = 0;
    auto add = [&](double w, double l) {
      const Mixture e = mixExpTail(px(l));
      for (const auto& cc : e.comps) K.comps.push_back({cc.sigma, cc.w * w});
    };
    for (double l : lc) add(wc / lc.size(), l);
    for (int k = 0; k < nh; ++k) add(v[k], lh[k]);
    for (std::size_t k = 0; k < lb.size(); ++k) add(wb * bw[k], lb[k]);
    out[c] = mixAffine(K, 1.0 - p, p);
    mixSimplify(out[c]);
  }
  return true;
}

// ---- boost -----------------------------------------------------------------------------------------------------

bool boostConstants(double M, double A, double P, double X, double& x0, double& a, double& k) {
  if (!(M >= 0) || !(A >= 0 && A <= 1) || !(P >= 0) || !(X > 0)) return false;
  x0 = std::clamp(0.184 * std::exp2(P), 0.0, X);
  a = std::pow(28.0, 1.0 - A);
  const double u = x0 / X;
  const double den = std::exp(a * (1 - u)) - a * (1 - u) - 1.0;
  if (!(den > 0)) return false;
  k = (std::exp2(M) - 1.0) / den;
  return true;
}

// ---- Gaussian taps -----------------------------------------------------------------------------------------------

void gaussTaps(double sigma, std::vector<float>& w, int& r) {
  r = sigma < 3.0 ? int(std::floor(3.0 * sigma + 0.5)) : int(std::ceil(4.0 * sigma));
  if (sigma <= 0 || r <= 0) {
    r = 0;
    w.assign(1, 1.f);
    return;
  }
  std::vector<double> g(r + 1);
  double sum = 0;
  for (int k = 0; k <= r; ++k) {
    g[k] = std::exp(-double(k) * k / (2.0 * sigma * sigma));
    sum += k == 0 ? g[k] : 2 * g[k];
  }
  w.resize(r + 1);
  for (int k = 0; k <= r; ++k) w[k] = float(g[k] / sum);
}

// ---- arena -----------------------------------------------------------------------------------------------------------

Arena::~Arena() {
  AlignedAlloc<float> al;
  for (Chunk& c : chunks_) al.deallocate(c.p, c.cap);
}

float* Arena::alloc(std::size_t floats) {
  floats = (floats + 15) & ~std::size_t(15);
  for (; cur_ < chunks_.size(); ++cur_) {
    Chunk& c = chunks_[cur_];
    if (c.cap - c.used >= floats) {
      float* p = c.p + c.used;
      c.used += floats;
      return p;
    }
  }
  AlignedAlloc<float> al;
  const std::size_t cap = std::max<std::size_t>(floats, std::size_t(1) << 22);
  chunks_.push_back({al.allocate(cap), cap, floats});
  cur_ = chunks_.size() - 1;
  return chunks_.back().p;
}

Plane Arena::plane(int w, int h) {
  Plane p;
  p.w = w;
  p.h = h;
  p.stride = w;
  p.p = alloc(std::size_t(w) * std::size_t(h));
  return p;
}

void Arena::reset() {
  for (Chunk& c : chunks_) c.used = 0;
  cur_ = 0;
}

std::size_t Arena::capacityBytes() const {
  std::size_t s = 0;
  for (const Chunk& c : chunks_) s += c.cap * 4;
  return s;
}

// ---- Gaussian mixture engine -----------------------------------------------------------------------------------------

namespace {

struct LevelComp {
  std::vector<float> w;   // half taps (unit sum)
  std::vector<float> wv;  // the same, premultiplied by the component weight (vertical pass)
  int r = 0;
};

// dst = w0 * src + sum_k comps_k G * src + up2(upPlane). Processed in 2D tiles (256 x 64 + halo) so the source window,
// the horizontal-pass buffer and the accumulator stay in L2; every pixel of src is read and of dst written once.
void levelCombine(const SpatialKernels& K, const Plane& src, const Plane& dst, double w0, const std::vector<LevelComp>& comps,
                  const Plane* up) {
  const int W = src.w, H = src.h;
  int R = 0;
  for (const auto& c : comps) R = std::max(R, c.r);
  const int TW = 256, TH = R > 16 ? 128 : 64;
  const int ntx = (W + TW - 1) / TW, nty = (H + TH - 1) / TH;
  parallelFor(
      ntx * nty,
      [&](int ti) {
        thread_local Scratch sS, sT, sDt;
        thread_local std::vector<float> tmp;
        const int tx = ti % ntx, ty = ti / ntx;
        const int x0 = tx * TW, x1 = std::min(W, x0 + TW), y0 = ty * TH, y1 = std::min(H, y0 + TH);
        const int nx = x1 - x0, ny = y1 - y0;
        const int wS = nx + 2 * R, hS = ny + 2 * R;
        const std::ptrdiff_t sS_ = (wS + 15) & ~15, sD = (nx + 15) & ~15;
        float* const S = sS.get(std::size_t(sS_) * hS);
        float* const T = sT.get(std::size_t(sD) * hS);
        float* const Dt = sDt.get(std::size_t(sD) * ny);
        // source window with reflect boundary
        const int gx0 = x0 - R;
        const int lo = std::clamp(-gx0, 0, wS), hi = std::clamp(W - gx0, lo, wS);
        for (int sy = 0; sy < hS; ++sy) {
          const bool needRow = comps.size() > 0 || w0 != 0.0;
          if (!needRow) break;
          const float* s = src.row(reflectIdx(y0 - R + sy, H));
          float* d = S + sy * sS_;
          for (int i = 0; i < lo; ++i) d[i] = s[reflectIdx(gx0 + i, W)];
          std::memcpy(d + lo, s + gx0 + lo, std::size_t(hi - lo) * 4);
          for (int i = hi; i < wS; ++i) d[i] = s[reflectIdx(gx0 + i, W)];
        }
        for (int y = 0; y < ny; ++y) {
          const float* s = S + std::ptrdiff_t(y + R) * sS_ + R;
          if (w0 != 0.0) K.scaleRow(s, Dt + y * sD, nx, float(w0));
          else std::memset(Dt + y * sD, 0, std::size_t(nx) * 4);
        }
        if (up) {
          const int lw = up->w, lh = up->h;
          const int ilo = x0 / 2, ihi = (x1 - 1) / 2;
          const int cnt = ihi - ilo + 5;  // low columns ilo-2 .. ihi+2
          if (int(tmp.size()) < cnt + 8) tmp.resize(cnt + 8);
          const int jlo = std::max(ilo - 2, 0), jhi = std::min(ihi + 2, lw - 1);
          static const float wEven[4] = {-0.0234375f, 0.2265625f, 0.8671875f, -0.0703125f};
          static const float wOdd[4] = {-0.0703125f, 0.8671875f, 0.2265625f, -0.0234375f};
          for (int y = 0; y < ny; ++y) {
            const int gy = y0 + y, j = gy >> 1;
            const int base = (gy & 1) ? j - 1 : j - 2;
            const float* rows[4];
            for (int t = 0; t < 4; ++t) rows[t] = up->row(reflectIdx(base + t, lh));
            float* t0 = tmp.data();  // t0[k] <-> low column ilo - 2 + k
            K.lerpRows(rows[0] + jlo, rows[1] + jlo, rows[2] + jlo, rows[3] + jlo, (gy & 1) ? wOdd : wEven,
                       t0 + (jlo - (ilo - 2)), jhi - jlo + 1);
            for (int k = 0; k < cnt; ++k) {
              const int jj = ilo - 2 + k;
              if (jj < jlo || jj > jhi) t0[k] = t0[reflectIdx(jj, lw) - (ilo - 2)];
            }
            K.up2Row(t0 + 2, Dt + y * sD, (nx + 1) / 2, nx);
          }
        }
        for (const LevelComp& c : comps) {
          const int r = c.r;
          for (int sy = R - r; sy < R + ny + r; ++sy)
            K.firRow(S + std::ptrdiff_t(sy) * sS_ + R, T + std::ptrdiff_t(sy - (R - r)) * sD, nx, c.w.data(), r);
          for (int y = 0; y < ny; ++y)
            K.firCol(T + std::ptrdiff_t(y + r) * sD, sD, Dt + y * sD, nx, c.wv.data(), r, 1);
        }
        for (int y = 0; y < ny; ++y) std::memcpy(dst.row(y0 + y) + x0, Dt + y * sD, std::size_t(nx) * 4);
      },
      1);
}

 void downPlane(const Plane& src, const Plane& dst) {
  const SpatialKernels& K = selectSpatialKernels();
  parallelFor(
      dst.h,
      [&](int j) {
        const int a = std::min(2 * j, src.h - 1), b = std::min(2 * j + 1, src.h - 1);
        K.down2Row(src.row(a), src.row(b), dst.row(j), dst.w, src.w);
      },
      4);
}

}  // namespace

void gaussianBlurPlane(const SpatialKernels& K, const Plane& src, const Plane& dst, double sigma) {
  LevelComp c;
  gaussTaps(sigma, c.w, c.r);
  c.wv = c.w;
  std::vector<LevelComp> cs;
  if (c.r > 0) cs.push_back(c);
  levelCombine(K, src, dst, c.r > 0 ? 0.0 : 1.0, cs, nullptr);
}

namespace {
// src: full-resolution input (null when `half` is given); half: input already box-averaged to half resolution.
void mixImpl(const SpatialKernels& K, Arena& arena, const Plane* src, const Plane* half, const Plane& dst, const Mixture& mix,
             double kMinLowSigma) {
  double w0 = mix.w0;
  // pyramid depth available
  int avail = 0;
  {
    int w = dst.w, h = dst.h;
    while (avail < 14) {
      const int nw = (w + 1) / 2, nh = (h + 1) / 2;
      if (nw < 2 || nh < 2) break;
      w = nw;
      h = nh;
      ++avail;
    }
  }
  std::vector<std::vector<LevelComp>> per(avail + 1);
  int maxLevel = 0;
  for (const GaussComp& g : mix.comps) {
    if (g.w == 0) continue;
    LevelComp lc;
    int k = 0;
    double sl = g.sigma;
    if (g.sigma >= 3.0) {
      k = int(std::floor(std::log2(g.sigma / kMinLowSigma)));
      k = std::clamp(k, half ? 1 : 0, avail);
      if (k > 0) {
        const double f = double(1 << std::min(k, 30));
        sl = std::sqrt(std::max(g.sigma * g.sigma - (f * f - 1.0) / 12.0, 0.25)) / f;
      }
    }
    std::vector<float> taps;
    int r;
    if (g.sigma < 3.0) gaussTaps(g.sigma, taps, r);
    else {
      r = int(std::ceil(4.0 * sl));
      std::vector<double> t(r + 1);
      double sum = 0;
      for (int i = 0; i <= r; ++i) {
        t[i] = std::exp(-double(i) * i / (2.0 * sl * sl));
        sum += i == 0 ? t[i] : 2 * t[i];
      }
      taps.resize(r + 1);
      for (int i = 0; i <= r; ++i) taps[i] = float(t[i] / sum);
    }
    if (r == 0) {
      w0 += g.w;
      continue;
    }
    lc.r = r;
    lc.w.resize(r + 1);
    lc.wv.resize(r + 1);
    for (int i = 0; i <= r; ++i) {
      lc.w[i] = taps[i];
      lc.wv[i] = float(taps[i] * g.w);
    }
    per[k].push_back(std::move(lc));
    maxLevel = std::max(maxLevel, k);
  }
  if (maxLevel == 0) {
    levelCombine(K, *src, dst, w0, per[0], nullptr);
    return;
  }
  // box pyramid P[k] and accumulators L[k]
  const Arena::Mark am = arena.mark();
  std::vector<Plane> P(maxLevel + 1), L(maxLevel + 1);
  if (src) P[0] = *src;
  for (int k = 1; k <= maxLevel; ++k) {
    if (k == 1 && half) {
      P[1] = *half;
      continue;
    }
    P[k] = arena.plane((P[k - 1].w + 1) / 2, (P[k - 1].h + 1) / 2);
    downPlane(P[k - 1], P[k]);
  }
  for (int k = maxLevel; k >= 1; --k) L[k] = arena.plane(P[k].w, P[k].h);
  for (int k = maxLevel; k >= 1; --k)
    levelCombine(K, P[k], L[k], 0.0, per[k], k < maxLevel ? &L[k + 1] : nullptr);
  levelCombine(K, src ? *src : dst, dst, src ? w0 : 0.0, per[0], &L[1]);
  arena.release(am);
}
}  // namespace

void applyMixture(const SpatialKernels& K, Arena& arena, const Plane& src, const Plane& dst, const Mixture& mix) {
  mixImpl(K, arena, &src, nullptr, dst, mix, 2.0);
}

bool mixtureFitsHalf(const Mixture& mix) {
  if (mix.w0 != 0 || mix.comps.empty()) return false;
  for (const auto& g : mix.comps)
    if (g.sigma < 3.2) return false;
  return true;
}

void applyMixtureHalf(const SpatialKernels& K, Arena& arena, const Plane& half, const Plane& dst, const Mixture& mix) {
  mixImpl(K, arena, nullptr, &half, dst, mix, 1.6);
}

// ---- plan ----------------------------------------------------------------------------------------------------------------

namespace {
// Full-tap (symmetric) kernel from the half-kernel of gaussTaps; r == 0 gives the unit impulse.
std::vector<double> fullTaps(double sigma) {
  std::vector<float> w;
  int r = 0;
  if (sigma > 0) gaussTaps(sigma, w, r);
  std::vector<double> k(std::size_t(2 * r + 1), 0.0);
  if (r == 0) {
    k[0] = 1.0;
    return k;
  }
  for (int i = -r; i <= r; ++i) k[std::size_t(i + r)] = w[std::size_t(std::abs(i))];
  return k;
}
std::vector<double> convolve(const std::vector<double>& a, const std::vector<double>& b) {
  std::vector<double> o(a.size() + b.size() - 1, 0.0);
  for (std::size_t i = 0; i < a.size(); ++i)
    for (std::size_t j = 0; j < b.size(); ++j) o[i + j] += a[i] * b[j];
  return o;
}
}  // namespace

double grainBoxFactor(double k, double grainBlur, double scanBlur, double unsharpSigma, double unsharpAmount) {
  if (!(k > 1.0)) return 1.0;
  // total 1-D kernel on the pixel grid: (impulse * (1 + a) - a * G_unsharp) * G_grain * G_scan
  std::vector<double> kern = convolve(fullTaps(grainBlur), fullTaps(scanBlur));
  if (unsharpSigma > 0 && unsharpAmount > 0) {
    std::vector<double> us = fullTaps(unsharpSigma);
    for (double& v : us) v *= -unsharpAmount;
    us[us.size() / 2] += 1.0 + unsharpAmount;
    kern = convolve(kern, us);
  }
  // box coverage of k pixels (fractional last pixel), phase 0
  const int nb = int(std::ceil(k));
  std::vector<double> box(std::size_t(nb), 1.0 / k);
  box.back() = (k - std::floor(k) > 1e-9 ? k - std::floor(k) : 1.0) / k;
  double s0 = 0;
  for (double v : box) s0 += v * v;
  const std::vector<double> h = convolve(box, kern);
  double s1 = 0;
  for (double v : h) s1 += v * v;
  const double r1 = s1 / s0;
  return std::clamp(r1 * r1, 0.02, 4.0);
}

SpatialPlan makeSpatialPlan(const FilmStock& film, const FilmParams& p, int W, int H, bool printPath, const FilmTables& tables,
                            std::string* err, const GrainFullRes* full) {
  (void)tables;
  SpatialPlan s;
  s.W = W;
  s.H = H;
  const double pix = p.camera.filmFormatMm * 1000.0 / double(std::max(W, H));
  s.pixelUm = pix;
  // --- exposure stage -------------------------------------------------------------------------------------------
  s.boost = p.filmRender.halation.boostEv > 0;
  s.boostM = p.filmRender.halation.boostEv;
  s.boostA = p.filmRender.halation.boostRange;
  s.boostP = p.filmRender.halation.protectEv;
  if (s.boost && !(s.boostA >= 0 && s.boostA <= 1 && s.boostP >= 0)) {
    if (err) *err = "halation.boost_range must be in [0,1] and protect_ev >= 0";
    s.boost = false;
  }
  {
    Mixture cam[3];
    const bool diff = buildDiffusionMixture(p.camera.diffusionFilter, pix, cam);
    const double lens = p.camera.lensBlurUm / pix;
    for (int c = 0; c < 3; ++c) {
      Mixture m = diff ? cam[c] : mixIdentity();
      if (p.camera.lensBlurUm > 0) m = mixConvolve(m, mixGauss(lens));
      mixSimplify(m);
      s.pre[c] = std::move(m);
    }
    s.preMix = diff || p.camera.lensBlurUm > 0;
  }
  {
    const auto& h = p.filmRender.halation;
    if (h.active) {
      for (int c = 0; c < 3; ++c) {
        Mixture m = mixIdentity();
        const double sg = h.scatterCoreUm[c] * h.scatterSpatialScale / pix, lt = h.scatterTailUm[c] * h.scatterSpatialScale / pix;
        if (h.scatterAmount > 0 && (sg > 0 || lt > 0)) {
          const double wt = h.scatterTailWeight[c];
          Mixture sc;
          sc.w0 = 0;
          sc.comps.push_back({std::max(sg, 1e-6), 1.0 - wt});
          for (const auto& cc : mixExpTail(std::max(lt, 1e-6)).comps) sc.comps.push_back({cc.sigma, cc.w * wt});
          m = mixAffine(sc, 1.0 - h.scatterAmount, h.scatterAmount);
        }
        const double a = h.halationStrength[c] * h.halationAmount;
        const double sh = h.halationFirstSigmaUm[c] * h.halationSpatialScale / pix;
        bool any = false;
        for (int cc = 0; cc < 3; ++cc) any |= h.halationStrength[cc] * h.halationAmount > 0;
        bool anySig = false;
        for (int cc = 0; cc < 3; ++cc) anySig |= h.halationFirstSigmaUm[cc] > 0;
        if (h.halationNBounces >= 1 && any && anySig && h.halationSpatialScale > 0) {
          double norm = 0, d = 1;
          for (int k = 1; k <= h.halationNBounces; ++k) {
            norm += d;
            d *= h.halationBounceDecay;
          }
          Mixture hal;
          hal.w0 = h.halationRenormalize ? 1.0 / (1.0 + a) : 1.0;
          const double wnorm = h.halationRenormalize ? 1.0 / (1.0 + a) : 1.0;
          d = 1;
          for (int k = 1; k <= h.halationNBounces; ++k) {
            hal.comps.push_back({std::max(sh * std::sqrt(double(k)), 1e-6), a * (d / norm) * wnorm});
            d *= h.halationBounceDecay;
          }
          m = mixConvolve(m, hal);
        }
        mixSimplify(m);
        s.halo[c] = std::move(m);
        s.haloMix |= !s.halo[c].identity();
      }
    }
  }
  // --- development ------------------------------------------------------------------------------------------------------
  {
    const auto& d = p.filmRender.dirCouplers;
    if (d.active && d.diffusionSizeUm > 0) {
      const double sg = d.diffusionSizeUm / pix, lt = d.diffusionTailUm / pix;
      Mixture m;
      m.w0 = 0;
      m.comps.push_back({sg, 1.0 - d.diffusionTailWeight});
      if (d.diffusionTailWeight > 0)
        for (const auto& cc : mixExpTail(lt).comps) m.comps.push_back({cc.sigma, cc.w * d.diffusionTailWeight});
      mixSimplify(m);
      for (int c = 0; c < 3; ++c) s.dir[c] = m;
      s.dirDiffuse = true;
    }
  }
  // --- grain ----------------------------------------------------------------------------------------------------------------
  {
    const auto& g = p.filmRender.grain;
    GrainPlan& gp = s.grain;
    const int nc = int(film.logExposure.size());
    if (g.active && g.particleAreaUm2 > 0 && nc >= 2 && film.densityCurves.size() == std::size_t(nc) * 3) {
      gp.active = true;
      const double A = pix * pix;
      const bool layers = g.sublayersActive && film.densityCurvesLayers.size() == std::size_t(nc) * 9;
      gp.blurSigma = layers ? (g.blur > 0 ? g.blur : 0) : (g.blur > 0.4 ? g.blur : 0);
      // scale-aware grain: this frame previews a finer render (see the declaration)
      const double kScale = full && full->pixelUm > 0 ? pix / full->pixelUm : 1.0;
      double nDiv = 1.0;  // particle count divisor (variance factor of the box average of the blurred full-resolution grain)
      if (kScale > 1.05) {
        // the scanner stage the full-resolution render applies is part of its grain; a frame that applies its own (final tier)
        // must not count it twice
        const bool ownUs = p.scanner.unsharpMask[0] > 0 && p.scanner.unsharpMask[1] > 0;
        const bool ownScan = p.scanner.lensBlur > 0;
        nDiv = grainBoxFactor(kScale, gp.blurSigma, ownScan ? 0.0 : full->scanBlur, ownUs ? 0.0 : full->unsharpSigma,
                              ownUs ? 0.0 : full->unsharpAmount);
        gp.blurSigma = gp.blurSigma / kScale;
        if (gp.blurSigma < 0.17) gp.blurSigma = 0;  // gaussTaps: radius 0 below this, i.e. identity
      }
      const bool pos = film.positive();
      for (int ch = 0; ch < 3; ++ch) {
        GrainChannelPlan& gc = gp.ch[ch];
        gc.active = true;
        std::vector<double> dn(nc);
        double mn = 1e30, mx = -1e30;
        for (int k = 0; k < nc; ++k)
          if (!std::isnan(film.densityCurves[k * 3 + ch])) mn = std::min(mn, double(film.densityCurves[k * 3 + ch]));
        for (int k = 0; k < nc; ++k) {
          dn[k] = nan0(film.densityCurves[k * 3 + ch]) - mn;
          mx = std::max(mx, dn[k]);
        }
        gc.tableN = 1024;
        gc.invScale = float(gc.tableN / std::max(mx, 1e-6));
        gc.inv.assign(std::size_t(gc.tableN + 2) * 4, 0.f);
        gc.u = float(g.uniformity[ch]);
        gc.dminCh = float(g.densityMin[ch]);
        gc.layers = layers ? 3 : 1;
        if (layers) {
          std::vector<double> lay[3];
          double dmaxL[3], sumMax = 0;
          for (int l = 0; l < 3; ++l) {
            lay[l].resize(nc);
            double m2 = -1e30;
            for (int k = 0; k < nc; ++k) {
              lay[l][k] = nan0(film.densityCurvesLayers[(std::size_t(k) * 3 + l) * 3 + ch]);
              m2 = std::max(m2, lay[l][k]);
            }
            dmaxL[l] = m2;
            sumMax += m2;
          }
          // inverse lookup D -> sub-layer curve value (monotone abscissa; both axes negated for positive stock)
          std::vector<double> xp(nc);
          for (int k = 0; k < nc; ++k) xp[k] = pos ? -dn[k] : dn[k];
          for (int i = 0; i <= gc.tableN + 1; ++i) {
            const double D = double(std::min(i, gc.tableN)) / gc.invScale;
            const double q = pos ? -D : D;
            int hi = int(std::lower_bound(xp.begin(), xp.end(), q) - xp.begin());
            double f;
            int lo;
            if (hi <= 0) {
              lo = 0;
              hi = 0;
              f = 0;
            } else if (hi >= nc) {
              lo = hi = nc - 1;
              f = 0;
            } else {
              lo = hi - 1;
              const double dx = xp[hi] - xp[lo];
              f = dx > 1e-12 ? (q - xp[lo]) / dx : 1.0;
            }
            for (int l = 0; l < 3; ++l) {
              const double fl = pos ? -lay[l][lo] : lay[l][lo], fh = pos ? -lay[l][hi] : lay[l][hi];
              const double v = fl + (fh - fl) * f;
              gc.inv[std::size_t(i) * 4 + l] = float(pos ? -v : v);
            }
          }
          const double layerScale[3] = {g.particleScaleLayers[0], g.particleScaleLayers[1], g.particleScaleLayers[2]};
          for (int l = 0; l < 3; ++l) {
            const double f = sumMax > 0 ? dmaxL[l] / sumMax : 1.0 / 3.0;
            const double dminL = f * g.densityMin[ch];
            const double dmaxP = dmaxL[l] + dminL;
            const double ap = g.particleAreaUm2 * g.particleScale[ch] * layerScale[l];
            const double n = A * f / ap / nDiv;
            gc.dminL[l] = float(dminL);
            gc.invDmaxL[l] = float(1.0 / dmaxP);
            gc.nL[l] = float(n);
            gc.scaleL[l] = float(dmaxP / n);
            gc.dyeSigma[l] = g.blurDyeCloudsUm > 0 ? g.blurDyeCloudsUm * std::sqrt(dmaxP / n) : 0.0;
          }
        } else {
          for (int i = 0; i <= gc.tableN + 1; ++i) gc.inv[std::size_t(i) * 4] = float(double(std::min(i, gc.tableN)) / gc.invScale);
          const double dmaxP = mx + g.densityMin[ch];
          const double n = A / (g.particleAreaUm2 * g.particleScale[ch]) / nDiv;
          gc.dminL[0] = float(g.densityMin[ch]);
          gc.invDmaxL[0] = float(1.0 / dmaxP);
          gc.nL[0] = float(n);
          gc.scaleL[0] = float(dmaxP / n);
          for (int l = 1; l < 3; ++l) {
            gc.dminL[l] = 0;
            gc.invDmaxL[l] = 1;
            gc.nL[l] = 1;
            gc.scaleL[l] = 0;
          }
        }
        // merged sampler table (sum of the layers as one variate): function of the density only
        gc.merged.assign(std::size_t(gc.tableN + 2) * 4, 0.f);
        for (int i = 0; i <= gc.tableN + 1; ++i) {
          double M = 0, V = 0, K3 = 0, Lam = 0;
          for (int l = 0; l < gc.layers; ++l) {
            const double Dl = double(gc.inv[std::size_t(i) * 4 + l]) + gc.dminL[l];
            const double Dmax = 1.0 / gc.invDmaxL[l], n = gc.nL[l];
            const double p = std::clamp(Dl / Dmax, 1e-6, 1.0 - 1e-6);
            const double s = 1.0 - p * gc.u * (1.0 - 1e-6);
            M += Dl;
            V += Dmax * Dl * s / n;
            K3 += p * Dmax * Dmax * Dmax * s * s / (n * n);
            Lam += n * p / s;
          }
          float* t = &gc.merged[std::size_t(i) * 4];
          t[0] = float(M);
          t[1] = float(std::sqrt(V));
          t[2] = V > 0 ? float(K3 / (V * std::sqrt(V))) : 0.f;
          t[3] = float(Lam);
        }
      }
      const double t = g.microStructure[1] * 0.001 / pix;
      if (layers && t > 0.05) {
        gp.microActive = true;
        gp.microStd = t;
        gp.microBlur = g.microStructure[0] / pix;
      }
    }
  }
  // --- print / scan ------------------------------------------------------------------------------------------------------------
  s.enlargerDiffusion = false;
  {
    Mixture e[3];
    if (printPath && buildDiffusionMixture(p.enlarger.diffusionFilter, pix, e)) {
      s.enlargerDiffusion = true;
      for (int c = 0; c < 3; ++c) s.enl[c] = e[c];
    }
  }
  {
    const auto& gl = p.printRender.glare;
    if (printPath && gl.active && gl.percent > 0 && !p.debug.deactivateStochasticEffects) {
      s.glare = true;
      const double r = gl.roughness;
      const double sig2 = std::log(1.0 + r * r);
      s.glareSigma = r > 1e-6 ? std::sqrt(sig2) : 0.0;
      s.glareMu = std::log(gl.percent) - (r > 1e-6 ? sig2 / 2 : 0.0);
      s.glareBlur = gl.blur;
      // scale-aware (see GrainFullRes): the preview's per-pixel glare field gets the variance of the box-averaged full-resolution field
      const double kS = full && full->pixelUm > 0 ? pix / full->pixelUm : 1.0;
      if (kS > 1.05 && r > 1e-6 && gl.blur <= 0) {
        const double f = grainBoxFactor(kS, full->glareBlur, full->scanBlur, full->unsharpSigma, full->unsharpAmount);
        const double sig2n = std::log(1.0 + (std::exp(sig2) - 1.0) * f / (kS * kS));
        s.glareSigma = std::sqrt(sig2n);
        s.glareMu = std::log(gl.percent) - sig2n / 2;
        s.glareBlur = gl.blur / kS;
      }
      s.glareMean = gl.percent / 100.0;
    }
  }
  s.scanBlur = p.scanner.lensBlur;
  if (p.scanner.unsharpMask[0] > 0 && p.scanner.unsharpMask[1] > 0) {
    s.unsharpSigma = p.scanner.unsharpMask[0];
    s.unsharpAmount = p.scanner.unsharpMask[1];
  }
  // Pixel-radius caps (the plan must never ask for more memory than the image can justify): a Gaussian wider than the
  // image is flat over it, so wide mixture components are clamped to the image extent; the tile-path blurs (grain,
  // glare, scanner) carry a halo per tile and are capped at kMaxTileSigma px.
  {
    const double cap = double(std::max(W, H));
    auto clampMix = [&](Mixture& m) {
      for (auto& c : m.comps) c.sigma = std::min(c.sigma, cap);
    };
    for (int c = 0; c < 3; ++c) {
      clampMix(s.pre[c]);
      clampMix(s.halo[c]);
      clampMix(s.dir[c]);
      clampMix(s.enl[c]);
    }
    constexpr double kMaxTileSigma = 64.0;
    auto clampT = [&](double& v) { v = std::min(v, kMaxTileSigma); };
    clampT(s.glareBlur);
    clampT(s.scanBlur);
    clampT(s.unsharpSigma);
    clampT(s.grain.blurSigma);
    clampT(s.grain.microBlur);
    for (auto& ch : s.grain.ch)
      for (double& d : ch.dyeSigma) clampT(d);
  }
  return s;
}

}  // namespace dapple::film::detail
