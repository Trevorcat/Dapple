// Film exposure that keeps the photo's key (film_match.h).
#include "dapple/film_match.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "color.h"
#include "dapple/renderer.h"

namespace dapple {

// Median CIE L* of an sRGB-encoded float image (BT.709 luminance of the linearised values).
float medianLightness(const ImageF& img) {
  const std::size_t n = std::size_t(img.width) * img.height;
  if (!n) return 0.f;
  std::vector<float> ls(n);
  std::size_t k = 0;
  for (int y = 0; y < img.height; ++y) {
    const float* r = img.row(y);
    for (int x = 0; x < img.width; ++x, ++k) {
      const float rl = color::srgbDecode(r[3 * x]), gl = color::srgbDecode(r[3 * x + 1]), bl = color::srgbDecode(r[3 * x + 2]);
      const float Y = std::clamp(0.2126f * rl + 0.7152f * gl + 0.0722f * bl, 0.f, 1.f);
      ls[k] = Y > 0.008856f ? 116.f * std::cbrt(Y) - 16.f : 903.3f * Y;
    }
  }
  std::nth_element(ls.begin(), ls.begin() + n / 2, ls.end());
  return ls[n / 2];
}

std::string filmMatchKey(const EditParams& p) {
  if (p.filmStock.empty()) return {};
  EditParams k = p;
  k.filmExposure = 0.f;
  k.filmKeyEv = 0.f;
  k.filmStrength = 100.f;
  k.filmGrain = 0.f;
  k.filmHalation = 0.f;
  k.clarity = k.texture = k.dehaze = k.grain = 0.f;
  k.sharpen = k.sharpRadius = k.nrLuma = k.nrColor = 0.f;
  k.vignette = 0.f;
  return toJson(k);
}

float solveFilmKeyEv(const std::function<float(float)>& filmL, float target, FilmMatchInfo* info) {
  constexpr float kMaxEv = 4.f, kTol = 0.35f;
  const bool trace = std::getenv("DAPPLE_MATCH_TRACE") != nullptr;
  float ev0 = 0.f, l0 = filmL(ev0);
  int evals = 1;
  const float filmL0 = l0;
  float best = ev0, bestErr = std::fabs(l0 - target), bestL = l0;
  // L* rises ~10-18 per EV around the mid-tones: a secant iteration from a nominal slope converges in 2-3 steps
  float ev1 = std::clamp((target - l0) / 14.f, -kMaxEv, kMaxEv);
  float l1 = l0;
  for (int it = 0; it < 6 && bestErr > kTol; ++it) {
    if (std::fabs(ev1 - ev0) < 1e-3f) break;
    l1 = filmL(ev1);
    if (trace) std::fprintf(stderr, "match: ev %+.3f -> L* %.2f (target %.2f)\n", ev1, l1, target);
    ++evals;
    const float err = std::fabs(l1 - target);
    if (err < bestErr) bestErr = err, best = ev1, bestL = l1;
    float slope = (l1 - l0) / (ev1 - ev0);
    // reversal stocks get darker with exposure (negative slope); a flat (clipped) region takes a bounded step
    if (std::fabs(slope) < 2.f || !std::isfinite(slope)) slope = slope < 0.f ? -2.f : 2.f;
    const float next = std::clamp(ev1 + (target - l1) / slope, -kMaxEv, kMaxEv);
    ev0 = ev1;
    l0 = l1;
    ev1 = next;
  }
  if (!std::isfinite(best)) best = 0.f;
  if (info) {
    info->ev = best;
    info->targetL = target;
    info->filmL = bestL;
    info->filmL0 = filmL0;
    info->evals = evals;
  }
  return best;
}

float matchFilmExposure(const EditParams& p, const CameraProfile& camera, const Lut3D* lut, const SourceImage& proxy,
                        FilmMatchInfo* info) try {
  if (info) *info = FilmMatchInfo{};
  if (p.filmStock.empty() || proxy.linear.width <= 0 || proxy.linear.height <= 0) return 0.f;

  // the target: the normal develop render of this edit (film off), tonal analysis taken on this same proxy
  EditParams ref = p;
  ref.filmStock.clear();
  ref.filmKeyEv = 0.f;
  ref.clarity = ref.texture = ref.dehaze = ref.grain = 0.f;
  ref.sharpen = ref.nrLuma = 0.f;
  float target;
  {
    Renderer r(ref, camera, lut);
    r.setToneProxy(&proxy);
    target = medianLightness(r.renderFloat(proxy, false));
  }

  // the film alone (strength 100), spatial stages off, at a trial exposure
  EditParams fp = p;
  fp.filmStrength = 100.f;
  fp.filmExposure = 0.f;
  fp.vignette = 0.f;
  auto evalAt = [&](float ev) {
    fp.filmKeyEv = ev;
    Renderer r(fp, camera, lut);
    r.setToneProxy(&proxy);
    return medianLightness(r.renderFloat(proxy, false));
  };

  const float best = solveFilmKeyEv(evalAt, target, info);
  return best;
} catch (...) {
  return 0.f;
}

}  // namespace dapple
