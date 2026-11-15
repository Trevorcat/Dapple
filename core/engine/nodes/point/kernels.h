#pragma once
// Per-pixel kernels of the point family (docs/engine/pipeline-architecture.md section 50): the base tone curve, the colour stage
// (PointOp), the lattice sampler (LutSampler) and the tone table layout, the geometry resamplers, the display stages after a film.
// Internal header of the family (nodes/point/*.cpp, nodes/film/film_glue.cpp): plain inline code, so every caller inlines the per-pixel
// work as the single translation unit it came from did (core/src/point_kernels.cpp until stage B).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "color.h"
#include "dapple/film.h"
#include "dapple/look.h"
#include "dapple/modern.h"
#include "dapple/tone.h"
#include "engine/nodes/frame_facts.h"
#include "engine/nodes/point/point.h"
#include "look_internal.h"
#include "parallel.h"

namespace dapple::engine::point {

using namespace dapple::color;

constexpr float kMidGrey = 0.18f;

// Base tone curve of the scene-referred path, fitted to the median response of
// camera JPEGs (Sony ILCE-7RM5, 50 ARW/JPEG pairs; see docs/architecture.md):
// a toe that darkens deep shadows (in stops, at the bottom end) plus the
// highlight shoulder below. Mid-tone placement and slope come from the
// CameraProfile (baselineExposureEv / baselineContrast).
constexpr float kToeStops = 2.0f;
constexpr float kToeStart = -1.0f, kToeEnd = -6.5f;

// Highlight shoulder: identity below `knee`, then an exponential roll-off
// that approaches (but never reaches) 1. Applied to scene-referred luminance.
inline float shoulder(float x) {
  constexpr float knee = 0.3f;
  if (x <= knee) return x;
  return knee + (1.f - knee) * (1.f - std::exp(-(x - knee) / (1.f - knee)));
}

// Monotone cubic (Fritsch-Carlson) through Lightroom-style 0..255 points (kCurveSamples samples over [0, 1]).
std::vector<float> sampleMonotoneCurve(std::vector<CurvePoint> pts);
// Lightroom's region ("parametric") curve, approximated with four smooth bumps.
std::vector<float> sampleParametric(const float amt[4], float split1, float split2, float split3);

// Lightroom hue (HSL-style degrees: 0 red, 60 yellow, 120 green, 240 blue)
// to the corresponding OKLab hue angle in radians.
inline float lightroomHueToOklab(float deg) {
  static const float lr[] = {0, 60, 120, 180, 240, 300, 360};
  static const float ok[] = {29, 110, 142, 195, 264, 328, 389};
  deg = std::fmod(std::fmod(deg, 360.f) + 360.f, 360.f);
  int i = 0;
  while (i < 5 && deg > lr[i + 1]) ++i;
  const float t = (deg - lr[i]) / (lr[i + 1] - lr[i]);
  return (ok[i] + t * (ok[i + 1] - ok[i])) * kPi / 180.f;
}

// OKLab hue centres (degrees) of the eight HSL bands.
constexpr float kBandHue[kHslBandCount] = {20, 50, 95, 145, 195, 260, 300, 340};

inline void bandWeights(float hueDeg, float w[kHslBandCount]) {
  std::fill(w, w + kHslBandCount, 0.f);
  for (int i = 0; i < kHslBandCount; ++i) {
    const int j = (i + 1) % kHslBandCount;
    float a = kBandHue[i], b = kBandHue[j];
    if (b < a) b += 360;
    float h = hueDeg;
    if (h < a) h += 360;
    if (h >= a && h <= b) {
      const float t = smoothstep(0.f, 1.f, (h - a) / (b - a));
      w[i] = 1 - t;
      w[j] = t;
      return;
    }
  }
}

// Trilinear lookup of the camera matching LUT (OKLab offsets), see ColorMatchProfile.
inline void sampleColorMatch(const ColorMatchProfile& m, const float lab[3], float off[3]) {
  const float fl = std::clamp(lab[0], 0.f, 1.f) * float(m.lutL - 1);
  const float k = float(m.lutAB - 1) / (2.f * m.lutABMax);
  const float fa = (std::clamp(lab[1], -m.lutABMax, m.lutABMax) + m.lutABMax) * k;
  const float fb = (std::clamp(lab[2], -m.lutABMax, m.lutABMax) + m.lutABMax) * k;
  const int il = std::min(int(fl), m.lutL - 2), ia = std::min(int(fa), m.lutAB - 2), ib = std::min(int(fb), m.lutAB - 2);
  const float tl = fl - il, ta = fa - ia, tb = fb - ib;
  off[0] = off[1] = off[2] = 0.f;
  for (int dl = 0; dl < 2; ++dl)
    for (int db = 0; db < 2; ++db)
      for (int da = 0; da < 2; ++da) {
        const float w = (dl ? tl : 1 - tl) * (db ? tb : 1 - tb) * (da ? ta : 1 - ta);
        const float* n = &m.lut[(std::size_t((il + dl) * m.lutAB + (ib + db)) * m.lutAB + (ia + da)) * 3];
        off[0] += w * n[0];
        off[1] += w * n[1];
        off[2] += w * n[2];
      }
}


// Catmull-Rom sample of an RGB float image at continuous position (x, y).
inline void sampleCubic(const ImageF& im, float x, float y, float out[3]) {
  x -= 0.5f;
  y -= 0.5f;
  const float fxl = std::floor(x), fyl = std::floor(y);
  const int ix = int(fxl), iy = int(fyl);
  auto weights = [](float t, float w[4]) {
    const float t2 = t * t, t3 = t2 * t;
    w[0] = -0.5f * t3 + t2 - 0.5f * t;
    w[1] = 1.5f * t3 - 2.5f * t2 + 1.f;
    w[2] = -1.5f * t3 + 2.f * t2 + 0.5f * t;
    w[3] = 0.5f * t3 - 0.5f * t2;
  };
  float wx[4], wy[4];
  weights(x - fxl, wx);
  weights(y - fyl, wy);
  out[0] = out[1] = out[2] = 0.f;
  for (int j = 0; j < 4; ++j) {
    const float* row = im.row(std::clamp(iy - 1 + j, 0, im.height - 1));
    float acc[3] = {0.f, 0.f, 0.f};
    for (int i = 0; i < 4; ++i) {
      const float* px = row + 3 * std::clamp(ix - 1 + i, 0, im.width - 1);
      acc[0] += wx[i] * px[0];
      acc[1] += wx[i] * px[1];
      acc[2] += wx[i] * px[2];
    }
    for (int c = 0; c < 3; ++c) out[c] += wy[j] * acc[c];
  }
  for (int c = 0; c < 3; ++c) out[c] = std::max(0.f, out[c]);
}

// sampleCubic on a window of a W x H picture (window pixel (0, 0) = picture pixel (ox, oy)): the same taps, clamped to the picture's edges, the
// same arithmetic (streaming export, section 42). The window must hold every tap (point::CpuFrame::sourceRect).
inline void sampleCubicWin(const ImageF& win, int ox, int oy, int W, int H, float x, float y, float out[3]) {
  x -= 0.5f;
  y -= 0.5f;
  const float fxl = std::floor(x), fyl = std::floor(y);
  const int ix = int(fxl), iy = int(fyl);
  auto weights = [](float t, float w[4]) {
    const float t2 = t * t, t3 = t2 * t;
    w[0] = -0.5f * t3 + t2 - 0.5f * t;
    w[1] = 1.5f * t3 - 2.5f * t2 + 1.f;
    w[2] = -1.5f * t3 + 2.f * t2 + 0.5f * t;
    w[3] = 0.5f * t3 - 0.5f * t2;
  };
  float wx[4], wy[4];
  weights(x - fxl, wx);
  weights(y - fyl, wy);
  out[0] = out[1] = out[2] = 0.f;
  for (int j = 0; j < 4; ++j) {
    const float* row = win.row(std::clamp(iy - 1 + j, 0, H - 1) - oy);
    float acc[3] = {0.f, 0.f, 0.f};
    for (int i = 0; i < 4; ++i) {
      const float* px = row + 3 * (std::clamp(ix - 1 + i, 0, W - 1) - ox);
      acc[0] += wx[i] * px[0];
      acc[1] += wx[i] * px[1];
      acc[2] += wx[i] * px[2];
    }
    for (int c = 0; c < 3; ++c) out[c] += wy[j] * acc[c];
  }
  for (int c = 0; c < 3; ++c) out[c] = std::max(0.f, out[c]);
}

// Lanczos-3 sample (6 x 6 taps, normalised weights, clamped to the picture's edges, >= 0) of a window of a W x H picture (window pixel (0, 0) =
// picture pixel (ox, oy); the whole picture: ox = oy = 0). The reads of the portrait reshaping field (section 45). Twin: warp_lanczos.glsl.
inline float lanczos3w(float d) {
  if (std::fabs(d) < 1e-6f) return 1.f;
  if (std::fabs(d) >= 3.f) return 0.f;
  const float a = 3.14159265f * d;
  return 3.f * std::sin(a) * std::sin(a * (1.f / 3.f)) / (a * a);
}
inline void sampleLanczos3(const ImageF& img, int ox, int oy, int W, int H, float x, float y, float out[3]) {
  x -= 0.5f;
  y -= 0.5f;
  const float fxl = std::floor(x), fyl = std::floor(y);
  const int ix = int(fxl), iy = int(fyl);
  const float tx = x - fxl, ty = y - fyl;
  float wx[6], wy[6], sx = 0.f, sy = 0.f;
  for (int k = 0; k < 6; ++k) {
    wx[k] = lanczos3w(float(k - 2) - tx), wy[k] = lanczos3w(float(k - 2) - ty);
    sx += wx[k], sy += wy[k];
  }
  for (int k = 0; k < 6; ++k) wx[k] /= sx, wy[k] /= sy;
  out[0] = out[1] = out[2] = 0.f;
  for (int j = 0; j < 6; ++j) {
    const float* row = img.row(std::clamp(iy - 2 + j, 0, H - 1) - oy);
    float acc[3] = {0.f, 0.f, 0.f};
    for (int i = 0; i < 6; ++i) {
      const float* px = row + 3 * (std::clamp(ix - 2 + i, 0, W - 1) - ox);
      acc[0] += wx[i] * px[0];
      acc[1] += wx[i] * px[1];
      acc[2] += wx[i] * px[2];
    }
    for (int c = 0; c < 3; ++c) out[c] += wy[j] * acc[c];
  }
  for (int c = 0; c < 3; ++c) out[c] = std::max(0.f, out[c]);
}

// ---- colour style: recipe + pad + strength, resolved to numbers once per render ----
struct StyleMix {
  bool active = false;
  float satMul = 1.f, contrastK = 0.f, dL = 0.f;
  float tintA = 0.f, tintB = 0.f, shA = 0.f, shB = 0.f, hiA = 0.f, hiB = 0.f;
  float hueRot = 0.f, gray = 0.f, fade = 0.f, grain = 0.f;
  bool tint = false, split = false;
};

StyleMix makeStyle(const EditParams& p);

// ---- colour grading zones (three zones plus a global tint), each (hue, saturation, luminance) ----
struct Grade {
  float a = 0, b = 0, dl = 0;
  bool active = false;
};

inline Grade makeGrade(const ToneZoneGrade& g) {
  Grade r;
  if (g.saturation <= 0 && g.luminance == 0) return r;
  const float h = lightroomHueToOklab(g.hue), s = g.saturation / 100.f * 0.06f;
  r.a = std::cos(h) * s;
  r.b = std::sin(h) * s;
  r.dl = g.luminance / 100.f * 0.15f;
  r.active = true;
  return r;
}

// Neighbourhood operations. For display-referred sources (camera JPEG: already sharpened and
// denoised in camera) the default sharpen 40 is the no-op level. Noise reduction resolves its own block (core/engine/nodes/nr).
// Everything of the per-pixel pipeline that depends on parameters only, resolved once.
struct PointOp {
  bool scene = true;
  float wb[3]{1, 1, 1};
  float exposureGain = 1.f;
  float contrast = 0.f, hi = 0.f, sh = 0.f, wh = 0.f, bl = 0.f, sat = 0.f, vib = 0.f;
  std::array<HslAdjust, kHslBandCount> hslEff{};
  bool anyHsl = false;
  Grade gS, gM, gH, gG;
  bool zoneGrade = false, anyGrade = false;
  float gWidth = 0.f, gC1 = 0.f, gC2 = 0.f;
  StyleMix style;
  const ColorMatchProfile* match = nullptr;
  bool matchLut = false, matchMat = false;
  Curve1D toneMove;
  bool colorStage = false;
  const Lut3D* lut = nullptr;
  float lutMix = 0.f;
  float fade = 0.f;
  bool p3 = false;  // export colour space Display P3 (EditParams::outputSpace): the lattice holds P3-encoded values
  const Curve1D *master = nullptr, *red = nullptr, *green = nullptr, *blue = nullptr;
  ToneAnalysis analysis;
  bool hasAnalysis = false;

  // The modern rendering family (section 37): resolved once per render. `s` = strength 0..1.
  struct Modern {
    bool active = false, tone = false;
    float s = 0.f;
    float contrast = 1.f, chroma = 1.f, blueDeep = 0.f, skinGuard = 0.f, lift = 0.f, tintA = 0.f, tintB = 0.f;
    float pb = 0.f, pw = 1.f, shoulderK = 1.f;  // encoded end points (user tone edits applied), shoulder share kept
    float tm = 0.5f, ym = 0.5f;  // pivot: mid-grey
  } mod;

  // The user's tone edits (contrast / highlights / shadows / whites / blacks) on the exposure value, without the standard toe and shoulder.
  float userY(float Y) const {
    float e = std::log2(std::max(Y, 1e-9f) / kMidGrey);
    e += 0.45f * contrast * e / (1.f + (e / 4.f) * (e / 4.f));
    if (hi != 0) e += hi * 1.2f * smoothstep(-0.5f, 2.5f, e);
    if (sh != 0) e += sh * 1.3f * smoothstep(0.5f, -4.f, e);
    if (wh != 0) e += wh * 0.7f * smoothstep(1.f, 3.f, e);
    if (bl != 0) e += bl * 0.9f * smoothstep(-3.f, -6.5f, e);
    return kMidGrey * std::exp2(e);
  }

  // The standard tone curve (before the tonal-class move): display-linear luminance of a post-gain luminance.
  float stdTone(float Y) const {
    float e = std::log2(Y / kMidGrey);
    e += 0.45f * contrast * e / (1.f + (e / 4.f) * (e / 4.f));
    if (scene) e -= kToeStops * smoothstep(kToeStart, kToeEnd, e);
    if (hi != 0) e += hi * 1.2f * smoothstep(-0.5f, 2.5f, e);
    if (sh != 0) e += sh * 1.3f * smoothstep(0.5f, -4.f, e);
    if (wh != 0) e += wh * 0.7f * smoothstep(1.f, 3.f, e);
    if (bl != 0) e += bl * 0.9f * smoothstep(-3.f, -6.5f, e);
    float Yd = kMidGrey * std::exp2(e);
    if (scene) Yd = shoulder(Yd);
    return Yd;
  }

  // The modern tone curve at strength s: end points -> display black / white, mid-grey fixed at its standard value, blended with the
  // standard curve in encoded display units (so every strength keeps mid-grey and is monotone).
  float modernTone(float Y, float Ystd) const {
    const float y1 = modernEncode(userY(Y));
    const float t = (y1 - mod.pb) / std::max(mod.pw - mod.pb, 1e-4f);
    const float ym = modernCurve(t, mod.tm, mod.ym, mod.contrast, mod.shoulderK);
    return modernDecode((1.f - mod.s) * modernEncode(Ystd) + mod.s * ym);
  }

  // Tone: ratio by which the luminance Y of a (post-gain) pixel is scaled; hue-preserving, so the
  // whole stage is a function of Y alone.
  float toneRatio(float Y) const {
    if (!(Y > 1e-7f)) return 1.f;
    float Yd = stdTone(Y);
    if (mod.tone) Yd = modernTone(Y, Yd);
    if (!toneMove.identity) Yd = srgbDecode(toneMove(srgbEncode(Yd)));
    return Yd / Y;
  }

  static float hueDist(float h, float c) {
    float d = std::fabs(h - c);
    return d > 180.f ? 360.f - d : d;
  }

  // Modern colour, before the user's colour edits: chroma rises in the mid-tones (lightness window), less where the colour is already
  // strong (soft per-hue ceiling: blues and greens deep, not neon), withheld in the skin window (OKLab hue 52 +- 20 deg, C 0.03 .. 0.2,
  // L 0.3 .. 0.95: a window of the colour space, no face detection), blue / cyan a little deeper. Hue is never touched.
  void modernBase(float& L, float& C, float hDeg) const {
    const float wL = smoothstep(0.10f, 0.42f, L) * (1.f - smoothstep(0.88f, 0.99f, L));
    const float dSkin = hueDist(hDeg, 52.f) / 20.f;
    const float wSkin = std::exp(-dSkin * dSkin) * smoothstep(0.02f, 0.05f, C) * (1.f - smoothstep(0.14f, 0.22f, C)) *
                        smoothstep(0.30f, 0.50f, L) * (1.f - smoothstep(0.92f, 1.0f, L));
    const float dG = hueDist(hDeg, 142.f) / 30.f, dB = hueDist(hDeg, 264.f) / 26.f;
    const float wG = std::exp(-dG * dG), wB = std::exp(-dB * dB);
    const float cap = 0.30f - 0.10f * wG - 0.09f * wB;
    const float room = 1.f - smoothstep(0.55f * cap, cap, C);
    C *= 1.f + (mod.chroma - 1.f) * wL * (1.f - mod.skinGuard * wSkin) * room;
    L -= mod.blueDeep * wB * smoothstep(0.04f, 0.12f, C) * wL;
    L += mod.lift * wL * smoothstep(0.02f, 0.08f, C);  // colours only: neutral mid-grey stays where the anchor put it
  }

  // Gamut mapping by chroma compression, in OKLab at constant lightness and hue: kmax = the largest scale of (a, b) that still fits
  // the sRGB cube (bisection), u = 1 / kmax = how far the colour is towards (u = 1) or beyond (u > 1) the boundary; a smooth knee
  // maps u to u' (identity up to 0.88, then an exponential approach to 1: nothing is clipped, the gradation of over-saturated colours
  // survives). The mapping is blended in with the strength; the standard chord below only sees what is left.
  void modernChromaCompress(float& L, float& a, float& b) const {
    if (std::hypot(a, b) < 2e-3f) return;
    L = std::min(L, 1.f);
    auto fits = [&](float k) {
      const float lab[3] = {L, k * a, k * b};
      float r0[3], r[3];
      oklabToLinearSrgb(lab, r0);
      if (p3) srgbToP3().apply(r0, r);  // the gamut to compress into is the output space's
      else r[0] = r0[0], r[1] = r0[1], r[2] = r0[2];
      return r[0] >= -2e-4f && r[1] >= -2e-4f && r[2] >= -2e-4f && r[0] <= 1.0002f && r[1] <= 1.0002f && r[2] <= 1.0002f;
    };
    float kmax = 3.f;
    if (!fits(kmax)) {
      float lo = 0.f, hi = 3.f;
      for (int i = 0; i < 9; ++i) {
        const float mid = 0.5f * (lo + hi);
        (fits(mid) ? lo : hi) = mid;
      }
      kmax = lo;
    }
    constexpr float k0 = 0.88f;
    const float u = kmax > 1e-4f ? 1.f / kmax : 1e4f;
    if (u <= k0) return;
    const float f = (k0 + (1.f - k0) * (1.f - std::exp(-(u - k0) / (1.f - k0)))) / u;
    const float fs = 1.f - mod.s * (1.f - f);
    a *= fs;
    b *= fs;
  }

  // `v_in` = scene-linear ProPhoto after white balance and exposure gain (>= 0); e3 = sRGB-encoded
  // display value (before vignette, not clamped).
  void apply(const float v_in[3], float e3[3]) const {
    const float ratio = toneRatio(proPhotoLuma(v_in));
    const float t[3] = {v_in[0] * ratio, v_in[1] * ratio, v_in[2] * ratio};
    colour(t, e3);
  }

  // Display P3 output (export only). `rgb` = linear sRGB primaries after the colour stage (unbounded). Same hue-preserving pull towards the
  // same-luminance grey as the sRGB path, but into the P3 cube; the display-referred stages (curves, look LUT, fade) are authored on
  // sRGB-encoded values, so they run there (extended past [0, 1] by an identity offset: a P3-only colour keeps its excess) and the result is
  // converted to P3 afterwards: a colour inside sRGB gets exactly the sRGB path's value, P3 only adds what sRGB could not hold.
  void colourP3(const float rgbIn[3], float e3[3]) const {
    float q[3];
    srgbToP3().apply(rgbIn, q);
    const float Ys = std::clamp(p3Luma(q), 0.f, 1.f);
    const float mn = std::min({q[0], q[1], q[2]});
    if (mn < 0.f) {
      const float t = Ys / std::max(1e-7f, Ys - mn);
      for (float& c : q) c = Ys + (c - Ys) * t;
    }
    const float mx = std::max({q[0], q[1], q[2]});
    if (mx > 1.f) {
      const float t = (1.f - Ys) / std::max(1e-7f, mx - Ys);
      for (float& c : q) c = Ys + (c - Ys) * t;
    }
    const bool plain = master->identity && red->identity && green->identity && blue->identity && !(lut && lutMix > 0) && !(fade > 0.f);
    if (plain) {
      for (int c = 0; c < 3; ++c) e3[c] = srgbEncode(q[c]);
      return;
    }
    float s[3], ex[3];
    p3ToSrgb().apply(q, s);
    for (int c = 0; c < 3; ++c) ex[c] = srgbEncodeExt(s[c]);
    auto ext = [](const Curve1D& f, float x) {
      const float k = std::clamp(x, 0.f, 1.f);
      return f(k) + (x - k);
    };
    for (int c = 0; c < 3; ++c) ex[c] = ext(*master, ex[c]);
    ex[0] = ext(*red, ex[0]);
    ex[1] = ext(*green, ex[1]);
    ex[2] = ext(*blue, ex[2]);
    if (lut && lutMix > 0) {
      float k[3] = {std::clamp(ex[0], 0.f, 1.f), std::clamp(ex[1], 0.f, 1.f), std::clamp(ex[2], 0.f, 1.f)}, l[3];
      lut->apply(k, l);
      for (int c = 0; c < 3; ++c) ex[c] += (l[c] - k[c]) * lutMix;
    }
    if (fade > 0.f)
      for (int c = 0; c < 3; ++c) ex[c] = fade + ex[c] * (1.f - fade);
    for (int c = 0; c < 3; ++c) s[c] = srgbDecodeExt(ex[c]);
    srgbToP3().apply(s, q);
    for (int c = 0; c < 3; ++c) e3[c] = srgbEncode(q[c]);
  }

  // Everything after the tone stage; `v` = tone-mapped linear ProPhoto. This is what the lattice holds.
  void colour(const float v[3], float e3[3]) const {
    float rgb[3];
    proPhotoToSrgb().apply(v, rgb);

    // ---- colour, in OKLab ----
    if (colorStage) {
      float lab[3];
      if (matchMat) {
        const float* m = match->matrix;
        const float r0 = rgb[0], g0 = rgb[1], b0 = rgb[2];
        rgb[0] = m[0] * r0 + m[1] * g0 + m[2] * b0;
        rgb[1] = m[3] * r0 + m[4] * g0 + m[5] * b0;
        rgb[2] = m[6] * r0 + m[7] * g0 + m[8] * b0;
      }
      linearSrgbToOklab(rgb, lab);
      if (matchLut) {
        float off[3];
        sampleColorMatch(*match, lab, off);
        lab[0] += off[0];
        lab[1] += off[1];
        lab[2] += off[2];
      }
      float L = lab[0];
      float C = std::hypot(lab[1], lab[2]);
      float h = std::atan2(lab[2], lab[1]);
      const float chromaW = smoothstep(0.f, 0.04f, C);
      float hDeg = h * 180.f / kPi;
      if (hDeg < 0) hDeg += 360.f;

      if (mod.active) modernBase(L, C, hDeg);
      if (anyHsl && chromaW > 0) {
        float w[kHslBandCount];
        bandWeights(hDeg, w);
        float dh = 0, ds = 0, dl = 0;
        for (int i = 0; i < kHslBandCount; ++i) {
          dh += w[i] * hslEff[i].hue;
          ds += w[i] * hslEff[i].saturation;
          dl += w[i] * hslEff[i].luminance;
        }
        h += dh / 100.f * (25.f * kPi / 180.f) * chromaW;
        C *= std::max(0.f, 1.f + ds / 100.f * chromaW);
        L *= 1.f + dl / 100.f * 0.3f * chromaW;
      }
      if (vib != 0) {
        const float room = 1.f - std::min(C / 0.2f, 1.f);
        const float skin = std::exp(-((hDeg - 55.f) / 22.f) * ((hDeg - 55.f) / 22.f));
        C *= 1.f + vib * room * (vib > 0 ? (1.f - 0.6f * skin) : 0.8f);
      }
      if (sat != 0) C *= std::max(0.f, 1.f + sat);

      // colour style (+ pad), after the user's colour edits, before grading
      if (style.active) {
        C *= style.satMul * (1.f - style.gray);
        h += style.hueRot * chromaW;
      }
      float a = C * std::cos(h), b = C * std::sin(h);
      if (style.active) {
        if (style.contrastK != 0.f) {
          const float Lc = std::clamp(L, 0.f, 1.f);
          L += style.contrastK * 4.f * Lc * (1.f - Lc) * (Lc - 0.5f);
        }
        L += style.dL;
        if (style.tint) {
          const float wt = smoothstep(0.f, 0.3f, L);
          a += style.tintA * wt;
          b += style.tintB * wt;
        }
        if (style.split) {
          const float ws = 1.f - smoothstep(0.2f, 0.7f, L), whl = smoothstep(0.4f, 0.9f, L);
          a += style.shA * ws + style.hiA * whl;
          b += style.shB * ws + style.hiB * whl;
        }
      }
      if (anyGrade) {
        const float Lc = std::clamp(L, 0.f, 1.f);
        float ws = 0.f, wm = 0.f, whl = 0.f;
        if (zoneGrade) {
          ws = 1.f - smoothstep(gC1 - gWidth, gC1 + gWidth, Lc);
          whl = smoothstep(gC2 - gWidth, gC2 + gWidth, Lc);
          wm = std::max(0.f, 1.f - ws - whl);
        }
        a += gS.a * ws + gM.a * wm + gH.a * whl + gG.a;
        b += gS.b * ws + gM.b * wm + gH.b * whl + gG.b;
        L += gS.dl * ws + gM.dl * wm + gH.dl * whl + gG.dl;
      }
      if (mod.active) {  // saturation roll-off towards white: colour fades with lightness instead of clipping
        // highlight bias on near-neutral highlights only (hue of a real colour is never touched)
        const float wT = smoothstep(0.62f, 0.90f, L) * (1.f - smoothstep(0.01f, 0.05f, std::hypot(a, b)));
        a += mod.tintA * wT;
        b += mod.tintB * wT;
        const float fade = 1.f - mod.s * smoothstep(0.94f, 1.0f, L);
        a *= fade;
        b *= fade;
        modernChromaCompress(L, a, b);
      }
      const float lab2[3] = {L, a, b};
      oklabToLinearSrgb(lab2, rgb);
    }

    if (p3) {
      colourP3(rgb, e3);
      return;
    }

    // ---- gamut map into [0,1] towards same-luminance grey ----
    float Ys = std::clamp(srgbLuma(rgb), 0.f, 1.f);

    const float mn = std::min({rgb[0], rgb[1], rgb[2]});
    if (mn < 0.f) {
      const float t = Ys / std::max(1e-7f, Ys - mn);
      for (float& c : rgb) c = Ys + (c - Ys) * t;
    }
    const float mx = std::max({rgb[0], rgb[1], rgb[2]});
    if (mx > 1.f) {
      const float t = (1.f - Ys) / std::max(1e-7f, mx - Ys);
      for (float& c : rgb) c = Ys + (c - Ys) * t;
    }

    // ---- display-referred stages ----
    for (int c = 0; c < 3; ++c) e3[c] = (*master)(srgbEncode(rgb[c]));
    e3[0] = (*red)(e3[0]);
    e3[1] = (*green)(e3[1]);
    e3[2] = (*blue)(e3[2]);

    if (lut && lutMix > 0) {
      float l[3];
      lut->apply(e3, l);
      for (int c = 0; c < 3; ++c) e3[c] += (l[c] - e3[c]) * lutMix;
    }

    if (fade > 0.f)
      for (int c = 0; c < 3; ++c) e3[c] = fade + e3[c] * (1.f - fade);
  }
};

PointOp makePointOp(const Params& rp, const SourceImage& source, const BakedLut* reuse);

// Curves + LUT on an encoded display value (the display stages after a film).
inline void displayStages(const Params& rp, float e3[3]) {
  for (int c = 0; c < 3; ++c) e3[c] = rp.curves.master(e3[c]);
  e3[0] = rp.curves.red(e3[0]);
  e3[1] = rp.curves.green(e3[1]);
  e3[2] = rp.curves.blue(e3[2]);
  if (rp.lut) {
    const float mix = std::clamp(rp.p.lutAmount / 100.f, 0.f, 1.f);
    if (mix > 0) {
      float l[3];
      rp.lut->apply(e3, l);
      for (int c = 0; c < 3; ++c) e3[c] += (l[c] - e3[c]) * mix;
    }
  }
}


// ---- the lattice and the tone table ------------------------------------------------------------------------------------------------
// The lattice holds the colour stages (everything after the luminance tone curve), indexed by the
// tone-mapped linear ProPhoto value in a square-root domain: node i of n sits at 2 * (i / (n-1))^2.
// The tone stage itself depends on luminance only and is a 1D table of the luminance ratio over
// log2(Y), evaluated exactly (no lattice error) per pixel.
constexpr float kBakeMax = 1.5f;
// The tone table is indexed by the float bit pattern (exponent + top 8 mantissa bits = 256 buckets per
// octave, linear inside a bucket), so no logarithm is needed per pixel.
constexpr int kToneOctLo = -24, kToneOctHi = 6;              // log2(Y) range covered
constexpr int kToneBase = (127 + kToneOctLo) << 8;           // first bucket index
constexpr int kToneN = ((kToneOctHi - kToneOctLo) << 8) + 2; // entries (one spare for interpolation)

struct LutSampler {
  const float* t;
  const float* tone;
  int n;
  float scale;  // n - 1
  float maxU;
  int sr, sg, sb;

  explicit LutSampler(const BakedLut& l) : t(l.rgb ? l.rgb->data() : nullptr), tone(l.tone.data()), n(l.n) {
    scale = float(n - 1);
    maxU = float(n - 1) - 1e-4f;
    sr = 3;
    sg = 3 * n;
    sb = 3 * n * n;
  }

  // v: post-gain scene-linear ProPhoto (>= 0) -> encoded display value. Returns false (out unset)
  // when the tone-mapped colour `v` lies beyond the lattice; the caller evaluates it exactly.
  inline bool operator()(const float v_in[3], float out[3], float v[3]) const {
    v[0] = v_in[0];
    v[1] = v_in[1];
    v[2] = v_in[2];
    const float Y = proPhotoLuma(v);
    if (Y > 1e-7f) {
      std::uint32_t bits;
      std::memcpy(&bits, &Y, 4);
      int k = int(bits >> 15) - kToneBase;
      float fr = float(bits & 0x7FFFu) * (1.f / 32768.f);
      if (k < 0) {
        k = 0;
        fr = 0.f;
      } else if (k > kToneN - 2) {
        k = kToneN - 2;
        fr = 0.f;
      }
      const float ratio = tone[k] + fr * (tone[k + 1] - tone[k]);
      v[0] *= ratio;
      v[1] *= ratio;
      v[2] *= ratio;
    }
    if (v[0] > kBakeMax || v[1] > kBakeMax || v[2] > kBakeMax) return false;
    int i[3];
    float f[3];
    for (int c = 0; c < 3; ++c) {
      float x = std::sqrt(std::max(v[c], 0.f) * (1.f / kBakeMax)) * scale;
      x = x > maxU ? maxU : x;
      i[c] = int(x);
      f[c] = x - float(i[c]);
    }
    const float* p0 = t + i[2] * sb + i[1] * sg + i[0] * sr;
    // tetrahedral interpolation: walk the axes from the largest fraction to the smallest. The order
    // is found by ranking (no data-dependent branches: noisy images would mispredict them constantly).
    const int g01 = f[0] >= f[1], g02 = f[0] >= f[2], g12 = f[1] >= f[2];
    const int rank[3] = {(1 - g01) + (1 - g02), g01 + (1 - g12), g02 + g12};  // 0 = largest fraction
    int step[3];
    float wt[3];
    step[rank[0]] = sr;
    step[rank[1]] = sg;
    step[rank[2]] = sb;
    wt[rank[0]] = f[0];
    wt[rank[1]] = f[1];
    wt[rank[2]] = f[2];
    const float* p1 = p0 + step[0];
    const float* p2 = p1 + step[1];
    const float* p3 = p2 + step[2];
    const float w0 = wt[0], w1 = wt[1], w2 = wt[2];
    for (int c = 0; c < 3; ++c) out[c] = p0[c] + w0 * (p1[c] - p0[c]) + w1 * (p2[c] - p1[c]) + w2 * (p3[c] - p2[c]);
    return true;
  }
};

// Every entry is independent, so the parallel fill is bit-identical to a serial one (inline when called from inside a pool job).
template <class Op>
void fillToneTable(std::vector<float>& tone, const Op& op) {
  tone.resize(kToneN);
  float* o = tone.data();
  parallelFor(
      kToneN,
      [&](int i) {
        const std::uint32_t bits = std::uint32_t(i + kToneBase) << 15;
        float y;
        std::memcpy(&y, &bits, 4);
        o[i] = op.toneRatio(y);
      },
      1024);
}

// EV range (around mid-grey) that a frame's tone curve shows well (the single-image HDR leaves it alone).
float hdrShownRange(const std::vector<float>& tone);

// ---- film glue -------------------------------------------------------------------------------------------------------------------
struct FilmRun {
  std::shared_ptr<const film::FilmLibrary> lib;
  const film::FilmStock* stock = nullptr;
  std::string paper;
  float strength = 1.f;  // 0..1
};

// Resolves stock / paper of the edit against the installed library (null stock = film not available).
FilmRun resolveFilm(const EditParams& p);
// The strength of the single-image HDR the frame asks for: the user's `hdr`, at least the modern rendering's own share (section 37).
float effectiveHdr(const EditParams& p);

// ---- geometry --------------------------------------------------------------------------------------------------------------------
// Framing (rotate 90 -> straighten (+ ratio crop without empty corners) -> flip) of a w x h source, then the region `R` of the picture.
// `scale`: the radii scale of the frame (full-picture long edge / 2048; a region at its own pixel pitch).
Geometry resolveGeometry(const EditParams& p, int w, int h, const RenderRegion& R, float* scale = nullptr);
inline FrameFacts geometryFacts(const Geometry& g) {
  FrameFacts f;
  f.geo = g;
  return f;
}

}  // namespace dapple::engine::point
