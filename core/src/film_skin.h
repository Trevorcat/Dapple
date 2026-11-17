// Skin protection for film stocks (EditParams::filmSkin, docs/engine/pipeline-architecture.md, film section "Skin protection").
//
// A film's own chroma gain on skin tones is high (Portra 400 x1.4-1.5, a pushed Portra 800 x2 in CIELAB chroma on a scene-referred chart). With filmSkin = 1
// the chroma of the film's picture inside the skin regions of the photo (the AI person-part planes "faceskin" and "bodyskin", feathered) is limited to
// kSkinChromaCap times the chroma of the film's own input; its hue and its lightness are the film's.
//
//   film input   the ungained source pixel after the inverse camera rendering of a display-referred source (what the film's exposure stage reads),
//                times the frame's gain (white balance x exposure x film exposure), ProPhoto -> linear sRGB, clamped to 0..1: skinRef()
//   film output  the film's display sRGB (encoded, unclamped) as the film nodes / the FilmSimulator produce it
//   cap          CIELAB (D65) chroma of the output above kSkinChromaCap x the chroma of the input: a, b scaled down, L kept; mixed in by mask x filmSkin
//
// CPU: point::CpuFrame::runFilm (nodes/film/film_glue.cpp) after the film plane; GPU: shaders/film_skin.comp in front of the display stages. The two are twins:
// keep the arithmetic identical. A frame without the planes (no person found / AI masks not computed / no photo id), a region source, a reshaped frame or a
// film strength of 0 is not touched at all: there is no fallback to a hue window.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "color.h"
#include "film_kernels.h"

namespace dapple::film {

inline constexpr float kSkinChromaCap = 1.25f;

// The skin planes of a photo as one feathered plane in the photo's normalised space (the masks' space: before crop / rotate / flip).
struct SkinMask {
  int w = 0, h = 0;
  std::vector<float> a;   // w * h, 0..1
  std::uint64_t key = 0;  // content key (the two planes' hashes)
  // bilinear, pixel centres at +0.5, clamped (local_eval.cpp lplane)
  float at(float u, float v) const {
    const float fx = std::fmin(std::fmax(u * float(w) - 0.5f, 0.f), float(w - 1)), fy = std::fmin(std::fmax(v * float(h) - 0.5f, 0.f), float(h - 1));
    const int x0 = std::min(int(fx), w - 1), y0 = std::min(int(fy), h - 1), x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
    const float tx = fx - float(x0), ty = fy - float(y0);
    const float p00 = a[std::size_t(y0) * w + x0], p01 = a[std::size_t(y0) * w + x1], p10 = a[std::size_t(y1) * w + x0], p11 = a[std::size_t(y1) * w + x1];
    const float t0 = p00 + (p01 - p00) * tx, t1 = p10 + (p11 - p10) * tx;
    return t0 + (t1 - t0) * ty;
  }
};

// The raster ids a photo's skin comes from: prefix + ":faceskin" / ":bodyskin" (prefix = "ai:<photo>", EditParams::filmSkinPhoto).
inline std::string skinFaceId(const std::string& prefix) { return prefix + ":faceskin"; }
inline std::string skinBodyId(const std::string& prefix) { return prefix + ":bodyskin"; }

// The mask of a photo, built from the raster store (null when neither plane is there), cached by content. Feather: three box passes of about 1.2 % of the
// long edge, so the cap fades out over a few percent of the picture instead of drawing the outline of the plane.
std::shared_ptr<const SkinMask> skinMaskFor(const std::string& prefix);
// The content key of the planes (0 = none): goes into the film's cache key, so a mask that lands (or changes) re-renders the frame.
std::uint64_t skinKeyFor(const std::string& prefix);

// ---- per pixel (twin of shaders/film_skin.comp) -----------------------------------------------------------------------------------------------------
// Film input of an ungained source pixel -> linear sRGB (>= 0): the display inverse of a display-referred source, gain, ProPhoto -> sRGB.
inline void skinRef(const float rgbIn[3], bool displayRef, const float gain[3], float out[3]) {
  float p[3] = {rgbIn[0], rgbIn[1], rgbIn[2]};
  if (displayRef) detail::displayInverseApply(detail::displayInverse(), rgbIn, p);
  const float g[3] = {p[0] * gain[0], p[1] * gain[1], p[2] * gain[2]};
  color::proPhotoToSrgb().apply(g, out);
  for (int c = 0; c < 3; ++c) out[c] = std::fmin(std::fmax(out[c], 0.f), 1.f);
}
inline void skinLab(const float lin[3], float lab[3]) {
  const float X = (0.4124564f * lin[0] + 0.3575761f * lin[1] + 0.1804375f * lin[2]) * (1.f / 0.95047f);
  const float Y = 0.2126729f * lin[0] + 0.7151522f * lin[1] + 0.0721750f * lin[2];
  const float Z = (0.0193339f * lin[0] + 0.1191920f * lin[1] + 0.9503041f * lin[2]) * (1.f / 1.08883f);
  auto f = [](float t) { return t > 0.008856f ? std::cbrt(t) : 7.787f * t + 16.f / 116.f; };
  const float fx = f(X), fy = f(Y), fz = f(Z);
  lab[0] = 116.f * fy - 16.f;
  lab[1] = 500.f * (fx - fy);
  lab[2] = 200.f * (fy - fz);
}
inline void skinFromLab(const float lab[3], float lin[3]) {
  const float fy = (lab[0] + 16.f) * (1.f / 116.f), fx = fy + lab[1] * (1.f / 500.f), fz = fy - lab[2] * (1.f / 200.f);
  auto g = [](float t) { return t > 0.206897f ? t * t * t : (t - 16.f / 116.f) * (1.f / 7.787f); };
  const float X = g(fx) * 0.95047f, Y = g(fy), Z = g(fz) * 1.08883f;
  lin[0] = 3.2404542f * X - 1.5371385f * Y - 0.4985314f * Z;
  lin[1] = -0.9692660f * X + 1.8760108f * Y + 0.0415560f * Z;
  lin[2] = 0.0556434f * X - 0.2040259f * Y + 1.0572252f * Z;
}
// The cap on one pixel. film: display sRGB (encoded, unclamped), in place; ref: skinRef(); amount: mask x filmSkin, 0..1.
inline void skinCap(float film[3], const float ref[3], float amount) {
  if (!(amount > 0.f)) return;
  float eo[3], lo[3], labO[3], labR[3];
  for (int c = 0; c < 3; ++c) {
    eo[c] = std::fmin(std::fmax(film[c], 0.f), 1.f);
    lo[c] = eo[c] <= 0.04045f ? eo[c] * (1.f / 12.92f) : std::pow((eo[c] + 0.055f) * (1.f / 1.055f), 2.4f);
  }
  skinLab(lo, labO);
  skinLab(ref, labR);
  const float Co = std::sqrt(labO[1] * labO[1] + labO[2] * labO[2]), lim = kSkinChromaCap * std::sqrt(labR[1] * labR[1] + labR[2] * labR[2]);
  if (!(Co > lim) || Co < 1e-3f) return;
  const float s = lim / Co;
  const float labN[3] = {labO[0], labO[1] * s, labO[2] * s};
  float ln[3];
  skinFromLab(labN, ln);
  for (int c = 0; c < 3; ++c) {
    const float v = std::fmin(std::fmax(ln[c], 0.f), 1.f);
    const float enc = v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f;
    film[c] += amount * (enc - eo[c]);
  }
}

}  // namespace dapple::film
