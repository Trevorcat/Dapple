#include "dapple/renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>

#include "color.h"
#include "dapple/film.h"
#include "dapple/look.h"
#include "dapple/tone.h"
#include "look_internal.h"
#include "parallel.h"
#include "spatial.h"

namespace dapple {

using namespace color;

namespace {

// Working frames (film input / float output of a frame) are recycled between renders: a fresh 20 MB buffer costs
// ~5 ms of page faults, more than the whole film run of a preview. Big frames (> 8 MP) are not kept.
class ScratchImage {
 public:
  ScratchImage() = default;
  ScratchImage(const ScratchImage&) = delete;
  ScratchImage& operator=(const ScratchImage&) = delete;
  ~ScratchImage() {
    if (img_.data.capacity() == 0 || img_.data.capacity() > kKeepFloats) return;
    std::lock_guard<std::mutex> lock(mtx());
    auto& s = store();
    s.push_back(std::move(img_));
    if (s.size() > 3) {
      auto smallest = std::min_element(s.begin(), s.end(), [](const ImageF& a, const ImageF& b) {
        return a.data.capacity() < b.data.capacity();
      });
      s.erase(smallest);
    }
  }
  // Frame of w x h; the pixel values are undefined (callers overwrite every pixel).
  ImageF& get(int w, int h) {
    const std::size_t need = std::size_t(w) * h * 3;
    {
      std::lock_guard<std::mutex> lock(mtx());
      auto& s = store();
      int best = -1;
      for (int i = 0; i < int(s.size()); ++i)
        if (s[i].data.capacity() >= need && (best < 0 || s[i].data.capacity() < s[best].data.capacity())) best = i;
      if (best >= 0) {
        img_ = std::move(s[best]);
        s.erase(s.begin() + best);
      }
    }
    img_.width = w;
    img_.height = h;
    img_.data.resize(need);
    return img_;
  }

 private:
  static constexpr std::size_t kKeepFloats = std::size_t(8'000'000) * 3;
  static std::mutex& mtx() {
    static std::mutex* m = new std::mutex;
    return *m;
  }
  static std::vector<ImageF>& store() {
    static std::vector<ImageF>* s = new std::vector<ImageF>;
    return *s;
  }
  ImageF img_;
};

constexpr float kMidGrey = 0.18f;
constexpr int kCurveSamples = 1024;

// Base tone curve of the scene-referred path, fitted to the median response of
// camera JPEGs (Sony ILCE-7RM5, 50 ARW/JPEG pairs; see docs/architecture.md):
// a toe that darkens deep shadows (in stops, at the bottom end) plus the
// highlight shoulder below. Mid-tone placement and slope come from the
// CameraProfile (baselineExposureEv / baselineContrast).
constexpr float kToeStops = 2.0f;
constexpr float kToeStart = -1.0f, kToeEnd = -6.5f;

// Highlight shoulder: identity below `knee`, then an exponential roll-off
// that approaches (but never reaches) 1. Applied to scene-referred luminance.
float shoulder(float x) {
  constexpr float knee = 0.3f;
  if (x <= knee) return x;
  return knee + (1.f - knee) * (1.f - std::exp(-(x - knee) / (1.f - knee)));
}

// Monotone cubic (Fritsch-Carlson) through Lightroom-style 0..255 points.
std::vector<float> sampleMonotoneCurve(std::vector<CurvePoint> pts) {
  std::vector<float> out(kCurveSamples);
  std::sort(pts.begin(), pts.end());
  const std::size_t n = pts.size();
  if (n < 2) {
    for (int i = 0; i < kCurveSamples; ++i) out[i] = i / float(kCurveSamples - 1);
    return out;
  }
  std::vector<double> x(n), y(n), m(n), d(n - 1);
  for (std::size_t i = 0; i < n; ++i) {
    x[i] = pts[i].first / 255.0;
    y[i] = pts[i].second / 255.0;
  }
  for (std::size_t i = 0; i + 1 < n; ++i) {
    const double dx = std::max(1e-6, x[i + 1] - x[i]);
    d[i] = (y[i + 1] - y[i]) / dx;
  }
  m[0] = d[0];
  m[n - 1] = d[n - 2];
  for (std::size_t i = 1; i + 1 < n; ++i) m[i] = (d[i - 1] * d[i] <= 0) ? 0 : (d[i - 1] + d[i]) / 2;
  for (std::size_t i = 0; i + 1 < n; ++i) {
    if (d[i] == 0) {
      m[i] = m[i + 1] = 0;
      continue;
    }
    const double a = m[i] / d[i], b = m[i + 1] / d[i], s = a * a + b * b;
    if (s > 9) {
      const double t = 3 / std::sqrt(s);
      m[i] = t * a * d[i];
      m[i + 1] = t * b * d[i];
    }
  }
  for (int k = 0; k < kCurveSamples; ++k) {
    const double xv = k / double(kCurveSamples - 1);
    double v;
    if (xv <= x[0]) {
      v = y[0];
    } else if (xv >= x[n - 1]) {
      v = y[n - 1];
    } else {
      std::size_t i = 0;
      while (i + 2 < n && xv > x[i + 1]) ++i;
      const double h = std::max(1e-6, x[i + 1] - x[i]), t = (xv - x[i]) / h;
      const double t2 = t * t, t3 = t2 * t;
      v = (2 * t3 - 3 * t2 + 1) * y[i] + (t3 - 2 * t2 + t) * h * m[i] + (-2 * t3 + 3 * t2) * y[i + 1] +
          (t3 - t2) * h * m[i + 1];
    }
    out[k] = float(std::clamp(v, 0.0, 1.0));
  }
  return out;
}

// Lightroom's region ("parametric") curve, approximated with four smooth bumps.
// amt = {shadows, darks, lights, highlights} in -100..100, splits in percent.
std::vector<float> sampleParametric(const float amt[4], float split1, float split2, float split3) {
  const float s1 = std::clamp(split1, 5.f, 90.f) / 100.f;
  const float s2 = std::clamp(split2, s1 * 100 + 5, 95.f) / 100.f;
  const float s3 = std::clamp(split3, s2 * 100 + 5, 98.f) / 100.f;
  const float c[4] = {s1 * 0.5f, (s1 + s2) / 2, (s2 + s3) / 2, (s3 + 1) / 2};
  const float w[4] = {s1 * 0.9f, (s2 - s1) * 0.9f, (s3 - s2) * 0.9f, (1 - s3) * 0.9f};
  std::vector<float> out(kCurveSamples);
  float prev = 0.f;
  for (int k = 0; k < kCurveSamples; ++k) {
    const float x = k / float(kCurveSamples - 1);
    const float envelope = std::min(1.f, x / 0.04f) * std::min(1.f, (1 - x) / 0.04f);
    float y = x;
    for (int i = 0; i < 4; ++i) {
      const float z = (x - c[i]) / std::max(0.02f, w[i]);
      y += amt[i] / 100.f * 0.15f * std::exp(-z * z) * envelope;
    }
    y = std::clamp(y, prev, 1.f);  // keep monotone
    out[k] = prev = y;
  }
  return out;
}

bool allZero(const float amt[4]) { return amt[0] == 0 && amt[1] == 0 && amt[2] == 0 && amt[3] == 0; }

// Lightroom hue (HSL-style degrees: 0 red, 60 yellow, 120 green, 240 blue)
// to the corresponding OKLab hue angle in radians.
float lightroomHueToOklab(float deg) {
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

void bandWeights(float hueDeg, float w[kHslBandCount]) {
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
void sampleColorMatch(const ColorMatchProfile& m, const float lab[3], float off[3]) {
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

}  // namespace

float Renderer::Curve1D::operator()(float x) const {
  if (identity) return x;
  const float f = std::clamp(x, 0.f, 1.f) * (kCurveSamples - 1);
  const int i = std::min(int(f), kCurveSamples - 2);
  const float t = f - i;
  return table[i] + t * (table[i + 1] - table[i]);
}

Renderer::Renderer(const EditParams& params, const CameraProfile& camera, const Lut3D* lut)
    : p_(params), camera_(camera), lut_(lut) {
  p_.clamp();
  constexpr float kTemp = 0.006f, kTint = 0.004f;
  wbGain_[0] = std::exp2(kTemp * p_.temperature);
  wbGain_[1] = std::exp2(-kTint * p_.tint);
  wbGain_[2] = std::exp2(-kTemp * p_.temperature);
  const float norm = proPhotoLuma(wbGain_);
  for (float& g : wbGain_) g /= norm;

  // Each curve = parametric (four regions) followed by the point curve.
  auto build = [&](Curve1D& c, const float amt[4], const std::vector<CurvePoint>& pts) {
    const bool para = !allZero(amt);
    const bool point = !pts.empty();
    if (!para && !point) return;
    const std::vector<float> pc =
        para ? sampleParametric(amt, p_.parametricShadowSplit, p_.parametricMidtoneSplit, p_.parametricHighlightSplit)
             : std::vector<float>{};
    const Curve1D tc = point ? Curve1D{sampleMonotoneCurve(pts), false} : Curve1D{};
    c.table.resize(kCurveSamples);
    for (int k = 0; k < kCurveSamples; ++k) {
      float v = k / float(kCurveSamples - 1);
      if (para) v = pc[k];
      if (point) v = tc(v);
      c.table[k] = v;
    }
    c.identity = false;
  };
  const float master[4] = {p_.parametricShadows, p_.parametricDarks, p_.parametricLights, p_.parametricHighlights};
  build(master_, master, p_.toneCurve);
  build(red_, p_.channelParametric[0].data(), p_.toneCurveRed);
  build(green_, p_.channelParametric[1].data(), p_.toneCurveGreen);
  build(blue_, p_.channelParametric[2].data(), p_.toneCurveBlue);
}

namespace {

// ---- geometry: rotate 90 -> straighten (+ ratio crop without empty corners) -> flip ----
struct Geometry {
  bool active = false;
  bool exact = true;  // no resampling: pure index mapping
  int srcW = 0, srcH = 0, outW = 0, outH = 0, rot = 0;
  bool flip = false;
  float cs = 1.f, sn = 0.f, fw = 0.f, fh = 0.f;

  void setup(const EditParams& p, int w, int h) {
    srcW = w;
    srcH = h;
    rot = (p.rotate / 90) % 4;
    flip = p.flip;
    fw = float(rot % 2 ? h : w);
    fh = float(rot % 2 ? w : h);
    const bool tilted = p.straighten != 0.f;
    float ar = fw / fh;
    if (p.crop == "1:1") ar = 1.f;
    else if (p.crop == "4:5") ar = 4.f / 5.f;
    else if (p.crop == "3:4") ar = 3.f / 4.f;
    else if (p.crop == "16:9") ar = 16.f / 9.f;
    else if (p.crop == "9:16") ar = 9.f / 16.f;
    const bool cropped = p.crop != "orig";
    active = rot != 0 || flip || tilted || cropped;
    exact = !tilted;
    if (!active) {
      outW = w;
      outH = h;
      return;
    }
    const float th = p.straighten * kPi / 180.f;
    cs = std::cos(th);
    sn = std::sin(th);
    const float c = std::fabs(cs), s = std::fabs(sn);
    // Largest box of ratio ar, rotated by th, that stays inside the fw x fh frame.
    float bw = std::min(fw / (c + s / ar), fh / (s + c / ar));
    if (tilted) bw -= 2.5f;  // keep the 4x4 resampling kernel off the image border
    bw = std::max(1.f, bw);
    outW = std::clamp(int(tilted ? std::floor(bw) : std::lround(bw)), 1, int(fw));
    outH = std::clamp(int(tilted ? std::floor(bw / ar) : std::lround(bw / ar)), 1, int(fh));
  }

  // Source position (continuous, pixel i spans [i, i+1)) of the centre of output pixel (ox, oy).
  void map(int ox, int oy, float& sx, float& sy) const { mapF(float(ox) + 0.5f, float(oy) + 0.5f, sx, sy); }

  // Same for a continuous output position (px, py), in output pixels (pixel i spans [i, i+1)).
  void mapF(float px, float py, float& sx, float& sy) const {
    float u = px - float(outW) * 0.5f;
    const float v = py - float(outH) * 0.5f;
    if (flip) u = -u;
    const float fx = u * cs + v * sn + fw * 0.5f;
    const float fy = -u * sn + v * cs + fh * 0.5f;
    switch (rot) {
      case 0: sx = fx; sy = fy; break;
      case 1: sx = fy; sy = float(srcH) - fx; break;
      case 2: sx = float(srcW) - fx; sy = float(srcH) - fy; break;
      default: sx = float(srcW) - fy; sy = fx; break;
    }
  }
};

// Catmull-Rom sample of an RGB float image at continuous position (x, y).
void sampleCubic(const ImageF& im, float x, float y, float out[3]) {
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

// ---- colour style: recipe + pad + strength, resolved to numbers once per render ----
struct StyleMix {
  bool active = false;
  float satMul = 1.f, contrastK = 0.f, dL = 0.f;
  float tintA = 0.f, tintB = 0.f, shA = 0.f, shB = 0.f, hiA = 0.f, hiB = 0.f;
  float hueRot = 0.f, gray = 0.f, fade = 0.f, grain = 0.f;
  bool tint = false, split = false;
};

StyleMix makeStyle(const EditParams& p) {
  StyleMix m;
  const int idx = std::max(0, lookIndex(p.look));
  const LookRecipe& r = lookRecipe(idx);
  const LookInfo& info = looks()[idx];
  const float s = p.lookStrength / 100.f;
  if (s <= 0.f) return m;
  const float px = p.hasPalette ? p.paletteX : info.padX, py = p.hasPalette ? p.paletteY : info.padY;
  auto vec = [](float mag, float hueDeg, float& a, float& b) {
    a += mag * std::cos(hueDeg * kPi / 180.f);
    b += mag * std::sin(hueDeg * kPi / 180.f);
  };
  // pad: x pale -> rich scales chroma; y warm (top, small) -> cool (bottom) tints the picture
  m.satMul = (1.f + (r.satMul - 1.f) * s) * (1.f + (px - 0.5f) * 0.9f * s);
  m.contrastK = (r.contrast - 1.f) * s;
  m.dL = r.dL * s;
  vec(r.tintMag * s, r.tintHue, m.tintA, m.tintB);
  const float w = (py - 0.5f) * s;
  if (w < 0.f) vec(-w * 0.07f, 60.f, m.tintA, m.tintB);
  else vec(w * 0.07f, 245.f, m.tintA, m.tintB);
  vec(r.shadowMag * s, r.shadowHue, m.shA, m.shB);
  vec(r.highMag * s, r.highHue, m.hiA, m.hiB);
  m.hueRot = r.hueRotDeg * s * kPi / 180.f;
  m.gray = r.gray * s;
  m.fade = r.fade * s;
  m.grain = r.grain * s;
  m.tint = std::fabs(m.tintA) + std::fabs(m.tintB) > 1e-6f;
  m.split = std::fabs(m.shA) + std::fabs(m.shB) + std::fabs(m.hiA) + std::fabs(m.hiB) > 1e-6f;
  m.active = std::fabs(m.satMul - 1.f) > 1e-6f || m.contrastK != 0.f || m.dL != 0.f || m.tint || m.split ||
             m.hueRot != 0.f || m.gray > 0.f;
  return m;
}

}  // namespace

namespace {

// ---- colour grading zones (three zones plus a global tint), each (hue, saturation, luminance) ----
struct Grade {
  float a = 0, b = 0, dl = 0;
  bool active = false;
};

Grade makeGrade(const ToneZoneGrade& g) {
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
// denoised in camera) the defaults sharpen 40 / nrColor 25 are the no-op level.
SpatialParams makeSpatial(const EditParams& p, bool scene, const StyleMix& style) {
  SpatialParams sp;
  sp.sharpen = std::max(0.f, p.sharpen - (scene ? 0.f : 40.f)) / 100.f;
  sp.sharpRadius = p.sharpRadius / 10.f;
  sp.nrColor = std::max(0.f, p.nrColor - (scene ? 0.f : 25.f)) / 100.f;
  sp.nrLuma = p.nrLuma / 100.f;
  sp.clarity = p.clarity / 100.f;
  sp.texture = p.texture / 100.f;
  sp.dehaze = p.dehaze / 100.f;
  sp.grain = std::min(100.f, p.grain + style.grain) / 100.f;
  return sp;
}

inline void applyVignette(float e3[3], float dx, float dy, float vig) {
  const float f = smoothstep(0.35f, 1.05f, std::sqrt((dx * dx + dy * dy) * 0.5f));
  for (int c = 0; c < 3; ++c) e3[c] = vig < 0 ? e3[c] * (1.f + vig * 0.6f * f) : e3[c] + (1.f - e3[c]) * vig * 0.6f * f;
}

}  // namespace

// Everything of the per-pixel pipeline that depends on parameters only, resolved once.
struct Renderer::PointOp {
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
  const Curve1D *master = nullptr, *red = nullptr, *green = nullptr, *blue = nullptr;
  ToneAnalysis analysis;
  bool hasAnalysis = false;

  // Tone: ratio by which the luminance Y of a (post-gain) pixel is scaled; hue-preserving, so the
  // whole stage is a function of Y alone.
  float toneRatio(float Y) const {
    if (!(Y > 1e-7f)) return 1.f;
    float e = std::log2(Y / kMidGrey);
    e += 0.45f * contrast * e / (1.f + (e / 4.f) * (e / 4.f));
    if (scene) e -= kToeStops * smoothstep(kToeStart, kToeEnd, e);
    if (hi != 0) e += hi * 1.2f * smoothstep(-0.5f, 2.5f, e);
    if (sh != 0) e += sh * 1.3f * smoothstep(0.5f, -4.f, e);
    if (wh != 0) e += wh * 0.7f * smoothstep(1.f, 3.f, e);
    if (bl != 0) e += bl * 0.9f * smoothstep(-3.f, -6.5f, e);
    float Yd = kMidGrey * std::exp2(e);
    if (scene) Yd = shoulder(Yd);
    if (!toneMove.identity) Yd = srgbDecode(toneMove(srgbEncode(Yd)));
    return Yd / Y;
  }

  // `v_in` = scene-linear ProPhoto after white balance and exposure gain (>= 0); e3 = sRGB-encoded
  // display value (before vignette, not clamped).
  void apply(const float v_in[3], float e3[3]) const {
    const float ratio = toneRatio(proPhotoLuma(v_in));
    const float t[3] = {v_in[0] * ratio, v_in[1] * ratio, v_in[2] * ratio};
    colour(t, e3);
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
      const float lab2[3] = {L, a, b};
      oklabToLinearSrgb(lab2, rgb);
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

Renderer::PointOp Renderer::makePointOp(const SourceImage& source, const BakedLut* reuse) const {
  PointOp op;
  const bool scene = source.sceneReferred;
  op.scene = scene;
  for (int c = 0; c < 3; ++c) op.wb[c] = wbGain_[c];
  op.exposureGain = std::exp2(p_.exposure + (scene ? camera_.baselineExposureEv : 0.f));
  op.contrast = (p_.contrast + (scene ? camera_.baselineContrast : 0.f)) / 100.f;
  op.hi = p_.highlights / 100.f;
  op.sh = p_.shadows / 100.f;
  op.wh = p_.whites / 100.f;
  op.bl = p_.blacks / 100.f;
  op.sat = p_.saturation / 100.f;
  op.vib = p_.vibrance / 100.f;

  // User HSL plus the camera's base colour rendering (scene-referred sources only).
  op.hslEff = p_.hsl;
  if (scene)
    for (int i = 0; i < kHslBandCount; ++i) {
      op.hslEff[i].hue += camera_.baselineHsl[i].hue;
      op.hslEff[i].saturation += camera_.baselineHsl[i].saturation;
      op.hslEff[i].luminance += camera_.baselineHsl[i].luminance;
    }
  for (const auto& b : op.hslEff) op.anyHsl |= (b.hue != 0 || b.saturation != 0 || b.luminance != 0);

  op.gS = makeGrade(p_.gradeShadows);
  op.gM = makeGrade(p_.gradeMidtones);
  op.gH = makeGrade(p_.gradeHighlights);
  op.gG = makeGrade(p_.gradeGlobal);
  op.zoneGrade = op.gS.active || op.gM.active || op.gH.active;
  op.anyGrade = op.zoneGrade || op.gG.active;
  // zone cross-over: blend widens the soft edges, balance moves them (positive = more highlight)
  op.gWidth = 0.05f + 0.30f * p_.gradeBlend / 100.f;
  const float gShift = -p_.gradeBalance / 100.f * 0.2f;
  op.gC1 = 0.30f + gShift;
  op.gC2 = 0.70f + gShift;

  op.style = makeStyle(p_);
  op.match = &camera_.colorMatch;
  const bool matchOn = scene && op.match->enabled();
  op.matchLut = matchOn && op.match->hasLut();
  op.matchMat = matchOn && op.match->hasMatrix;

  // Tonal-class move (EditParams::toneTarget / toneStrength): a curve on encoded luminance,
  // built from the tonal analysis of this very render without the move.
  if (reuse) {
    // same parameters as the lattice this was baked from: reuse its tone move and analysis
    op.analysis = reuse->analysis;
    op.hasAnalysis = true;
    if (!reuse->toneMove.empty()) {
      op.toneMove.table = reuse->toneMove;
      op.toneMove.identity = false;
    }
  } else if (p_.toneStrength > 0.f) {
    op.analysis = analyzeTone(toneProxy_ ? *toneProxy_ : source, camera_, p_);
    op.hasAnalysis = true;
    const auto pts = toneTargetCurve(op.analysis, p_.toneTarget, p_.toneStrength / 100.f, toneReach_);
    if (!pts.empty()) {
      op.toneMove.table = sampleMonotoneCurve(pts);
      op.toneMove.identity = false;
    }
  }

  op.colorStage = op.anyHsl || op.anyGrade || op.sat != 0 || op.vib != 0 || matchOn || op.style.active;
  op.lut = lut_;
  op.lutMix = std::clamp(p_.lutAmount / 100.f, 0.f, 1.f);
  op.fade = op.style.fade;
  op.master = &master_;
  op.red = &red_;
  op.green = &green_;
  op.blue = &blue_;
  return op;
}

// ---- film simulation stage ----------------------------------------------------------------------------------

namespace {

constexpr float kWarmthFilterUnits = 12.f;  // enlarger filtration (CC-like units) at warmth / tint = +-100

struct FilmRun {
  std::shared_ptr<const film::FilmLibrary> lib;
  const film::FilmStock* stock = nullptr;
  std::string paper;
  float strength = 1.f;  // 0..1
};

// Resolves stock / paper of the edit against the installed library (null stock = film not available).
FilmRun resolveFilm(const EditParams& p) {
  FilmRun r;
  if (p.filmStock.empty()) return r;
  r.lib = film::defaultLibrary();
  if (!r.lib) return r;
  const film::FilmStock* s = r.lib->find(p.filmStock);
  if (!s || s->isPrintMedium()) return r;
  r.stock = s;
  r.paper = p.filmPaper;
  if (r.paper.empty() || (r.paper != "none" && !(r.lib->find(r.paper) && r.lib->find(r.paper)->isPrintMedium())))
    r.paper = film::defaultPaperFor(*r.lib, p.filmStock);
  r.strength = std::clamp(p.filmStrength / 100.f, 0.f, 1.f);
  return r;
}

film::FilmParams makeFilmParams(const EditParams& p, const FilmRun& run, SpatialQuality q, float formatScale) {
  film::FilmParams fp;
  fp.film = p.filmStock;
  fp.paper = run.paper;
  fp.camera.autoExposure = false;  // the develop pipeline's own exposure slider is the exposure control
  // (the exposure offset is folded into the input gain in runFilmStage: it then costs no re-bake while dragging)
  fp.camera.filmFormatMm *= formatScale;
  // print filtration: more yellow / magenta filtration = less of that dye = cooler / greener print
  fp.enlarger.yFilterShift = -p.filmWarmth / 100.f * kWarmthFilterUnits;
  fp.enlarger.mFilterShift = -p.filmTint / 100.f * kWarmthFilterUnits;
  const double g = std::clamp(p.filmGrain / 100.f, 0.f, 1.f);
  if (g <= 0) {
    fp.filmRender.grain.active = false;
  } else {
    fp.filmRender.grain.particleAreaUm2 *= g * g;  // grain amplitude ~ sqrt(particle area)
  }
  const double h = std::clamp(p.filmHalation / 100.f, 0.f, 1.f);
  fp.filmRender.halation.halationAmount = h;
  fp.filmRender.halation.scatterAmount = h;
  if (q == SpatialQuality::Off) fp.disableSpatialAndStochastic();
  else if (q == SpatialQuality::Interactive) fp.settings.previewMode = true;
  return fp;
}

// Film simulation of a w x h frame into `out` (display sRGB-encoded, unclamped). `src` is the source in linear
// ProPhoto; when the frame is a resampling of it (geometry / region) `fetch(y, row)` produces row y (w * 3 floats,
// >= 0), otherwise `src` is passed on as it is. `gain` = white balance x exposure, folded into the input matrix.
// `pitch` = size of an output pixel in source pixels (spatial effects are defined in film-frame micrometres).
template <class Fetch>
bool runFilmStage(const EditParams& p, const FilmRun& run, const float gain[3], const ImageF& src, bool resample, int w,
                  int h, float pitch, SpatialQuality q, Fetch&& fetch, ImageF& out) {
  if (!run.stock) return false;
  const float formatScale = pitch * float(std::max(w, h)) / float(std::max(src.width, src.height));
  const float evGain = std::exp2(p.filmExposure);
  const float gainEv[3] = {gain[0] * evGain, gain[1] * evGain, gain[2] * evGain};
  film::SimulatorLease lease = film::acquireSimulator(run.lib, p.filmStock, run.paper);
  std::string err;
  if (!lease->setParams(makeFilmParams(p, run, q, formatScale), &err)) return false;
  ScratchImage linScratch;
  const ImageF* in = &src;
  if (resample) {
    ImageF& lin = linScratch.get(w, h);
    parallelRows(h, [&](int y) { fetch(y, lin.row(y)); });
    in = &lin;
  }
  const bool ok = lease->render(*in, out, nullptr, gainEv);
  return ok;
}

}  // namespace

bool Renderer::filmActive() const { return resolveFilm(p_).stock != nullptr; }

void Renderer::displayStages(float e3[3]) const {
  for (int c = 0; c < 3; ++c) e3[c] = master_(e3[c]);
  e3[0] = red_(e3[0]);
  e3[1] = green_(e3[1]);
  e3[2] = blue_(e3[2]);
  if (lut_) {
    const float mix = std::clamp(p_.lutAmount / 100.f, 0.f, 1.f);
    if (mix > 0) {
      float l[3];
      lut_->apply(e3, l);
      for (int c = 0; c < 3; ++c) e3[c] += (l[c] - e3[c]) * mix;
    }
  }
}

ImageF Renderer::renderFloat(const SourceImage& source, bool spatial) const {
  const ImageF& in = source.linear;
  Geometry geo;
  geo.setup(p_, in.width, in.height);
  ImageF out(geo.outW, geo.outH);
  const PointOp op = makePointOp(source, nullptr);
  const float vig = p_.vignette / 100.f;
  const float cx = out.width * 0.5f, cy = out.height * 0.5f;

  // film simulation: replaces tone + colour + look (the result lands in `out`, finished in place below)
  const FilmRun fr = resolveFilm(p_);
  bool filmOk = false;
  if (fr.stock) {
    const float gain[3] = {op.wb[0] * op.exposureGain, op.wb[1] * op.exposureGain, op.wb[2] * op.exposureGain};
    filmOk = runFilmStage(
        p_, fr, gain, in, geo.active, out.width, out.height, 1.f,
        spatial ? SpatialQuality::Full : SpatialQuality::Off,
        [&](int y, float* row) {
          for (int x = 0; x < out.width; ++x) {
            float v[3], sx, sy;
            geo.map(x, y, sx, sy);
            if (geo.exact) {
              const float* px = in.row(std::clamp(int(std::floor(sy)), 0, in.height - 1)) +
                                3 * std::clamp(int(std::floor(sx)), 0, in.width - 1);
              v[0] = px[0], v[1] = px[1], v[2] = px[2];
            } else {
              sampleCubic(in, sx, sy, v);
            }
            row[3 * x] = v[0];
            row[3 * x + 1] = v[1];
            row[3 * x + 2] = v[2];
          }
        },
        out);
  }
  const bool needNormal = !filmOk || fr.strength < 1.f;

  parallelRows(out.height, [&](int y) {
    const float* srcRow = geo.active ? nullptr : in.row(y);
    float* dst = out.row(y);
    for (int x = 0; x < out.width; ++x) {
      float e3[3];
      if (needNormal) {
        float v[3];
        if (!geo.active) {
          for (int c = 0; c < 3; ++c) v[c] = srcRow[3 * x + c];
        } else {
          float sx, sy;
          geo.map(x, y, sx, sy);
          if (geo.exact) {
            const float* px = in.row(std::clamp(int(std::floor(sy)), 0, in.height - 1)) +
                              3 * std::clamp(int(std::floor(sx)), 0, in.width - 1);
            v[0] = px[0], v[1] = px[1], v[2] = px[2];
          } else {
            sampleCubic(in, sx, sy, v);
          }
        }
        for (int c = 0; c < 3; ++c) v[c] = std::max(0.f, v[c] * op.wb[c] * op.exposureGain);
        op.apply(v, e3);
      }
      if (filmOk) {
        float ef[3] = {dst[3 * x], dst[3 * x + 1], dst[3 * x + 2]};
        displayStages(ef);
        for (int c = 0; c < 3; ++c) e3[c] = needNormal ? e3[c] + fr.strength * (ef[c] - e3[c]) : ef[c];
      }
      if (vig != 0) applyVignette(e3, (x - cx) / cx, (y - cy) / cy, vig);
      for (int c = 0; c < 3; ++c) dst[3 * x + c] = std::clamp(e3[c], 0.f, 1.f);
    }
  });

  if (spatial) {
    // a film at full strength brings its own grain: the look's grain would double it
    EditParams pe = p_;
    if (filmOk && fr.strength >= 1.f) pe.look = "std";
    applySpatial(out, makeSpatial(p_, source.sceneReferred, filmOk && fr.strength >= 1.f ? makeStyle(pe) : op.style));
  }
  return out;
}

// ================================================================================================
// Baked path
// ================================================================================================
namespace {

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

  explicit LutSampler(const BakedLut& l) : t(l.rgb.data()), tone(l.tone.data()), n(l.n) {
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

template <class Op>
void fillToneTable(std::vector<float>& tone, const Op& op) {
  tone.resize(kToneN);
  for (int i = 0; i < kToneN; ++i) {
    const std::uint32_t bits = std::uint32_t(i + kToneBase) << 15;
    float y;
    std::memcpy(&y, &bits, 4);
    tone[std::size_t(i)] = op.toneRatio(y);
  }
}

struct FastDims {
  int w = 0, h = 0;
  float ox0 = 0.f, oy0 = 0.f, stepX = 1.f, stepY = 1.f;  // output-space origin and pixel pitch
  float fullW = 0.f, fullH = 0.f;
  float fx0 = 0.f, fy0 = 0.f, fx1 = 1.f, fy1 = 1.f;
};

FastDims fastDims(const Geometry& geo, const RenderRegion& R) {
  FastDims d;
  d.fullW = float(geo.outW);
  d.fullH = float(geo.outH);
  d.fx0 = std::clamp(R.x0, 0.f, 1.f);
  d.fy0 = std::clamp(R.y0, 0.f, 1.f);
  d.fx1 = std::clamp(R.x1, d.fx0 + 1e-4f, 1.f);
  d.fy1 = std::clamp(R.y1, d.fy0 + 1e-4f, 1.f);
  if (!R.active()) {
    d.w = geo.outW;
    d.h = geo.outH;
    return d;
  }
  const float rw = (d.fx1 - d.fx0) * d.fullW, rh = (d.fy1 - d.fy0) * d.fullH;
  const float s = R.outLongEdge > 0 ? float(R.outLongEdge) / std::max(rw, rh) : 1.f;
  d.w = std::max(1, int(std::lround(rw * s)));
  d.h = std::max(1, int(std::lround(rh * s)));
  d.ox0 = d.fx0 * d.fullW;
  d.oy0 = d.fy0 * d.fullH;
  d.stepX = rw / float(d.w);
  d.stepY = rh / float(d.h);
  return d;
}

}  // namespace

std::string Renderer::pointKey() const {
  EditParams k = p_;
  // everything that is not point-wise
  k.clarity = k.texture = k.dehaze = k.vignette = k.grain = 0.f;
  k.sharpen = k.sharpRadius = k.nrLuma = k.nrColor = 0.f;
  k.filmGrain = k.filmHalation = 100.f;  // spatial film parameters do not touch the lattice
  // Nor do the film's own continuous controls: the film branch applies them inside the film simulator, the lattice is
  // only the blend partner of the normal render, and the tonal class analysis ignores the film look.
  k.filmExposure = k.filmWarmth = k.filmTint = 0.f;
  k.crop = "orig";
  k.straighten = 0.f;
  k.rotate = 0;
  k.flip = false;
  char buf[96];
  std::snprintf(buf, sizeof buf, "|%p|%s|%.3f|%d", static_cast<const void*>(lut_), camera_.id.c_str(), toneReach_,
                toneProxy_ ? 1 : 0);
  return toJson(k) + buf;
}

std::string Renderer::colourKey() const {
  Renderer r(*this);
  r.p_.exposure = r.p_.contrast = r.p_.highlights = r.p_.shadows = r.p_.whites = r.p_.blacks = 0.f;
  r.p_.temperature = r.p_.tint = 0.f;
  r.p_.toneTarget.clear();
  r.p_.toneStrength = 0.f;
  return r.pointKey();
}


std::shared_ptr<const BakedLut> Renderer::bake(const SourceImage& source, int n, const BakedLut* colourFrom,
                                               bool fastAnalysis) const {
  const std::string ck = colourKey();
  if (colourFrom && (colourFrom->colourKey != ck || colourFrom->rgb.empty())) colourFrom = nullptr;
  n = colourFrom ? colourFrom->n : std::clamp(n, 17, 65);
  auto lut = std::make_shared<BakedLut>();
  lut->n = n;
  lut->key = pointKey() + "#" + std::to_string(n);
  lut->colourKey = ck;
  const FilmRun fr = resolveFilm(p_);
  const bool filmOnly = fr.stock && fr.strength >= 1.f;  // the lattice is only needed to blend against the normal render
  // The colour lattice first: it does not depend on the tone stage, the tonal analysis or the tonal-class move, so
  // it is either reused (same colourKey) or evaluated with the tone parameters neutralised.
  Renderer r0(*this);
  r0.p_.toneStrength = 0.f;
  r0.p_.toneTarget.clear();
  r0.toneProxy_ = nullptr;
  const PointOp op0 = r0.makePointOp(source, nullptr);  // no analysis (tone move off); same colour stage as *this
  if (colourFrom) {
    bakeCounters().latticeReused.fetch_add(1, std::memory_order_relaxed);
    lut->rgb = colourFrom->rgb;  // same colour stage: only the tone table and the analysis change
  } else {
    bakeCounters().latticeEvaluated.fetch_add(1, std::memory_order_relaxed);
    lut->rgb.resize(filmOnly ? 3 : std::size_t(n) * n * n * 3);
    std::vector<float> node(n);
    for (int i = 0; i < n; ++i) {
      const float u = float(i) / float(n - 1);
      node[i] = kBakeMax * u * u;
    }
    float* table = lut->rgb.data();
    if (!filmOnly)
      parallelFor(n * n, [&](int bg) {
        const int b = bg / n, g = bg % n;
        float* o = table + std::size_t(bg) * n * 3;
        for (int r = 0; r < n; ++r) {
          const float v[3] = {node[r], node[g], node[b]};
          op0.colour(v, o + 3 * r);
        }
      });
  }

  // Tonal analysis (histogram + the tonal-class move built from it). Exact: the 256 px proxy through the whole point
  // pipeline (~40 ms on a Snapdragon 680 class phone, on every drag frame). Fast (interactive frames): the proxy
  // through the tone table without the move + the lattice just made, ~20x cheaper; edit-bench measures the same
  // dE2000 vs exact either way. Final frames always use the exact analysis.
  BakedLut pre;
  bool havePre = false;
  if (fastAnalysis && !filmOnly && toneProxy_ && !fr.stock && !toneProxy_->linear.empty()) {
    BakedLut t;
    t.n = n;
    t.rgb = std::move(lut->rgb);
    fillToneTable(t.tone, op0);
    FastOptions fo;
    fo.spatial = SpatialQuality::Off;
    pre.analysis = analyzeToneImage(r0.renderBakedFloat(*toneProxy_, t, fo));
    lut->rgb = std::move(t.rgb);
    if (p_.toneStrength > 0.f) {
      const auto pts = toneTargetCurve(pre.analysis, p_.toneTarget, p_.toneStrength / 100.f, toneReach_);
      if (!pts.empty()) pre.toneMove = sampleMonotoneCurve(pts);
    }
    havePre = true;
  }
  const PointOp op = makePointOp(source, havePre ? &pre : nullptr);
  lut->analysis = op.hasAnalysis ? op.analysis : analyzeTone(toneProxy_ ? *toneProxy_ : source, camera_, p_);
  if (!op.toneMove.identity) lut->toneMove = op.toneMove.table;
  fillToneTable(lut->tone, op);
  bakeCounters().toneTables.fetch_add(1, std::memory_order_relaxed);
  return lut;
}

BakeCounters& bakeCounters() {
  static BakeCounters c;
  return c;
}

bool Renderer::latticeFilmOnly() const {
  const FilmRun fr = resolveFilm(p_);
  return fr.stock && fr.strength >= 1.f;
}

std::vector<float> Renderer::bakeColourLattice(const SourceImage& source, int n) const {
  n = std::clamp(n, 17, 65);
  const bool filmOnly = latticeFilmOnly();
  Renderer r0(*this);
  r0.p_.toneStrength = 0.f;
  r0.p_.toneTarget.clear();
  r0.toneProxy_ = nullptr;
  const PointOp op0 = r0.makePointOp(source, nullptr);
  std::vector<float> rgb(filmOnly ? 3 : std::size_t(n) * n * n * 3);
  std::vector<float> node(n);
  for (int i = 0; i < n; ++i) {
    const float u = float(i) / float(n - 1);
    node[i] = kBakeMax * u * u;
  }
  float* table = rgb.data();
  if (!filmOnly)
    parallelFor(n * n, [&](int bg) {
      const int b = bg / n, g = bg % n;
      float* o = table + std::size_t(bg) * n * 3;
      for (int r = 0; r < n; ++r) {
        const float v[3] = {node[r], node[g], node[b]};
        op0.colour(v, o + 3 * r);
      }
    });
  return rgb;
}

Renderer::TonePre Renderer::bakeTonePre(const SourceImage& source, std::vector<float>* rgb, int n, bool fast) const {
  TonePre pre;
  const FilmRun fr = resolveFilm(p_);
  const bool filmOnly = fr.stock && fr.strength >= 1.f;
  if (fast && rgb && !filmOnly && toneProxy_ && !fr.stock && !toneProxy_->linear.empty()) {
    Renderer r0(*this);
    r0.p_.toneStrength = 0.f;
    r0.p_.toneTarget.clear();
    r0.toneProxy_ = nullptr;
    const PointOp op0 = r0.makePointOp(source, nullptr);
    BakedLut t;
    t.n = n;
    t.rgb = std::move(*rgb);
    fillToneTable(t.tone, op0);
    FastOptions fo;
    fo.spatial = SpatialQuality::Off;
    pre.analysis = analyzeToneImage(r0.renderBakedFloat(*toneProxy_, t, fo));
    *rgb = std::move(t.rgb);
    if (p_.toneStrength > 0.f) {
      const auto pts = toneTargetCurve(pre.analysis, p_.toneTarget, p_.toneStrength / 100.f, toneReach_);
      if (!pts.empty()) pre.toneMove = sampleMonotoneCurve(pts);
    }
    pre.has = true;
  } else if (p_.toneStrength > 0.f) {
    pre.analysis = analyzeTone(toneProxy_ ? *toneProxy_ : source, camera_, p_);
    const auto pts = toneTargetCurve(pre.analysis, p_.toneTarget, p_.toneStrength / 100.f, toneReach_);
    if (!pts.empty()) pre.toneMove = sampleMonotoneCurve(pts);
    pre.has = true;
  }
  return pre;
}

void Renderer::bakeToneTable(const SourceImage& source, const TonePre& pre, BakedLut& out) const {
  BakedLut preL;
  preL.analysis = pre.analysis;
  preL.toneMove = pre.toneMove;
  const PointOp op = makePointOp(source, pre.has ? &preL : nullptr);
  out.toneMove.clear();
  if (!op.toneMove.identity) out.toneMove = op.toneMove.table;
  fillToneTable(out.tone, op);
  bakeCounters().toneTables.fetch_add(1, std::memory_order_relaxed);
}

ToneAnalysis Renderer::bakeAnalysis(const SourceImage& source, const TonePre& pre) const {
  return pre.has ? pre.analysis : analyzeTone(toneProxy_ ? *toneProxy_ : source, camera_, p_);
}

void Renderer::fastSize(const SourceImage& source, const RenderRegion& region, int& w, int& h) const {
  Geometry geo;
  geo.setup(p_, source.linear.width, source.linear.height);
  const FastDims d = fastDims(geo, region);
  w = d.w;
  h = d.h;
}

std::uint8_t* Renderer::renderBakedImpl(const SourceImage& source, const BakedLut& lut, const FastOptions& opt, int& w,
                                        int& h, ImageF* outFloat) const {
  const ImageF& in = source.linear;
  if (in.empty() || lut.n < 2) return nullptr;
  Geometry geo;
  geo.setup(p_, in.width, in.height);
  const RenderRegion& R = opt.region;
  const bool region = R.active();
  const FastDims d = fastDims(geo, R);
  w = d.w;
  h = d.h;

  // film simulation (replaces tone + colour + look; the lattice is then only the blend partner)
  const FilmRun fr = resolveFilm(p_);

  // neighbourhood stages
  EditParams pStyle = p_;
  if (fr.stock && fr.strength >= 1.f) pStyle.look = "std";  // a film at full strength brings its own grain
  const StyleMix style = makeStyle(pStyle);
  SpatialParams sp = makeSpatial(p_, source.sceneReferred, style);
  if (opt.spatial == SpatialQuality::Off) {
    sp = SpatialParams{};
  } else if (opt.spatial == SpatialQuality::Interactive) {
    sp.grain = 0.f;
    if (!opt.noise) sp.nrLuma = sp.nrColor = 0.f;
  }
  if (region) sp.scale = std::max(float(w) / (d.fx1 - d.fx0), float(h) / (d.fy1 - d.fy0)) / 2048.f;
  const bool needFloat = sp.any() || outFloat != nullptr || fr.stock != nullptr;

  std::uint8_t* rgba = nullptr;
  if (!outFloat) {
    rgba = static_cast<std::uint8_t*>(std::malloc(std::size_t(w) * h * 4));
    if (!rgba) return nullptr;
  }
  const float expo0 = std::exp2(p_.exposure + (source.sceneReferred ? camera_.baselineExposureEv : 0.f));
  const float gain0[3] = {wbGain_[0] * expo0, wbGain_[1] * expo0, wbGain_[2] * expo0};

  // the source pixel (x, y) of the output, cubic-resampled through the geometry (>= 0, no gain)
  auto fetchPixel = [&](int x, int y, float v[3]) {
    const float fx = d.ox0 + (float(x) + 0.5f) * d.stepX, fy = d.oy0 + (float(y) + 0.5f) * d.stepY;
    float sx, sy;
    geo.mapF(fx, fy, sx, sy);
    if (geo.exact && !region) {
      const float* px = in.row(std::clamp(int(std::floor(sy)), 0, in.height - 1)) +
                        3 * std::clamp(int(std::floor(sx)), 0, in.width - 1);
      v[0] = px[0], v[1] = px[1], v[2] = px[2];
    } else {
      sampleCubic(in, sx, sy, v);
    }
  };
  // Streaming film path (full-strength film, 8-bit target, no neighbourhood stage, no resampling): the film chain
  // runs in row bands through a thread-local tile and the display stages + 8-bit encode consume the hot tile. The
  // 312 MB (26 MP) float frame, its page faults and two more passes over it never exist.
  if (fr.stock && fr.strength >= 1.f && rgba && !sp.any()) {
    film::SimulatorLease lease = film::acquireSimulator(fr.lib, p_.filmStock, fr.paper);
    std::string err;
    const float evGain = std::exp2(p_.filmExposure);
    const float gainEv[3] = {gain0[0] * evGain, gain0[1] * evGain, gain0[2] * evGain};
    if (lease && lease->setParams(makeFilmParams(p_, fr, opt.spatial, region ? std::max(d.stepX, d.stepY) : 1.f), &err)) {
      const float vigS = p_.vignette / 100.f;
      const bool plain = master_.identity && red_.identity && green_.identity && blue_.identity &&
                         !(lut_ && p_.lutAmount > 0);
      const float cx = d.fullW * 0.5f, cy = d.fullH * 0.5f;
      auto sink = [&](int y, float* t, int W) {
        std::uint8_t* o = rgba + std::size_t(y) * W * 4;
        if (!plain || vigS != 0) {
          for (int x = 0; x < W; ++x) {
            float e3[3] = {t[3 * x], t[3 * x + 1], t[3 * x + 2]};
            if (!plain) displayStages(e3);
            if (vigS != 0) {
              const float px = d.ox0 + (float(x) + 0.5f) * d.stepX - 0.5f,
                          py = d.oy0 + (float(y) + 0.5f) * d.stepY - 0.5f;
              applyVignette(e3, (px - cx) / cx, (py - cy) / cy, vigS);
            }
            t[3 * x] = e3[0], t[3 * x + 1] = e3[1], t[3 * x + 2] = e3[2];
          }
        }
        for (int i = 0; i < W; ++i) {  // clamp + encode (vectorises: fixed trip count, no branches)
          const float r = std::min(std::max(t[3 * i], 0.f), 1.f), g = std::min(std::max(t[3 * i + 1], 0.f), 1.f),
                      b = std::min(std::max(t[3 * i + 2], 0.f), 1.f);
          o[4 * i] = std::uint8_t(r * 255.f + 0.5f);
          o[4 * i + 1] = std::uint8_t(g * 255.f + 0.5f);
          o[4 * i + 2] = std::uint8_t(b * 255.f + 0.5f);
          o[4 * i + 3] = 255;
        }
      };
      auto tramp = [](void* u, int y, float* t, int W) { (*static_cast<decltype(sink)*>(u))(y, t, W); };
      // resampled frames (zoom / geometry / preview from a larger source) fetch their rows straight into the band
      auto fetchRow = [&](int y, float* row) {
        for (int x = 0; x < w; ++x) fetchPixel(x, y, row + 3 * x);
      };
      auto fetchTramp = [](void* u, int y, float* row) { (*static_cast<decltype(fetchRow)*>(u))(y, row); };
      const bool resampled = geo.active || region;
      if (lease->renderRows(in, w, h, gainEv, resampled ? fetchTramp : nullptr, &fetchRow, tramp, &sink)) return rgba;
    }
  }

  ScratchImage localScratch;
  ImageF* F = outFloat ? outFloat : (needFloat ? &localScratch.get(w, h) : nullptr);
  if (outFloat) *F = ImageF(w, h);

  const LutSampler sample(lut);
  // Tone-mapped colours above the lattice's range (bright, strongly saturated pixels) take the exact
  // colour path; the operator is only built when such a pixel shows up.
  std::once_flag opOnce;
  std::optional<PointOp> overflowOp;
  const float expo = std::exp2(p_.exposure + (source.sceneReferred ? camera_.baselineExposureEv : 0.f));
  const float gain[3] = {wbGain_[0] * expo, wbGain_[1] * expo, wbGain_[2] * expo};
  const float vig = p_.vignette / 100.f;
  const bool useGeo = geo.active || region;
  const bool direct8 = !F;

  bool filmOk = false;
  if (fr.stock) {
    SpatialQuality q = opt.spatial;
    filmOk = runFilmStage(
        p_, fr, gain, in, useGeo, w, h, region ? std::max(d.stepX, d.stepY) : 1.f, q,
        [&](int y, float* row) {
          for (int x = 0; x < w; ++x) fetchPixel(x, y, row + 3 * x);
        },
        *F);
  }
  const bool needNormal = !filmOk || fr.strength < 1.f;

  parallelRows(h, [&](int y) {
    const float* srcRow = useGeo ? nullptr : in.row(y);
    float* dstF = F ? F->row(y) : nullptr;
    std::uint8_t* dst8 = direct8 ? rgba + std::size_t(y) * w * 4 : nullptr;
    for (int x = 0; x < w; ++x) {
      float e3[3];
      if (needNormal) {
        float v[3];
        if (!useGeo) {
          v[0] = srcRow[3 * x];
          v[1] = srcRow[3 * x + 1];
          v[2] = srcRow[3 * x + 2];
        } else {
          fetchPixel(x, y, v);
        }
        for (int c = 0; c < 3; ++c) v[c] = std::max(0.f, v[c] * gain[c]);
        float tm[3];
        if (!sample(v, e3, tm)) {
          std::call_once(opOnce, [&] { overflowOp.emplace(makePointOp(source, &lut)); });
          overflowOp->colour(tm, e3);
        }
      }
      if (filmOk) {
        float ef[3] = {dstF[3 * x], dstF[3 * x + 1], dstF[3 * x + 2]};
        displayStages(ef);
        for (int c = 0; c < 3; ++c) e3[c] = needNormal ? e3[c] + fr.strength * (ef[c] - e3[c]) : ef[c];
      }
      if (vig != 0) {
        const float cx = d.fullW * 0.5f, cy = d.fullH * 0.5f;
        const float px = d.ox0 + (float(x) + 0.5f) * d.stepX - 0.5f, py = d.oy0 + (float(y) + 0.5f) * d.stepY - 0.5f;
        applyVignette(e3, (px - cx) / cx, (py - cy) / cy, vig);
      }
      for (float& c : e3) c = std::clamp(c, 0.f, 1.f);
      if (dstF) {
        dstF[3 * x] = e3[0];
        dstF[3 * x + 1] = e3[1];
        dstF[3 * x + 2] = e3[2];
      } else {
        dst8[4 * x] = std::uint8_t(e3[0] * 255.f + 0.5f);
        dst8[4 * x + 1] = std::uint8_t(e3[1] * 255.f + 0.5f);
        dst8[4 * x + 2] = std::uint8_t(e3[2] * 255.f + 0.5f);
        dst8[4 * x + 3] = 255;
      }
    }
  });

  if (F && sp.any()) applySpatial(*F, sp);
  if (rgba && F) {
    parallelFor(
        h,
        [&](int y) {
          const float* s = F->row(y);
          std::uint8_t* o = rgba + std::size_t(y) * w * 4;
          for (int x = 0; x < w; ++x) {
            o[4 * x] = std::uint8_t(s[3 * x] * 255.f + 0.5f);
            o[4 * x + 1] = std::uint8_t(s[3 * x + 1] * 255.f + 0.5f);
            o[4 * x + 2] = std::uint8_t(s[3 * x + 2] * 255.f + 0.5f);
            o[4 * x + 3] = 255;
          }
        },
        16);
  }
  return rgba;
}

std::uint8_t* Renderer::renderBakedRgba(const SourceImage& source, const BakedLut& lut, const FastOptions& opt, int& w,
                                        int& h) const {
  return renderBakedImpl(source, lut, opt, w, h, nullptr);
}

ImageF Renderer::renderBakedFloat(const SourceImage& source, const BakedLut& lut, const FastOptions& opt) const {
  ImageF out;
  int w = 0, h = 0;
  renderBakedImpl(source, lut, opt, w, h, &out);
  return out;
}

Image8 Renderer::render(const SourceImage& source) const {
  const ImageF f = renderFloat(source);
  Image8 out;
  out.width = f.width;
  out.height = f.height;
  out.data.resize(f.data.size());
  parallelFor(
      f.height,
      [&](int y) {
        const float* s = f.row(y);
        std::uint8_t* d = out.data.data() + std::size_t(y) * f.width * 3;
        for (int i = 0; i < f.width * 3; ++i) d[i] = std::uint8_t(s[i] * 255.f + 0.5f);
      },
      16);
  return out;
}

// ---- read-only accessors for the GPU backend (core/gpu), see dapple/frame_plan.h ----------------------------
FramePlan Renderer::framePlan(int srcW, int srcH, bool sceneReferred, const FastOptions& opt) const {
  FramePlan fp;
  fp.srcW = srcW;
  fp.srcH = srcH;
  if (resolveFilm(p_).stock) {
    fp.supported = false;
    fp.why = "film simulation";
    return fp;
  }
  Geometry geo;
  geo.setup(p_, srcW, srcH);
  const RenderRegion& R = opt.region;
  const bool region = R.active();
  const FastDims d = fastDims(geo, R);
  fp.outW = d.w;
  fp.outH = d.h;
  fp.useGeo = geo.active || region;
  fp.exact = geo.exact && !region;
  fp.rot = geo.rot;
  fp.flip = geo.flip;
  fp.cs = geo.cs;
  fp.sn = geo.sn;
  fp.fw = geo.fw;
  fp.fh = geo.fh;
  fp.ox0 = d.ox0;
  fp.oy0 = d.oy0;
  fp.stepX = d.stepX;
  fp.stepY = d.stepY;
  fp.fullW = d.fullW;
  fp.fullH = d.fullH;
  const float expo = std::exp2(p_.exposure + (sceneReferred ? camera_.baselineExposureEv : 0.f));
  for (int c = 0; c < 3; ++c) fp.gain[c] = wbGain_[c] * expo;
  fp.vignette = p_.vignette / 100.f;
  SpatialParams sp = makeSpatial(p_, sceneReferred, makeStyle(p_));
  if (opt.spatial == SpatialQuality::Off) {
    sp = SpatialParams{};
  } else if (opt.spatial == SpatialQuality::Interactive) {
    sp.grain = 0.f;
    if (!opt.noise) sp.nrLuma = sp.nrColor = 0.f;
  }
  if (region) sp.scale = std::max(float(d.w) / (d.fx1 - d.fx0), float(d.h) / (d.fy1 - d.fy0)) / 2048.f;
  fp.nrLuma = sp.nrLuma;
  fp.nrColor = sp.nrColor;
  fp.dehaze = sp.dehaze;
  fp.clarity = sp.clarity;
  fp.texture = sp.texture;
  fp.sharpen = sp.sharpen;
  fp.sharpRadius = sp.sharpRadius;
  fp.grain = sp.grain;
  fp.spatialScale = sp.scale > 0.f ? sp.scale : float(std::max(d.w, d.h)) / 2048.f;
  return fp;
}

std::vector<float> Renderer::bakeOverflow(const SourceImage& source, const BakedLut& base, int n, float maxV) const {
  if (resolveFilm(p_).stock || base.rgb.size() < 4) return {};
  n = std::clamp(n, 9, 65);
  const PointOp op = makePointOp(source, &base);
  std::vector<float> out(std::size_t(n) * n * n * 3);
  std::vector<float> node(n);
  for (int i = 0; i < n; ++i) {
    const float u = float(i) / float(n - 1);
    node[i] = maxV * u * u;
  }
  float* table = out.data();
  parallelFor(n * n, [&](int bg) {
    const int b = bg / n, g = bg % n;
    float* o = table + std::size_t(bg) * n * 3;
    for (int r = 0; r < n; ++r) {
      const float v[3] = {node[r], node[g], node[b]};
      op.colour(v, o + 3 * r);
    }
  });
  return out;
}

}  // namespace dapple
