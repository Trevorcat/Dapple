// The inverse camera rendering a display-referred source gets in front of a film stock (film_kernels.h DisplayInverse).
//
// Calibration (2026-11-17, our own engine; scratch scripts were build/band/cal): four public RAWs (Canon R10 portrait with skin, Canon R5 indoor still life,
// Sony A6700 landscape, Canon 5D IV with a colour chart) were developed without a film into 8-bit sRGB (our standard rendering standing in for a camera's) and
// the linear luminance of that picture was paired with the linear luminance of the RAW decode, pixel by pixel (4x4 block means at 800 px); the curve is the
// per-image-offset least-squares fit of scene log2 Y against display log2 Y over 1-stop bins. The same fit on the two Canon embedded camera JPEGs (a real camera
// rendering: a steeper S curve, chroma x1.4 in the shadows and mid-tones) gives the same shape with slopes 0.8 0.6 0.6 0.5 0.7 0.7 1.25 1.8 against ours
// 0.7 0.7 0.7 0.7 0.9 0.9 1.0 1.8; the table is the mean of the two (a camera JPEG is what the app mostly meets, our own rendering is what the engine
// produces). The chroma factor is the regression slope of (rgb / Y - 1) of the RAW on that of the display picture: 1.0 for our rendering, 0.7 (shadows,
// mid-tones) to 1.0 (highlights) for the Canon JPEGs; 0.85 .. 1.0 is a compromise chosen on film-on-RAW against film-on-JPEG (k = 1: our rendering's chroma
// ratio 0.947 but the camera JPEG's 1.079; k = 0.85: 0.893 / 1.003; k = 0.7: 0.831 / 0.914). The curve is anchored: 0.18 -> 0.18 (the knots below are log2 of the
// scene luminance in that scale). film_display_table.inc is that curve sampled (65 values at sqrt(Y) = i / 64) and is what both the C++ and the shaders read;
// buildDisplayInverseReference() rebuilds it from the knots and the unit test (film_source.display_table) holds the two together.
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "film_kernels.h"

namespace dapple::film::detail {

namespace {
// log2 of the display-linear luminance (knots, one stop apart) and log2 of the scene luminance relative to the 0.18 anchor
constexpr int kKnots = 9;
constexpr double kX[kKnots] = {-8, -7, -6, -5, -4, -3, -2, -1, 0};
constexpr double kH[kKnots] = {-6.348, -5.597, -4.952, -4.320, -3.689, -2.907, -2.084, -0.967, 0.818};

// monotone cubic (Fritsch-Carlson) through the knots; linear extrapolation below the first knot with the first slope
double curve(double x) {
  double d[kKnots - 1], m[kKnots];
  for (int i = 0; i < kKnots - 1; ++i) d[i] = (kH[i + 1] - kH[i]) / (kX[i + 1] - kX[i]);
  m[0] = d[0];
  m[kKnots - 1] = d[kKnots - 2];
  for (int i = 1; i < kKnots - 1; ++i) m[i] = d[i - 1] * d[i] <= 0 ? 0 : 0.5 * (d[i - 1] + d[i]);
  for (int i = 0; i < kKnots - 1; ++i) {
    if (d[i] == 0) {
      m[i] = m[i + 1] = 0;
      continue;
    }
    const double a = m[i] / d[i], b = m[i + 1] / d[i], s = a * a + b * b;
    if (s > 9) {
      const double t = 3 / std::sqrt(s);
      m[i] = t * a * d[i];
      m[i + 1] = t * b * d[i];
    }
  }
  if (x <= kX[0]) return kH[0] + d[0] * (x - kX[0]);
  if (x >= kX[kKnots - 1]) return kH[kKnots - 1] + d[kKnots - 2] * (x - kX[kKnots - 1]);
  int i = 0;
  while (x > kX[i + 1]) ++i;
  const double h = kX[i + 1] - kX[i], t = (x - kX[i]) / h, t2 = t * t, t3 = t2 * t;
  return (2 * t3 - 3 * t2 + 1) * kH[i] + (t3 - 2 * t2 + t) * h * m[i] + (-2 * t3 + 3 * t2) * kH[i + 1] + (t3 - t2) * h * m[i + 1];
}

DisplayInverse buildReference() {
  DisplayInverse d{};
  d.tone[0] = 0.f;
  for (int i = 1; i <= 64; ++i) {
    const double Y = double(i) * double(i) / 4096.0;
    d.tone[i] = float(std::exp2(curve(std::log2(Y))));
  }
  d.k0 = 0.85f, d.k1 = 1.0f, d.e0 = 0.15f, d.e1 = 0.65f;
  return d;
}
DisplayInverse fromTable() {
  // the list the shaders read (film_display_ref.glsl): film_display_table.inc, 65 values at sqrt(Y) = i / 64
  static const float kTone[65] = {
#include "../engine/shaders/film_display_table.inc"
  };
  DisplayInverse d{};
  std::copy(kTone, kTone + 65, d.tone);
  d.k0 = 0.85f, d.k1 = 1.0f, d.e0 = 0.15f, d.e1 = 0.65f;  // = kDrChroma in film_display_ref.glsl
  return d;
}
}  // namespace

DisplayInverse buildDisplayInverseReference() { return buildReference(); }

bool displayInverseEnabled() {
  static const bool on = [] {
    const char* e = std::getenv("DAPPLE_FILM_SRC_INVERSE");
    return !(e && e[0] == '0');
  }();
  return on;
}

const DisplayInverse& displayInverse() {
  static const DisplayInverse d = fromTable();
  return d;
}

}  // namespace dapple::film::detail
