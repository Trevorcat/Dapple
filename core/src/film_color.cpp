// Colorimetry primitives for the film engine: CIE tables, illuminants, chromatic adaptation,
// dichroic filters and the CAM16-UCS output gamut compression. Double precision reference math.
#include <algorithm>
#include <cmath>
#include <mutex>

#include "film_internal.h"
#include "parallel.h"

namespace dapple::film::detail {

#include "film_tables.inc"

Mat3 mul(const Mat3& a, const Mat3& b) {
  Mat3 r{};
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      for (int k = 0; k < 3; ++k) r[i * 3 + j] += a[i * 3 + k] * b[k * 3 + j];
  return r;
}
Vec3 mul(const Mat3& a, const Vec3& v) {
  return {a[0] * v[0] + a[1] * v[1] + a[2] * v[2], a[3] * v[0] + a[4] * v[1] + a[5] * v[2],
          a[6] * v[0] + a[7] * v[1] + a[8] * v[2]};
}
Mat3 diag(const Vec3& d) { return {d[0], 0, 0, 0, d[1], 0, 0, 0, d[2]}; }
Mat3 inverse(const Mat3& m) {
  const double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];
  const double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
  const double det = a * A + b * B + c * C;
  const double r = 1.0 / det;
  return {A * r, -(b * i - c * h) * r, (b * f - c * e) * r, B * r, (a * i - c * g) * r, -(a * f - c * d) * r,
          C * r, -(a * h - b * g) * r, (a * e - b * d) * r};
}

const Spec& cmf(int ch) {
  static const Spec tabs[3] = {
      [] { Spec s; for (int i = 0; i < kN; ++i) s[i] = kTab_XBAR[i]; return s; }(),
      [] { Spec s; for (int i = 0; i < kN; ++i) s[i] = kTab_YBAR[i]; return s; }(),
      [] { Spec s; for (int i = 0; i < kN; ++i) s[i] = kTab_ZBAR[i]; return s; }()};
  return tabs[ch];
}

static Spec fromTable(const double* t) {
  Spec s;
  for (int i = 0; i < kN; ++i) s[i] = t[i];
  return s;
}
static void normaliseMean(Spec& s) {
  double m = 0;
  for (double v : s) m += v;
  m /= kN;
  for (double& v : s) v /= m;
}

Spec kg3Transmittance() { return fromTable(kTab_KG3); }

static Spec planck(double kelvin) {
  const double h = 6.62607e-34, c = 299792458.0, k = 1.38065e-23;
  const double c2 = h * c / k;
  Spec s;
  for (int i = 0; i < kN; ++i) {
    const double l = wavelength(i) * 1e-9;
    s[i] = std::pow(l, -5.0) / (std::exp(c2 / (l * kelvin)) - 1.0);
  }
  return s;
}

// CIE standard illuminant A, computed from its analytic definition (CIE 15:2004 eq. 3.1 / ISO/CIE 11664-2):
// Planck radiator at 2848 K with c2 = 1.4350e-2 m K (= 2856 K with the current c2 = 1.4388e-2 m K).
static Spec cieIlluminantA() {
  Spec s;
  for (int i = 0; i < kN; ++i) {
    const double l = wavelength(i);  // nm
    s[i] = 100.0 * std::pow(560.0 / l, 5.0) * (std::exp(1.435e7 / (2848.0 * 560.0)) - 1.0) /
           (std::exp(1.435e7 / (2848.0 * l)) - 1.0);
  }
  return s;
}

bool illuminant(const std::string& label, Spec& out) {
  if (label == "D50") out = fromTable(kTab_D50);
  else if (label == "D55") out = fromTable(kTab_D55);
  else if (label == "D65") out = fromTable(kTab_D65);
  else if (label == "K75P") out = fromTable(kTab_K75P);
  else if (label == "T") out = cieIlluminantA();
  else if (label == "TH-KG3" || label == "TH-KG3-L") {
    out = planck(3400.0);
    const Spec kg = kg3Transmittance();
    for (int i = 0; i < kN; ++i) out[i] *= kg[i] * (label == "TH-KG3-L" ? kTab_LENS[i] : 1.0);
  } else if (label.size() > 2 && label.compare(0, 2, "BB") == 0) {
    const double k = std::atof(label.c_str() + 2);
    if (k < 500) return false;
    out = planck(k);
  } else {
    return false;
  }
  normaliseMean(out);
  return true;
}

Vec3 xyzOfWhite(double x, double y) { return {x / y, 1.0, (1.0 - x - y) / y}; }

Vec3 illuminantXyz(const Spec& e) {
  Vec3 xyz{0, 0, 0};
  double n = 0;
  for (int i = 0; i < kN; ++i) {
    for (int c = 0; c < 3; ++c) xyz[c] += e[i] * cmf(c)[i];
    n += e[i] * cmf(1)[i];
  }
  for (double& v : xyz) v /= n;
  return xyz;
}

void xyOfXyz(const Vec3& v, double& x, double& y) {
  const double s = v[0] + v[1] + v[2];
  x = v[0] / s;
  y = v[1] / s;
}

const Mat3& cat16() {
  static const Mat3 m{0.401288, 0.650173, -0.051461, -0.250268, 1.204414, 0.045854, -0.002079, 0.048952, 0.953127};
  return m;
}
const Mat3& cat02() {
  static const Mat3 m{0.7328, 0.4296, -0.1624, -0.7036, 1.6975, 0.0061, 0.0030, 0.0136, 0.9834};
  return m;
}
const Mat3& prophotoToXyz() {
  static const Mat3 m{0.7977, 0.1352, 0.0313, 0.288, 0.7119, 0.0001, 0, 0, 0.8249};
  return m;
}
const Mat3& srgbToXyz() {
  static const Mat3 m{0.4124, 0.3576, 0.1805, 0.2126, 0.7152, 0.0722, 0.0193, 0.1192, 0.9505};
  return m;
}
const Mat3& xyzToSrgb() {
  static const Mat3 m{3.2406, -1.5372, -0.4986, -0.9689, 1.8758, 0.0415, 0.0557, -0.2040, 1.0570};
  return m;
}

Mat3 adaptationMatrix(const Mat3& cone, double xs, double ys, double xd, double yd) {
  const Vec3 rs = mul(cone, xyzOfWhite(xs, ys)), rd = mul(cone, xyzOfWhite(xd, yd));
  return mul(inverse(cone), mul(diag({rd[0] / rs[0], rd[1] / rs[1], rd[2] / rs[2]}), cone));
}

void dichroicBase(Spec& fc, Spec& fm, Spec& fy) {
  for (int i = 0; i < kN; ++i) {
    const double l = wavelength(i);
    fy[i] = 0.5 * std::erf((l - 516.0) / 12.0) + 0.5;
    fm[i] = 0.5 * (l <= 550.0 ? -std::erf((l - 500.0) / 8.0) : std::erf((l - 610.0) / 8.0)) + 0.5;
    fc[i] = 0.5 * -std::erf((l - 607.0) / 8.0) + 0.5;
  }
}

double srgbOetf(double v) { return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055; }
double srgbEotf(double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }

// ---- CAM16-UCS ------------------------------------------------------------------------------------------

Cam16::Cam16() {
  const double LA = 64.0, Yb = 20.0, Yw = 100.0, F = 1.0;
  c = 0.69;
  nc = 1.0;
  const double k = 1.0 / (5.0 * LA + 1.0), k4 = k * k * k * k;
  Fl = 0.2 * k4 * (5.0 * LA) + 0.1 * (1 - k4) * (1 - k4) * std::cbrt(5.0 * LA);
  n = Yb / Yw;
  z = 1.48 + std::sqrt(n);
  nbb = ncb = 0.725 * std::pow(n, -0.2);
  D = F * (1.0 - (1.0 / 3.6) * std::exp(-(LA + 42.0) / 92.0));
  Vec3 xyzw = xyzOfWhite(kD65x, kD65y);
  for (double& v : xyzw) v *= 100.0;
  rgbw = mul(cat16(), xyzw);
  for (int i = 0; i < 3; ++i) drgb[i] = D * Yw / rgbw[i] + 1.0 - D;
  double ra[3];
  for (int i = 0; i < 3; ++i) {
    const double x = std::pow(Fl * std::fabs(drgb[i] * rgbw[i]) / 100.0, 0.42);
    ra[i] = 400.0 * x / (27.13 + x) + 0.1;
  }
  Aw = (2 * ra[0] + ra[1] + ra[2] / 20.0 - 0.305) * nbb;
}

static constexpr double kC1 = 0.007, kC2 = 0.0228;

void Cam16::forward(const Vec3& xyz01, double& Jp, double& ap, double& bp) const {
  const Vec3 xyz{xyz01[0] * 100, xyz01[1] * 100, xyz01[2] * 100};
  const Vec3 rgb = mul(cat16(), xyz);
  double ra[3];
  for (int i = 0; i < 3; ++i) {
    const double v = drgb[i] * rgb[i];
    const double x = std::pow(Fl * std::fabs(v) / 100.0, 0.42);
    ra[i] = (v < 0 ? -400.0 : 400.0) * x / (27.13 + x) + 0.1;
  }
  const double a = ra[0] - 12.0 * ra[1] / 11.0 + ra[2] / 11.0;
  const double b = (ra[0] + ra[1] - 2.0 * ra[2]) / 9.0;
  const double h = std::atan2(b, a);
  const double A = (2 * ra[0] + ra[1] + ra[2] / 20.0 - 0.305) * nbb;
  const double J = A > 0 ? 100.0 * std::pow(A / Aw, c * z) : 0.0;
  const double et = (std::cos(h + 2.0) + 3.8) / 4.0;
  const double den = ra[0] + ra[1] + 21.0 * ra[2] / 20.0;
  const double t = den != 0 ? (50000.0 / 13.0 * nc * ncb) * et * std::hypot(a, b) / den : 0.0;
  const double C = J > 0 ? std::pow(t, 0.9) * std::sqrt(J / 100.0) * std::pow(1.64 - std::pow(0.29, n), 0.73) : 0.0;
  const double M = C * std::pow(Fl, 0.25);
  Jp = (1.0 + 100.0 * kC1) * J / (1.0 + kC1 * J);
  const double Mp = std::log(1.0 + kC2 * M) / kC2;
  ap = Mp * std::cos(h);
  bp = Mp * std::sin(h);
}

Vec3 Cam16::inverse(double Jp, double ap, double bp) const {
  const double Jd = 1.0 + 100.0 * kC1 - kC1 * Jp;
  const double J = Jp > 0 && Jd > 0 ? Jp / Jd : 0.0;
  const double Mp = std::hypot(ap, bp);
  const double h = std::atan2(bp, ap);
  const double M = (std::exp(kC2 * Mp) - 1.0) / kC2;
  const double C = M / std::pow(Fl, 0.25);
  const double sj = std::sqrt(J / 100.0) * std::pow(1.64 - std::pow(0.29, n), 0.73);
  const double t = sj > 0 && C > 0 ? std::pow(C / sj, 1.0 / 0.9) : 0.0;
  const double A = Aw * std::pow(J / 100.0, 1.0 / (c * z));
  const double et = (std::cos(h + 2.0) + 3.8) / 4.0;
  const double p2 = A / nbb + 0.305;
  const double p3 = 21.0 / 20.0;
  double a = 0, b = 0;
  if (t > 0) {
    const double p1 = (50000.0 / 13.0 * nc * ncb) * et / t;
    const double sh = std::sin(h), ch = std::cos(h);
    if (std::fabs(sh) >= std::fabs(ch)) {
      const double p4 = p1 / sh;
      b = p2 * (2.0 + p3) * (460.0 / 1403.0) /
          (p4 + (2.0 + p3) * (220.0 / 1403.0) * (ch / sh) - 27.0 / 1403.0 + p3 * (6300.0 / 1403.0));
      a = b * ch / sh;
    } else {
      const double p5 = p1 / ch;
      a = p2 * (2.0 + p3) * (460.0 / 1403.0) /
          (p5 + (2.0 + p3) * (220.0 / 1403.0) - (27.0 / 1403.0 - p3 * (6300.0 / 1403.0)) * (sh / ch));
      b = a * sh / ch;
    }
  }
  const double ra[3] = {(460.0 * p2 + 451.0 * a + 288.0 * b) / 1403.0, (460.0 * p2 - 891.0 * a - 261.0 * b) / 1403.0,
                        (460.0 * p2 - 220.0 * a - 6300.0 * b) / 1403.0};
  Vec3 rgb;
  for (int i = 0; i < 3; ++i) {
    const double u = ra[i] - 0.1;
    const double au = std::fabs(u);
    const double x = au < 400.0 ? 27.13 * au / (400.0 - au) : 1e9;
    const double rc = (u < 0 ? -1.0 : 1.0) * (100.0 / Fl) * std::pow(x, 1.0 / 0.42);
    rgb[i] = rc / drgb[i];
  }
  static const Mat3 cinv = detail::inverse(cat16());
  const Vec3 xyz = mul(cinv, rgb);
  return {xyz[0] / 100.0, xyz[1] / 100.0, xyz[2] / 100.0};
}

static double knee(double x, double t, double l, double p) {
  if (x <= t) return x;
  const double s = l - t, n = (x - t) / s;
  return t + s * n / std::pow(1.0 + std::pow(n, p), 1.0 / p);
}

static double cmaxLookup(const CmaxTable& tab, double Jp, double h) {
  const double jc = std::min(std::max(Jp, 1.0), 110.0);
  const double jf = (jc - 1.0) / 109.0 * (tab.nJ - 1);
  int j0 = std::min(std::max(int(std::floor(jf)), 0), tab.nJ - 2);
  const double fj = jf - j0;
  double hf = (h + kPi) / (2.0 * kPi / tab.nH);
  const double hfl = std::floor(hf);
  const double fh = hf - hfl;
  int h0 = int(hfl) % tab.nH;
  if (h0 < 0) h0 += tab.nH;
  const int h1 = (h0 + 1) % tab.nH;
  auto at = [&](int j, int hh) { return double(tab.data[std::size_t(j) * tab.nH + hh]); };
  const double a = at(j0, h0) * (1 - fh) + at(j0, h1) * fh;
  const double b = at(j0 + 1, h0) * (1 - fh) + at(j0 + 1, h1) * fh;
  return a * (1 - fj) + b * fj;
}

Vec3 compressOutput(const Cam16& cam, const CmaxTable& tab, const Vec3& rgb) {
  const Vec3 xyz = mul(srgbToXyz(), rgb);
  double J, a, b;
  cam.forward(xyz, J, a, b);
  const double jn = knee(J / 100.0, 0.7, 1.0, 2.2);
  const double Jc = 100.0 * jn;
  const double C = std::hypot(a, b), h = std::atan2(b, a);
  const double cmax = std::max(cmaxLookup(tab, Jc, h), 1e-9);
  const double d = C / cmax;
  const double Cn = knee(d, 0.0, 1.0, 6.0) * cmax;
  const Vec3 xo = cam.inverse(Jc, Cn * std::cos(h), Cn * std::sin(h));
  return mul(xyzToSrgb(), xo);
}

CmaxTable buildCmaxTable() {
  CmaxTable t;
  t.data.assign(std::size_t(t.nJ) * t.nH, 0.f);
  static const Cam16 cam;
  parallelFor(
      t.nJ,
      [&](int j) {
        const double Jp = 1.0 + 109.0 * j / (t.nJ - 1);
        for (int hi = 0; hi < t.nH; ++hi) {
          const double h = -kPi + 2.0 * kPi * hi / t.nH;
          const double ch = std::cos(h), sh = std::sin(h);
          double lo = 0, hiC = 150.0;
          for (int it = 0; it < 18; ++it) {
            const double mid = 0.5 * (lo + hiC);
            const Vec3 rgb = mul(xyzToSrgb(), cam.inverse(Jp, mid * ch, mid * sh));
            const bool inside = rgb[0] >= -1e-6 && rgb[0] <= 1 + 1e-6 && rgb[1] >= -1e-6 && rgb[1] <= 1 + 1e-6 &&
                                rgb[2] >= -1e-6 && rgb[2] <= 1 + 1e-6;
            (inside ? lo : hiC) = mid;
          }
          t.data[std::size_t(j) * t.nH + hi] = float(lo);
        }
      },
      1);
  return t;
}

}  // namespace dapple::film::detail
