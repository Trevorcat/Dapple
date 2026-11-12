#pragma once
// Delta E 2000 of linear ProPhoto (D50) pixels at a viewing exposure; shared by dapple_demosaic_eval and dapple_demosaic_tests.
#include <algorithm>
#include <cmath>

namespace demo {
struct Lab { float L, a, b; };
constexpr double kPi = 3.14159265358979323846;

inline Lab toLab(const float* p, float g) {
  const float r = std::clamp(p[0] * g, 0.f, 4.f), gg = std::clamp(p[1] * g, 0.f, 4.f), b = std::clamp(p[2] * g, 0.f, 4.f);
  const float X = 0.7977f * r + 0.1352f * gg + 0.0313f * b, Y = 0.2880f * r + 0.7119f * gg + 0.0001f * b, Z = 0.8249f * b;
  auto f = [](float t) { return t > 0.008856f ? std::cbrt(t) : 7.787f * t + 16.f / 116.f; };
  const float fx = f(X / 0.9642f), fy = f(Y), fz = f(Z / 0.8249f);
  return {116.f * fy - 16.f, 500.f * (fx - fy), 200.f * (fy - fz)};
}

inline double dE2000(const Lab& p, const Lab& q) {
  const double L1 = p.L, a1 = p.a, b1 = p.b, L2 = q.L, a2 = q.a, b2 = q.b;
  const double C1 = std::hypot(a1, b1), C2 = std::hypot(a2, b2), Cm = 0.5 * (C1 + C2);
  const double G = 0.5 * (1.0 - std::sqrt(std::pow(Cm, 7) / (std::pow(Cm, 7) + std::pow(25.0, 7))));
  const double ap1 = (1 + G) * a1, ap2 = (1 + G) * a2, Cp1 = std::hypot(ap1, b1), Cp2 = std::hypot(ap2, b2);
  auto hue = [](double b, double a) { double h = std::atan2(b, a) * 180.0 / kPi; return h < 0 ? h + 360 : h; };
  const double hp1 = Cp1 == 0 ? 0 : hue(b1, ap1), hp2 = Cp2 == 0 ? 0 : hue(b2, ap2);
  const double dL = L2 - L1, dC = Cp2 - Cp1;
  double dh = 0;
  if (Cp1 * Cp2 != 0) {
    dh = hp2 - hp1;
    if (dh > 180) dh -= 360;
    else if (dh < -180) dh += 360;
  }
  const double dH = 2 * std::sqrt(Cp1 * Cp2) * std::sin(dh * kPi / 360.0);
  const double Lm = 0.5 * (L1 + L2), Cpm = 0.5 * (Cp1 + Cp2);
  double hm = hp1 + hp2;
  if (Cp1 * Cp2 == 0) hm = hp1 + hp2;
  else if (std::fabs(hp1 - hp2) <= 180) hm *= 0.5;
  else hm = (hp1 + hp2 < 360) ? 0.5 * (hm + 360) : 0.5 * (hm - 360);
  const double T = 1 - 0.17 * std::cos((hm - 30) * kPi / 180) + 0.24 * std::cos(2 * hm * kPi / 180) + 0.32 * std::cos((3 * hm + 6) * kPi / 180) - 0.20 * std::cos((4 * hm - 63) * kPi / 180);
  const double dth = 30 * std::exp(-std::pow((hm - 275) / 25, 2));
  const double Rc = 2 * std::sqrt(std::pow(Cpm, 7) / (std::pow(Cpm, 7) + std::pow(25.0, 7)));
  const double Sl = 1 + 0.015 * std::pow(Lm - 50, 2) / std::sqrt(20 + std::pow(Lm - 50, 2)), Sc = 1 + 0.045 * Cpm, Sh = 1 + 0.015 * Cpm * T;
  const double Rt = -std::sin(2 * dth * kPi / 180) * Rc;
  return std::sqrt(std::pow(dL / Sl, 2) + std::pow(dC / Sc, 2) + std::pow(dH / Sh, 2) + Rt * (dC / Sc) * (dH / Sh));
}
}  // namespace demo
