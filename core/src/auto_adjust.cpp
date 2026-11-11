#include "dapple/auto_adjust.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "color.h"

namespace dapple {

using namespace color;

namespace {

constexpr int kAnalysisEdge = 512;

// Weighted percentile over log2 luminance using a fine histogram.
struct LogHistogram {
  static constexpr float kMin = -16.f, kMax = 4.f, kStep = 0.02f;
  std::vector<double> bins = std::vector<double>(std::size_t((kMax - kMin) / kStep) + 1, 0.0);
  double total = 0;

  void add(float luma, double w) {
    const float e = std::log2(std::max(luma, 1e-6f));
    const int i = std::clamp(int((e - kMin) / kStep), 0, int(bins.size()) - 1);
    bins[i] += w;
    total += w;
  }
  float percentile(double q) const {
    double acc = 0;
    for (std::size_t i = 0; i < bins.size(); ++i) {
      acc += bins[i];
      if (acc >= q * total) return std::exp2(kMin + (i + 0.5f) * kStep);
    }
    return std::exp2(kMax);
  }
};

// OKLab (a, b) of a mid-grey ProPhoto pixel after a relative WB change,
// mirroring the renderer's gain model.
void greyLab(float temperature, float tint, float out[2]) {
  constexpr float kTemp = 0.006f, kTint = 0.004f;
  float g[3] = {std::exp2(kTemp * temperature), std::exp2(-kTint * tint), std::exp2(-kTemp * temperature)};
  const float norm = proPhotoLuma(g);
  float pro[3], rgb[3], lab[3];
  for (int c = 0; c < 3; ++c) pro[c] = 0.18f * g[c] / norm;
  proPhotoToSrgb().apply(pro, rgb);
  linearSrgbToOklab(rgb, lab);
  out[0] = lab[1];
  out[1] = lab[2];
}

std::string fmt(const char* f, double v) {
  char buf[128];
  std::snprintf(buf, sizeof buf, f, v);
  return buf;
}

}  // namespace

AutoResult computeAutoAdjust(const SourceImage& image, const CameraProfile& camera, const AutoOptions& opt) {
  AutoResult r;
  const ImageF small = resizeToLongEdge(image.linear, kAnalysisEdge);
  if (small.empty()) return r;

  // Everything below is judged on what the viewer sees: the camera-matched render. The camera's own
  // rendering is the baseline of "correct"; the correction only acts on clear evidence of a defect.
  r.tone = analyzeTone(image, camera);
  const ToneAnalysis& tone = r.tone;

  const float baseGain = std::exp2(image.sceneReferred ? camera.baselineExposureEv : 0.f);
  const float clipLevel = image.sceneReferred ? 0.99f : 0.995f;

  LogHistogram hist;
  std::size_t clipped = 0;
  const float cx = small.width * 0.5f, cy = small.height * 0.5f;
  for (int y = 0; y < small.height; ++y) {
    const float* row = small.row(y);
    for (int x = 0; x < small.width; ++x) {
      const float* px = row + 3 * x;
      const float dx = (x - cx) / cx, dy = (y - cy) / cy;
      const double w = 1.0 + 1.5 * std::exp(-(dx * dx + dy * dy) / 0.18);  // centre weighted
      hist.add(proPhotoLuma(px) * baseGain, w);
      if (std::max({px[0], px[1], px[2]}) >= clipLevel) ++clipped;
    }
  }
  ImageStats& s = r.stats;
  s.medianLuma = hist.percentile(0.5);
  s.p01Luma = hist.percentile(0.01);
  s.p99Luma = hist.percentile(0.99);
  s.clippedFraction = float(clipped) / float(std::size_t(small.width) * small.height);

  const float k = std::clamp(opt.strength, 0.f, 1.f);
  EditParams& c = r.correction;

  // ---- exposure: rescue only frames that are uniformly dark ----
  // A low-key photo (night scene, dark room, dark subject on black) has a low median but real
  // highlights; the camera rendered it as the photographer intended and it must stay that way.
  // Only a frame where *nothing* reaches a normal highlight (p95 of the rendered luminance below
  // 0.45, median below 0.16) is treated as under-exposed, and it is lifted only part of the way.
  bool rescued = false;
  if (opt.exposure && tone.p50 < 0.16f && tone.p95 < 0.45f && tone.p50 > 0.005f) {
    const float want = 0.20f;  // encoded median to aim for
    const float ev = std::log2(srgbDecode(want) / srgbDecode(tone.p50));
    c.exposure = std::clamp(ev * 0.6f, 0.f, 0.8f) * k;
    if (c.exposure > 0.05f) {
      rescued = true;
      r.notes.push_back(fmt("Uniformly dark frame, nothing bright in it: exposure +%.2f EV", c.exposure));
    } else {
      c.exposure = 0.f;
    }
  }

  // ---- tonal class: gentle move towards the photo's own class ----
  // Empty target = nearest class, and only the part of the deviation beyond the class tolerance is
  // corrected (see toneTargetCurve), so a photo that is typical of its class is not touched and a
  // low-key / high-key photo is never pulled towards middle grey.
  if (opt.tone && !rescued) {
    // Only when the curve would really move something (more than ~1% of the tonal scale).
    float moved = 0.f;
    for (const auto& p : toneTargetCurve(tone, "", 1.f)) moved = std::max(moved, std::fabs(p.second - p.first) / 255.f);
    if (moved > 0.01f) {
      c.toneStrength = 60.f * k;
      c.toneTarget.clear();
      r.notes.push_back("Tone: eased towards its own class (" + tone.nearest + ") where it falls outside the class tolerance");
    }
  }

  // ---- white balance: only on strong evidence ----
  if (opt.whiteBalance) {
    const Mat3& toSrgb = proPhotoToSrgb();
    const float gain = baseGain * std::exp2(c.exposure);
    double sa = 0, sb = 0, sw = 0;
    std::size_t used = 0;
    std::vector<std::pair<float, float>> casts;
    for (int y = 0; y < small.height; ++y) {
      const float* row = small.row(y);
      for (int x = 0; x < small.width; ++x) {
        const float* px = row + 3 * x;
        if (std::max({px[0], px[1], px[2]}) >= clipLevel) continue;
        const float Y = proPhotoLuma(px) * gain;
        if (Y < 0.03f || Y > 0.6f) continue;
        float rgb[3], lab[3];
        toSrgb.apply(px, rgb);
        linearSrgbToOklab(rgb, lab);
        const float C = std::hypot(lab[1], lab[2]);
        if (C > 0.035f) continue;  // saturated pixels are colours, not evidence of a cast
        // Blue-dominant pixels (sky, clouds, shade, snow in shadow) are legitimately bluish.
        float hDeg = std::atan2(lab[2], lab[1]) * 180.f / kPi;
        if (hDeg < 0) hDeg += 360.f;
        if (C > 0.008f && hDeg > 190.f && hDeg < 310.f) continue;
        sa += lab[1];
        sb += lab[2];
        sw += 1;
        ++used;
        casts.emplace_back(lab[1], lab[2]);
      }
    }
    s.neutralCoverage = float(used) / float(std::size_t(small.width) * small.height);
    if (sw > 0) {
      s.neutralCastA = float(sa / sw);
      s.neutralCastB = float(sb / sw);
    }
    const float cast = std::hypot(s.neutralCastA, s.neutralCastB);

    // For a RAW the camera's as-shot white balance is the first piece of evidence: when it is a
    // plausible illuminant (everything from tungsten to open shade) it is trusted and the pixels are
    // not second-guessed. Display-referred sources have no such metadata.
    const float wbR = image.meta.asShotWb[0], wbB = image.meta.asShotWb[2];
    const bool asShotPlausible = image.sceneReferred && wbR > 1.0f && wbR < 4.5f && wbB > 1.0f && wbB < 5.0f;
    // Evidence: a large share of the frame is near-neutral, the cast is clearly visible, and nearly all
    // those pixels agree on it.
    float agree = 0.f;
    if (!casts.empty() && cast > 1e-6f) {
      std::size_t ok = 0;
      for (const auto& p : casts)
        if ((p.first * s.neutralCastA + p.second * s.neutralCastB) / (std::hypot(p.first, p.second) * cast + 1e-9f) > 0.5f) ++ok;
      agree = float(ok) / float(casts.size());
    }
    const bool strong = s.neutralCoverage > 0.25f && cast > 0.020f && agree > 0.85f;
    if (!asShotPlausible && strong) {
      // Solve J * [dTemp, dTint] = -cast with a finite-difference Jacobian.
      float g0[2], gt[2], gi[2];
      greyLab(0, 0, g0);
      greyLab(10, 0, gt);
      greyLab(0, 10, gi);
      const float j00 = (gt[0] - g0[0]) / 10, j10 = (gt[1] - g0[1]) / 10;
      const float j01 = (gi[0] - g0[0]) / 10, j11 = (gi[1] - g0[1]) / 10;
      const float det = j00 * j11 - j01 * j10;
      if (std::abs(det) > 1e-12f) {
        const float ta = -s.neutralCastA, tb = -s.neutralCastB;
        const float dTemp = (ta * j11 - j01 * tb) / det;
        const float dTint = (j00 * tb - ta * j10) / det;
        constexpr float kPartial = 0.5f;  // keep some of the scene's mood
        c.temperature = std::clamp(dTemp * kPartial, -12.f, 12.f) * k;
        c.tint = std::clamp(dTint * kPartial, -12.f, 12.f) * k;
        r.notes.push_back(fmt("Strong colour cast on a large neutral area: temperature %+.0f", c.temperature) +
                          fmt(", tint %+.0f", c.tint));
      }
    }
  }
  return r;
}

}  // namespace dapple
