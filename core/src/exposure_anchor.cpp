#include "exposure_anchor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "color.h"
#include "dapple/decoder.h"

namespace dapple {
namespace {

constexpr int kEdge = 192;                        // comparison grid, long edge
constexpr int kMinPreviewEdge = 160;              // smaller previews carry too little
constexpr float kMidLoDefault = 0.30f, kMidHiDefault = 0.70f;   // preview encoded luma window (sRGB 77 .. 179): where tone curves are close to plain sRGB
constexpr float kBorder = 0.10f;                  // vignetting / lens-correction border ignored
constexpr float kMaxSpread = 1.0f;                // EV: robust sigma of the per-pixel ratio above which the preview is not a usable witness

// center-crop to aspect `ar` (w / h)
ImageF cropToAspect(const ImageF& in, double ar) {
  const int w = in.width, h = in.height;
  int nw = w, nh = h;
  if (double(w) / h > ar) nw = std::max(1, int(std::lround(h * ar)));
  else nh = std::max(1, int(std::lround(w / ar)));
  const int x0 = (w - nw) / 2, y0 = (h - nh) / 2;
  ImageF out(nw, nh);
  for (int y = 0; y < nh; ++y) std::copy_n(in.row(y0 + y) + 3 * x0, std::size_t(nw) * 3, out.row(y));
  return out;
}

float percentileOf(std::vector<float>& v, double q) {
  if (v.empty()) return 0.f;
  const std::size_t k = std::min(v.size() - 1, std::size_t(q * double(v.size())));
  std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(k), v.end());
  return v[k];
}

// DAPPLE_ANCHOR_WINDOW=lo,hi overrides the mid-tone window (sensitivity study only)
void midWindow(float& lo, float& hi) {
  lo = kMidLoDefault, hi = kMidHiDefault;
  if (const char* e = std::getenv("DAPPLE_ANCHOR_WINDOW")) std::sscanf(e, "%f,%f", &lo, &hi);
}

}  // namespace

ExposureAnchor computeExposureAnchor(const std::filesystem::path& raw, const ImageF& linear, float dngBaselineEv) {
  ExposureAnchor a;
  if (linear.empty()) return a;

  // ---- tier 3 statistics (always computed: diagnostics and last resort) ----
  {
    const ImageF s = resizeToLongEdge(linear, kEdge);
    std::vector<float> y;
    y.reserve(std::size_t(s.width) * s.height);
    for (int i = 0; i < s.width * s.height; ++i) y.push_back(color::proPhotoLuma(s.data.data() + 3 * i));
    double sum = 0;
    for (float v : y) sum += v;
    const float mean = std::max(float(sum / double(std::max<std::size_t>(1, y.size()))), 1e-5f), hi = std::max(percentileOf(y, 0.995), 1e-5f);
    // Constants derived on 40 public RAWs whose tier 1 anchor was valid (docs/engine/pipeline-architecture.md section 29): the mean luminance
    // predicts the preview anchor best (residual 0.57 EV, against 0.83 for the median and 0.64 for the 99.5th percentile) as log2(0.22 / mean);
    // shrinking that halfway to the median anchor of the set (1.0 EV) gives 0.39 EV residual, max 0.96, and keeps a low-key photo from being
    // pulled towards middle grey; the 99.5th percentile may not pass 1.5 after the anchor (highlight protection).
    constexpr float kMeanTarget = 0.22f, kPrior = 1.0f, kShrink = 0.5f, kHighlightCap = 1.5f;
    float ev = kShrink * std::log2(kMeanTarget / mean) + (1.f - kShrink) * kPrior;
    ev = std::min(ev, std::log2(kHighlightCap / hi));
    a.statsEv = std::clamp(ev, -1.5f, 3.5f);
  }

  // ---- tier 1: the embedded preview ----
  bool tier1 = false;
  auto prev = extractEmbeddedPreview(raw, 0);
  if (!prev) {
    a.note = "no embedded preview";
  } else if (std::max(prev->width, prev->height) < kMinPreviewEdge) {
    a.note = "preview too small (" + std::to_string(prev->width) + "x" + std::to_string(prev->height) + ")";
  } else {
    ImageF p(prev->width, prev->height);
    for (std::size_t i = 0; i < p.data.size(); ++i) p.data[i] = prev->data[i] / 255.f;
    const double arP = double(p.width) / p.height, arR = double(linear.width) / linear.height;
    const double ratio = std::max(arP, arR) / std::min(arP, arR);
    if (ratio > 1.25) {
      a.note = "preview aspect cannot be reconciled";
    } else {
      const double arP2 = ratio > 1.02 ? std::min(arP, arR) : arP, arR2 = ratio > 1.02 ? std::min(arP, arR) : arR;
      const ImageF pc = resizeToLongEdge(cropToAspect(p, arP2), kEdge);
      const ImageF rc = resizeToLongEdge(cropToAspect(linear, arR2), kEdge);
      const int W = std::min(pc.width, rc.width), H = std::min(pc.height, rc.height);
      float midLo, midHi;
      midWindow(midLo, midHi);
      std::vector<float> lr;
      lr.reserve(std::size_t(W) * H);
      const int bx = int(W * kBorder), by = int(H * kBorder);
      std::size_t total = 0;
      for (int y = by; y < H - by; ++y)
        for (int x = bx; x < W - bx; ++x) {
          ++total;
          const float* q = pc.row(y) + 3 * x;
          const float* r = rc.row(y) + 3 * x;
          const float enc = 0.2126729f * q[0] + 0.7151522f * q[1] + 0.0721750f * q[2];  // encoded luma
          if (enc < midLo || enc > midHi) continue;
          if (std::max({q[0], q[1], q[2]}) > 0.97f || std::min({q[0], q[1], q[2]}) < 0.03f) continue;
          if (std::max({r[0], r[1], r[2]}) > 0.95f) continue;  // clipped raw
          const float yo = color::proPhotoLuma(r);
          if (yo < 2e-4f) continue;
          const float yp = 0.2126729f * color::srgbDecode(q[0]) + 0.7151522f * color::srgbDecode(q[1]) + 0.0721750f * color::srgbDecode(q[2]);
          lr.push_back(std::log2(yp / yo));
        }
      a.previewPixels = int(lr.size());
      if (total > 0 && lr.size() >= std::max<std::size_t>(300, total / 50)) {
        std::vector<float> t1 = lr, t2 = lr, t3 = lr;
        const float med = percentileOf(t1, 0.5), q1 = percentileOf(t2, 0.25), q3 = percentileOf(t3, 0.75);
        const float spread = (q3 - q1) / 1.349f;
        a.previewEv = med;
        a.previewSpread = spread;
        a.previewConfidence = std::clamp(float(lr.size()) / (0.25f * float(total)), 0.f, 1.f) * std::exp(-spread / 0.7f);
        if (spread <= kMaxSpread && a.previewConfidence >= 0.30f) tier1 = true;
        char b[160];
        std::snprintf(b, sizeof b, "preview %dx%d, %zu mid-tone px, spread %.2f EV", prev->width, prev->height, lr.size(), spread);
        a.note = b;
      } else {
        a.note = "preview has too few mid-tone pixels";
      }
    }
  }

  if (tier1) {
    a.tier = 1, a.ev = a.previewEv, a.confidence = a.previewConfidence;
  } else if (std::isfinite(dngBaselineEv) && dngBaselineEv != 0.f) {  // 0 is the specification default: not information
    a.tier = 2, a.ev = dngBaselineEv, a.confidence = 0.5f;
    a.note += (a.note.empty() ? "" : "; ") + std::string("DNG BaselineExposure");
  } else {
    a.tier = 3, a.ev = a.statsEv, a.confidence = 0.2f;
    a.note += (a.note.empty() ? "" : "; ") + std::string("scene statistics (low confidence)");
  }
  a.ev = std::clamp(a.ev, -3.f, 5.f);
  return a;
}

}  // namespace dapple
