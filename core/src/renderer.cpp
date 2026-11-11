#include "dapple/renderer.h"

#include <algorithm>
#include <cmath>

#include "color.h"
#include "dapple/look.h"
#include "dapple/tone.h"
#include "look_internal.h"
#include "parallel.h"
#include "spatial.h"

namespace dapple {

using namespace color;

namespace {

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
  void map(int ox, int oy, float& sx, float& sy) const {
    float u = float(ox) + 0.5f - float(outW) * 0.5f;
    const float v = float(oy) + 0.5f - float(outH) * 0.5f;
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

ImageF Renderer::renderFloat(const SourceImage& source, bool spatial) const {
  const ImageF& in = source.linear;
  Geometry geo;
  geo.setup(p_, in.width, in.height);
  ImageF out(geo.outW, geo.outH);
  const bool scene = source.sceneReferred;

  const float exposureGain = std::exp2(p_.exposure + (scene ? camera_.baselineExposureEv : 0.f));
  const float contrast = (p_.contrast + (scene ? camera_.baselineContrast : 0.f)) / 100.f;
  const float hi = p_.highlights / 100.f, sh = p_.shadows / 100.f;
  const float wh = p_.whites / 100.f, bl = p_.blacks / 100.f;
  const float sat = p_.saturation / 100.f, vib = p_.vibrance / 100.f;

  // User HSL plus the camera's base colour rendering (scene-referred sources only).
  std::array<HslAdjust, kHslBandCount> hslEff = p_.hsl;
  if (scene)
    for (int i = 0; i < kHslBandCount; ++i) {
      hslEff[i].hue += camera_.baselineHsl[i].hue;
      hslEff[i].saturation += camera_.baselineHsl[i].saturation;
      hslEff[i].luminance += camera_.baselineHsl[i].luminance;
    }
  bool anyHsl = false;
  for (const auto& b : hslEff) anyHsl |= (b.hue != 0 || b.saturation != 0 || b.luminance != 0);

  // Colour grading: three zones plus a global tint, each (hue, saturation, luminance).
  struct Grade {
    float a = 0, b = 0, dl = 0;
    bool active = false;
  };
  auto makeGrade = [](const ToneZoneGrade& g) {
    Grade r;
    if (g.saturation <= 0 && g.luminance == 0) return r;
    const float h = lightroomHueToOklab(g.hue), s = g.saturation / 100.f * 0.06f;
    r.a = std::cos(h) * s;
    r.b = std::sin(h) * s;
    r.dl = g.luminance / 100.f * 0.15f;
    r.active = true;
    return r;
  };
  const Grade gS = makeGrade(p_.gradeShadows), gM = makeGrade(p_.gradeMidtones), gH = makeGrade(p_.gradeHighlights),
              gG = makeGrade(p_.gradeGlobal);
  const bool zoneGrade = gS.active || gM.active || gH.active;
  const bool anyGrade = zoneGrade || gG.active;
  // zone cross-over: blend widens the soft edges, balance moves them (positive = more highlight)
  const float gWidth = 0.05f + 0.30f * p_.gradeBlend / 100.f;
  const float gShift = -p_.gradeBalance / 100.f * 0.2f;
  const float gC1 = 0.30f + gShift, gC2 = 0.70f + gShift;

  const StyleMix style = makeStyle(p_);

  const ColorMatchProfile& match = camera_.colorMatch;
  const bool matchOn = scene && match.enabled();
  const bool matchLut = matchOn && match.hasLut();
  const bool matchMat = matchOn && match.hasMatrix;

  // Tonal-class move (EditParams::toneTarget / toneStrength): a curve on encoded luminance,
  // built from the tonal analysis of this very render without the move.
  Curve1D toneMove;
  if (p_.toneStrength > 0.f) {
    const ToneAnalysis analysis = analyzeTone(source, camera_, p_);
    const auto pts = toneTargetCurve(analysis, p_.toneTarget, p_.toneStrength / 100.f, toneReach_);
    if (!pts.empty()) {
      toneMove.table = sampleMonotoneCurve(pts);
      toneMove.identity = false;
    }
  }

  const bool colorStage = anyHsl || anyGrade || sat != 0 || vib != 0 || matchOn || style.active;
  const Mat3& toSrgb = proPhotoToSrgb();
  const float lutMix = std::clamp(p_.lutAmount / 100.f, 0.f, 1.f);
  const float vig = p_.vignette / 100.f;
  const float cx = out.width * 0.5f, cy = out.height * 0.5f;
  const float fade = style.fade;

  parallelRows(out.height, [&](int y) {
    const float* srcRow = geo.active ? nullptr : in.row(y);
    float* dst = out.row(y);
    for (int x = 0; x < out.width; ++x) {
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
      for (int c = 0; c < 3; ++c) v[c] = std::max(0.f, v[c] * wbGain_[c] * exposureGain);

      // ---- tone, on luminance in log2 space around mid grey ----
      const float Y = proPhotoLuma(v);
      if (Y > 1e-7f) {
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
        const float ratio = Yd / Y;
        for (float& c : v) c *= ratio;
      }

      float rgb[3];
      toSrgb.apply(v, rgb);

      // ---- colour, in OKLab ----
      if (colorStage) {
        float lab[3];
        if (matchMat) {
          const float* m = match.matrix;
          const float r0 = rgb[0], g0 = rgb[1], b0 = rgb[2];
          rgb[0] = m[0] * r0 + m[1] * g0 + m[2] * b0;
          rgb[1] = m[3] * r0 + m[4] * g0 + m[5] * b0;
          rgb[2] = m[6] * r0 + m[7] * g0 + m[8] * b0;
        }
        linearSrgbToOklab(rgb, lab);
        if (matchLut) {
          float off[3];
          sampleColorMatch(match, lab, off);
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
      float e3[3];
      for (int c = 0; c < 3; ++c) e3[c] = master_(srgbEncode(rgb[c]));
      e3[0] = red_(e3[0]);
      e3[1] = green_(e3[1]);
      e3[2] = blue_(e3[2]);

      if (lut_ && lutMix > 0) {
        float l[3];
        lut_->apply(e3, l);
        for (int c = 0; c < 3; ++c) e3[c] += (l[c] - e3[c]) * lutMix;
      }

      if (fade > 0.f)
        for (float& c : e3) c = fade + c * (1.f - fade);

      if (vig != 0) {
        const float dx = (x - cx) / cx, dy = (y - cy) / cy;
        const float f = smoothstep(0.35f, 1.05f, std::sqrt((dx * dx + dy * dy) * 0.5f));
        for (float& c : e3) c = vig < 0 ? c * (1.f + vig * 0.6f * f) : c + (1.f - c) * vig * 0.6f * f;
      }

      for (int c = 0; c < 3; ++c) dst[3 * x + c] = std::clamp(e3[c], 0.f, 1.f);
    }
  });

  if (spatial) {
    // Neighbourhood operations. For display-referred sources (camera JPEG: already sharpened and
    // denoised in camera) the defaults sharpen 40 / nrColor 25 are the no-op level.
    SpatialParams sp;
    sp.sharpen = std::max(0.f, p_.sharpen - (scene ? 0.f : 40.f)) / 100.f;
    sp.sharpRadius = p_.sharpRadius / 10.f;
    sp.nrColor = std::max(0.f, p_.nrColor - (scene ? 0.f : 25.f)) / 100.f;
    sp.nrLuma = p_.nrLuma / 100.f;
    sp.clarity = p_.clarity / 100.f;
    sp.texture = p_.texture / 100.f;
    sp.dehaze = p_.dehaze / 100.f;
    sp.grain = std::min(100.f, p_.grain + style.grain) / 100.f;
    applySpatial(out, sp);
  }
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

}  // namespace dapple
