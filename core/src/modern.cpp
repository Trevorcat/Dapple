#include "dapple/modern.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace dapple {

ModernKind modernKind(const std::string& id) {
  if (id == "modern_clean") return ModernKind::Clean;
  if (id == "modern_vivid") return ModernKind::Vivid;
  if (id == "modern_soft") return ModernKind::Soft;
  return ModernKind::None;
}

const std::array<ModernInfo, 3>& modernRenderings() {
  static const std::array<ModernInfo, 3> k = {{
      {"modern_clean", "\xE9\x80\x9A\xE9\x80\x8F", "\xE5\xB9\xB2\xE5\x87\x80 \xC2\xB7 \xE9\xBB\x91\xE7\x99\xBD\xE5\x88\xB0\xE4\xBD\x8D \xC2\xB7 \xE8\x89\xB2\xE5\xBD\xA9\xE5\xA6\x82\xE5\xAE\x9E"},
      {"modern_vivid", "\xE9\xB2\x9C\xE6\x98\x8E", "\xE6\xB5\x93\xE9\x83\x81 \xC2\xB7 \xE5\x8F\x8D\xE5\xB7\xAE\xE5\xBC\xBA \xC2\xB7 \xE8\x93\x9D\xE7\xBB\xBF\xE6\xB7\xB1\xE8\x80\x8C\xE4\xB8\x8D\xE8\x89\xB3"},
      {"modern_soft", "\xE6\x9F\x94\xE5\x92\x8C", "\xE5\x8F\x8D\xE5\xB7\xAE\xE5\xB9\xB3\xE7\xBC\x93 \xC2\xB7 \xE8\x82\xA4\xE8\x89\xB2\xE6\xB6\xA6 \xC2\xB7 \xE9\xAB\x98\xE5\x85\x89\xE4\xBB\x8E\xE5\xAE\xB9"},
  }};
  return k;
}

std::string modernJson() {
  std::string j = "{\"modern\":[";
  const auto& m = modernRenderings();
  for (std::size_t i = 0; i < m.size(); ++i)
    j += std::string(i ? "," : "") + "{\"id\":\"" + m[i].id + "\",\"name\":\"" + m[i].name + "\",\"tags\":\"" + m[i].tags + "\"}";
  return j + "]}";
}

const ModernRecipe& modernRecipe(ModernKind k) {
  // Three intents (thumbnail-level different): 通透 neutral colour, crisp micro-contrast, cool clean whites; 鲜明 strong contrast, rich
  // colour, deep skies and foliage, warm highlights; 柔和 flat mid-tones, airy (lifted colour lightness), restrained chroma, no added clarity.
  //                                 contrast chroma blueDeep skin  clarity texture hdr  lift     tint  hue
  static const ModernRecipe kClean{1.14f, 1.10f, 0.020f, 0.70f, 22.f, 14.f, 35.f, 0.000f, 0.008f, 255.f};
  static const ModernRecipe kVivid{1.42f, 1.45f, 0.045f, 0.75f, 20.f, 6.f, 45.f, -0.010f, 0.008f, 65.f};
  static const ModernRecipe kSoft{0.88f, 1.00f, 0.000f, 0.90f, -8.f, -6.f, 55.f, 0.035f, 0.004f, 75.f};
  switch (k) {
    case ModernKind::Vivid: return kVivid;
    case ModernKind::Soft: return kSoft;
    default: return kClean;
  }
}

float modernEncode(float x) {
  x = std::max(x, 0.f);
  return x <= 0.0031308f ? 12.92f * x : 1.055f * std::pow(x, 1.f / 2.4f) - 0.055f;
}
float modernDecode(float x) {
  x = std::max(x, 0.f);
  return x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f);
}

namespace {
float pct(std::vector<float>& v, double q) {
  const std::size_t k = std::min(v.size() - 1, std::size_t(q * double(v.size() - 1) + 0.5));
  std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(k), v.end());
  return v[k];
}
}  // namespace

// End points. Black = P0.5 of the luminance (a few dead / hot pixels and a sliver of deep shadow may sit under it; never above
// 3 EV below mid-grey). White = P99.7 (the brightest 0.3 % may clip: a lamp, a glint, a small sun does not set the white point),
// limited to 4x P97 (a few specular pixels cannot flatten the whole picture) and at least 1.5 EV above mid-grey and 1.5 x P90 (a large bright area, a washed-out background, keeps 0.6 EV of headroom instead of being driven to white).
// Clipped data sits at or above the white point and maps to display white. Low-contrast guard: the encoded (sRGB OETF) span
// white - black is at least kMinSpan = 0.62, i.e. the stretch is at most 1 / 0.62 = 1.6 x in encoded units; a narrower scene
// (fog, flat overcast, a dark frame) has both end points moved away from the data, symmetrically about mid-grey, black not below 0.
ModernEnds modernEnds(float* Y, std::size_t n, float mid) {
  ModernEnds e;
  if (n < 16) return e;
  std::vector<float> v(Y, Y + n);
  for (float& x : v) x = std::max(x, 0.f);
  const float p50 = pct(v, 0.50), p90 = pct(v, 0.90), p97 = pct(v, 0.97);
  float yb = pct(v, 0.005), yw = pct(v, 0.997);
  e.p50 = p50;
  yw = std::min(yw, 4.f * p97);
  yw = std::max(yw, std::max(1.5f * p90, mid * std::exp2(1.5f)));
  yb = std::min(yb, mid * std::exp2(-3.f));
  constexpr float kMinSpan = 0.62f;
  float pb = modernEncode(yb), pw = modernEncode(yw);
  const float pm = modernEncode(mid);
  if (pw - pb < kMinSpan) {
    const float k = kMinSpan / std::max(pw - pb, 1e-4f);
    pb = pm - (pm - pb) * k;
    pw = pm + (pw - pm) * k;
    if (pb < 0.f) {
      pw += -pb;
      pb = 0.f;
    }
    pw = std::max(pw, pb + kMinSpan);
    e.stretchLimited = true;
  }
  e.span = pw - pb;
  e.black = modernDecode(pb);
  e.white = modernDecode(pw);
  return e;
}

ModernDist modernDistribution(const float* E, const float* S, std::size_t n, float pb, float pw) {
  ModernDist d;
  if (n < 16) return d;
  const float span = std::max(pw - pb, 1e-4f);
  double hi = 0, lo = 0, mid = 0, sat = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const float t = (E[i] - pb) / span;
    hi += t > 0.72f;
    lo += t < 0.22f;
    mid += t > 0.3f && t < 0.7f;
    sat += S[i];
  }
  const float fh = float(hi / double(n)), fl = float(lo / double(n)), fm = float(mid / double(n)), ms = float(sat / double(n));
  auto ss = [](float a, float b, float x) {
    const float u = std::clamp((x - a) / (b - a), 0.f, 1.f);
    return u * u * (3.f - 2.f * u);
  };
  d.wHi = ss(0.25f, 0.55f, fh);
  d.wLo = ss(0.30f, 0.60f, fl);
  d.gap = (1.f - ss(0.08f, 0.25f, fm)) * (1.f - ss(0.25f, 0.50f, ms)) * ((fh + fl) > 0.8f ? 1.f : 0.f);
  return d;
}

// Two power pieces meeting at the pivot with equal slope: toe y = ym (t / tm)^a (exponent >= 1: a short clean toe, black stays
// black, nothing lifted), shoulder y = 1 - (1 - ym) (1 - u)^b with u = (t - tm) / (1 - tm), b >= 1 (zero slope at white: the
// highest tones roll into display white). Slope at the pivot = contrast * (identity slope through the pivot).
float modernCurve(float t, float tm, float ym, float contrast, float shoulderK) {
  if (!(t > 0.f)) return 0.f;
  if (t >= 1.f) return 1.f;
  tm = std::clamp(tm, 0.05f, 0.95f);
  ym = std::clamp(ym, 0.05f, 0.95f);
  const float lo = std::min(1.f, contrast);  // a rendering below 1 (soft) may flatten the mid-tones, never below its own value
  const float a = std::clamp(contrast * tm / ym, lo, 2.2f);
  const float sp = a * ym / tm;
  float b = std::clamp(sp * (1.f - tm) / (1.f - ym), lo, 3.f);
  b = 1.f + (b - 1.f) * shoulderK;
  if (t < tm) return ym * std::pow(t / tm, a);
  const float u = (t - tm) / (1.f - tm);
  return 1.f - (1.f - ym) * std::pow(1.f - u, b);
}

}  // namespace dapple
