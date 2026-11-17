#include "dapple/auto_adjust.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "color.h"
#include "engine/exec/engine_session.h"

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

// Encoded-luminance statistics of one proxy render (what the viewer sees).
struct RenderStats {
  float p005 = 0, p05 = 0, p10 = 0, p50 = 0, p90 = 0, p99 = 0, p995 = 0;
  float hiClip = 0, loClip = 0;  // shares of pixels at >= 254 / <= 1 (8-bit)
  ToneAnalysis tone;
};

bool renderStats(const EditParams& p, const CameraProfile& cam, const SourceImage& proxy, RenderStats& out) {
  ImageF f;
  std::string err;
  if (!engine::renderImageF(p, cam, nullptr, proxy, false, f, &err)) return false;
  const std::size_t n = std::size_t(f.width) * f.height;
  if (n == 0) return false;
  constexpr int kBins = 1024;
  std::vector<std::uint32_t> h(kBins, 0);
  std::size_t hi = 0, lo = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const float* px = &f.data[3 * i];
    const float lin = 0.2126729f * srgbDecode(std::clamp(px[0], 0.f, 1.f)) + 0.7151522f * srgbDecode(std::clamp(px[1], 0.f, 1.f)) +
                      0.0721750f * srgbDecode(std::clamp(px[2], 0.f, 1.f));
    const float e = srgbEncode(lin);
    ++h[std::size_t(std::min(kBins - 1, int(e * kBins)))];
    const float m = std::max({px[0], px[1], px[2]});
    if (std::lround(m * 255.f) >= 254) ++hi;
    if (std::lround(m * 255.f) <= 1) ++lo;
  }
  auto pct = [&](double q) {
    double acc = 0;
    for (int i = 0; i < kBins; ++i) {
      acc += h[std::size_t(i)];
      if (acc >= q * double(n)) return float(i + 0.5) / kBins;
    }
    return 1.f;
  };
  out.p005 = pct(0.005), out.p05 = pct(0.05), out.p10 = pct(0.10), out.p50 = pct(0.5);
  out.p90 = pct(0.90), out.p99 = pct(0.99), out.p995 = pct(0.995);
  out.hiClip = float(hi) / float(n);
  out.loClip = float(lo) / float(n);
  out.tone = analyzeToneImage(f);
  return true;
}

// EV between two encoded luminances.
float lumaEv(float from, float to) {
  return std::log2(std::max(srgbDecode(to), 1e-5f) / std::max(srgbDecode(from), 1e-5f));
}

}  // namespace

AutoResult computeAutoAdjust(const SourceImage& image, const CameraProfile& camera, const AutoOptions& opt) {
  AutoResult r;
  const ImageF small = resizeToLongEdge(image.linear, kAnalysisEdge);
  if (small.empty()) return r;

  SourceImage proxy;
  proxy.sceneReferred = image.sceneReferred;
  proxy.linear = resizeToLongEdge(image.linear, 256);

  // Everything is judged on what the viewer sees: the camera-rendered tone, encoded. The correction is a
  // Lightroom-style "Auto": a moderate, visible move on every photo, re-measured after each step.
  RenderStats base;
  if (!renderStats(EditParams{}, camera, proxy, base)) return r;
  r.tone = base.tone;

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

  const float k = std::clamp(opt.strength / 0.6f, 0.f, 1.67f);
  EditParams& c = r.correction;
  RenderStats st = base;
  auto rerender = [&]() {
    RenderStats t;
    if (renderStats(c, camera, proxy, t)) st = t;
  };

  // ---- exposure ----
  if (opt.exposure && k > 0.f) {
    float target = 0.45f;
    if (base.tone.key == "low") target = 0.22f;
    else if (base.tone.key == "high") target = 0.70f;
    // 70 % of the way, upwards only: a bright or high-key frame is intended (and its subject may stand against it), so a high
    // median never lowers the exposure; bright frames are handled by highlights and whites, and only real clipping lowers it below.
    float ev = std::max(lumaEv(base.p50, target), 0.f) * 0.7f;
    if (base.tone.key == "low") ev *= 0.7f;
    if (base.tone.key == "high") ev = 0.f;
    ev = std::clamp(ev, 0.f, 1.5f);
    // Do not push the brightest areas beyond what the highlights rule can recover.
    if (ev > 0.f) {
      const float room = lumaEv(std::max(base.p99, 0.05f), 0.985f);
      ev = std::min(ev, std::max(room, 0.f) + 0.35f);
    }
    if (std::fabs(ev) >= 0.1f) {
      c.exposure = ev * k;
      rerender();
      r.notes.push_back(fmt("Exposure %+.2f EV", c.exposure) + fmt(" (median %.2f", base.p50) + fmt(" -> %.2f)", st.p50));
    }
  }

  // ---- tone: highlights, shadows, whites, blacks, contrast, vibrance ----
  if (opt.tone && k > 0.f) {
    const bool lowKey = base.tone.key == "low";
    // Only clipping or a really hot top end: a bright frame with room to spare keeps its highlights (and the subject in front of it).
    const bool highKey = base.tone.key == "high";  // a white backdrop is the picture: only real clipping is pulled back
    if ((!highKey && st.p99 > 0.95f) || st.hiClip > 0.005f) {
      const float need = std::max(highKey ? 0.f : (st.p99 - 0.92f) / 0.06f, st.hiClip / 0.02f);
      c.highlights = -std::clamp(60.f * need, 20.f, 60.f) * k;
    }
    if (st.p05 < 0.13f && !lowKey) c.shadows = std::clamp(40.f * (0.13f - st.p05) / 0.09f, 16.f, 40.f) * k;
    if (c.highlights != 0.f || c.shadows != 0.f) rerender();

    c.whites = std::clamp((0.96f - st.p995) * 160.f, -20.f, 25.f) * k;
    c.blacks = std::clamp((0.025f - st.p005) * 400.f, -40.f, 15.f) * k;
    const float spread = st.p90 - st.p10;
    if (spread < 0.35f) c.contrast = 20.f * std::clamp((0.35f - spread) / 0.15f, 0.25f, 1.f) * k;
    else if (spread > 0.7f) c.contrast = -15.f * std::clamp((spread - 0.7f) / 0.1f, 0.25f, 1.f) * k;
    c.vibrance = 10.f * k;
    rerender();
    // Highlights are at their limit and the picture still clips: lower the exposure, by at most 0.3 EV.
    if (st.hiClip > 0.01f && c.highlights <= -59.f * k) {
      c.exposure = std::max(c.exposure - 0.3f * k, -0.3f * k);
      rerender();
    }

    // Verify: never create blown highlights or crushed blacks that were not there.
    for (int it = 0; it < 4; ++it) {
      const bool hiBad = st.hiClip > base.hiClip + 0.002f;
      const bool loBad = st.loClip > base.loClip + 0.002f;
      if (!hiBad && !loBad) break;
      if (hiBad) {
        c.whites -= 10.f;
        c.highlights -= 10.f;
        c.exposure -= 0.1f;
      }
      if (loBad) {
        c.blacks += 12.f;
        c.contrast = std::min(c.contrast, 0.f);
      }
      rerender();
    }
    r.notes.push_back(fmt("Tone: highlights %+.0f", c.highlights) + fmt(", shadows %+.0f", c.shadows) + fmt(", whites %+.0f", c.whites) +
                      fmt(", blacks %+.0f", c.blacks) + fmt(", contrast %+.0f", c.contrast));
  }

  // ---- white balance ----
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
    // plausible illuminant it is trusted and the pixels are not second-guessed.
    const float wbR = image.meta.asShotWb[0], wbB = image.meta.asShotWb[2];
    const bool asShotPlausible = image.sceneReferred && wbR > 1.0f && wbR < 4.5f && wbB > 1.0f && wbB < 5.0f;
    float agree = 0.f;
    if (!casts.empty() && cast > 1e-6f) {
      std::size_t ok = 0;
      for (const auto& p : casts)
        if ((p.first * s.neutralCastA + p.second * s.neutralCastB) / (std::hypot(p.first, p.second) * cast + 1e-9f) > 0.5f) ++ok;
      agree = float(ok) / float(casts.size());
    }
    const bool strong = s.neutralCoverage > 0.15f && cast > 0.012f && agree > 0.75f;
    if (!asShotPlausible && strong && k > 0.f) {
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
        constexpr float kPartial = 0.6f;  // keep some of the scene's mood
        c.temperature = std::clamp(dTemp * kPartial, -15.f, 15.f) * k;
        c.tint = std::clamp(dTint * kPartial, -15.f, 15.f) * k;
        r.notes.push_back(fmt("Colour cast on a neutral area: temperature %+.0f", c.temperature) + fmt(", tint %+.0f", c.tint));
      }
    }
  }
  return r;
}

}  // namespace dapple
