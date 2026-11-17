// The film's reading of a display-referred source: the inverse of a typical camera rendering (core/src/film_kernels.h displayInverseApply, calibrated in
// core/src/film_display_ref.cpp). Included by film_point.comp (FilmExposure) and film_skin.comp (the skin cap's reference). Twin of the C++: keep identical.
#extension GL_GOOGLE_include_directive : enable
// scene luminance at sqrt(Y) = i / 64 (film_display_table.inc is the one list both languages read)
const float kDrTone[65] = float[65](
#include "film_display_table.inc"
);
const vec4 kDrChroma = vec4(0.85, 1.0, 0.15, 0.65);  // k0, k1, e0, e1 (DisplayInverse::k0 ...)

vec3 drInverse(vec3 rgb) {
  const float Y = 0.2880402 * rgb.x + 0.7118741 * rgb.y + 0.0000857 * rgb.z;
  if (!(Y > 1e-9)) return rgb;
  float gY;
  if (Y >= 1.0) {
    gY = kDrTone[64] * Y;
  } else {
    const float pos = sqrt(Y) * 64.0;
    const int i = min(int(pos), 63);
    const float f = pos - float(i);
    gY = kDrTone[i] + f * (kDrTone[i + 1] - kDrTone[i]);
  }
  const float t = clamp((Y - kDrChroma.z) / (kDrChroma.w - kDrChroma.z), 0.0, 1.0);
  const float k = kDrChroma.x + (kDrChroma.y - kDrChroma.x) * (t * t * (3.0 - 2.0 * t));
  return vec3(gY * (1.0 - k)) + rgb * (gY / Y * k);
}
