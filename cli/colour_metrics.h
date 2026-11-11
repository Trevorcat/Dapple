#pragma once
// Colour metrics for the evaluation / calibration tools: sRGB -> CIELAB (D65),
// CIEDE2000, hue sectors. Double precision, header only. Not used by the core.

#include <algorithm>
#include <cmath>

namespace dapple::metrics {

struct Lab {
  double L = 0, a = 0, b = 0;
  double C() const { return std::hypot(a, b); }
  double h() const {  // degrees 0..360
    double d = std::atan2(b, a) * 180.0 / 3.14159265358979323846;
    return d < 0 ? d + 360.0 : d;
  }
};

inline double srgbToLinear(double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }

// `rgb` are sRGB-encoded values in 0..1.
inline Lab srgbToLab(const float rgb[3]) {
  const double r = srgbToLinear(std::clamp<double>(rgb[0], 0, 1));
  const double g = srgbToLinear(std::clamp<double>(rgb[1], 0, 1));
  const double b = srgbToLinear(std::clamp<double>(rgb[2], 0, 1));
  const double X = (0.4124564 * r + 0.3575761 * g + 0.1804375 * b) / 0.95047;
  const double Y = 0.2126729 * r + 0.7151522 * g + 0.0721750 * b;
  const double Z = (0.0193339 * r + 0.1191920 * g + 0.9503041 * b) / 1.08883;
  auto f = [](double t) { return t > 216.0 / 24389.0 ? std::cbrt(t) : (24389.0 / 27.0 * t + 16.0) / 116.0; };
  const double fx = f(X), fy = f(Y), fz = f(Z);
  return {116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
}

inline double deltaE2000(const Lab& p, const Lab& q) {
  constexpr double kPi = 3.14159265358979323846;
  auto rad = [](double d) { return d * kPi / 180.0; };
  auto deg = [](double r) { return r * 180.0 / kPi; };
  const double C1 = p.C(), C2 = q.C();
  const double Cb = (C1 + C2) / 2;
  const double Cb7 = std::pow(Cb, 7);
  const double G = 0.5 * (1 - std::sqrt(Cb7 / (Cb7 + 6103515625.0)));  // 25^7
  const double a1 = (1 + G) * p.a, a2 = (1 + G) * q.a;
  const double C1p = std::hypot(a1, p.b), C2p = std::hypot(a2, q.b);
  auto hp = [&](double b, double a) {
    if (a == 0 && b == 0) return 0.0;
    double h = deg(std::atan2(b, a));
    return h < 0 ? h + 360 : h;
  };
  const double h1 = hp(p.b, a1), h2 = hp(q.b, a2);
  const double dL = q.L - p.L, dC = C2p - C1p;
  double dh = 0;
  if (C1p * C2p != 0) {
    dh = h2 - h1;
    if (dh > 180) dh -= 360;
    else if (dh < -180) dh += 360;
  }
  const double dH = 2 * std::sqrt(C1p * C2p) * std::sin(rad(dh / 2));
  const double Lb = (p.L + q.L) / 2, Cbp = (C1p + C2p) / 2;
  double hb = h1 + h2;
  if (C1p * C2p == 0) {
    hb = h1 + h2;
  } else if (std::fabs(h1 - h2) <= 180) {
    hb = (h1 + h2) / 2;
  } else {
    hb = (h1 + h2 + (h1 + h2 < 360 ? 360 : -360)) / 2;
  }
  const double T = 1 - 0.17 * std::cos(rad(hb - 30)) + 0.24 * std::cos(rad(2 * hb)) + 0.32 * std::cos(rad(3 * hb + 6)) -
                   0.20 * std::cos(rad(4 * hb - 63));
  const double dTheta = 30 * std::exp(-std::pow((hb - 275) / 25, 2));
  const double Cbp7 = std::pow(Cbp, 7);
  const double Rc = 2 * std::sqrt(Cbp7 / (Cbp7 + 6103515625.0));
  const double Sl = 1 + 0.015 * (Lb - 50) * (Lb - 50) / std::sqrt(20 + (Lb - 50) * (Lb - 50));
  const double Sc = 1 + 0.045 * Cbp, Sh = 1 + 0.015 * Cbp * T;
  const double Rt = -std::sin(rad(2 * dTheta)) * Rc;
  const double tl = dL / Sl, tc = dC / Sc, th = dH / Sh;
  return std::sqrt(tl * tl + tc * tc + th * th + Rt * tc * th);
}

// Hue sectors (CIELAB hue angle of the camera pixel).
enum Sector { kReds, kOranges, kYellows, kGreens, kCyans, kBlues, kPurples, kSectorCount };
inline const char* sectorName(int s) {
  static const char* n[] = {"reds", "oranges", "yellows", "greens", "cyans", "blues", "purples"};
  return n[s];
}
inline int sectorOf(double hab) {
  if (hab >= 345 || hab < 35) return kReds;
  if (hab < 70) return kOranges;
  if (hab < 110) return kYellows;
  if (hab < 170) return kGreens;
  if (hab < 235) return kCyans;
  if (hab < 295) return kBlues;
  return kPurples;
}
inline double wrapDeg(double d) {
  while (d > 180) d -= 360;
  while (d < -180) d += 360;
  return d;
}

}  // namespace dapple::metrics
