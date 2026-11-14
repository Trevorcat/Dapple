// The structure-aware continuous field: photo analysis and the sparse solve (warp_structure.h).
#include "warp_structure.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <list>
#include <mutex>

namespace dapple::warp {

namespace {

inline double sstepd(double a, double b, double x) {
  const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
  return t * t * (3 - 2 * t);
}

// weights (relative; the smoothness of a textured cell is 1)
constexpr double kSmoothTextured = 1.0, kSmoothPlain = 0.2;  // membrane weight on texture / on plain background
constexpr double kStructure = 400.0;                          // across-edge displacement must not change along the edge
constexpr double kKeep = 0.08;                               // strong edges prefer to stay put (relative to the smoothness over one decay length)
constexpr double kIdentity = 9.0;                            // decay length = decay / 3 on texture

std::mutex g_mu;
std::list<std::shared_ptr<const StructureGrid>> g_cache;

void blurSep(std::vector<float>& a, int w, int h, double sigma) {
  const int R = std::max(1, int(std::ceil(3 * sigma)));
  std::vector<float> k(2 * R + 1);
  double s = 0;
  for (int i = -R; i <= R; ++i) s += (k[i + R] = float(std::exp(-0.5 * i * i / (sigma * sigma))));
  for (float& v : k) v = float(v / s);
  std::vector<float> t(a.size());
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      float acc = 0;
      for (int i = -R; i <= R; ++i) acc += k[i + R] * a[std::size_t(y) * w + std::clamp(x + i, 0, w - 1)];
      t[std::size_t(y) * w + x] = acc;
    }
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      float acc = 0;
      for (int i = -R; i <= R; ++i) acc += k[i + R] * t[std::size_t(std::clamp(y + i, 0, h - 1)) * w + x];
      a[std::size_t(y) * w + x] = acc;
    }
}

}  // namespace

void structureCacheClear() {
  std::lock_guard<std::mutex> lk(g_mu);
  g_cache.clear();
}

std::shared_ptr<const StructureGrid> structureAnalyse(const std::uint8_t* luma, int lw, int lh, std::uint64_t hash, int gw, int gh, bool* hit) {
  const std::uint64_t key = (hash * 1099511628211ull) ^ (std::uint64_t(gw) << 20) ^ std::uint64_t(gh) ^ (std::uint64_t(lw) << 40) ^ (std::uint64_t(lh) << 52);
  {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto it = g_cache.begin(); it != g_cache.end(); ++it)
      if ((*it)->key == key) {
        auto r = *it;
        g_cache.erase(it);
        g_cache.push_front(r);
        if (hit) *hit = true;
        return r;
      }
  }
  const auto t0 = std::chrono::steady_clock::now();
  auto out = std::make_shared<StructureGrid>();
  out->w = gw, out->h = gh, out->key = key;
  const std::size_t N = std::size_t(gw) * gh;
  out->nx.assign(N, 1.f), out->ny.assign(N, 0.f), out->c.assign(N, 0.f), out->tex.assign(N, 0.f);
  if (luma && lw >= 8 && lh >= 8 && gw >= 4 && gh >= 4) {
    const double Lan = std::max(lw, lh);
    // Sobel gradients per analysis pixel, normalised to a 512 px long edge (so the thresholds mean the same at any analysis size)
    const float gs = float(Lan / 512.0) / 8.f;
    std::vector<float> jxx(std::size_t(lw) * lh), jxy(jxx.size()), jyy(jxx.size());
    auto L = [&](int x, int y) { return float(luma[std::size_t(std::clamp(y, 0, lh - 1)) * lw + std::clamp(x, 0, lw - 1)]); };
    for (int y = 0; y < lh; ++y)
      for (int x = 0; x < lw; ++x) {
        const float gx = (L(x + 1, y - 1) + 2 * L(x + 1, y) + L(x + 1, y + 1) - L(x - 1, y - 1) - 2 * L(x - 1, y) - L(x - 1, y + 1)) * gs;
        const float gy = (L(x - 1, y + 1) + 2 * L(x, y + 1) + L(x + 1, y + 1) - L(x - 1, y - 1) - 2 * L(x, y - 1) - L(x + 1, y - 1)) * gs;
        const std::size_t i = std::size_t(y) * lw + x;
        jxx[i] = gx * gx, jxy[i] = gx * gy, jyy[i] = gy * gy;
      }
    const double sigma = std::max(1.0, 0.005 * Lan);  // 0.005 of the long edge: about one cell of the solve grid
    blurSep(jxx, lw, lh, sigma), blurSep(jxy, lw, lh, sigma), blurSep(jyy, lw, lh, sigma);
    std::vector<float> tex(N, 0.f);
    for (int j = 1; j < gh - 1; ++j)
      for (int i = 1; i < gw - 1; ++i) {
        // the node's cell (box) in the analysis plane, averaged
        const double x0 = double(i - 0.5) / (gw - 1) * lw, x1 = double(i + 0.5) / (gw - 1) * lw, y0 = double(j - 0.5) / (gh - 1) * lh, y1 = double(j + 0.5) / (gh - 1) * lh;
        const int ia = std::clamp(int(x0), 0, lw - 1), ib = std::clamp(int(std::ceil(x1)), ia + 1, lw), ja = std::clamp(int(y0), 0, lh - 1), jb = std::clamp(int(std::ceil(y1)), ja + 1, lh);
        double axx = 0, axy = 0, ayy = 0;
        int n = 0;
        for (int y = ja; y < jb; ++y)
          for (int x = ia; x < ib; ++x) {
            const std::size_t q = std::size_t(y) * lw + x;
            axx += jxx[q], axy += jxy[q], ayy += jyy[q], ++n;
          }
        axx /= n, axy /= n, ayy /= n;
        const double tr = axx + ayy, df = axx - ayy, rt = std::sqrt(df * df + 4 * axy * axy);
        const double l1 = 0.5 * (tr + rt), l2 = 0.5 * (tr - rt);
        const double th = 0.5 * std::atan2(2 * axy, df);  // direction of the dominant gradient (the edge normal)
        const std::size_t k = std::size_t(j) * gw + i;
        out->nx[k] = float(std::cos(th)), out->ny[k] = float(std::sin(th));
        const double strength = sstepd(3.0, 10.0, std::sqrt(std::max(l1, 0.0)));
        const double mu = (l1 - l2) / (l1 + l2 + 1e-6);
        out->c[k] = float(strength * sstepd(0.35, 0.75, mu));
        tex[k] = float(sstepd(2.0, 6.0, std::sqrt(std::max(tr, 0.0))));
      }
    // texture: a 3 x 3 maximum then a 3 x 3 mean (so a thin line / a patch of texture also stiffens its surroundings)
    std::vector<float> t2(N, 0.f);
    for (int j = 0; j < gh; ++j)
      for (int i = 0; i < gw; ++i) {
        float m = 0;
        for (int dj = -1; dj <= 1; ++dj)
          for (int di = -1; di <= 1; ++di) m = std::max(m, tex[std::size_t(std::clamp(j + dj, 0, gh - 1)) * gw + std::clamp(i + di, 0, gw - 1)]);
        t2[std::size_t(j) * gw + i] = m;
      }
    for (int j = 0; j < gh; ++j)
      for (int i = 0; i < gw; ++i) {
        float s = 0;
        for (int dj = -1; dj <= 1; ++dj)
          for (int di = -1; di <= 1; ++di) s += t2[std::size_t(std::clamp(j + dj, 0, gh - 1)) * gw + std::clamp(i + di, 0, gw - 1)];
        out->tex[std::size_t(j) * gw + i] = s / 9.f;
      }
  }
  out->ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_cache.push_front(out);
    while (g_cache.size() > 6) g_cache.pop_back();
  }
  return out;
}

std::vector<double> structureDistance(const std::vector<char>& marked, int w, int h, double hx, double hy) {
  std::vector<double> d(std::size_t(w) * h, 1e9);
  for (std::size_t i = 0; i < d.size(); ++i)
    if (marked[i]) d[i] = 0;
  const double dd = std::hypot(hx, hy);
  for (int j = 0; j < h; ++j)
    for (int i = 0; i < w; ++i) {
      double& v = d[std::size_t(j) * w + i];
      if (i > 0) v = std::min(v, d[std::size_t(j) * w + i - 1] + hx);
      if (j > 0) {
        v = std::min(v, d[std::size_t(j - 1) * w + i] + hy);
        if (i > 0) v = std::min(v, d[std::size_t(j - 1) * w + i - 1] + dd);
        if (i < w - 1) v = std::min(v, d[std::size_t(j - 1) * w + i + 1] + dd);
      }
    }
  for (int j = h - 1; j >= 0; --j)
    for (int i = w - 1; i >= 0; --i) {
      double& v = d[std::size_t(j) * w + i];
      if (i < w - 1) v = std::min(v, d[std::size_t(j) * w + i + 1] + hx);
      if (j < h - 1) {
        v = std::min(v, d[std::size_t(j + 1) * w + i] + hy);
        if (i < w - 1) v = std::min(v, d[std::size_t(j + 1) * w + i + 1] + dd);
        if (i > 0) v = std::min(v, d[std::size_t(j + 1) * w + i - 1] + dd);
      }
    }
  return d;
}

StructureSolveOut structureSolve(const StructureSolveIn& in, std::vector<double>& x) {
  const auto t0 = std::chrono::steady_clock::now();
  StructureSolveOut res;
  const int w = in.w, h = in.h;
  const std::size_t N = std::size_t(w) * h;
  const StructureGrid* S = in.S && in.S->w == w && in.S->h == h ? in.S : nullptr;
  const double hm = 0.5 * (in.hx + in.hy), q = (hm / in.decay) * (hm / in.decay);
  std::vector<double> ws(N), cw(N, 0.0), kp(N, 0.0), dd(N), wx(N, 0.0), wy(N, 0.0), tx(N, 0.0), ty(N, 0.0), nx(N, 1.0), ny(N, 0.0);
  for (std::size_t k = 0; k < N; ++k) {
    const double tex = S ? S->tex[k] : 1.0, c = S ? S->c[k] : 0.0;
    ws[k] = (kSmoothPlain + (kSmoothTextured - kSmoothPlain) * tex) * (in.stiff ? (*in.stiff)[k] : 1.0);
    cw[k] = kStructure * c;
    kp[k] = kKeep * c * q * kSmoothTextured;
    const double s = std::clamp((*in.dist)[k] / in.decay, 0.0, 1.0);
    dd[k] = (*in.wd)[k] + kIdentity * kSmoothTextured * q * s;
    if (S) nx[k] = S->nx[k], ny[k] = S->ny[k], tx[k] = -ny[k], ty[k] = nx[k];
  }
  for (int j = 0; j < h; ++j)
    for (int i = 0; i < w; ++i) {
      const std::size_t k = std::size_t(j) * w + i;
      if (i + 1 < w) wx[k] = 0.5 * (ws[k] + ws[k + 1]);
      if (j + 1 < h) wy[k] = 0.5 * (ws[k] + ws[k + w]);
    }
  // block diagonal (and its inverse) for the preconditioner
  std::vector<double> A00(N), A01(N), A11(N);
  for (int j = 0; j < h; ++j)
    for (int i = 0; i < w; ++i) {
      const std::size_t k = std::size_t(j) * w + i;
      double a = dd[k] + kp[k] * nx[k] * nx[k], b = kp[k] * nx[k] * ny[k], c = dd[k] + kp[k] * ny[k] * ny[k];
      const double mem = (i > 0 ? wx[k - 1] : 0.0) + wx[k] + (j > 0 ? wy[k - w] : 0.0) + wy[k];
      a += mem, c += mem;
      // the structure terms of the four neighbours that read this node
      auto add = [&](std::size_t m, double coef) {
        if (cw[m] <= 0) return;
        a += cw[m] * coef * nx[m] * nx[m], b += cw[m] * coef * nx[m] * ny[m], c += cw[m] * coef * ny[m] * ny[m];
      };
      if (i > 0) add(k - 1, 0.25 * tx[k - 1] * tx[k - 1]);
      if (i + 1 < w) add(k + 1, 0.25 * tx[k + 1] * tx[k + 1]);
      if (j > 0) add(k - w, 0.25 * ty[k - w] * ty[k - w]);
      if (j + 1 < h) add(k + w, 0.25 * ty[k + w] * ty[k + w]);
      const double det = std::max(a * c - b * b, 1e-30);
      A00[k] = c / det, A01[k] = -b / det, A11[k] = a / det;
    }
  std::vector<double> r(N * 2), z(N * 2), p(N * 2), Ap(N * 2), sres(N, 0.0), rhs(N * 2);
  auto matvec = [&](const std::vector<double>& v, std::vector<double>& y) {
    for (int j = 0; j < h; ++j)
      for (int i = 0; i < w; ++i) {
        const std::size_t k = std::size_t(j) * w + i;
        const double vx = v[2 * k], vy = v[2 * k + 1];
        const double nv = nx[k] * vx + ny[k] * vy;
        double ax = dd[k] * vx + kp[k] * nx[k] * nv, ay = dd[k] * vy + kp[k] * ny[k] * nv;
        if (i + 1 < w) ax += wx[k] * (vx - v[2 * k + 2]), ay += wx[k] * (vy - v[2 * k + 3]);
        if (i > 0) ax += wx[k - 1] * (vx - v[2 * k - 2]), ay += wx[k - 1] * (vy - v[2 * k - 1]);
        if (j + 1 < h) ax += wy[k] * (vx - v[2 * (k + w)]), ay += wy[k] * (vy - v[2 * (k + w) + 1]);
        if (j > 0) ax += wy[k - w] * (vx - v[2 * (k - w)]), ay += wy[k - w] * (vy - v[2 * (k - w) + 1]);
        y[2 * k] = ax, y[2 * k + 1] = ay;
        // residual of the structure term at this node: r = 0.5 (tx n.(F_r - F_l) + ty n.(F_d - F_u))
        if (cw[k] > 0 && i > 0 && j > 0 && i + 1 < w && j + 1 < h) {
          const double dx = nx[k] * (v[2 * k + 2] - v[2 * k - 2]) + ny[k] * (v[2 * k + 3] - v[2 * k - 1]);
          const double dy = nx[k] * (v[2 * (k + w)] - v[2 * (k - w)]) + ny[k] * (v[2 * (k + w) + 1] - v[2 * (k - w) + 1]);
          sres[k] = cw[k] * 0.5 * (tx[k] * dx + ty[k] * dy);
        } else {
          sres[k] = 0;
        }
      }
    for (int j = 1; j + 1 < h; ++j)
      for (int i = 1; i + 1 < w; ++i) {
        const std::size_t k = std::size_t(j) * w + i;
        const double s = sres[k];
        if (s == 0) continue;
        const double ex = 0.5 * tx[k] * s, ey = 0.5 * ty[k] * s;
        y[2 * (k + 1)] += ex * nx[k], y[2 * (k + 1) + 1] += ex * ny[k];
        y[2 * (k - 1)] -= ex * nx[k], y[2 * (k - 1) + 1] -= ex * ny[k];
        y[2 * (k + w)] += ey * nx[k], y[2 * (k + w) + 1] += ey * ny[k];
        y[2 * (k - w)] -= ey * nx[k], y[2 * (k - w) + 1] -= ey * ny[k];
      }
  };
  auto precond = [&](const std::vector<double>& a, std::vector<double>& o) {
    for (std::size_t k = 0; k < N; ++k) {
      o[2 * k] = A00[k] * a[2 * k] + A01[k] * a[2 * k + 1];
      o[2 * k + 1] = A01[k] * a[2 * k] + A11[k] * a[2 * k + 1];
    }
  };
  for (std::size_t k = 0; k < N; ++k) rhs[2 * k] = (*in.wd)[k] * (*in.T)[2 * k], rhs[2 * k + 1] = (*in.wd)[k] * (*in.T)[2 * k + 1];
  if (x.size() != 2 * N) x.assign(2 * N, 0.0);
  matvec(x, Ap);
  double r0 = 0;
  for (std::size_t k = 0; k < 2 * N; ++k) r[k] = rhs[k] - Ap[k], r0 += r[k] * r[k];
  double rr = r0;
  if (r0 > 0) {
    precond(r, z);
    p = z;
    double rz = 0;
    for (std::size_t k = 0; k < 2 * N; ++k) rz += r[k] * z[k];
    for (res.iters = 0; res.iters < in.maxIter; ++res.iters) {
      if (rr <= 1e-6 * r0) break;
      matvec(p, Ap);
      double pAp = 0;
      for (std::size_t k = 0; k < 2 * N; ++k) pAp += p[k] * Ap[k];
      if (pAp <= 0) break;
      const double alpha = rz / pAp;
      rr = 0;
      for (std::size_t k = 0; k < 2 * N; ++k) x[k] += alpha * p[k], r[k] -= alpha * Ap[k], rr += r[k] * r[k];
      precond(r, z);
      double rz2 = 0;
      for (std::size_t k = 0; k < 2 * N; ++k) rz2 += r[k] * z[k];
      const double beta = rz2 / std::max(rz, 1e-300);
      rz = rz2;
      for (std::size_t k = 0; k < 2 * N; ++k) p[k] = z[k] + beta * p[k];
    }
  }
  res.rel = r0 > 0 ? std::sqrt(rr / r0) : 0;
  res.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  return res;
}

}  // namespace dapple::warp
