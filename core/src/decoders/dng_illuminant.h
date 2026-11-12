#pragma once
// As-shot illuminant from the camera neutral and the DNG calibration matrices: DNG specification 1.6 chapter 6 ("Translating Camera
// Neutral Coordinates to White Balance xy Coordinates"). Header-only, no LibRaw dependency, unit-tested with a synthetic forward model.
//
//   XYZtoCamera(T) = AB * CC(T) * CM(T)      CM / CC interpolated between the two calibration illuminants by inverse CCT (mired)
//   XYZ = inverse(XYZtoCamera) * CameraNeutral, xy = chromaticity of XYZ, iterate until the xy values converge
// With one calibration illuminant the matrix does not depend on the guess and the loop ends after one step (the old matrix-only estimate).

#include <algorithm>
#include <cmath>

#include "dapple/illuminant.h"

namespace dapple {

// CCT (K) of an EXIF LightSource / CalibrationIlluminant code, 0 = unknown / not a temperature.
inline double illuminantCodeCct(int code) {
  switch (code) {
    case 1: case 9: case 20: return 5500.0;                          // daylight, fine weather, D55
    case 2: return 4150.0;                                           // fluorescent
    case 3: case 17: return 2856.0;                                  // tungsten, standard illuminant A
    case 4: return 5500.0;                                           // flash
    case 10: return 6500.0;                                          // cloudy
    case 11: case 22: return 7500.0;                                 // shade, D75
    case 12: return 5700.0;                                          // daylight fluorescent
    case 13: return 4600.0;                                          // day white fluorescent
    case 14: return 3500.0;                                          // cool white fluorescent
    case 15: return 3000.0;                                          // white fluorescent
    case 18: return 4874.0;                                          // standard illuminant B
    case 19: return 6774.0;                                          // standard illuminant C
    case 21: return 6504.0;                                          // D65
    case 23: return 5003.0;                                          // D50
    case 24: return 3200.0;                                          // ISO studio tungsten
    default: return 0.0;
  }
}

struct DngCalibration {
  double cm[9];                                      // XYZ -> camera, row major 3 x 3
  double cc[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};        // CameraCalibration
  double cct = 0;                                    // of the calibration illuminant (K)
};

namespace detail {
inline bool invert3(const double m[9], double out[9]) {
  const double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], k = m[8];
  const double det = a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);
  if (std::fabs(det) < 1e-12) return false;
  out[0] = (e * k - f * h) / det, out[1] = (c * h - b * k) / det, out[2] = (b * f - c * e) / det;
  out[3] = (f * g - d * k) / det, out[4] = (a * k - c * g) / det, out[5] = (c * d - a * f) / det;
  out[6] = (d * h - e * g) / det, out[7] = (b * g - a * h) / det, out[8] = (a * e - b * d) / det;
  return true;
}
inline void mul3(const double a[9], const double b[9], double out[9]) {
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) out[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
}
// XYZtoCamera = AB * CC * CM for the interpolation weight w of the first calibration (second gets 1 - w)
inline void dngXyzToCamera(const DngCalibration* c, int n, double w, const double ab[3], double out[9]) {
  double cm[9], cc[9];
  for (int i = 0; i < 9; ++i) {
    cm[i] = n == 2 ? w * c[0].cm[i] + (1 - w) * c[1].cm[i] : c[0].cm[i];
    cc[i] = n == 2 ? w * c[0].cc[i] + (1 - w) * c[1].cc[i] : c[0].cc[i];
  }
  double t[9];
  mul3(cc, cm, t);
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) out[i * 3 + j] = ab[i] * t[i * 3 + j];
}
}  // namespace detail

// Camera neutral (the camera RGB of a white object under the unknown illuminant, as in AsShotNeutral) -> illuminant xy.
// `c` holds one or two calibrations (two: the interpolation of the spec, weights by inverse CCT, clamped outside the two illuminants).
// Returns false when the matrices cannot be inverted or the neutral gives a non-physical chromaticity; `iters` reports the passes used.
inline bool solveIlluminantFromNeutral(const DngCalibration* c, int n, const double analogBalance[3], const double neutral[3], double& x, double& y,
                                       int* iters = nullptr) {
  if (n < 1 || n > 2 || !(neutral[0] > 0 && neutral[1] > 0 && neutral[2] > 0)) return false;
  double T1 = c[0].cct, T2 = n == 2 ? c[1].cct : 0.0;
  if (n == 2 && !(T1 > 0 && T2 > 0 && std::fabs(1.0 / T1 - 1.0 / T2) > 1e-9)) n = 1;  // illuminants indistinct: first calibration only
  double gx = 0.3457, gy = 0.3585;                                                   // D50
  for (int pass = 0; pass < 40; ++pass) {
    double w = 1.0;
    if (n == 2) {
      const double T = std::clamp(xyToCct(gx, gy), 1500.0, 20000.0);
      w = std::clamp((1.0 / T - 1.0 / T2) / (1.0 / T1 - 1.0 / T2), 0.0, 1.0);
    }
    double m[9], inv[9];
    detail::dngXyzToCamera(c, n, w, analogBalance, m);
    if (!detail::invert3(m, inv)) return false;
    const double X = inv[0] * neutral[0] + inv[1] * neutral[1] + inv[2] * neutral[2];
    const double Y = inv[3] * neutral[0] + inv[4] * neutral[1] + inv[5] * neutral[2];
    const double Z = inv[6] * neutral[0] + inv[7] * neutral[1] + inv[8] * neutral[2];
    const double sum = X + Y + Z;
    if (!(sum > 1e-9) || !(X > 0 && Y > 0 && Z > 0)) return false;
    double nx = X / sum, ny = Y / sum;
    const bool done = std::fabs(nx - gx) + std::fabs(ny - gy) < 1e-7;
    if (pass > 20) nx = 0.5 * (nx + gx), ny = 0.5 * (ny + gy);  // damp a rare 2-cycle (the reference implementation does the same)
    gx = nx, gy = ny;
    if (done) {
      if (iters) *iters = pass + 1;
      break;
    }
    if (iters) *iters = pass + 1;
  }
  x = gx, y = gy;
  return true;
}

}  // namespace dapple
