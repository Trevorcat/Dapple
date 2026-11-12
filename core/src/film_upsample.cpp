// Own spectral-upsampling table (chromaticity -> smooth bounded emission spectrum).
//
// Model (Jakob & Hanika 2019 style sigmoid-polynomial): s(l) = 1/2 + 1/2 p / sqrt(p^2 + 1),
// p = a0 + a1 u + a2 u^2, u = (l - 580 nm) / 200 nm. A chromaticity (2 numbers) leaves a one-parameter
// family of coefficient triples; for every node of the triangle-coordinate (tc) grid we pick the
// minimum-norm member (the smoothest spectrum) by a projected Gauss-Newton iteration that satisfies the
// chromaticity exactly and descends |a| inside the solution manifold, continuing outward from the
// equal-energy white (a = 0). Nodes outside the attainable region fall back to a damped least-squares
// compromise. The spectrum is later normalised to unit X+Y+Z so that the film exposure of a pixel is
// E_m = (X+Y+Z) * sum_l T(l) S_m(l) with exactly the pixel's own brightness.
// Clean-room: no data from the reference engine is used, only the published method.
#include <algorithm>
#include <cmath>
#include <numeric>

#include "film_internal.h"

namespace dapple::film::detail {

void spectrumFromCoeffs(const double a[3], Spec& s) {
  for (int i = 0; i < kN; ++i) {
    const double u = (wavelength(i) - 580.0) / 200.0;
    const double p = a[0] + (a[1] + a[2] * u) * u;
    s[i] = 0.5 + 0.5 * p / std::sqrt(p * p + 1.0);
  }
}

namespace {

struct Fitter {
  double u1[kN], u2[kN];
  double cx[kN], cy[kN], cz[kN];
  Fitter() {
    for (int i = 0; i < kN; ++i) {
      u1[i] = (wavelength(i) - 580.0) / 200.0;
      u2[i] = u1[i] * u1[i];
      cx[i] = cmf(0)[i];
      cy[i] = cmf(1)[i];
      cz[i] = cmf(2)[i];
    }
  }

  // Residuals (x - xt, y - yt) and the 2x3 Jacobian.
  void eval(const double a[3], double xt, double yt, double r[2], double J[2][3]) const {
    double X = 0, Y = 0, Z = 0, dX[3] = {0, 0, 0}, dY[3] = {0, 0, 0}, dS[3] = {0, 0, 0};
    for (int i = 0; i < kN; ++i) {
      const double p = a[0] + (a[1] + a[2] * u1[i]) * u1[i];
      const double q = std::sqrt(p * p + 1.0);
      const double s = 0.5 + 0.5 * p / q;
      const double ds = 0.5 / (q * q * q);
      X += s * cx[i];
      Y += s * cy[i];
      Z += s * cz[i];
      const double w[3] = {1.0, u1[i], u2[i]};
      for (int k = 0; k < 3; ++k) {
        const double d = ds * w[k];
        dX[k] += d * cx[i];
        dY[k] += d * cy[i];
        dS[k] += d * (cx[i] + cy[i] + cz[i]);
      }
    }
    const double S = X + Y + Z;
    const double x = X / S, y = Y / S;
    r[0] = x - xt;
    r[1] = y - yt;
    for (int k = 0; k < 3; ++k) {
      J[0][k] = (dX[k] - x * dS[k]) / S;
      J[1][k] = (dY[k] - y * dS[k]) / S;
    }
  }

  static constexpr double kMax = 500.0;

  // Newton corrector: minimum-norm steps onto the manifold { chromaticity = target }.
  double correct(double xt, double yt, double a[3]) const {
    double r[2], J[2][3];
    for (int it = 0; it < 30; ++it) {
      eval(a, xt, yt, r, J);
      const double rn = std::hypot(r[0], r[1]);
      if (rn < 1e-13) return rn;
      const double g00 = J[0][0] * J[0][0] + J[0][1] * J[0][1] + J[0][2] * J[0][2];
      const double g01 = J[0][0] * J[1][0] + J[0][1] * J[1][1] + J[0][2] * J[1][2];
      const double g11 = J[1][0] * J[1][0] + J[1][1] * J[1][1] + J[1][2] * J[1][2];
      const double det = g00 * g11 - g01 * g01;
      if (!(det > 1e-24 * (g00 * g11 + 1e-300))) return rn;
      const double y0 = (g11 * r[0] - g01 * r[1]) / det, y1 = (-g01 * r[0] + g00 * r[1]) / det;
      double t[3];
      for (int k = 0; k < 3; ++k) t[k] = a[k] - (J[0][k] * y0 + J[1][k] * y1);
      double rt[2], Jt[2][3];
      eval(t, xt, yt, rt, Jt);
      if (std::hypot(rt[0], rt[1]) >= rn) {  // damp
        for (int k = 0; k < 3; ++k) t[k] = a[k] - 0.5 * (J[0][k] * y0 + J[1][k] * y1);
      }
      for (int k = 0; k < 3; ++k) a[k] = std::clamp(t[k], -kMax, kMax);
    }
    eval(a, xt, yt, r, J);
    return std::hypot(r[0], r[1]);
  }

  // Minimum-norm solution of the 2 chromaticity equations near the start point `a` (predictor-corrector
  // descent of |a|^2 on the solution manifold). Returns the final residual norm.
  double fitExact(double xt, double yt, double a[3]) const {
    double err = correct(xt, yt, a);
    if (err > 1e-12) return err;
    double tau = 0.5;
    for (int it = 0; it < 200; ++it) {
      double r[2], J[2][3];
      eval(a, xt, yt, r, J);
      const double g00 = J[0][0] * J[0][0] + J[0][1] * J[0][1] + J[0][2] * J[0][2];
      const double g01 = J[0][0] * J[1][0] + J[0][1] * J[1][1] + J[0][2] * J[1][2];
      const double g11 = J[1][0] * J[1][0] + J[1][1] * J[1][1] + J[1][2] * J[1][2];
      const double det = g00 * g11 - g01 * g01;
      if (!(det > 1e-24 * (g00 * g11 + 1e-300))) break;
      const double Ja0 = J[0][0] * a[0] + J[0][1] * a[1] + J[0][2] * a[2];
      const double Ja1 = J[1][0] * a[0] + J[1][1] * a[1] + J[1][2] * a[2];
      const double z0 = (g11 * Ja0 - g01 * Ja1) / det, z1 = (-g01 * Ja0 + g00 * Ja1) / det;
      double w[3], wn = 0, an = 0;
      for (int k = 0; k < 3; ++k) {
        w[k] = -(a[k] - (J[0][k] * z0 + J[1][k] * z1));
        wn += w[k] * w[k];
        an += a[k] * a[k];
      }
      wn = std::sqrt(wn);
      if (wn < 1e-6 * (1.0 + std::sqrt(an))) break;
      bool ok = false;
      for (int bt = 0; bt < 20; ++bt) {
        const double s = std::min(tau, 0.25 * (1.0 + std::sqrt(an)) / wn);
        double t[3];
        for (int k = 0; k < 3; ++k) t[k] = a[k] + s * w[k];
        const double e = correct(xt, yt, t);
        double tn = 0;
        for (int k = 0; k < 3; ++k) tn += t[k] * t[k];
        if (e < 1e-12 && tn < an) {
          for (int k = 0; k < 3; ++k) a[k] = t[k];
          ok = true;
          tau = std::min(tau * 1.5, 1.0);
          break;
        }
        tau *= 0.5;
      }
      if (!ok) break;
    }
    return correct(xt, yt, a);
  }

  // Damped least squares with a small norm penalty: used where the chromaticity is (nearly) outside the
  // region the 3-coefficient family can reach.
  double fitLeastSquares(double xt, double yt, double a[3]) const {
    constexpr double eps = 1e-4;
    double lambda = 1e-3;
    double r[2], J[2][3];
    eval(a, xt, yt, r, J);
    auto cost = [&](const double rr[2], const double aa[3]) {
      return rr[0] * rr[0] + rr[1] * rr[1] + eps * eps * (aa[0] * aa[0] + aa[1] * aa[1] + aa[2] * aa[2]);
    };
    double c = cost(r, a);
    for (int it = 0; it < 100; ++it) {
      double H[3][3] = {}, g[3] = {};
      for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) H[i][j] = J[0][i] * J[0][j] + J[1][i] * J[1][j];
        H[i][i] += eps * eps;
        g[i] = J[0][i] * r[0] + J[1][i] * r[1] + eps * eps * a[i];
      }
      bool improved = false;
      for (int tries = 0; tries < 14; ++tries) {
        double M[3][3];
        for (int i = 0; i < 3; ++i)
          for (int j = 0; j < 3; ++j) M[i][j] = H[i][j] + (i == j ? lambda * (H[i][i] + 1e-12) : 0.0);
        const double det = M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) -
                           M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
                           M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
        if (std::fabs(det) < 1e-300) {
          lambda *= 10;
          continue;
        }
        double d[3];
        for (int col = 0; col < 3; ++col) {
          double T[3][3];
          for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) T[i][j] = (j == col) ? -g[i] : M[i][j];
          d[col] = (T[0][0] * (T[1][1] * T[2][2] - T[1][2] * T[2][1]) - T[0][1] * (T[1][0] * T[2][2] - T[1][2] * T[2][0]) +
                    T[0][2] * (T[1][0] * T[2][1] - T[1][1] * T[2][0])) /
                   det;
        }
        double an[3];
        for (int i = 0; i < 3; ++i) an[i] = std::clamp(a[i] + d[i], -kMax, kMax);
        double rn[2], Jn[2][3];
        eval(an, xt, yt, rn, Jn);
        const double cn = cost(rn, an);
        if (cn < c) {
          for (int i = 0; i < 3; ++i) a[i] = an[i];
          r[0] = rn[0];
          r[1] = rn[1];
          std::copy(&Jn[0][0], &Jn[0][0] + 6, &J[0][0]);
          improved = c - cn > 1e-24;
          c = cn;
          lambda = std::max(lambda * 0.3, 1e-12);
          break;
        }
        lambda *= 5;
      }
      if (!improved) break;
    }
    return std::hypot(r[0], r[1]);
  }

  // (x0, y0): chromaticity that the start point `a` solves (the already solved neighbour).
  double fit(double xt, double yt, double a[3], double x0, double y0) const {
    double t[3] = {a[0], a[1], a[2]};
    double e = fitExact(xt, yt, t);
    if (e >= 1e-10) {
      // homotopy: walk the solution manifold from the neighbour to the target in small chromaticity steps
      for (int steps = 8; steps <= 256 && e >= 1e-10; steps *= 4) {
        double h[3] = {a[0], a[1], a[2]};
        bool ok = true;
        for (int s = 1; s <= steps && ok; ++s) {
          const double f = double(s) / steps;
          ok = correct(x0 + (xt - x0) * f, y0 + (yt - y0) * f, h) < 1e-12;
        }
        if (ok) {
          const double e2 = fitExact(xt, yt, h);
          if (e2 < e) {
            e = e2;
            std::copy(h, h + 3, t);
          }
        }
      }
    }
    if (e < 1e-10) {
      std::copy(t, t + 3, a);
      return e;
    }
    // retry from the least-squares compromise, then polish
    double u[3] = {a[0], a[1], a[2]};
    const double e2 = fitLeastSquares(xt, yt, u);
    if (e2 < 1e-5) {
      const double e3 = fitExact(xt, yt, u);
      if (e3 < e) {
        std::copy(u, u + 3, a);
        return e3;
      }
    }
    if (e2 < e) {
      std::copy(u, u + 3, a);
      return e2;
    }
    std::copy(t, t + 3, a);
    return e;
  }
};

}  // namespace

UpsampleTable generateUpsampleTable(int n) {
  UpsampleTable t;
  t.n = n;
  t.coef.assign(std::size_t(n) * n * 4, 0.f);
  const Fitter fit;
  std::vector<double> xs(std::size_t(n) * n), ys(xs.size()), dist(xs.size());
  for (int j = 0; j < n; ++j)
    for (int i = 0; i < n; ++i) {
      double x, y;
      tcToXy(double(i) / (n - 1), double(j) / (n - 1), x, y);
      const std::size_t k = std::size_t(j) * n + i;
      xs[k] = x;
      ys[k] = y;
      dist[k] = std::hypot(x - 1.0 / 3.0, y - 1.0 / 3.0);
    }
  std::vector<int> order(xs.size());
  std::iota(order.begin(), order.end(), 0);
  std::sort(order.begin(), order.end(), [&](int a, int b) { return dist[a] < dist[b]; });
  std::vector<double> sol(xs.size() * 3, 0.0);
  std::vector<char> done(xs.size(), 0);
  for (int k : order) {
    const int i = k % n, j = k / n;
    double a[3] = {0, 0, 0};
    int best = -1;
    for (int dj = -1; dj <= 1; ++dj)
      for (int di = -1; di <= 1; ++di) {
        const int ii = i + di, jj = j + dj;
        if ((di == 0 && dj == 0) || ii < 0 || jj < 0 || ii >= n || jj >= n) continue;
        const int kk = jj * n + ii;
        if (done[kk] && (best < 0 || dist[kk] < dist[best])) best = kk;
      }
    if (best >= 0)
      for (int c = 0; c < 3; ++c) a[c] = sol[std::size_t(best) * 3 + c];
    fit.fit(xs[k], ys[k], a, best >= 0 ? xs[best] : 1.0 / 3.0, best >= 0 ? ys[best] : 1.0 / 3.0);
    for (int c = 0; c < 3; ++c) sol[std::size_t(k) * 3 + c] = a[c];
    done[k] = 1;
    Spec s;
    spectrumFromCoeffs(a, s);
    double sum = 0;
    for (int l = 0; l < kN; ++l) sum += s[l] * (cmf(0)[l] + cmf(1)[l] + cmf(2)[l]);
    float* o = &t.coef[std::size_t(k) * 4];
    o[0] = float(a[0]);
    o[1] = float(a[1]);
    o[2] = float(a[2]);
    o[3] = float(1.0 / sum);
  }
  return t;
}

}  // namespace dapple::film::detail
