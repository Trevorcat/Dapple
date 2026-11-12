#pragma once
// Correlated colour temperature <-> CIE xy helpers (scene illuminants, docs/engine/pipeline-architecture.md section 18).
#include <cmath>

namespace dapple {

// CIE xy of a correlated colour temperature: the Planckian locus below 4000 K (Kang et al.), the CIE daylight locus above (D-series fit).
inline void cctToXy(double T, double& x, double& y) {
  if (T < 4000.0) {
    x = -0.2661239e9 / (T * T * T) - 0.2343589e6 / (T * T) + 0.8776956e3 / T + 0.179910;
    y = T < 2222.0 ? -1.1063814 * x * x * x - 1.34811020 * x * x + 2.18555832 * x - 0.20219683
                   : -0.9549476 * x * x * x - 1.37418593 * x * x + 2.09137015 * x - 0.16748867;
  } else {
    x = T <= 7000.0 ? 0.244063 + 0.09911e3 / T + 2.9678e6 / (T * T) - 4.6070e9 / (T * T * T)
                    : 0.237040 + 0.24748e3 / T + 1.9018e6 / (T * T) - 2.0064e9 / (T * T * T);
    y = -3.0 * x * x + 2.870 * x - 0.275;
  }
}

// Correlated colour temperature of a chromaticity (McCamy).
inline double xyToCct(double x, double y) {
  const double n = (x - 0.3320) / (0.1858 - y);
  return 449.0 * n * n * n + 3525.0 * n * n + 6823.3 * n + 5520.33;
}

}  // namespace dapple
