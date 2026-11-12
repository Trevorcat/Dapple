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

// Removes black bars (rows / columns whose pixels are all below 0.06 encoded) from the four edges, at most a quarter of the size per side.
ImageF trimLetterbox(const ImageF& in) {
  auto rowBlack = [&](int y) {
    const float* r = in.row(y);
    for (int x = 0; x < in.width * 3; ++x)
      if (r[x] > 0.06f) return false;
    return true;
  };
  auto colBlack = [&](int x) {
    for (int y = 0; y < in.height; ++y) {
      const float* r = in.row(y) + 3 * x;
      if (r[0] > 0.06f || r[1] > 0.06f || r[2] > 0.06f) return false;
    }
    return true;
  };
  int t = 0, b = 0, l = 0, rr = 0;
  while (t < in.height / 4 && rowBlack(t)) ++t;
  while (b < in.height / 4 && rowBlack(in.height - 1 - b)) ++b;
  while (l < in.width / 4 && colBlack(l)) ++l;
  while (rr < in.width / 4 && colBlack(in.width - 1 - rr)) ++rr;
  if (t + b + l + rr == 0) return in;
  ImageF out(in.width - l - rr, in.height - t - b);
  for (int y = 0; y < out.height; ++y) std::copy_n(in.row(y + t) + 3 * l, std::size_t(out.width) * 3, out.row(y));
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

ExposureAnchor computeExposureAnchor(const std::filesystem::path& raw, const ImageF& linear, float dngBaselineEv, const Image8* reference, const Image8* embedded, bool monochrome) {
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
    // A frame whose mean is below 1 % of white (6.6 stops under it) is a black frame or a noise frame: it carries no exposure information and a
    // lift would only amplify noise (a Galaxy Note10+ frame at 0.4 % got +3.2 EV). The largest tier 1 anchor of the 36 samples is 1.87 EV, so
    // the statistic never goes above 2.0 EV, and below 1 % it is held at the prior.
    ev = std::min(ev, 2.0f);
    if (mean < 0.01f) ev = std::min(ev, kPrior);
    a.statsEv = std::clamp(ev, -1.5f, 2.0f);
  }

  // ---- tier 1: the embedded preview ----
  bool tier1 = false;
  Result<Image8> prev = reference ? Result<Image8>(*reference) : embedded ? Result<Image8>(*embedded) : extractEmbeddedPreview(raw, 0);
  if (!prev) {
    a.note = "no embedded preview";
  } else if (std::max(prev->width, prev->height) < kMinPreviewEdge) {
    a.note = "preview too small (" + std::to_string(prev->width) + "x" + std::to_string(prev->height) + ")";
  } else {
    // box-average the 8-bit preview by an integer factor first (a 45 MP preview as float would cost 540 MB and 0.3 s): the grid below is 192 px
    const int k = std::max(1, std::max(prev->width, prev->height) / (4 * kEdge));
    ImageF p(prev->width / k, prev->height / k);
    for (int y = 0; y < p.height; ++y) {
      float* d = p.row(y);
      for (int x = 0; x < p.width; ++x) {
        int s[3] = {0, 0, 0};
        for (int dy = 0; dy < k; ++dy) {
          const std::uint8_t* q = &prev->data[(std::size_t(y * k + dy) * prev->width + std::size_t(x) * k) * 3];
          for (int dx = 0; dx < k; ++dx) s[0] += q[3 * dx], s[1] += q[3 * dx + 1], s[2] += q[3 * dx + 2];
        }
        const float inv = 1.f / (255.f * float(k * k));
        d[3 * x] = s[0] * inv, d[3 * x + 1] = s[1] * inv, d[3 * x + 2] = s[2] * inv;
      }
    }
    p = trimLetterbox(p);  // a 4:3 thumbnail of a 3:2 picture carries black bars (Leica M Monochrom: 160 x 120 with the picture in 160 x 107)
    const double arP = double(p.width) / p.height, arR = double(linear.width) / linear.height;
    const double ratio = std::max(arP, arR) / std::min(arP, arR);
    if (ratio > 1.25) {
      a.note = "preview aspect cannot be reconciled";
    } else {
      const double arP2 = ratio > 1.02 ? std::min(arP, arR) : arP, arR2 = ratio > 1.02 ? std::min(arP, arR) : arR;
      const ImageF pc0 = resizeToLongEdge(cropToAspect(p, arP2), kEdge);
      const ImageF rc = resizeToLongEdge(cropToAspect(linear, arR2), kEdge);
      if (monochrome) {
        // luminance-only witness: ratio of the medians over the interior, accepted by rank correlation (see the header); the preview is tried as is and
        // turned by 180 degrees (a thumbnail may follow the sensor or the display orientation) and the better correlated one is used
        struct Mono { double rho = -2; float ev = 0; int n = 0; };
        auto monoRun = [&](const ImageF& pcv) {
          Mono mr;
          const int W = std::min(pcv.width, rc.width), H = std::min(pcv.height, rc.height);
          const int bx = int(W * kBorder), by = int(H * kBorder);
          std::vector<float> yp, yo;
          std::size_t total = 0;
          for (int y = by; y < H - by; ++y)
            for (int x = bx; x < W - bx; ++x) {
              ++total;
              const float* q = pcv.row(y) + 3 * x;
              const float* r = rc.row(y) + 3 * x;
              if (std::max({r[0], r[1], r[2]}) > 0.95f) continue;  // clipped raw
              const float lp = 0.2126729f * color::srgbDecode(q[0]) + 0.7151522f * color::srgbDecode(q[1]) + 0.0721750f * color::srgbDecode(q[2]);
              if (lp < 0.004f) continue;  // crushed blacks of the camera's contrasty rendering: ties, no information
              yp.push_back(lp);
              yo.push_back(color::proPhotoLuma(r));
            }
          mr.n = int(yp.size());
          if (total == 0 || yp.size() < std::max<std::size_t>(300, total / 10)) return mr;
          const std::size_t n = yp.size();
          auto ranks = [&](const std::vector<float>& v) {
            std::vector<std::size_t> idx(n);
            for (std::size_t i = 0; i < n; ++i) idx[i] = i;
            std::sort(idx.begin(), idx.end(), [&](std::size_t a2, std::size_t b2) { return v[a2] < v[b2]; });
            std::vector<float> r(n);
            for (std::size_t i = 0; i < n; ++i) r[idx[i]] = float(i);
            return r;
          };
          const std::vector<float> rp = ranks(yp), ro = ranks(yo);
          double mp = 0, mo = 0;
          for (std::size_t i = 0; i < n; ++i) mp += rp[i], mo += ro[i];
          mp /= double(n), mo /= double(n);
          double sxy = 0, sxx = 0, syy = 0;
          for (std::size_t i = 0; i < n; ++i) sxy += (rp[i] - mp) * (ro[i] - mo), sxx += (rp[i] - mp) * (rp[i] - mp), syy += (ro[i] - mo) * (ro[i] - mo);
          mr.rho = sxx > 0 && syy > 0 ? sxy / std::sqrt(sxx * syy) : 0.0;
          std::vector<float> a1 = yp, a2 = yo;
          const float medP = percentileOf(a1, 0.5), medO = percentileOf(a2, 0.5);
          mr.ev = medP > 1e-6f && medO > 1e-6f ? std::log2(medP / medO) : 0.f;
          if (!(medP > 1e-6f && medO > 1e-6f)) mr.rho = -2;
          return mr;
        };
        Mono best = monoRun(pc0);
        {
          ImageF rot(pc0.width, pc0.height);
          for (int y = 0; y < pc0.height; ++y)
            for (int x = 0; x < pc0.width; ++x) std::copy_n(pc0.row(pc0.height - 1 - y) + 3 * (pc0.width - 1 - x), 3, rot.row(y) + 3 * x);
          const Mono alt = monoRun(rot);
          if (alt.rho > best.rho) best = alt;
        }
        a.previewPixels = best.n;
        a.previewSpread = 0.f;
        a.previewConfidence = float(std::clamp(best.rho, 0.0, 1.0));
        a.previewEv = best.ev;
        if (best.rho >= 0.3) tier1 = true;  // wrong geometry gives -0.6 .. 0.05, the registered tiny Leica thumbnail 0.36, the K-3 III 1.00
        char b[160];
        std::snprintf(b, sizeof b, "monochrome: median luminance match, preview %dx%d, rank correlation %.2f", prev->width, prev->height, best.rho);
        a.note = b;
      } else {
      float midLo, midHi;
      midWindow(midLo, midHi);
      // log-ratios of the mid-tone pixels for one orientation of the preview
      struct Run { std::vector<float> lr; std::size_t total = 0; float spread = 1e9f; };
      auto collect = [&](const ImageF& pcv) {
        Run run;
        const int W = std::min(pcv.width, rc.width), H = std::min(pcv.height, rc.height);
        run.lr.reserve(std::size_t(W) * H);
        const int bx = int(W * kBorder), by = int(H * kBorder);
        for (int y = by; y < H - by; ++y)
          for (int x = bx; x < W - bx; ++x) {
            ++run.total;
            const float* q = pcv.row(y) + 3 * x;
            const float* r = rc.row(y) + 3 * x;
            const float enc = 0.2126729f * q[0] + 0.7151522f * q[1] + 0.0721750f * q[2];  // encoded luma
            if (enc < midLo || enc > midHi) continue;
            if (std::max({q[0], q[1], q[2]}) > 0.97f || std::min({q[0], q[1], q[2]}) < 0.03f) continue;
            if (std::max({r[0], r[1], r[2]}) > 0.95f) continue;  // clipped raw
            const float yo = color::proPhotoLuma(r);
            if (yo < 2e-4f) continue;
            const float yp = 0.2126729f * color::srgbDecode(q[0]) + 0.7151522f * color::srgbDecode(q[1]) + 0.0721750f * color::srgbDecode(q[2]);
            run.lr.push_back(std::log2(yp / yo));
          }
        if (run.lr.size() >= 50) {
          std::vector<float> t2 = run.lr, t3 = run.lr;
          run.spread = (percentileOf(t3, 0.75) - percentileOf(t2, 0.25)) / 1.349f;
        }
        return run;
      };
      Run run = collect(pc0);
      // A preview may keep the sensor orientation or be upright; for 180 degrees the two differ and the file's Orientation tag cannot tell which
      // one an embedded thumbnail follows: try the preview turned by 180 degrees too and keep the orientation that agrees with the raw better.
      if (run.spread > 0.35f) {
        ImageF rot(pc0.width, pc0.height);
        for (int y = 0; y < pc0.height; ++y)
          for (int x = 0; x < pc0.width; ++x) std::copy_n(pc0.row(pc0.height - 1 - y) + 3 * (pc0.width - 1 - x), 3, rot.row(y) + 3 * x);
        Run alt = collect(rot);
        if (alt.spread < run.spread - 0.15f) run = std::move(alt);
      }
      std::vector<float>& lr = run.lr;
      const std::size_t total = run.total;
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
      }  // !monochrome
    }
  }

  if (tier1) {
    a.tier = 1, a.ev = a.previewEv, a.confidence = a.previewConfidence;
    a.source = reference ? "reference-image" : "embedded-preview";
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
