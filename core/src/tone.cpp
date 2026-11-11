#include "dapple/tone.h"

#include <algorithm>
#include <cmath>

#include "color.h"
#include "dapple/renderer.h"

namespace dapple {

using namespace color;

namespace {
constexpr int kFine = 512;  // fine histogram for percentiles
constexpr int kProxyEdge = 256;
}  // namespace

const std::array<ToneClassInfo, 9>& toneClasses() {
  // Prototypes (p5, p50, p95) of encoded luminance; set from the distribution of ILCE-7RM5
  // photos rendered with the camera look (see docs/architecture.md).
  static const std::array<ToneClassInfo, 9> k{{
      {"low-short", "low", "short", 0.10f, 0.22f, 0.40f},
      {"low-mid", "low", "mid", 0.04f, 0.22f, 0.66f},
      {"low-long", "low", "long", 0.015f, 0.24f, 0.93f},
      {"mid-short", "mid", "short", 0.28f, 0.46f, 0.64f},
      {"mid-mid", "mid", "mid", 0.10f, 0.46f, 0.76f},
      {"mid-long", "mid", "long", 0.04f, 0.46f, 0.95f},
      {"high-short", "high", "short", 0.55f, 0.70f, 0.88f},
      {"high-mid", "high", "mid", 0.30f, 0.72f, 0.94f},
      {"high-long", "high", "long", 0.07f, 0.72f, 0.985f},
  }};
  return k;
}

int toneClassIndex(const std::string& name) {
  const auto& c = toneClasses();
  for (int i = 0; i < 9; ++i)
    if (name == c[i].name) return i;
  return -1;
}

ToneAnalysis analyzeToneImage(const ImageF& im) {
  ToneAnalysis a;
  a.histogram.assign(ToneAnalysis::kBins, 0.f);
  if (im.empty()) {
    a.nearest = "mid-mid";
    a.key = "mid";
    a.range = "mid";
    return a;
  }
  std::vector<double> fine(kFine, 0.0);
  const std::size_t n = std::size_t(im.width) * im.height;
  for (std::size_t i = 0; i < n; ++i) {
    const float* p = &im.data[3 * i];
    const float lin = 0.2126729f * srgbDecode(std::clamp(p[0], 0.f, 1.f)) +
                      0.7151522f * srgbDecode(std::clamp(p[1], 0.f, 1.f)) +
                      0.0721750f * srgbDecode(std::clamp(p[2], 0.f, 1.f));
    const float e = srgbEncode(lin);
    fine[std::min(kFine - 1, int(e * kFine))] += 1.0;
  }
  for (int i = 0; i < kFine; ++i) a.histogram[i * ToneAnalysis::kBins / kFine] += float(fine[i] / double(n));
  auto pct = [&](double q) {
    double acc = 0;
    for (int i = 0; i < kFine; ++i) {
      const double next = acc + fine[i];
      if (next >= q * double(n)) {
        const double t = fine[i] > 0 ? (q * double(n) - acc) / fine[i] : 0.5;
        return float((i + t) / kFine);
      }
      acc = next;
    }
    return 1.f;
  };
  a.p5 = pct(0.05);
  a.p50 = pct(0.50);
  a.p95 = pct(0.95);

  const auto& cls = toneClasses();
  float best = 1e9f, second = 1e9f;
  int bi = 4;
  for (int i = 0; i < 9; ++i) {
    const float d5 = a.p5 - cls[i].p5, d50 = (a.p50 - cls[i].p50) * 1.5f, d95 = a.p95 - cls[i].p95;
    const float d = std::sqrt(d5 * d5 + d50 * d50 + d95 * d95);
    if (d < best) {
      second = best;
      best = d;
      bi = i;
    } else if (d < second) {
      second = d;
    }
  }
  a.key = cls[bi].key;
  a.range = cls[bi].range;
  a.nearest = cls[bi].name;
  a.distance = best;
  a.margin = second - best;
  return a;
}

ToneAnalysis analyzeTone(const SourceImage& source, const CameraProfile& camera, const EditParams& look) {
  SourceImage proxy;
  proxy.sceneReferred = source.sceneReferred;
  proxy.linear = resizeToLongEdge(source.linear, kProxyEdge);
  EditParams p = look;
  p.toneStrength = 0.f;
  p.toneTarget.clear();
  return analyzeToneImage(Renderer(p, camera, nullptr).renderFloat(proxy, false));
}

std::vector<std::pair<float, float>> toneTargetCurve(const ToneAnalysis& a, const std::string& target,
                                                     float strength, float reach) {
  strength = std::clamp(strength, 0.f, 1.f);
  const int ti = target.empty() ? toneClassIndex(a.nearest) : toneClassIndex(target);
  if (ti < 0 || strength <= 0.f || a.histogram.empty()) return {};
  const ToneClassInfo& t = toneClasses()[ti];

  // Wanted anchor shifts, limited so the move stays gentle and protects both ends.
  float d5 = t.p5 - a.p5, d50 = t.p50 - a.p50, d95 = t.p95 - a.p95;
  if (target.empty()) {
    // "Own class": a photo that is typical of its class is left alone; only the part of the
    // deviation beyond the class tolerance is corrected.
    auto beyond = [](float d, float tol) { return d > tol ? d - tol : d < -tol ? d + tol : 0.f; };
    d5 = beyond(d5, 0.10f);
    d50 = beyond(d50, 0.15f);
    d95 = beyond(d95, 0.10f);
  }
  reach = std::max(1.f, reach);
  d50 = std::clamp(d50, -0.10f * reach, 0.10f * reach);
  d5 = std::clamp(d5, -0.04f * reach, 0.04f * reach) * smoothstep(0.005f, 0.06f, a.p5);     // true blacks stay black
  d95 = std::clamp(d95, -0.05f * reach, 0.05f * reach) * (reach > 1.f ? 1.f : smoothstep(0.995f, 0.93f, a.p95));  // clipped highlights stay
  d5 *= strength;
  d50 *= strength;
  d95 *= strength;

  const float x[3] = {a.p5, a.p50, a.p95};
  const float y[3] = {a.p5 + d5, a.p50 + d50, a.p95 + d95};
  std::vector<std::pair<float, float>> pts;
  pts.emplace_back(0.f, 0.f);
  float px = 0.f, py = 0.f;
  for (int i = 0; i < 3; ++i) {
    if (x[i] - px < 0.04f || x[i] > (reach > 1.f ? 0.997f : 0.97f)) continue;  // anchors that collapse onto a neighbour
    const float dx = x[i] - px;
    const float lo = reach > 1.f ? 0.15f : 0.4f, hi = reach > 1.f ? 4.f : 2.5f;
    float yy = std::clamp(y[i], py + lo * dx, py + hi * dx);  // slope limits
    yy = std::min(yy, 1.f - 0.4f * (1.f - x[i]));                   // and room left for the top end
    pts.emplace_back(x[i] * 255.f, yy * 255.f);
    px = x[i];
    py = yy;
  }
  pts.emplace_back(255.f, 255.f);
  return pts;
}

}  // namespace dapple
