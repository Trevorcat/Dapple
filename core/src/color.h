#pragma once
// Internal colour math shared by the renderer and the analysis code.

#include <algorithm>
#include <cmath>

namespace dapple::color {

struct Mat3 {
  float m[3][3];
  void apply(const float in[3], float out[3]) const {
    const float r = in[0], g = in[1], b = in[2];
    out[0] = m[0][0] * r + m[0][1] * g + m[0][2] * b;
    out[1] = m[1][0] * r + m[1][1] * g + m[1][2] * b;
    out[2] = m[2][0] * r + m[2][1] * g + m[2][2] * b;
  }
};

inline Mat3 mul(const Mat3& a, const Mat3& b) {
  Mat3 r{};
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      double s = 0;
      for (int k = 0; k < 3; ++k) s += double(a.m[i][k]) * b.m[k][j];
      r.m[i][j] = float(s);
    }
  return r;
}

// ProPhoto (ROMM) RGB -> XYZ D50
inline constexpr Mat3 kProPhotoToXyzD50{{{0.7976749f, 0.1351917f, 0.0313534f},
                                         {0.2880402f, 0.7118741f, 0.0000857f},
                                         {0.0000000f, 0.0000000f, 0.8252100f}}};
// Bradford D50 -> D65
inline constexpr Mat3 kD50ToD65{{{0.9555766f, -0.0230393f, 0.0631636f},
                                 {-0.0282895f, 1.0099416f, 0.0210077f},
                                 {0.0122982f, -0.0204830f, 1.3299098f}}};
// XYZ D65 -> linear sRGB
inline constexpr Mat3 kXyzD65ToSrgb{{{3.2404542f, -1.5371385f, -0.4985314f},
                                     {-0.9692660f, 1.8760108f, 0.0415560f},
                                     {0.0556434f, -0.2040259f, 1.0572252f}}};
// linear sRGB -> XYZ D65
inline constexpr Mat3 kSrgbToXyzD65{{{0.4124564f, 0.3575761f, 0.1804375f},
                                     {0.2126729f, 0.7151522f, 0.0721750f},
                                     {0.0193339f, 0.1191920f, 0.9503041f}}};
// Bradford D65 -> D50
inline constexpr Mat3 kD65ToD50{{{1.0478112f, 0.0228866f, -0.0501270f},
                                 {0.0295424f, 0.9904844f, -0.0170491f},
                                 {-0.0092345f, 0.0150436f, 0.7521316f}}};
// XYZ D50 -> ProPhoto
inline constexpr Mat3 kXyzD50ToProPhoto{{{1.3459433f, -0.2556075f, -0.0511118f},
                                         {-0.5445989f, 1.5081673f, 0.0205351f},
                                         {0.0000000f, 0.0000000f, 1.2118128f}}};

inline const Mat3& proPhotoToSrgb() {
  static const Mat3 m = mul(kXyzD65ToSrgb, mul(kD50ToD65, kProPhotoToXyzD50));
  return m;
}
inline const Mat3& srgbToProPhoto() {
  static const Mat3 m = mul(kXyzD50ToProPhoto, mul(kD65ToD50, kSrgbToXyzD65));
  return m;
}

inline float proPhotoLuma(const float* rgb) {
  return 0.2880402f * rgb[0] + 0.7118741f * rgb[1] + 0.0000857f * rgb[2];
}
inline float srgbLuma(const float* rgb) {
  return 0.2126729f * rgb[0] + 0.7151522f * rgb[1] + 0.0721750f * rgb[2];
}

inline float srgbEncode(float x) {
  x = std::clamp(x, 0.f, 1.f);
  return x <= 0.0031308f ? 12.92f * x : 1.055f * std::pow(x, 1.f / 2.4f) - 0.055f;
}
inline float srgbDecode(float x) {
  return x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f);
}

// OKLab (Björn Ottosson) from/to linear sRGB. Works for out-of-gamut values.
inline void linearSrgbToOklab(const float in[3], float out[3]) {
  const float l = 0.4122214708f * in[0] + 0.5363325363f * in[1] + 0.0514459929f * in[2];
  const float m = 0.2119034982f * in[0] + 0.6806995451f * in[1] + 0.1073969566f * in[2];
  const float s = 0.0883024619f * in[0] + 0.2817188376f * in[1] + 0.6299787005f * in[2];
  const float l_ = std::cbrt(l), m_ = std::cbrt(m), s_ = std::cbrt(s);
  out[0] = 0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_;
  out[1] = 1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_;
  out[2] = 0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_;
}
inline void oklabToLinearSrgb(const float in[3], float out[3]) {
  const float l_ = in[0] + 0.3963377774f * in[1] + 0.2158037573f * in[2];
  const float m_ = in[0] - 0.1055613458f * in[1] - 0.0638541728f * in[2];
  const float s_ = in[0] - 0.0894841775f * in[1] - 1.2914855480f * in[2];
  const float l = l_ * l_ * l_, m = m_ * m_ * m_, s = s_ * s_ * s_;
  out[0] = 4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
  out[1] = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
  out[2] = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;
}

inline float smoothstep(float e0, float e1, float x) {
  const float t = std::clamp((x - e0) / (e1 - e0), 0.f, 1.f);
  return t * t * (3.f - 2.f * t);
}

constexpr float kPi = 3.14159265358979f;

}  // namespace dapple::color
