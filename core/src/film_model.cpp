// Double-precision film model: filming exposure (raw LUT), density curves + DIR couplers, enlarger,
// paper, scan and output gamut compression, plus the bake of all parameter-only work into the float
// tables used by the run kernels. This file is also the test oracle ("direct evaluation").
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "film_internal.h"
#include "parallel.h"

namespace dapple::film::detail {

namespace {

constexpr double kLog2_10 = 3.32192809488736234787;
inline double pow10neg(double d) { return std::exp2(-d * kLog2_10); }
inline double nan0(float v) { return std::isnan(v) ? 0.0 : double(v); }

// Mitchell-Netravali B = C = 1/3 weights for taps ia-1 .. ia+2.
inline double mitK0(double t) { return ((7.0 * t - 12.0) * t * t + 16.0 / 3.0) / 6.0; }  // |t| < 1
inline double mitK1(double t) { return (((-7.0 / 3.0 * t + 12.0) * t - 20.0) * t + 32.0 / 3.0) / 6.0; }  // 1<=|t|<2
inline void mitchell(double f, double w[4]) {
  w[0] = mitK1(f + 1.0);
  w[1] = mitK0(f);
  w[2] = mitK0(1.0 - f);
  w[3] = mitK1(2.0 - f);
}

double knee(double x, double t, double l, double p) {
  if (x <= t) return x;
  const double s = l - t, n = (x - t) / s;
  return t + s * n / std::pow(1.0 + std::pow(n, p), 1.0 / p);
}

// Spectral locus polygon (monochromatic stimuli 380..700 nm) for the input gamut compression.
struct Locus {
  double x[66], y[66];
  int n = 0;
  Locus() {
    for (int i = 0; i <= 64; ++i) {  // 380 .. 700
      const double s = cmf(0)[i] + cmf(1)[i] + cmf(2)[i];
      x[i] = cmf(0)[i] / s;
      y[i] = cmf(1)[i] / s;
    }
    x[65] = x[0];
    y[65] = y[0];
    n = 65;
  }
  // Distance from w along unit direction u to the polygon.
  double distance(double wx, double wy, double ux, double uy) const {
    double best = 1e30;
    for (int k = 0; k < n; ++k) {
      const double ex = x[k + 1] - x[k], ey = y[k + 1] - y[k];
      const double det = ux * (-ey) - uy * (-ex);
      if (std::fabs(det) < 1e-18) continue;
      const double rx = x[k] - wx, ry = y[k] - wy;
      const double t = (rx * (-ey) - ry * (-ex)) / det;
      const double s = (ux * ry - uy * rx) / det;
      if (t > 1e-9 && s >= 0 && s <= 1 && t < best) best = t;
    }
    return best;
  }
};

}  // namespace

// ---- raw LUT ---------------------------------------------------------------------------------------------


std::shared_ptr<RawLut> bakeRawLut(const FilmStock& film, const UpsampleTable& up, const RawLutParams& rp,
                                   const Spec& iRef, double wx, double wy, const Mat3& mIn) {
  auto lut = std::make_shared<RawLut>();
  const int n = up.n;
  lut->n = n;
  lut->node.assign(std::size_t(n) * n * 4, 0.f);

  // Sensitivities S(l,m), optionally through the camera UV / IR band-pass filters.
  Spec S[3], Sf[3];
  for (int m = 0; m < 3; ++m)
    for (int l = 0; l < kN; ++l) S[m][l] = Sf[m][l] = std::exp2(nan0(film.logSensitivity[l * 3 + m]) * kLog2_10);
  if (rp.uv[0] > 0 || rp.ir[0] > 0) {
    const double au = std::clamp(rp.uv[0], 0.0, 1.0), ai = std::clamp(rp.ir[0], 0.0, 1.0);
    Spec F;
    for (int l = 0; l < kN; ++l) {
      const double wl = wavelength(l);
      F[l] = (1 - au + au * (0.5 + 0.5 * std::erf((wl - rp.uv[1]) / rp.uv[2]))) *
             (1 - ai + ai * (0.5 + 0.5 * std::erf((wl - rp.ir[1]) / (-rp.ir[2]))));
    }
    for (int m = 0; m < 3; ++m) {
      double num = 0, den = 0;
      for (int l = 0; l < kN; ++l) {
        num += S[m][l] * F[l] * iRef[l];
        den += S[m][l] * iRef[l];
      }
      for (int l = 0; l < kN; ++l) Sf[m][l] = S[m][l] * F[l] / (num / den);
    }
  }

  // Own white balance: the reference table is built so that a neutral 18.4 % grey integrates to
  // exposure 1.0 in all three layers with the window disabled; our table differs, so we balance it here
  // (per film, per layer) against our own white spectrum.
  // The grey is evaluated through exactly the run-time lookup (Mitchell kernel over the 16 nodes around the
  // film white), so the balance holds for what the kernels actually sample.
  const Vec3 wxyz = mul(mIn, Vec3{0.184, 0.184, 0.184});
  const double bGrey = wxyz[0] + wxyz[1] + wxyz[2];
  double fm[3];
  {
    double tc0, tc1;
    xyToTc(wx, wy, tc0, tc1);
    const double a = tc0 * (n - 1), c = tc1 * (n - 1);
    const int ia = std::min(int(a), n - 2), ic = std::min(int(c), n - 2);
    double wa[4], wc[4];
    mitchell(a - ia, wa);
    mitchell(c - ic, wc);
    auto refl = [n](int k) { return k < 0 ? -k : (k >= n ? 2 * (n - 1) - k : k); };
    double rw[3] = {0, 0, 0};
    for (int dj = 0; dj < 4; ++dj)
      for (int di = 0; di < 4; ++di) {
        const float* cf = &up.coef[(std::size_t(refl(ic - 1 + dj)) * n + refl(ia - 1 + di)) * 4];
        const double co[3] = {cf[0], cf[1], cf[2]};
        Spec s;
        spectrumFromCoeffs(co, s);
        for (int l = 0; l < kN; ++l)
          for (int m = 0; m < 3; ++m) rw[m] += wa[di] * wc[dj] * double(cf[3]) * s[l] * S[m][l];
      }
    for (int m = 0; m < 3; ++m) fm[m] = 1.0 / (bGrey * rw[m]);
  }

  // Window (erf band-pass) normalised so an I_ref-shaped spectrum keeps its exposure.
  Spec SW[3];
  for (int m = 0; m < 3; ++m) {
    Spec w;
    w.fill(1.0);
    if (rp.window && film.windowParams.size() == 4) {
      const double cu = film.windowParams[0], su = film.windowParams[1], ci = film.windowParams[2],
                   si = film.windowParams[3];
      double num = 0, den = 0;
      for (int l = 0; l < kN; ++l) {
        const double wl = wavelength(l);
        w[l] = 0.5 * (1 + std::erf((wl - cu) / (su * std::sqrt(2.0)))) * 0.5 * (1 - std::erf((wl - ci) / (si * std::sqrt(2.0))));
        num += Sf[m][l] * iRef[l] * w[l];
        den += Sf[m][l] * iRef[l];
      }
      for (int l = 0; l < kN; ++l) w[l] /= num / den;
    }
    for (int l = 0; l < kN; ++l) SW[m][l] = Sf[m][l] * w[l] * fm[m];
  }

  // Raw exposure per unit X+Y+Z at every tc node.
  std::vector<float> R(std::size_t(n) * n * 4, 0.f);
  parallelFor(
      n,
      [&](int j) {
        Spec s;
        for (int i = 0; i < n; ++i) {
          const float* c = &up.coef[(std::size_t(j) * n + i) * 4];
          const double a[3] = {c[0], c[1], c[2]};
          spectrumFromCoeffs(a, s);
          double r[3] = {0, 0, 0};
          for (int l = 0; l < kN; ++l)
            for (int m = 0; m < 3; ++m) r[m] += s[l] * SW[m][l];
          float* o = &R[(std::size_t(j) * n + i) * 4];
          for (int m = 0; m < 3; ++m) o[m] = float(r[m] * c[3]);
        }
      },
      4);

  // Optional polynomial adaptation surface.
  if (rp.surface && film.surfaceParams.size() == 45) {
    double tx, ty;
    xyToTc(wx, wy, tx, ty);
    parallelFor(
        n,
        [&](int j) {
          for (int i = 0; i < n; ++i) {
            const double X = double(i) / (n - 1) - tx, Y = double(j) / (n - 1) - ty;
            const double mono[14] = {X, Y, X * X, Y * Y, X * Y, X * X * X, Y * Y * Y, X * X * Y, X * Y * Y,
                                     X * X * X * X, Y * Y * Y * Y, X * X * X * Y, X * X * Y * Y, X * Y * Y * Y};
            float* o = &R[(std::size_t(j) * n + i) * 4];
            for (int m = 0; m < 3; ++m) {
              double P = 0;
              for (int k = 0; k < 14; ++k) P += double(film.surfaceParams[m * 15 + 1 + k]) * mono[k];
              o[m] = float(o[m] * std::exp2(P / std::sqrt(1.0 + 0.25 * P * P)));
            }
          }
        },
        4);
  }

  // Input gamut compression ("xy" algorithm): remap every node toward the film white.
  if (rp.gamut) {
    static const Locus locus;
    parallelFor(
        n,
        [&](int j) {
          for (int i = 0; i < n; ++i) {
            double x, y;
            tcToXy(double(i) / (n - 1), double(j) / (n - 1), x, y);
            double dx = x - wx, dy = y - wy;
            const double d = std::hypot(dx, dy);
            double px = x, py = y;
            if (d >= 1e-9) {
              const double ux = dx / d, uy = dy / d;
              const double B = locus.distance(wx, wy, ux, uy);
              const double dn = d / std::max(B, 1e-12);
              const double dk = knee(dn, rp.knee[0], rp.knee[1], rp.knee[2]);
              px = wx + ux * dk * B;
              py = wy + uy * dk * B;
            }
            double t0, t1;
            xyToTc(px, py, t0, t1);
            const double fi = t0 * (n - 1), fj = t1 * (n - 1);
            const int i0 = std::min(int(fi), n - 2), j0 = std::min(int(fj), n - 2);
            const double ai = fi - i0, aj = fj - j0;
            float* o = &lut->node[(std::size_t(j) * n + i) * 4];
            for (int m = 0; m < 3; ++m) {
              auto at = [&](int ii, int jj) { return double(R[(std::size_t(jj) * n + ii) * 4 + m]); };
              o[m] = float((at(i0, j0) * (1 - ai) + at(i0 + 1, j0) * ai) * (1 - aj) +
                           (at(i0, j0 + 1) * (1 - ai) + at(i0 + 1, j0 + 1) * ai) * aj);
            }
          }
        },
        4);
  } else {
    lut->node = R;
  }
  return lut;
}

// ---- FilmModel -------------------------------------------------------------------------------------------

namespace {
double lookupTab(const std::vector<double>& tab, double lex0, double dlex, double xs) {
  const int n = int(tab.size());
  double f = (xs - lex0) / dlex;
  if (!(f > 0)) return tab[0];
  if (f >= n - 1) return tab[n - 1];
  const int i = int(f);
  return tab[i] + (f - i) * (tab[i + 1] - tab[i]);
}
// np.interp semantics with clamped ends; xp must be non-decreasing.
double interp1(double x, const std::vector<double>& xp, const std::vector<double>& fp) {
  const int n = int(xp.size());
  if (x <= xp[0]) return fp[0];
  if (x >= xp[n - 1]) return fp[n - 1];
  int lo = 0, hi = n - 1;
  while (hi - lo > 1) {
    const int mid = (lo + hi) / 2;
    (xp[mid] <= x ? lo : hi) = mid;
  }
  const double dx = xp[hi] - xp[lo];
  return dx > 0 ? fp[lo] + (x - xp[lo]) / dx * (fp[hi] - fp[lo]) : fp[hi];
}
}  // namespace

FilmModel::FilmModel(const FilmStock& film, const FilmStock* paper, const FilmParams& p, const UpsampleTable& up,
                     const CmaxTable& cmax, std::shared_ptr<const RawLut> raw)
    : film_(film), paper_(paper), p_(p), cmax_(&cmax), raw_(std::move(raw)) {
  (void)up;
  init();
}

void FilmModel::init() {
  scanFilm_ = p_.io.scanFilm || !paper_;
  if (!illuminant(film_.referenceIlluminant, iRef_)) {
    err_ = "unknown reference illuminant " + film_.referenceIlluminant;
    return;
  }
  xyOfXyz(illuminantXyz(iRef_), wx_, wy_);
  mIn_ = mul(adaptationMatrix(cat16(), kD50x, kD50y, wx_, wy_), prophotoToXyz());
  pe_ = p_.enlarger.printExposure;
  gam_ = {p_.filmRender.densityCurveGamma * p_.filmRender.densityCurveGammaRgb[0],
          p_.filmRender.densityCurveGamma * p_.filmRender.densityCurveGammaRgb[1],
          p_.filmRender.densityCurveGamma * p_.filmRender.densityCurveGammaRgb[2]};
  buildCurves();
  if (!err_.empty()) return;

  // spectral media
  auto fillMedium = [&](const FilmStock& s, std::array<bool, kN>& valid, Spec dye[3], Spec& base) {
    for (int l = 0; l < kN; ++l) {
      bool v = !std::isnan(s.baseDensity[l]);
      for (int m = 0; m < 3; ++m) v = v && !std::isnan(s.channelDensity[l * 3 + m]);
      valid[l] = v;
      for (int m = 0; m < 3; ++m) dye[m][l] = v ? double(s.channelDensity[l * 3 + m]) : 0.0;
      base[l] = v ? double(s.baseDensity[l]) : 0.0;
    }
  };
  fillMedium(film_, filmValid_, filmDye_, filmBase_);
  const FilmStock& scanned = scanFilm_ ? film_ : *paper_;
  if (!scanFilm_) {
    fillMedium(*paper_, paperValid_, paperDye_, paperBase_);
    for (int m = 0; m < 3; ++m)
      for (int l = 0; l < kN; ++l) paperS_[m][l] = std::exp2(nan0(paper_->logSensitivity[l * 3 + m]) * kLog2_10);
  } else {
    paperValid_ = filmValid_;
    for (int m = 0; m < 3; ++m) paperDye_[m] = filmDye_[m];
    paperBase_ = filmBase_;
  }
  if (!illuminant(scanned.viewingIlluminant, eView_)) {
    err_ = "unknown viewing illuminant " + scanned.viewingIlluminant;
    return;
  }
  if (!illuminant(p_.enlarger.illuminant, eEnl_)) {
    err_ = "unknown enlarger illuminant " + p_.enlarger.illuminant;
    return;
  }
  xyzIll_ = illuminantXyz(eView_);
  normView_ = 0;
  for (int l = 0; l < kN; ++l) normView_ += eView_[l] * cmf(1)[l];
  {
    double xi, yi;
    xyOfXyz(xyzIll_, xi, yi);
    xyzToRgb_ = mul(xyzToSrgb(), adaptationMatrix(cat02(), xi, yi, kD65x, kD65y));
  }
  // film density limits (spec 02 s3.1)
  for (int m = 0; m < 3; ++m) {
    double mx = -1e30;
    for (int k = 0; k < nCurve_; ++k) mx = std::max(mx, double(film_.densityCurves[k * 3 + m]));
    dMaxFilm_[m] = mx;
    dMinFilm_[m] = -p_.filmRender.grain.densityMin[m];
  }
  if (!scanFilm_) {
    // paper curves from the fitted CDF model (optionally with the gamma morph)
    if (paper_->modelCenters.size() != 9 || paper_->modelSigmas.size() != 9 || paper_->modelAmplitudes.size() != 9) {
      err_ = "paper profile has no density_curves_model";
      return;
    }
    const auto& morph = p_.printRender.densityCurvesMorph;
    const int pn = int(paper_->logExposure.size());
    paperAxis_.assign(paper_->logExposure.begin(), paper_->logExposure.end());
    for (int c = 0; c < 3; ++c) {
      double lowest = 1e30;
      int fastIdx = 0;
      for (int i = 0; i < 3; ++i)
        if (paper_->modelCenters[c * 3 + i] < lowest) {
          lowest = paper_->modelCenters[c * 3 + i];
          fastIdx = i;
        }
      const double gch[3] = {morph.gammaFactorRed, morph.gammaFactorGreen, morph.gammaFactorBlue};
      paperCurve_[c].assign(pn, 0.0);
      for (int i = 0; i < 3; ++i) {
        double mu = paper_->modelCenters[c * 3 + i], sg = paper_->modelSigmas[c * 3 + i];
        const double A = paper_->modelAmplitudes[c * 3 + i];
        if (morph.active) {
          const double g = morph.gammaFactor * gch[c] * (i == fastIdx ? morph.gammaFactorFast : morph.gammaFactorSlow);
          sg = std::max(sg / g, 0.05);
          mu = mu / g;
        }
        for (int k = 0; k < pn; ++k) {
          const double z = (paperAxis_[k] - mu) / sg;
          paperCurve_[c][k] += A * 0.5 * std::erfc(-z / std::sqrt(2.0));
        }
      }
    }
  }
  dMinScan_ = dMinFilm_;
  dMaxScan_ = dMaxFilm_;
  if (!scanFilm_) {
    for (int m = 0; m < 3; ++m) {
      double mn = 1e30, mx = -1e30;
      for (int k = 0; k < int(paperAxis_.size()); ++k) {
        mn = std::min(mn, double(paper_->densityCurves[k * 3 + m]));
        mx = std::max(mx, double(paper_->densityCurves[k * 3 + m]));
      }
      dMinScan_[m] = mn;
      dMaxScan_[m] = mx;
    }
  }

  // mid-grey film density with DIR at EV 0 (neutral solver, mid-grey normaliser)
  {
    const Vec3 x = logExposure(exposureScaled(Vec3{0.184, 0.184, 0.184}, 1.0));
    dMidDir_ = develop(x);
  }
  if (!scanFilm_) {
    if (p_.settings.neutralPrintFilters) solveNeutral();
    mSolved_ = p_.enlarger.mFilterNeutral;
    ySolved_ = p_.enlarger.yFilterNeutral;
    castKnown_ = p_.settings.neutralPrintFilters;
    if (castKnown_ && p_.enlarger.neutralMix != 0) {  // partial neutralisation (EditParams::filmCast)
      p_.enlarger.mFilterNeutral = mSolved_ + (p_.enlarger.refMNeutral - mSolved_) * p_.enlarger.neutralMix;
      p_.enlarger.yFilterNeutral = ySolved_ + (p_.enlarger.refYNeutral - ySolved_) * p_.enlarger.neutralMix;
    }
    buildEnlarger(p_.enlarger.cFilterNeutral, p_.enlarger.mFilterNeutral + p_.enlarger.mFilterShift,
                  p_.enlarger.yFilterNeutral + p_.enlarger.yFilterShift);
    buildBlackWhite();
  } else {
    buildBlackWhite();  // a film with no print: only the paper-to-screen mapping (scanner.paperReference) applies
  }
}

void FilmModel::buildCurves() {
  // Uniform log-exposure axis expected (all shipped profiles).
  nCurve_ = int(film_.logExposure.size());
  lex0_ = film_.logExposure[0];
  dlex_ = (double(film_.logExposure[nCurve_ - 1]) - lex0_) / (nCurve_ - 1);
  for (int k = 1; k < nCurve_; ++k)
    if (std::fabs(double(film_.logExposure[k]) - (lex0_ + k * dlex_)) > 1e-3) {
      err_ = "film profile log_exposure axis is not uniform";
      return;
    }
  for (int m = 0; m < 3; ++m) {
    dn_[m].assign(nCurve_, 0.0);
    double mn = 1e30;
    for (int k = 0; k < nCurve_; ++k)
      if (!std::isnan(film_.densityCurves[k * 3 + m])) mn = std::min(mn, double(film_.densityCurves[k * 3 + m]));
    for (int k = 0; k < nCurve_; ++k) dn_[m][k] = nan0(film_.densityCurves[k * 3 + m]) - mn;
    double mx = -1e30;
    for (double v : dn_[m]) mx = std::max(mx, v);
    dMax_[m] = mx;
  }
  const auto& dc = p_.filmRender.dirCouplers;
  dirActive_ = dc.active;
  Mat3 M{};
  M[0] = dc.gammaSamelayerRgb[0] * dc.inhibitionSamelayer;
  M[4] = dc.gammaSamelayerRgb[1] * dc.inhibitionSamelayer;
  M[8] = dc.gammaSamelayerRgb[2] * dc.inhibitionSamelayer;
  M[1] = dc.gammaInterlayerRToGb[0] * dc.inhibitionInterlayer;
  M[2] = dc.gammaInterlayerRToGb[1] * dc.inhibitionInterlayer;
  M[3] = dc.gammaInterlayerGToRb[0] * dc.inhibitionInterlayer;
  M[5] = dc.gammaInterlayerGToRb[1] * dc.inhibitionInterlayer;
  M[6] = dc.gammaInterlayerBToRg[0] * dc.inhibitionInterlayer;
  M[7] = dc.gammaInterlayerBToRg[1] * dc.inhibitionInterlayer;
  for (double& v : M) v *= dc.amount;
  dirM_ = M;
  if (!dirActive_) dirM_.fill(0.0);
  // pre-coupler curves so that the coupler model applied to a grey ramp reproduces Dn
  const bool pos = film_.positive();
  std::vector<double> axis(nCurve_);
  for (int k = 0; k < nCurve_; ++k) axis[k] = lex0_ + k * dlex_;
  for (int m = 0; m < 3; ++m) {
    if (!dirActive_) {
      d0_[m] = dn_[m];
      continue;
    }
    std::vector<double> x0(nCurve_), fp(nCurve_);
    for (int k = 0; k < nCurve_; ++k) {
      double c = 0;
      for (int kk = 0; kk < 3; ++kk) {
        const double T = pos ? dMax_[kk] - dn_[kk][k] : dn_[kk][k];
        c += T * dirM_[kk * 3 + m];
      }
      x0[k] = axis[k] - c;
      if (k > 0 && x0[k] < x0[k - 1] + 1e-9) x0[k] = x0[k - 1] + 1e-9;  // monotonicity guard
      fp[k] = pos ? -dn_[m][k] : dn_[m][k];
    }
    d0_[m].resize(nCurve_);
    for (int k = 0; k < nCurve_; ++k) {
      const double v = interp1(axis[k], x0, fp);
      d0_[m][k] = pos ? -v : v;
    }
  }
  // inhibitor: q_m = sum_k T_k M[k][m], T = d (negative) or Dmax - d (positive)
  const double sgn = pos ? -1.0 : 1.0;
  for (int m = 0; m < 3; ++m) {
    dirQ0_[m] = 0;
    for (int k = 0; k < 3; ++k) {
      dirQ_[m * 3 + k] = sgn * dirM_[k * 3 + m];
      if (pos) dirQ0_[m] += dirM_[k * 3 + m] * dMax_[k];
    }
  }
}

Vec3 FilmModel::exposureScaled(const Vec3& rgb, double scale, const Mat3* mm) const {
  const Vec3 xyz = mul(mm ? *mm : mIn_, rgb);
  const double b = xyz[0] + xyz[1] + xyz[2];
  const double bb = std::max(b, 1e-10);
  double tc0, tc1;
  xyToTc(xyz[0] / bb, xyz[1] / bb, tc0, tc1);
  const int n = raw_->n;
  const double a = tc0 * (n - 1), c = tc1 * (n - 1);
  int ia, ic;
  double fa, fc;
  if (a >= n - 1) { ia = n - 2; fa = 1.0; } else { ia = int(a); fa = a - ia; }
  if (c >= n - 1) { ic = n - 2; fc = 1.0; } else { ic = int(c); fc = c - ic; }
  double wa[4], wc[4];
  mitchell(fa, wa);
  mitchell(fc, wc);
  auto refl = [n](int k) { return k < 0 ? -k : (k >= n ? 2 * (n - 1) - k : k); };
  Vec3 r{0, 0, 0};
  double ws = 0;
  for (int dj = 0; dj < 4; ++dj) {
    const int jj = refl(ic - 1 + dj);
    for (int di = 0; di < 4; ++di) {
      const int ii = refl(ia - 1 + di);
      const double w = wa[di] * wc[dj];
      ws += w;
      const float* o = &raw_->node[(std::size_t(jj) * n + ii) * 4];
      for (int m = 0; m < 3; ++m) r[m] += w * o[m];
    }
  }
  const double s = scale * b / ws;
  return {r[0] * s, r[1] * s, r[2] * s};
}

Vec3 FilmModel::exposure(const Vec3& rgb) const {
  return exposureScaled(rgb, std::exp2(p_.camera.exposureCompensationEv));
}

Vec3 FilmModel::logExposure(const Vec3& e) const {
  return {std::log10(std::max(e[0], 0.0) + 1e-10), std::log10(std::max(e[1], 0.0) + 1e-10),
          std::log10(std::max(e[2], 0.0) + 1e-10)};
}

Vec3 FilmModel::developPlain(const Vec3& x) const {
  Vec3 d;
  for (int m = 0; m < 3; ++m) d[m] = lookupTab(dn_[m], lex0_, dlex_, gam_[m] * x[m]);
  return d;
}

Vec3 FilmModel::develop(const Vec3& x) const {
  const Vec3 d = developPlain(x);
  if (!dirActive_) return d;
  Vec3 q;
  for (int m = 0; m < 3; ++m) q[m] = dirQ0_[m] + dirQ_[m * 3] * d[0] + dirQ_[m * 3 + 1] * d[1] + dirQ_[m * 3 + 2] * d[2];
  Vec3 out;
  for (int m = 0; m < 3; ++m) out[m] = lookupTab(d0_[m], lex0_, dlex_, gam_[m] * (x[m] - q[m]));
  return out;
}

// ---- print + scan ---------------------------------------------------------------------------------------

Vec3 FilmModel::paperRaw(const Vec3& d, const Spec& ef) const {
  Vec3 r{0, 0, 0};
  for (int l = 0; l < kN; ++l) {
    if (!filmValid_[l]) continue;
    const double D = filmBase_[l] + filmDye_[0][l] * d[0] + filmDye_[1][l] * d[1] + filmDye_[2][l] * d[2];
    const double L = ef[l] * pow10neg(D);
    r[0] += L * paperS_[0][l];
    r[1] += L * paperS_[1][l];
    r[2] += L * paperS_[2][l];
  }
  return r;
}

Vec3 FilmModel::paperDensity(const Vec3& logE) const {
  Vec3 d;
  const int n = int(paperAxis_.size());
  const double x0 = paperAxis_[0], dx = (paperAxis_[n - 1] - x0) / (n - 1);
  for (int c = 0; c < 3; ++c) d[c] = lookupTab(paperCurve_[c], x0, dx, logE[c]);
  return d;
}

Vec3 FilmModel::scanXyz(const Vec3& dp) const {
  Vec3 xyz{0, 0, 0};
  for (int l = 0; l < kN; ++l) {
    if (!paperValid_[l]) continue;
    const double D = paperBase_[l] + paperDye_[0][l] * dp[0] + paperDye_[1][l] * dp[1] + paperDye_[2][l] * dp[2];
    const double L = eView_[l] * pow10neg(D);
    xyz[0] += L * cmf(0)[l];
    xyz[1] += L * cmf(1)[l];
    xyz[2] += L * cmf(2)[l];
  }
  for (double& v : xyz) v = v / normView_ + 1e-10;
  return xyz;
}

Vec3 FilmModel::printExposure(const Vec3& d) const {
  const Vec3 R = paperRaw(d, ef_);
  Vec3 e;
  for (int c = 0; c < 3; ++c) e[c] = (g_ * R[c] + rawPre_[c]) * pe_ * bwExposure_;
  return e;
}

Vec3 FilmModel::printScanLinear(const Vec3& d, bool glare) const {
  Vec3 dp = d;
  if (!scanFilm_) {
    const Vec3 e = printExposure(d);
    Vec3 logE;
    for (int c = 0; c < 3; ++c) logE[c] = std::log10(std::max(e[c], 0.0) + 1e-10);
    dp = paperDensity(logE);
  }
  return scanLinear(dp, glare);
}

Vec3 FilmModel::scanOut(const Vec3& dp) const {
  Vec3 rgb = scanLinear(dp, true);
  if (p_.io.outputGamutCompress) rgb = compressOutput(cam_, *cmax_, rgb);
  if (postOn_)
    for (double& v : rgb) v = v * postM_ + postQ_;
  return {srgbOetf(rgb[0]), srgbOetf(rgb[1]), srgbOetf(rgb[2])};
}

Vec3 FilmModel::scanLinear(const Vec3& dp, bool glare) const {
  Vec3 xyz = scanXyz(dp);
  if (scanFilm_ && pe_ != 1.0)  // no print: the exposure multiplier is the scan / projection lamp (1 / pe)
    for (double& v : xyz) v /= pe_;
  if (bwOn_) {
    const double Y = xyz[1];
    const double Yp = std::clamp(bwM_ * Y + bwQ_, 0.0, 1.0);
    for (double& v : xyz) v *= Yp / (Y + 1e-10);
  }
  const auto& gl = p_.printRender.glare;
  if (glare && !scanFilm_ && gl.active && gl.percent > 0 && !p_.debug.deactivateStochasticEffects)
    for (int c = 0; c < 3; ++c) xyz[c] += gl.percent / 100.0 * xyzIll_[c];
  return mul(xyzToRgb_, xyz);
}

Vec3 FilmModel::printScan(const Vec3& d, Vec3* linRgb) const {
  Vec3 rgb = printScanLinear(d, true);
  if (linRgb) *linRgb = rgb;
  if (p_.io.outputGamutCompress) rgb = compressOutput(cam_, *cmax_, rgb);
  if (postOn_)
    for (double& v : rgb) v = v * postM_ + postQ_;
  return {srgbOetf(rgb[0]), srgbOetf(rgb[1]), srgbOetf(rgb[2])};
}

Vec3 FilmModel::render(const Vec3& rgb) const { return printScan(develop(logExposure(exposure(rgb)))); }

void FilmModel::buildEnlarger(double cc, double mm, double yy) {
  Spec fc, fm, fy;
  dichroicBase(fc, fm, fy);
  auto filtered = [&](double c, double m, double y) {
    const double tc = std::pow(10.0, -c / 100.0), tm = std::pow(10.0, -m / 100.0), ty = std::pow(10.0, -y / 100.0);
    Spec e;
    for (int l = 0; l < kN; ++l)
      e[l] = eEnl_[l] * (1 - (1 - fc[l]) * (1 - tc)) * (1 - (1 - fm[l]) * (1 - tm)) * (1 - (1 - fy[l]) * (1 - ty));
    return e;
  };
  ef_ = filtered(cc, mm, yy);
  // exposure normaliser g (spec 02 s3.5): mid grey through the film (sRGB input, no spatial effects)
  auto geomeanGain = [&](double evScale) {
    const Mat3 m = mul(adaptationMatrix(cat16(), kD65x, kD65y, wx_, wy_), srgbToXyz());
    const Vec3 x = logExposure(exposureScaled(Vec3{0.184, 0.184, 0.184}, evScale, &m));
    // raw (not min-subtracted) curves, no DIR
    Vec3 d;
    for (int c = 0; c < 3; ++c) {
      std::vector<double> raw(nCurve_);
      for (int k = 0; k < nCurve_; ++k) raw[k] = nan0(film_.densityCurves[k * 3 + c]);
      d[c] = lookupTab(raw, lex0_, dlex_, gam_[c] * x[c]);
    }
    const Vec3 R = paperRaw(d, ef_);
    double lg = 0;
    for (int c = 0; c < 3; ++c) lg += std::log(std::max(R[c], 1e-10));
    return 1.0 / std::exp(lg / 3.0);
  };
  const bool comp = p_.enlarger.printExposureCompensation, norm = p_.enlarger.normalizePrintExposure;
  const double ev = std::exp2(p_.camera.exposureCompensationEv);
  if (comp && norm) g_ = geomeanGain(ev);
  else if (comp && !norm) g_ = geomeanGain(ev) / geomeanGain(1.0);
  else if (!comp && norm) g_ = geomeanGain(1.0);
  else g_ = 1.0;
  // preflash
  rawPre_ = {0, 0, 0};
  if (p_.enlarger.preflashExposure > 0) {
    const Spec epf = filtered(p_.enlarger.cFilterNeutral, p_.enlarger.mFilterNeutral + p_.enlarger.preflashMFilterShift,
                              p_.enlarger.yFilterNeutral + p_.enlarger.preflashYFilterShift);
    for (int l = 0; l < kN; ++l) {
      if (!filmValid_[l]) continue;
      const double L = epf[l] * pow10neg(filmBase_[l]);
      for (int c = 0; c < 3; ++c) rawPre_[c] += L * paperS_[c][l];
    }
    for (double& v : rawPre_) v *= p_.enlarger.preflashExposure;
  }
}

// Paper-to-screen mapping after the output gamut compression (stage C, docs/engine/pipeline-architecture.md section 18): the
// displayed paper white (unexposed paper, Dmin; for a film with no print the clear film) maps to scanner.whiteLevel and the
// displayed black (Dmax) to scanner.blackLevel, a linear map of the display light that keeps the colour of every pixel
// (relative colorimetric with black point compensation; the paper base tint survives as far as the gamut compression keeps it).
void FilmModel::buildPostMap() {
  const auto& sc = p_.scanner;
  Vec3 dLo, dHi;
  if (scanFilm_) {  // the reachable clear film (the lowest density of the curves) and the densest film
    const int fn = int(film_.logExposure.size());
    for (int c = 0; c < 3; ++c) {
      double lo = 1e30, hi = -1e30;
      for (int k = 0; k < fn; ++k) {
        const double v = nan0(film_.densityCurves[k * 3 + c]);
        lo = std::min(lo, v);
        hi = std::max(hi, v);
      }
      dLo[c] = lo;
      dHi[c] = hi;
    }
    const double y0 = scanXyz(dLo)[1], y1 = scanXyz(dHi)[1];
    if (y0 < y1) std::swap(dLo, dHi);  // lowest density = brightest
  } else {
    const int pn = int(paperAxis_.size());
    for (int c = 0; c < 3; ++c) {
      double lo = 1e30, hi = -1e30;
      for (int k = 0; k < pn; ++k) {
        const double v = nan0(paper_->densityCurves[k * 3 + c]);
        lo = std::min(lo, v);
        hi = std::max(hi, v);
      }
      dLo[c] = lo;
      dHi[c] = hi;
    }
  }
  auto shown = [&](const Vec3& dp) {
    Vec3 rgb = mul(xyzToRgb_, scanXyz(dp));
    if (p_.io.outputGamutCompress) rgb = compressOutput(cam_, *cmax_, rgb);
    return 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2];
  };
  const double Yw = shown(dLo), Yb = shown(dHi);
  const double lw = sc.whiteCorrection ? srgbEotf(sc.whiteLevel) : Yw;
  // black: 0 = the paper's true relative luminance (white scaling only), +1 = display black (v4), -1 = a matte lifted black
  const double mix = std::clamp(sc.blackCorrection ? 1.0 : sc.blackMix, -1.0, 1.0);
  postOn_ = Yw > Yb + 1e-6;
  if (!postOn_) {
    postM_ = Yw > 1e-6 ? lw / Yw : 1.0;
    postQ_ = 0.0;
    postOn_ = Yw > 1e-6;
    return;
  }
  const double ybTrue = Yb * lw / Yw;
  const double ybT = mix >= 0 ? ybTrue + (srgbEotf(sc.blackLevel) - ybTrue) * mix : ybTrue + (0.02 - ybTrue) * (-mix);
  postM_ = (lw - ybT) / (Yw - Yb);
  postQ_ = ybT - postM_ * Yb;
}

// Scanner black / white correction (spec 02 s3.10), print path only: a linear rescale of the print luminance so that
// the print's black and white reference map to the requested levels, plus the matching paper exposure factor that
// keeps mid-grey where it was.
void FilmModel::buildBlackWhite() {
  postOn_ = false;
  postM_ = 1;
  postQ_ = 0;
  bwOn_ = false;
  bwM_ = 1;
  bwQ_ = 0;
  bwExposure_ = 1;
  const auto& sc = p_.scanner;
  if (!sc.blackCorrection && !sc.whiteCorrection) return;
  if (sc.paperReference) {
    buildPostMap();
    return;
  }
  if (scanFilm_) return;
  const int pn = int(paperAxis_.size());
  for (int c = 0; c < 3; ++c) {
    paperTab_[c].assign(pn, 0.0);
    for (int k = 0; k < pn; ++k) paperTab_[c][k] = nan0(paper_->densityCurves[k * 3 + c]);
  }
  const double x0 = paperAxis_[0], dx = (paperAxis_[pn - 1] - x0) / (pn - 1);
  auto lumaAt = [&](const Vec3& dFilm) {
    const Vec3 R = paperRaw(dFilm, ef_);
    Vec3 dp;
    for (int c = 0; c < 3; ++c) {
      const double e = g_ * R[c] + rawPre_[c];
      dp[c] = lookupTab(paperTab_[c], x0, dx, std::log10(std::max(e, 0.0) + 1e-10));
    }
    return scanXyz(dp)[1];
  };
  const double Yb = lumaAt(dMinFilm_), Yw = lumaAt(dMaxFilm_);
  const double Lb = srgbEotf(sc.blackLevel), Lw = srgbEotf(sc.whiteLevel);
  const double lb = sc.blackCorrection ? Lb : Yb, lw = sc.whiteCorrection ? Lw : Yw;
  bwM_ = (lw - lb) / (Yw - Yb + 1e-10);
  bwQ_ = lb - bwM_ * Yb;
  // exposure side: shift the mid-grey on the channel-mean tabulated paper curve
  std::vector<double> cav(pn), axis(pn);
  for (int k = 0; k < pn; ++k) {
    cav[k] = (paperTab_[0][k] + paperTab_[1][k] + paperTab_[2][k]) / 3.0;
    axis[k] = paperAxis_[k];
    if (k > 0 && cav[k] < cav[k - 1]) cav[k] = cav[k - 1];
  }
  double base = 0;
  int nb = 0;
  for (int l = 0; l < kN; ++l)
    if (!std::isnan(paper_->baseDensity[l])) {
      base += paper_->baseDensity[l];
      ++nb;
    }
  base = nb ? base / nb : 0.0;
  const double dMid = -std::log10(0.184);
  const double YmidP = (0.184 - bwQ_) / bwM_;
  if (YmidP > 0 && std::isfinite(YmidP)) {
    const double dMidP = -std::log10(YmidP);
    const double la = interp1(dMid - base, cav, axis), lbb = interp1(dMidP - base, cav, axis);
    bwExposure_ = std::pow(10.0, lbb - la);
  }
  bwOn_ = true;
}

// Solves the enlarger magenta / yellow filtration for a neutral 18.4 % grey print (Newton on the full
// chain; cyan stays 0), replacing the reference's shipped database.
void FilmModel::solveNeutral() {
  const Vec3 dGrey = dMidDir_;
  auto resid = [&](double m, double y, double r[2]) {
    buildEnlarger(0.0, m, y);
    // grey must be evaluated at EV 0 without preflash/glare: use a neutral copy of the settings
    const double ge = g_;
    (void)ge;
    const Vec3 lin = printScanLinear(dGrey, false);
    const double a = std::max(lin[0], 1e-6), b = std::max(lin[1], 1e-6), c = std::max(lin[2], 1e-6);
    r[0] = std::log(a / b);
    r[1] = std::log(c / b);
  };
  const double evSave = p_.camera.exposureCompensationEv, peSave = pe_, pfSave = p_.enlarger.preflashExposure;
  p_.camera.exposureCompensationEv = 0;
  pe_ = 1.0;
  p_.enlarger.preflashExposure = 0;
  double m = 55, y = 55;
  for (int it = 0; it < 40; ++it) {
    double r0[2], r1[2], r2[2];
    resid(m, y, r0);
    if (std::hypot(r0[0], r0[1]) < 1e-9) break;
    const double h = 0.05;
    resid(m + h, y, r1);
    resid(m, y + h, r2);
    const double a = (r1[0] - r0[0]) / h, b = (r2[0] - r0[0]) / h, c = (r1[1] - r0[1]) / h, d = (r2[1] - r0[1]) / h;
    const double det = a * d - b * c;
    if (std::fabs(det) < 1e-14) break;
    double dm = -(d * r0[0] - b * r0[1]) / det, dy = -(-c * r0[0] + a * r0[1]) / det;
    const double lim = 30.0;
    dm = std::clamp(dm, -lim, lim);
    dy = std::clamp(dy, -lim, lim);
    m += dm;
    y += dy;
    if (std::fabs(dm) < 1e-7 && std::fabs(dy) < 1e-7) break;
  }
  p_.camera.exposureCompensationEv = evSave;
  pe_ = peSave;
  p_.enlarger.preflashExposure = pfSave;
  p_.enlarger.cFilterNeutral = 0;
  p_.enlarger.mFilterNeutral = m;
  p_.enlarger.yFilterNeutral = y;
}

// ---- bake ------------------------------------------------------------------------------------------------

FilmTables FilmModel::bake(BakeTimings* tm) const {
  using Clock = std::chrono::steady_clock;
  auto ms = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
  const auto t0 = Clock::now();
  FilmTables t;
  bakeInput(t);
  bakeCurves(t);
  const auto t1 = Clock::now();
  bakePrintScan(t);
  const auto t2 = Clock::now();
  if (tm) {
    tm->curvesMs = ms(t0, t1);
    tm->lutMs = ms(t1, t2);
    tm->totalMs = ms(t0, t2);
  }
  return t;
}

// Input stage tables: the 3x3 into the film's balance white and the reflect-padded raw-exposure LUT.
void FilmModel::bakeInput(FilmTables& t) const {
  for (int c = 0; c < 3; ++c) {
    t.mIn[c * 4 + 0] = float(mIn_[0 * 3 + c]);
    t.mIn[c * 4 + 1] = float(mIn_[1 * 3 + c]);
    t.mIn[c * 4 + 2] = float(mIn_[2 * 3 + c]);
    t.mIn[c * 4 + 3] = 0.f;
  }
  const int n = raw_->n;
  t.rawN = n;
  const int pn = n + 2;
  t.raw.assign(std::size_t(pn) * pn * 4, 0.f);
  auto refl = [n](int k) { return k < 0 ? -k : (k >= n ? 2 * (n - 1) - k : k); };
  for (int j = -1; j <= n; ++j)
    for (int i = -1; i <= n; ++i)
      std::memcpy(&t.raw[(std::size_t(j + 1) * pn + (i + 1)) * 4], &raw_->node[(std::size_t(refl(j)) * n + refl(i)) * 4],
                  16);
}

// Density curves, pre-coupler curves and the DIR inhibitor matrix (depend on the film and the DIR parameters only).
void FilmModel::bakeCurves(FilmTables& t) const {
  t.curveN = nCurve_;
  t.curveX0 = float(lex0_);
  t.curveInvDx = float(1.0 / dlex_);
  for (int m = 0; m < 3; ++m) t.gamma[m] = float(gam_[m]);
  t.dn.assign(std::size_t(3) * nCurve_ * 2, 0.f);
  t.d0.assign(std::size_t(3) * nCurve_ * 2, 0.f);
  for (int m = 0; m < 3; ++m)
    for (int k = 0; k < nCurve_; ++k) {
      const int kn = std::min(k + 1, nCurve_ - 1);
      t.dn[(std::size_t(m) * nCurve_ + k) * 2] = float(dn_[m][k]);
      t.dn[(std::size_t(m) * nCurve_ + k) * 2 + 1] = float(dn_[m][kn] - dn_[m][k]);
      t.d0[(std::size_t(m) * nCurve_ + k) * 2] = float(d0_[m][k]);
      t.d0[(std::size_t(m) * nCurve_ + k) * 2 + 1] = float(d0_[m][kn] - d0_[m][k]);
    }
  for (int k = 0; k < 3; ++k) {  // column k of Q: contribution of d_k to (q_R, q_G, q_B)
    for (int m = 0; m < 3; ++m) t.dirQ[k * 4 + m] = float(dirQ_[m * 3 + k]);
    t.dirQ[k * 4 + 3] = 0.f;
  }
  for (int m = 0; m < 3; ++m) t.dirQ0[m] = float(dirQ0_[m]);
}

// film density -> display sRGB, whole print + scan chain (needs the paper; changes with the enlarger filtration)
void FilmModel::bakePrintScan(FilmTables& t) const {
  // film density -> display sRGB, whole print + scan chain
  const int N = p_.settings.lutResolution;
  t.lutN = N;
  for (int c = 0; c < 3; ++c) {
    t.dmin[c] = float(dMinFilm_[c]);
    t.dscale[c] = float(1.0 / (dMaxFilm_[c] - dMinFilm_[c]));
  }
  t.lut.assign(std::size_t(N) * N * N * 4, 0.f);
  parallelFor(
      N * N,
      [&](int row) {
        const int k = row / N, j = row % N;
        for (int i = 0; i < N; ++i) {
          const Vec3 d{dMinFilm_[0] + (dMaxFilm_[0] - dMinFilm_[0]) * i / (N - 1),
                       dMinFilm_[1] + (dMaxFilm_[1] - dMinFilm_[1]) * j / (N - 1),
                       dMinFilm_[2] + (dMaxFilm_[2] - dMinFilm_[2]) * k / (N - 1)};
          const Vec3 o = printScan(d);
          float* dst = &t.lut[((std::size_t(k) * N + j) * N + i) * 4];
          dst[0] = float(o[0]);
          dst[1] = float(o[1]);
          dst[2] = float(o[2]);
        }
      },
      4);
  {
    const Vec3 wr = mul(xyzToRgb_, xyzIll_);
    for (int c = 0; c < 3; ++c) t.glareRgb[c] = float(wr[c]);
    const auto& gl = p_.printRender.glare;
    t.glareRgb[3] = (!scanFilm_ && gl.active && gl.percent > 0 && !p_.debug.deactivateStochasticEffects) ? float(gl.percent / 100.0) : 0.f;
  }
  // split print path for the enlarger diffusion filter (spec 03 s6.1): blur acts on the linear paper exposure
  if (!scanFilm_ && p_.enlarger.diffusionFilter.active) {
    t.split = true;
    t.lut1.assign(std::size_t(N) * N * N * 4, 0.f);
    parallelFor(
        N * N,
        [&](int row) {
          const int k = row / N, j = row % N;
          for (int i = 0; i < N; ++i) {
            const Vec3 d{dMinFilm_[0] + (dMaxFilm_[0] - dMinFilm_[0]) * i / (N - 1),
                         dMinFilm_[1] + (dMaxFilm_[1] - dMinFilm_[1]) * j / (N - 1),
                         dMinFilm_[2] + (dMaxFilm_[2] - dMinFilm_[2]) * k / (N - 1)};
            const Vec3 e = printExposure(d);
            float* dst = &t.lut1[((std::size_t(k) * N + j) * N + i) * 4];
            for (int c = 0; c < 3; ++c) dst[c] = float(std::log10(std::max(e[c], 0.0) + 1e-10));
          }
        },
        4);
    const int pn = int(paperAxis_.size());
    t.paperN = pn;
    t.paperX0 = float(paperAxis_[0]);
    t.paperInvDx = float((pn - 1) / (paperAxis_[pn - 1] - paperAxis_[0]));
    t.paperCurve.assign(std::size_t(3) * pn * 2, 0.f);
    Vec3 lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30};
    for (int c = 0; c < 3; ++c)
      for (int k = 0; k < pn; ++k) {
        const int kn = std::min(k + 1, pn - 1);
        t.paperCurve[(std::size_t(c) * pn + k) * 2] = float(paperCurve_[c][k]);
        t.paperCurve[(std::size_t(c) * pn + k) * 2 + 1] = float(paperCurve_[c][kn] - paperCurve_[c][k]);
        lo[c] = std::min(lo[c], paperCurve_[c][k]);
        hi[c] = std::max(hi[c], paperCurve_[c][k]);
      }
    for (int c = 0; c < 3; ++c) {
      t.dminS[c] = float(lo[c]);
      t.dscaleS[c] = float(1.0 / std::max(hi[c] - lo[c], 1e-6));
    }
    t.lut3.assign(std::size_t(N) * N * N * 4, 0.f);
    parallelFor(
        N * N,
        [&](int row) {
          const int k = row / N, j = row % N;
          for (int i = 0; i < N; ++i) {
            const Vec3 dp{lo[0] + (hi[0] - lo[0]) * i / (N - 1), lo[1] + (hi[1] - lo[1]) * j / (N - 1),
                          lo[2] + (hi[2] - lo[2]) * k / (N - 1)};
            const Vec3 o = scanOut(dp);
            float* dst = &t.lut3[((std::size_t(k) * N + j) * N + i) * 4];
            for (int c = 0; c < 3; ++c) dst[c] = float(o[c]);
          }
        },
        4);
  }
  t.srgbDecode.resize(4097);
  for (int i = 0; i <= 4096; ++i) t.srgbDecode[i] = float(srgbEotf(double(i) / 4096.0));
}

// Re-balances the print for new enlarger filtration shifts without rebuilding the film side. The neutral filtration
// solved at construction does not depend on the shifts, so the result equals a model constructed with these shifts.
void FilmModel::rebalance(double mShift, double yShift) {
  if (scanFilm_) return;
  p_.enlarger.mFilterShift = mShift;
  p_.enlarger.yFilterShift = yShift;
  buildEnlarger(p_.enlarger.cFilterNeutral, p_.enlarger.mFilterNeutral + p_.enlarger.mFilterShift,
                p_.enlarger.yFilterNeutral + p_.enlarger.yFilterShift);
  buildBlackWhite();
}

}  // namespace dapple::film::detail
