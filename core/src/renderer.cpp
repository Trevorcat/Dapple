#include "dapple/renderer.h"
#include "hdr_field.h"

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
#include "dapple/film_match.h"
#include "dapple/look.h"
#include "dapple/modern.h"
#include "dapple/tone.h"
#include "look_internal.h"
#include "parallel.h"
#include "engine/exec/engine_session.h"
#include "engine/nodes/film/film_nodes.h"
#include "frame_kernels.h"
#include "local_eval.h"
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

void Renderer::whiteBalanceGain(const EditParams& p, float out[3]) {
  constexpr float kTemp = 0.006f, kTint = 0.004f;
  out[0] = std::exp2(kTemp * p.temperature);
  out[1] = std::exp2(-kTint * p.tint);
  out[2] = std::exp2(-kTemp * p.temperature);
  const float norm = proPhotoLuma(out);
  for (int c = 0; c < 3; ++c) out[c] /= norm;
}

void Renderer::inputGain(const EditParams& params, const CameraProfile& camera, bool sceneReferred, float out[3]) {
  EditParams p = params;
  p.clamp();
  float wb[3];
  whiteBalanceGain(p, wb);
  const float expo = std::exp2(p.exposure + (sceneReferred ? camera.baselineExposureEv : 0.f));
  for (int c = 0; c < 3; ++c) out[c] = wb[c] * expo;
}

Renderer::Renderer(const EditParams& params, const CameraProfile& camera, const Lut3D* lut)
    : p_(params), camera_(camera), lut_(lut) {
  p_.clamp();
  // DAPPLE_ENGINE=off (the golden oracle) ignores the modern rendering family, as it ignores `hdr` (section 37)
  if (isModernId(p_.filmStock) && engine::mode() == engine::Mode::Off) p_.filmStock.clear();
  whiteBalanceGain(p_, wbGain_);
  // Film frames are authored for sRGB output (their LUTs, grain, paper mapping): they never render in P3; the C layer converts the finished sRGB frame.
  if (p_.outputSpace == 1 && filmActive()) p_.outputSpace = 0;

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
  if (const ModernKind mk = modernKind(p.filmStock); mk != ModernKind::None) {  // local contrast of the modern rendering (section 37)
    const ModernRecipe& r = modernRecipe(mk);
    const float k = std::clamp(p.filmStrength / 100.f, 0.f, 1.f);
    sp.clarity = std::clamp((p.clarity + r.clarity * k) / 100.f, -1.f, 1.f);
    sp.texture = std::clamp((p.texture + r.texture * k) / 100.f, -1.f, 1.f);
  }
  sp.dehaze = p.dehaze / 100.f;
  sp.grain = std::min(100.f, p.grain + style.grain) / 100.f;
  return sp;
}

// `p3`: e3 holds Display P3 encoded values (export colour space). The vignette is authored on sRGB-encoded values, so it runs there (P3 -> sRGB
// primaries, extended transfer curve, the formula, back): a colour inside sRGB gets the sRGB path's value. GPU twin: point.comp (u.geoC.w).
inline void applyVignette(float e3[3], float dx, float dy, float vig, bool p3 = false) {
  const float f = smoothstep(0.35f, 1.05f, std::sqrt((dx * dx + dy * dy) * 0.5f));
  if (!p3) {
    for (int c = 0; c < 3; ++c) e3[c] = vig < 0 ? e3[c] * (1.f + vig * 0.6f * f) : e3[c] + (1.f - e3[c]) * vig * 0.6f * f;
    return;
  }
  const float q[3] = {srgbDecode(e3[0]), srgbDecode(e3[1]), srgbDecode(e3[2])};
  float s[3];
  p3ToSrgb().apply(q, s);
  for (int c = 0; c < 3; ++c) {
    const float e = srgbEncodeExt(s[c]);
    s[c] = srgbDecodeExt(vig < 0 ? e * (1.f + vig * 0.6f * f) : e + (1.f - e) * vig * 0.6f * f);
  }
  float o[3];
  srgbToP3().apply(s, o);
  for (int c = 0; c < 3; ++c) e3[c] = srgbEncode(o[c]);
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
  bool p3 = false;  // export colour space Display P3 (EditParams::outputSpace): the lattice holds P3-encoded values
  const Curve1D *master = nullptr, *red = nullptr, *green = nullptr, *blue = nullptr;
  ToneAnalysis analysis;
  bool hasAnalysis = false;

  // The modern rendering family (section 37): resolved once per render. `s` = strength 0..1.
  struct Modern {
    bool active = false, tone = false;
    float s = 0.f;
    float contrast = 1.f, chroma = 1.f, blueDeep = 0.f, skinGuard = 0.f, lift = 0.f, tintA = 0.f, tintB = 0.f;
    float pb = 0.f, pw = 1.f, shoulderK = 1.f;  // encoded end points (user tone edits applied), shoulder share kept
    float tm = 0.5f, ym = 0.5f;  // pivot: mid-grey
  } mod;

  // The user's tone edits (contrast / highlights / shadows / whites / blacks) on the exposure value, without the standard toe and shoulder.
  float userY(float Y) const {
    float e = std::log2(std::max(Y, 1e-9f) / kMidGrey);
    e += 0.45f * contrast * e / (1.f + (e / 4.f) * (e / 4.f));
    if (hi != 0) e += hi * 1.2f * smoothstep(-0.5f, 2.5f, e);
    if (sh != 0) e += sh * 1.3f * smoothstep(0.5f, -4.f, e);
    if (wh != 0) e += wh * 0.7f * smoothstep(1.f, 3.f, e);
    if (bl != 0) e += bl * 0.9f * smoothstep(-3.f, -6.5f, e);
    return kMidGrey * std::exp2(e);
  }

  // The standard tone curve (before the tonal-class move): display-linear luminance of a post-gain luminance.
  float stdTone(float Y) const {
    float e = std::log2(Y / kMidGrey);
    e += 0.45f * contrast * e / (1.f + (e / 4.f) * (e / 4.f));
    if (scene) e -= kToeStops * smoothstep(kToeStart, kToeEnd, e);
    if (hi != 0) e += hi * 1.2f * smoothstep(-0.5f, 2.5f, e);
    if (sh != 0) e += sh * 1.3f * smoothstep(0.5f, -4.f, e);
    if (wh != 0) e += wh * 0.7f * smoothstep(1.f, 3.f, e);
    if (bl != 0) e += bl * 0.9f * smoothstep(-3.f, -6.5f, e);
    float Yd = kMidGrey * std::exp2(e);
    if (scene) Yd = shoulder(Yd);
    return Yd;
  }

  // The modern tone curve at strength s: end points -> display black / white, mid-grey fixed at its standard value, blended with the
  // standard curve in encoded display units (so every strength keeps mid-grey and is monotone).
  float modernTone(float Y, float Ystd) const {
    const float y1 = modernEncode(userY(Y));
    const float t = (y1 - mod.pb) / std::max(mod.pw - mod.pb, 1e-4f);
    const float ym = modernCurve(t, mod.tm, mod.ym, mod.contrast, mod.shoulderK);
    return modernDecode((1.f - mod.s) * modernEncode(Ystd) + mod.s * ym);
  }

  // Tone: ratio by which the luminance Y of a (post-gain) pixel is scaled; hue-preserving, so the
  // whole stage is a function of Y alone.
  float toneRatio(float Y) const {
    if (!(Y > 1e-7f)) return 1.f;
    float Yd = stdTone(Y);
    if (mod.tone) Yd = modernTone(Y, Yd);
    if (!toneMove.identity) Yd = srgbDecode(toneMove(srgbEncode(Yd)));
    return Yd / Y;
  }

  static float hueDist(float h, float c) {
    float d = std::fabs(h - c);
    return d > 180.f ? 360.f - d : d;
  }

  // Modern colour, before the user's colour edits: chroma rises in the mid-tones (lightness window), less where the colour is already
  // strong (soft per-hue ceiling: blues and greens deep, not neon), withheld in the skin window (OKLab hue 52 +- 20 deg, C 0.03 .. 0.2,
  // L 0.3 .. 0.95: a window of the colour space, no face detection), blue / cyan a little deeper. Hue is never touched.
  void modernBase(float& L, float& C, float hDeg) const {
    const float wL = smoothstep(0.10f, 0.42f, L) * (1.f - smoothstep(0.88f, 0.99f, L));
    const float dSkin = hueDist(hDeg, 52.f) / 20.f;
    const float wSkin = std::exp(-dSkin * dSkin) * smoothstep(0.02f, 0.05f, C) * (1.f - smoothstep(0.14f, 0.22f, C)) *
                        smoothstep(0.30f, 0.50f, L) * (1.f - smoothstep(0.92f, 1.0f, L));
    const float dG = hueDist(hDeg, 142.f) / 30.f, dB = hueDist(hDeg, 264.f) / 26.f;
    const float wG = std::exp(-dG * dG), wB = std::exp(-dB * dB);
    const float cap = 0.30f - 0.10f * wG - 0.09f * wB;
    const float room = 1.f - smoothstep(0.55f * cap, cap, C);
    C *= 1.f + (mod.chroma - 1.f) * wL * (1.f - mod.skinGuard * wSkin) * room;
    L -= mod.blueDeep * wB * smoothstep(0.04f, 0.12f, C) * wL;
    L += mod.lift * wL * smoothstep(0.02f, 0.08f, C);  // colours only: neutral mid-grey stays where the anchor put it
  }

  // Gamut mapping by chroma compression, in OKLab at constant lightness and hue: kmax = the largest scale of (a, b) that still fits
  // the sRGB cube (bisection), u = 1 / kmax = how far the colour is towards (u = 1) or beyond (u > 1) the boundary; a smooth knee
  // maps u to u' (identity up to 0.88, then an exponential approach to 1: nothing is clipped, the gradation of over-saturated colours
  // survives). The mapping is blended in with the strength; the standard chord below only sees what is left.
  void modernChromaCompress(float& L, float& a, float& b) const {
    if (std::hypot(a, b) < 2e-3f) return;
    L = std::min(L, 1.f);
    auto fits = [&](float k) {
      const float lab[3] = {L, k * a, k * b};
      float r0[3], r[3];
      oklabToLinearSrgb(lab, r0);
      if (p3) srgbToP3().apply(r0, r);  // the gamut to compress into is the output space's
      else r[0] = r0[0], r[1] = r0[1], r[2] = r0[2];
      return r[0] >= -2e-4f && r[1] >= -2e-4f && r[2] >= -2e-4f && r[0] <= 1.0002f && r[1] <= 1.0002f && r[2] <= 1.0002f;
    };
    float kmax = 3.f;
    if (!fits(kmax)) {
      float lo = 0.f, hi = 3.f;
      for (int i = 0; i < 9; ++i) {
        const float mid = 0.5f * (lo + hi);
        (fits(mid) ? lo : hi) = mid;
      }
      kmax = lo;
    }
    constexpr float k0 = 0.88f;
    const float u = kmax > 1e-4f ? 1.f / kmax : 1e4f;
    if (u <= k0) return;
    const float f = (k0 + (1.f - k0) * (1.f - std::exp(-(u - k0) / (1.f - k0)))) / u;
    const float fs = 1.f - mod.s * (1.f - f);
    a *= fs;
    b *= fs;
  }

  // `v_in` = scene-linear ProPhoto after white balance and exposure gain (>= 0); e3 = sRGB-encoded
  // display value (before vignette, not clamped).
  void apply(const float v_in[3], float e3[3]) const {
    const float ratio = toneRatio(proPhotoLuma(v_in));
    const float t[3] = {v_in[0] * ratio, v_in[1] * ratio, v_in[2] * ratio};
    colour(t, e3);
  }

  // Display P3 output (export only). `rgb` = linear sRGB primaries after the colour stage (unbounded). Same hue-preserving pull towards the
  // same-luminance grey as the sRGB path, but into the P3 cube; the display-referred stages (curves, look LUT, fade) are authored on
  // sRGB-encoded values, so they run there (extended past [0, 1] by an identity offset: a P3-only colour keeps its excess) and the result is
  // converted to P3 afterwards: a colour inside sRGB gets exactly the sRGB path's value, P3 only adds what sRGB could not hold.
  void colourP3(const float rgbIn[3], float e3[3]) const {
    float q[3];
    srgbToP3().apply(rgbIn, q);
    const float Ys = std::clamp(p3Luma(q), 0.f, 1.f);
    const float mn = std::min({q[0], q[1], q[2]});
    if (mn < 0.f) {
      const float t = Ys / std::max(1e-7f, Ys - mn);
      for (float& c : q) c = Ys + (c - Ys) * t;
    }
    const float mx = std::max({q[0], q[1], q[2]});
    if (mx > 1.f) {
      const float t = (1.f - Ys) / std::max(1e-7f, mx - Ys);
      for (float& c : q) c = Ys + (c - Ys) * t;
    }
    const bool plain = master->identity && red->identity && green->identity && blue->identity && !(lut && lutMix > 0) && !(fade > 0.f);
    if (plain) {
      for (int c = 0; c < 3; ++c) e3[c] = srgbEncode(q[c]);
      return;
    }
    float s[3], ex[3];
    p3ToSrgb().apply(q, s);
    for (int c = 0; c < 3; ++c) ex[c] = srgbEncodeExt(s[c]);
    auto ext = [](const Curve1D& f, float x) {
      const float k = std::clamp(x, 0.f, 1.f);
      return f(k) + (x - k);
    };
    for (int c = 0; c < 3; ++c) ex[c] = ext(*master, ex[c]);
    ex[0] = ext(*red, ex[0]);
    ex[1] = ext(*green, ex[1]);
    ex[2] = ext(*blue, ex[2]);
    if (lut && lutMix > 0) {
      float k[3] = {std::clamp(ex[0], 0.f, 1.f), std::clamp(ex[1], 0.f, 1.f), std::clamp(ex[2], 0.f, 1.f)}, l[3];
      lut->apply(k, l);
      for (int c = 0; c < 3; ++c) ex[c] += (l[c] - k[c]) * lutMix;
    }
    if (fade > 0.f)
      for (int c = 0; c < 3; ++c) ex[c] = fade + ex[c] * (1.f - fade);
    for (int c = 0; c < 3; ++c) s[c] = srgbDecodeExt(ex[c]);
    srgbToP3().apply(s, q);
    for (int c = 0; c < 3; ++c) e3[c] = srgbEncode(q[c]);
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

      if (mod.active) modernBase(L, C, hDeg);
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
      if (mod.active) {  // saturation roll-off towards white: colour fades with lightness instead of clipping
        // highlight bias on near-neutral highlights only (hue of a real colour is never touched)
        const float wT = smoothstep(0.62f, 0.90f, L) * (1.f - smoothstep(0.01f, 0.05f, std::hypot(a, b)));
        a += mod.tintA * wT;
        b += mod.tintB * wT;
        const float fade = 1.f - mod.s * smoothstep(0.94f, 1.0f, L);
        a *= fade;
        b *= fade;
        modernChromaCompress(L, a, b);
      }
      const float lab2[3] = {L, a, b};
      oklabToLinearSrgb(lab2, rgb);
    }

    if (p3) {
      colourP3(rgb, e3);
      return;
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

  if (const ModernKind mk = modernKind(p_.filmStock); mk != ModernKind::None && p_.filmStrength > 0.f) {
    const ModernRecipe& r = modernRecipe(mk);
    const float s = std::clamp(p_.filmStrength / 100.f, 0.f, 1.f);
    PointOp::Modern& m = op.mod;
    m.active = true;
    m.s = s;
    m.contrast = r.contrast;
    m.chroma = 1.f + (r.chroma - 1.f) * s;
    m.blueDeep = r.blueDeep * s;
    m.skinGuard = r.skinGuard;
    m.lift = r.lift * s;
    m.tintA = r.tintMag * s * std::cos(r.tintHue * kPi / 180.f);
    m.tintB = r.tintMag * s * std::sin(r.tintHue * kPi / 180.f);
    {
      // scene end points from the proxy (or a small sample of the source): white balance and the camera baseline, NOT the user's exposure
      const SourceImage& img = modernProxy_ ? *modernProxy_ : source;
      const int W = img.linear.width, H = img.linear.height;
      const int stride = std::max(1, std::max(W, H) / 256);
      const float baseGain = std::exp2(scene ? camera_.baselineExposureEv : 0.f);
      std::vector<float> Y, S;
      Y.reserve(std::size_t((W / stride + 1) * (H / stride + 1)));
      S.reserve(Y.capacity());
      for (int y = 0; y < H; y += stride) {
        const int y1 = std::min(H - 1, y + stride / 2);
        for (int x = 0; x < W; x += stride) {
          const int x1 = std::min(W - 1, x + stride / 2);
          const float* a = img.linear.row(y1) + 3 * x1;
          const float v[3] = {a[0] * wbGain_[0] * baseGain, a[1] * wbGain_[1] * baseGain, a[2] * wbGain_[2] * baseGain};
          Y.push_back(proPhotoLuma(v));
          const float mx = std::max({v[0], v[1], v[2]}), mn = std::min({v[0], v[1], v[2]});
          S.push_back(mx > 1e-6f ? (mx - mn) / mx : 0.f);
        }
      }
      const ModernEnds ends = modernEnds(Y.data(), Y.size(), kMidGrey);
      m.tone = true;
      m.pb = modernEncode(op.userY(ends.black));
      m.pw = modernEncode(op.userY(ends.white));
      {  // where the mass of the picture is (section 37): empty middle -> smaller black pull; mass in the upper / lower tones -> flatter slope
        std::vector<float> E(Y.size());
        for (std::size_t i = 0; i < Y.size(); ++i) E[i] = modernEncode(op.userY(Y[i]));
        const ModernDist d0 = modernDistribution(E.data(), S.data(), E.size(), m.pb, m.pw);
        m.pb *= 1.f - 0.6f * d0.gap;
        m.lift *= 1.f - d0.wHi;  // no airy lift on a picture that is already bright
        m.shoulderK = 1.f - 0.7f * d0.wHi;  // mass in the upper tones: the upper piece stays straight (no lift of the background)
        m.pw = modernEncode(modernDecode(m.pw) * (1.f + 0.7f * d0.wHi));  // a large bright area keeps headroom above it instead of being driven to white
        const ModernDist d1 = modernDistribution(E.data(), S.data(), E.size(), m.pb, m.pw);
        m.contrast = 1.f + (r.contrast - 1.f) * (1.f - 0.85f * std::max(d1.wHi, d1.wLo));
        if (std::getenv("DAPPLE_MODERN_LOG")) std::fprintf(stderr, "modern: black %.4f white %.4f wHi %.2f wLo %.2f gap %.2f contrast %.3f tm %.3f ym %.3f\n", double(modernDecode(m.pb)), double(modernDecode(m.pw)), double(d1.wHi), double(d1.wLo), double(d0.gap), double(m.contrast), double((modernEncode(op.userY(kMidGrey)) - m.pb) / (m.pw - m.pb)), double(modernEncode(op.stdTone(kMidGrey))));
      }
      const float pm = modernEncode(op.userY(kMidGrey));
      m.tm = (pm - m.pb) / std::max(m.pw - m.pb, 1e-4f);
      m.ym = modernEncode(op.stdTone(kMidGrey));
    }
  }
  op.colorStage = op.anyHsl || op.anyGrade || op.sat != 0 || op.vib != 0 || matchOn || op.style.active || op.mod.active;
  op.lut = lut_;
  op.lutMix = std::clamp(p_.lutAmount / 100.f, 0.f, 1.f);
  op.fade = op.style.fade;
  op.p3 = p_.outputSpace == 1;
  op.master = &master_;
  op.red = &red_;
  op.green = &green_;
  op.blue = &blue_;
  return op;
}

float Renderer::modernSdrWhite(const SourceImage& source) const {
  const PointOp op = makePointOp(source, nullptr);
  if (!op.mod.tone) return 1.f;
  return std::pow(std::max(modernDecode(op.mod.pw), 1e-4f), op.mod.s);
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
  fp.enlarger.printExposure = std::exp2(-double(std::clamp(p.filmPrintEv, -9.f, 9.f)));  // print exposure (film_match.h)
  applyFilmPaperMapping(fp, p.filmBlack);  // paper white / black -> display white / black (stage C)
  applyFilmWb(fp, p);  // film white balance: the scene illuminant reaches the film with degree of adaptation 1 - film_wb
  // (the exposure offset is folded into the input gain in runFilmStage: it then costs no re-bake while dragging)
  fp.camera.filmFormatMm *= formatScale;
  // print filtration: more yellow / magenta filtration = less of that dye = cooler / greener print
  fp.enlarger.yFilterShift = -p.filmWarmth / 100.f * kWarmthFilterUnits;
  fp.enlarger.mFilterShift = -p.filmTint / 100.f * kWarmthFilterUnits;
  const double g = std::clamp(p.filmGrain / 100.f, 0.f, 2.f);
  if (g <= 0) {
    fp.filmRender.grain.active = false;
  } else {
    fp.filmRender.grain.particleAreaUm2 *= g * g;  // grain amplitude ~ sqrt(particle area)
  }
  const double h = std::clamp(p.filmHalation / 100.f, 0.f, 2.f);
  fp.filmRender.halation.halationAmount = h;  // 0..2: above 1 the halation is heavier than the stock's own
  fp.filmRender.halation.scatterAmount = std::min(h, 1.0);  // the scatter is a mix weight: above 1 it would extrapolate (negative lobe)
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
                  int h, float pitch, SpatialQuality q, Fetch&& fetch, ImageF& out, const FastOptions* fo = nullptr) {
  if (!run.stock) return false;
  const float formatScale = pitch * float(std::max(w, h)) / float(std::max(src.width, src.height));
  const float evGain = std::exp2(p.filmExposure + p.filmKeyEv);
  const float gainEv[3] = {gain[0] * evGain, gain[1] * evGain, gain[2] * evGain};
  if (engine::filmnode::enabled()) {
    // the film as engine nodes (core/engine/nodes/film): byte-identical with the FilmSimulator below
    engine::filmnode::FilmFrameArgs a;
    a.lib = run.lib.get();
    a.stock = p.filmStock.c_str();
    a.paper = run.paper.c_str();
    for (int c = 0; c < 3; ++c) a.gain[c] = gain[c];
    a.filmExposure = p.filmExposure + p.filmKeyEv;
    a.printEv = p.filmPrintEv;
    a.black = p.filmBlack;
    {
      double sx, sy, sd;
      filmInputAdaptation(p, &sx, &sy, &sd);
      a.sceneX = float(sx), a.sceneY = float(sy), a.adapt = float(sd);
    }
    a.warmth = p.filmWarmth;
    a.tint = p.filmTint;
    a.grainPct = p.filmGrain;
    a.halationPct = p.filmHalation;
    a.formatScale = formatScale;
    a.w = w;
    a.h = h;
    a.quality = q == SpatialQuality::Off ? 0 : (q == SpatialQuality::Interactive ? 1 : 2);
    if (fo) {
      a.keepGrain = fo->keepGrain;
      a.coarse = fo->coarseFilm;
      a.fullLongEdge = fo->fullLongEdge;
      a.frameIndex = fo->frameIndex;
    }
    ScratchImage nodeScratch;
    const ImageF* nin = &src;
    if (resample) {
      ImageF& lin = nodeScratch.get(w, h);
      parallelRows(h, [&](int y) { fetch(y, lin.row(y)); });
      nin = &lin;
    }
    if (out.width != nin->width || out.height != nin->height) out = ImageF(nin->width, nin->height);
    if (nin->width == w && nin->height == h &&
        engine::filmnode::renderFilmFrame(a, nin->data.data(), std::size_t(nin->width) * 3, out.data.data(), std::size_t(out.width) * 3))
      return true;
  }
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

void Renderer::sceneGain(const SourceImage& source, float gain[3]) const {
  const float expo = std::exp2(p_.exposure + (source.sceneReferred ? camera_.baselineExposureEv : 0.f));
  for (int c = 0; c < 3; ++c) gain[c] = wbGain_[c] * expo;
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
      if (vig != 0) applyVignette(e3, (x - cx) / cx, (y - cy) / cy, vig, p_.outputSpace == 1);
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

// EV range (around mid-grey) that this frame's tone curve shows well: the contiguous span where the encoded output of a neutral keeps at
// least 30 % of its peak slope per EV (the toe and the shoulder fall outside). This is the range the single-image HDR leaves alone.
float hdrShownRange(const std::vector<float>& tone) {
  if (tone.size() < std::size_t(kToneN)) return 8.f;
  auto ratioAt = [&](float Y) {
    std::uint32_t bits;
    std::memcpy(&bits, &Y, 4);
    int k = int(bits >> 15) - kToneBase;
    float fr = float(bits & 0x7FFFu) * (1.f / 32768.f);
    if (k < 0) k = 0, fr = 0.f;
    else if (k > kToneN - 2) k = kToneN - 2, fr = 0.f;
    return tone[std::size_t(k)] + fr * (tone[std::size_t(k) + 1] - tone[std::size_t(k)]);
  };
  constexpr int kN = 69;  // -14 .. +3 EV in quarter steps
  float enc[kN];
  for (int i = 0; i < kN; ++i) {
    const float Y = std::exp2(-14.f + 0.25f * float(i));
    const float y = std::min(1.f, Y * ratioAt(Y));
    enc[i] = y <= 0.0031308f ? 12.92f * y : 1.055f * std::pow(y, 1.f / 2.4f) - 0.055f;
  }
  float best = 0.f;
  int ip = 0;
  for (int i = 0; i + 1 < kN; ++i)
    if (enc[i + 1] - enc[i] > best) best = enc[i + 1] - enc[i], ip = i;
  if (best <= 0.f) return 8.f;
  int a = ip, b = ip;
  while (a > 0 && enc[a] - enc[a - 1] >= 0.6f * best) --a;
  while (b + 1 < kN - 1 && enc[b + 2] - enc[b + 1] >= 0.6f * best) ++b;
  return 0.25f * float(b + 1 - a);
}

}  // namespace

// The strength of the single-image HDR the frame asks for: the user's `hdr`, at least the modern rendering's own share (section 37).
static float effectiveHdr(const EditParams& p) {
  float h = p.hdr;
  if (const ModernKind mk = modernKind(p.filmStock); mk != ModernKind::None)
    h = std::max(h, modernRecipe(mk).hdr * std::clamp(p.filmStrength / 100.f, 0.f, 1.f));
  return h;
}

// The user's 100 is internal strength 0.65 (round 2 review: the internal 100 greys foliage and darkens shadows); the stored value stays 0..100.
constexpr float kHdrUserMax = 0.65f;

void Renderer::hdrBind(const SourceImage& source, FramePlan::Hdr& out, const std::vector<float>* tone) const {
  out = FramePlan::Hdr{};
  if (!(effectiveHdr(p_) > 0.f) || !source.sceneReferred) return;
  const FilmRun fr = resolveFilm(p_);
  if (fr.stock && fr.strength > 0.f) return;  // the film branch takes the source before this stage (not implemented there)
  const float expoLog2 = p_.exposure + camera_.baselineExposureEv;
  float gain[3];
  sceneGain(source, gain);
  const std::shared_ptr<const HdrField> f = hdr::fieldFor(source, wbGain_);
  if (!f) return;
  const float T = tone ? hdrShownRange(*tone) : 8.f;
  const HdrParams hp = hdr::resolve(*f, kHdrUserMax * std::clamp(effectiveHdr(p_), 0.f, 100.f) / 100.f, expoLog2, source.meta.iso, gain, T);
  out.range = hp.hi - hp.lo, out.shown = T;
  if (!hp.active()) return;  // the scene fits the range the tone curve shows: nothing to compress
  out.s = hp.s, out.ev = hp.ev, out.key = hp.key, out.yn = hp.yn, out.yclip = hp.yclip, out.lo = hp.lo, out.hi = hp.hi, out.ls = hp.ls, out.hp = hp.hp;
  out.field = f;
}

namespace {

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
  k.clarity = k.texture = k.dehaze = k.grain = 0.f;
  k.sharpen = k.sharpRadius = k.nrLuma = k.nrColor = 0.f;
  k.filmGrain = k.filmHalation = 100.f;  // spatial film parameters do not touch the lattice
  // Nor do the film's own continuous controls: the film branch applies them inside the film simulator, the lattice is
  // only the blend partner of the normal render, and the tonal class analysis ignores the film look.
  k.filmExposure = k.filmWarmth = k.filmTint = 0.f;
  // crop, straighten, rotate, flip and vignette stay in the key: the tonal analysis (histogram, tonal class, the move built
  // from it) is taken from the cropped / vignetted render of the proxy, so a bake made for another framing is stale
  // (the old key masked them: a crop change returned the previous crop's analysis; documented in docs/engine, a4b).
  char buf[104];
  std::snprintf(buf, sizeof buf, "|%p|%s|%.3f|%d|%d", static_cast<const void*>(lut_), camera_.id.c_str(), toneReach_,
                toneProxy_ ? 1 : 0, p_.outputSpace);
  return toJson(k) + buf;
}

std::string Renderer::colourKey() const {
  Renderer r(*this);
  r.p_.exposure = r.p_.contrast = r.p_.highlights = r.p_.shadows = r.p_.whites = r.p_.blacks = 0.f;
  r.p_.temperature = r.p_.tint = 0.f;
  r.p_.toneTarget.clear();
  r.p_.toneStrength = 0.f;
  // the lattice is a function of the colour stage alone: neither the framing nor the vignette reaches it
  r.p_.vignette = 0.f;
  r.p_.crop = "orig";
  r.p_.straighten = 0.f;
  r.p_.rotate = 0;
  r.p_.flip = false;
  return r.pointKey();
}


std::shared_ptr<const BakedLut> Renderer::bake(const SourceImage& source, int n, const BakedLut* colourFrom,
                                               bool fastAnalysis) const {
  const std::string ck = colourKey();
  if (colourFrom && (colourFrom->colourKey != ck || colourFrom->rgb.empty())) colourFrom = nullptr;
  n = colourFrom ? colourFrom->n : std::clamp(n, 17, 65);
  auto lut = std::make_shared<BakedLut>();
  lut->n = n;
  lut->key = pointKey() + (fastAnalysis ? "#f" : "#x") + std::to_string(n);  // fast (lattice) and exact analysis are different bakes
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
    if (!opt.keepGrain) sp.grain = 0.f;  // kept while a grain slider is dragged (14 Q8)
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
  // (scale-aware / kept grain exists in the node chain only: those frames take the float path below)
  if (fr.stock && fr.strength >= 1.f && rgba && !sp.any() && !opt.keepGrain && !(opt.fullLongEdge > 0 && opt.spatial == SpatialQuality::Full)) {
    film::SimulatorLease lease = film::acquireSimulator(fr.lib, p_.filmStock, fr.paper);
    std::string err;
    const float evGain = std::exp2(p_.filmExposure + p_.filmKeyEv);
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
        *F, &opt);
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
        applyVignette(e3, (px - cx) / cx, (py - cy) / cy, vig, p_.outputSpace == 1);
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
  const FilmRun filmRun = resolveFilm(p_);
  if (filmRun.stock && !engine::filmnode::enabled()) {
    fp.supported = false;
    fp.why = "film simulation as nodes is switched off (DAPPLE_FILM_NODES=0)";
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
  fp.outputSpace = p_.outputSpace;
  EditParams pStyle = p_;
  if (filmRun.stock) pStyle.look = "std";  // a film at full strength brings its own grain (FrameKernels::Impl)
  SpatialParams sp = makeSpatial(p_, sceneReferred, makeStyle(pStyle));
  if (opt.spatial == SpatialQuality::Off) {
    sp = SpatialParams{};
  } else if (opt.spatial == SpatialQuality::Interactive) {
    if (!opt.keepGrain) sp.grain = 0.f;  // kept while a grain slider is dragged (14 Q8)
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
  if (filmRun.stock) {  // the film stage's arguments (FrameKernels::runFilm / runFilmStage)
    FramePlan::Film& f = fp.film;
    f.active = true;
    f.displayPlain = master_.identity && red_.identity && green_.identity && blue_.identity && !(lut_ && p_.lutAmount > 0);
    f.strength = filmRun.strength;
    if (!f.displayPlain) {  // the display stages after the film: the curve tables and the look LUT go to the backend as they are
      FramePlan::Display& d = fp.display;
      const Curve1D* cs[4] = {&master_, &red_, &green_, &blue_};
      d.curveSamples = kCurveSamples;
      for (int c = 0; c < 4; ++c)
        if (!cs[c]->identity && int(cs[c]->table.size()) == kCurveSamples) d.curveMask |= 1u << c;
      if (d.curveMask) {
        d.curves.assign(std::size_t(4) * kCurveSamples, 0.f);
        for (int c = 0; c < 4; ++c)
          if ((d.curveMask >> c) & 1u) std::copy(cs[c]->table.begin(), cs[c]->table.end(), d.curves.begin() + std::ptrdiff_t(c) * kCurveSamples);
      }
      if (lut_ && p_.lutAmount > 0) {
        d.lut = lut_;
        d.lutMix = std::clamp(p_.lutAmount / 100.f, 0.f, 1.f);
        d.lutId = lut_->contentId();
      }
    }
    f.stock = p_.filmStock;
    f.paper = filmRun.paper;
    f.filmEv = p_.filmExposure + p_.filmKeyEv;
    f.printEv = p_.filmPrintEv;
    f.black = p_.filmBlack;
    {
      double sx, sy, sd;
      filmInputAdaptation(p_, &sx, &sy, &sd);
      f.sceneX = float(sx), f.sceneY = float(sy), f.adapt = float(sd);
    }
    f.warmth = p_.filmWarmth;
    f.tint = p_.filmTint;
    f.grainPct = p_.filmGrain;
    f.halationPct = p_.filmHalation;
    const float pitch = region ? std::max(d.stepX, d.stepY) : 1.f;
    f.formatScale = pitch * float(std::max(d.w, d.h)) / float(std::max(srcW, srcH));
    f.quality = opt.spatial == SpatialQuality::Off ? 0 : (opt.spatial == SpatialQuality::Interactive ? 1 : 2);
    f.keepGrain = opt.keepGrain;
    f.coarse = opt.coarseFilm;
    f.fullLongEdge = opt.fullLongEdge;
    f.frameIndex = opt.frameIndex;
  }
  return fp;
}

std::vector<float> Renderer::bakeOverflow(const SourceImage& source, const BakedLut& base, int n, float maxV) const {
  if (latticeFilmOnly() || base.rgb.size() < 4) return {};
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

// ================================================================================================
// FrameKernels: the per-pixel loop of renderBakedImpl / renderFloat as block fragments (core/engine, src/frame_kernels.h)
// ================================================================================================
struct FrameKernels::Impl {
  const Renderer& r;
  const SourceImage& source;
  const ImageF& in;
  const BakedLut* lutp;
  const bool exact;
  FastOptions opt;
  Geometry geo;
  FastDims d;
  bool region = false, useGeo = false, exactMap = false;
  FilmRun fr;
  SpatialParams sp;
  float gain[3] = {1.f, 1.f, 1.f};
  float vig = 0.f;
  FramePlan::Hdr hdr;           // single-image HDR (inactive: strength 0, film, display-referred source)
  HdrParams hdrP;
  std::optional<LutSampler> sample;
  mutable std::once_flag opOnce;
  mutable std::optional<Renderer::PointOp> overflowOp;
  std::optional<Renderer::PointOp> exactOp;
  bool filmRan = false, filmOkV = false, valid = false;
  float pitch = 1.f;
  std::shared_ptr<const local::Frame> local;  // local adjustments (docs/engine/pipeline-architecture.md section 40); null for film frames in v1

  Impl(const Renderer& rr, const SourceImage& src, const BakedLut* lut, const FastOptions& o)
      : r(rr), source(src), in(src.linear), lutp(lut), exact(lut == nullptr), opt(o) {
    if (in.empty() || (lut && lut->n < 2)) return;
    geo.setup(r.p_, in.width, in.height);
    fr = resolveFilm(r.p_);
    if (!exact) {
      const RenderRegion& R = opt.region;
      region = R.active();
      d = fastDims(geo, R);
      EditParams pStyle = r.p_;
      if (fr.stock && fr.strength >= 1.f) pStyle.look = "std";  // a film at full strength brings its own grain
      const StyleMix style = makeStyle(pStyle);
      sp = makeSpatial(r.p_, source.sceneReferred, style);
      if (opt.spatial == SpatialQuality::Off) {
        sp = SpatialParams{};
      } else if (opt.spatial == SpatialQuality::Interactive) {
        if (!opt.keepGrain) sp.grain = 0.f;
        if (!opt.noise) sp.nrLuma = sp.nrColor = 0.f;
      }
      if (region) sp.scale = std::max(float(d.w) / (d.fx1 - d.fx0), float(d.h) / (d.fy1 - d.fy0)) / 2048.f;
      const float expo = std::exp2(r.p_.exposure + (source.sceneReferred ? r.camera_.baselineExposureEv : 0.f));
      for (int c = 0; c < 3; ++c) gain[c] = r.wbGain_[c] * expo;
      sample.emplace(*lut);
      pitch = region ? std::max(d.stepX, d.stepY) : 1.f;
    } else {
      d = fastDims(geo, RenderRegion{});
      exactOp.emplace(r.makePointOp(source, nullptr));
      sp = opt.spatial == SpatialQuality::Off ? SpatialParams{} : makeSpatial(r.p_, source.sceneReferred, exactOp->style);
    }
    vig = r.p_.vignette / 100.f;
    useGeo = geo.active || region;
    exactMap = geo.exact && !region;
    if (effectiveHdr(r.p_) > 0.f) {
      std::vector<float> exactTone;
      if (exact) fillToneTable(exactTone, *exactOp);
      r.hdrBind(source, hdr, exact ? &exactTone : &lut->tone);
    }
    if (hdr.active()) {
      hdrP.s = hdr.s, hdrP.ev = hdr.ev, hdrP.key = hdr.key, hdrP.yn = hdr.yn, hdrP.yclip = hdr.yclip;
      hdrP.lo = hdr.lo, hdrP.hi = hdr.hi, hdrP.ls = hdr.ls, hdrP.hp = hdr.hp;
    }
    if (!r.p_.local.empty()) local = local::bind(r.p_.local, source, opt.spatial == SpatialQuality::Interactive);
    valid = true;
  }

  // source position (continuous, pixel centres at +0.5) of output pixel (x, y), normalised by the source size
  void srcUV(int x, int y, float& u, float& v) const {
    if (!useGeo) {
      u = (float(x) + 0.5f) / float(in.width);
      v = (float(y) + 0.5f) / float(in.height);
      return;
    }
    const float fx = d.ox0 + (float(x) + 0.5f) * d.stepX, fy = d.oy0 + (float(y) + 0.5f) * d.stepY;
    float sx, sy;
    geo.mapF(fx, fy, sx, sy);
    u = sx / float(in.width);
    v = sy / float(in.height);
  }
  void hdrPixel(int x, int y, float* v3) const {
    float u, v;
    srcUV(x, y, u, v);
    hdr::apply(*hdr.field, hdrP, u, v, v3);
  }

  void fetchPixel(int x, int y, float v[3]) const {
    const float fx = d.ox0 + (float(x) + 0.5f) * d.stepX, fy = d.oy0 + (float(y) + 0.5f) * d.stepY;
    float sx, sy;
    geo.mapF(fx, fy, sx, sy);
    if (exactMap) {
      const float* px = in.row(std::clamp(int(std::floor(sy)), 0, in.height - 1)) +
                        3 * std::clamp(int(std::floor(sx)), 0, in.width - 1);
      v[0] = px[0], v[1] = px[1], v[2] = px[2];
    } else {
      sampleCubic(in, sx, sy, v);
    }
  }
};

FrameKernels::FrameKernels(const Renderer& r, const SourceImage& src, const BakedLut* lut, const FastOptions& opt)
    : impl_(std::make_unique<Impl>(r, src, lut, opt)) {}
FrameKernels::~FrameKernels() = default;

bool FrameKernels::valid() const { return impl_->valid; }
int FrameKernels::width() const { return impl_->d.w; }
int FrameKernels::height() const { return impl_->d.h; }
bool FrameKernels::exactVariant() const { return impl_->exact; }
bool FrameKernels::useGeo() const { return impl_->useGeo; }
bool FrameKernels::filmSelected() const { return impl_->fr.stock != nullptr; }
float FrameKernels::filmStrength() const { return impl_->fr.strength; }
bool FrameKernels::vignetteActive() const { return impl_->vig != 0.f; }
const SpatialParams& FrameKernels::spatial() const { return impl_->sp; }
float FrameKernels::spatialScale() const {
  return impl_->sp.scale > 0.f ? impl_->sp.scale : float(std::max(impl_->d.w, impl_->d.h)) / 2048.f;
}
bool FrameKernels::filmOk() const { return impl_->filmOkV; }
bool FrameKernels::needNormal() const { return !impl_->filmOkV || impl_->fr.strength < 1.f; }

FramePlan FrameKernels::framePlan() const {
  const Impl& m = *impl_;
  FramePlan fp;
  fp.srcW = m.in.width;
  fp.srcH = m.in.height;
  fp.outW = m.d.w;
  fp.outH = m.d.h;
  fp.useGeo = m.useGeo;
  fp.exact = m.exactMap;
  fp.rot = m.geo.rot;
  fp.flip = m.geo.flip;
  fp.cs = m.geo.cs;
  fp.sn = m.geo.sn;
  fp.fw = m.geo.fw;
  fp.fh = m.geo.fh;
  fp.ox0 = m.d.ox0;
  fp.oy0 = m.d.oy0;
  fp.stepX = m.d.stepX;
  fp.stepY = m.d.stepY;
  fp.fullW = m.d.fullW;
  fp.fullH = m.d.fullH;
  for (int c = 0; c < 3; ++c) fp.gain[c] = m.gain[c];
  fp.vignette = m.vig;
  fp.outputSpace = m.r.p_.outputSpace;
  fp.hdr = m.hdr;
  fp.nrLuma = m.sp.nrLuma;
  fp.nrColor = m.sp.nrColor;
  fp.dehaze = m.sp.dehaze;
  fp.clarity = m.sp.clarity;
  fp.texture = m.sp.texture;
  fp.sharpen = m.sp.sharpen;
  fp.sharpRadius = m.sp.sharpRadius;
  fp.grain = m.sp.grain;
  fp.spatialScale = spatialScale();
  fp.local = m.local;
  fp.localDetail = m.local && m.local->anyWindow;
  return fp;
}

void FrameKernels::srcUV(int x, int y, float& u, float& v) const { impl_->srcUV(x, y, u, v); }
void FrameKernels::sourceRow(float* px, int n, int x0, int y) const {
  if (impl_->useGeo) fetchRow(px, n, x0, y);
  else copyRow(px, n, x0, y);
}
const local::Frame* FrameKernels::localFrame() const { return impl_->local.get(); }

void FrameKernels::copyRow(float* px, int n, int x0, int y) const {
  std::memcpy(px, impl_->in.row(y) + 3 * std::size_t(x0), std::size_t(n) * 3 * sizeof(float));
}

void FrameKernels::fetchRow(float* px, int n, int x0, int y) const {
  for (int i = 0; i < n; ++i) impl_->fetchPixel(x0 + i, y, px + 3 * i);
}

void FrameKernels::gain(float* px, int n) const {
  const Impl& m = *impl_;
  if (m.exact) {  // renderFloat: (v * wb) * exposure
    const Renderer::PointOp& op = *m.exactOp;
    for (int i = 0; i < n; ++i)
      for (int c = 0; c < 3; ++c) px[3 * i + c] = std::max(0.f, px[3 * i + c] * op.wb[c] * op.exposureGain);
    return;
  }
  for (int i = 0; i < n; ++i)
    for (int c = 0; c < 3; ++c) px[3 * i + c] = std::max(0.f, px[3 * i + c] * m.gain[c]);
}

void FrameKernels::toneColour(float* px, int n) const {
  const Impl& m = *impl_;
  if (m.exact) {
    for (int i = 0; i < n; ++i) {
      const float v[3] = {px[3 * i], px[3 * i + 1], px[3 * i + 2]};
      m.exactOp->apply(v, px + 3 * i);
    }
    return;
  }
  const LutSampler sample = *m.sample;
  for (int i = 0; i < n; ++i) {
    float* p = px + 3 * i;
    const float v[3] = {p[0], p[1], p[2]};
    float e3[3], tm[3];
    if (!sample(v, e3, tm)) {
      std::call_once(m.opOnce, [&] { m.overflowOp.emplace(m.r.makePointOp(m.source, m.lutp)); });
      m.overflowOp->colour(tm, e3);
    }
    p[0] = e3[0], p[1] = e3[1], p[2] = e3[2];
  }
}

bool FrameKernels::canFuseHead() const { return !impl_->exact; }
bool FrameKernels::hdrActive() const { return impl_->hdr.active(); }
void FrameKernels::hdrBlock(float* px, int n, int x0, int y) const {
  if (!impl_->hdr.active()) return;
  for (int i = 0; i < n; ++i) impl_->hdrPixel(x0 + i, y, px + 3 * i);
}

void FrameKernels::headGainTone(float* px, int n, int x0, int y) const {
  const Impl& m = *impl_;
  // local copies: the compiler cannot keep fields of an object it may alias with the float stores below in registers
  const LutSampler sample = *m.sample;
  const float gain[3] = {m.gain[0], m.gain[1], m.gain[2]};
  const float* srcRow = m.useGeo ? nullptr : m.in.row(y) + 3 * std::size_t(x0);
  const bool hdrOn = m.hdr.active();
  for (int i = 0; i < n; ++i) {
    float v[3];
    if (srcRow) {
      v[0] = srcRow[3 * i], v[1] = srcRow[3 * i + 1], v[2] = srcRow[3 * i + 2];
    } else {
      m.fetchPixel(x0 + i, y, v);
    }
    for (int c = 0; c < 3; ++c) v[c] = std::max(0.f, v[c] * gain[c]);
    if (hdrOn) m.hdrPixel(x0 + i, y, v);
    float e3[3], tm[3];
    if (!sample(v, e3, tm)) {
      std::call_once(m.opOnce, [&] { m.overflowOp.emplace(m.r.makePointOp(m.source, m.lutp)); });
      m.overflowOp->colour(tm, e3);
    }
    float* p = px + 3 * i;
    p[0] = e3[0], p[1] = e3[1], p[2] = e3[2];
  }
}

void FrameKernels::displayStages(float* px, int n) const {
  for (int i = 0; i < n; ++i) impl_->r.displayStages(px + 3 * i);
}

void FrameKernels::blend(float* main, const float* film, int n) const {
  const float s = impl_->fr.strength;
  for (int i = 0; i < n; ++i)
    for (int c = 0; c < 3; ++c) main[3 * i + c] = main[3 * i + c] + s * (film[3 * i + c] - main[3 * i + c]);
}

void FrameKernels::vignette(float* px, int n, int x0, int y) const {
  const Impl& m = *impl_;
  const float cx = m.d.fullW * 0.5f, cy = m.d.fullH * 0.5f;
  const float py = m.d.oy0 + (float(y) + 0.5f) * m.d.stepY - 0.5f;
  for (int i = 0; i < n; ++i) {
    const float pxx = m.d.ox0 + (float(x0 + i) + 0.5f) * m.d.stepX - 0.5f;
    applyVignette(px + 3 * i, (pxx - cx) / cx, (py - cy) / cy, m.vig, m.r.p_.outputSpace == 1);
  }
}

bool FrameKernels::streamFilm(std::uint8_t* rgba) {
  Impl& m = *impl_;
  if (m.exact || !(m.fr.stock && m.fr.strength >= 1.f) || !rgba || m.sp.any()) return false;
  if (m.opt.keepGrain || (m.opt.fullLongEdge > 0 && m.opt.spatial == SpatialQuality::Full)) return false;  // scale-aware / kept grain: node chain only
  const Renderer& rr = m.r;
  const int w = m.d.w, h = m.d.h;
  film::SimulatorLease lease = film::acquireSimulator(m.fr.lib, rr.p_.filmStock, m.fr.paper);
  std::string err;
  const float evGain = std::exp2(rr.p_.filmExposure + rr.p_.filmKeyEv);
  const float gainEv[3] = {m.gain[0] * evGain, m.gain[1] * evGain, m.gain[2] * evGain};
  if (!(lease && lease->setParams(makeFilmParams(rr.p_, m.fr, m.opt.spatial, m.pitch), &err))) return false;
  const bool plain = rr.master_.identity && rr.red_.identity && rr.green_.identity && rr.blue_.identity &&
                     !(rr.lut_ && rr.p_.lutAmount > 0);
  auto sink = [&](int y, float* t, int W) {
    std::uint8_t* o = rgba + std::size_t(y) * W * 4;
    if (!plain || m.vig != 0) {
      for (int x = 0; x < W; ++x) {
        float e3[3] = {t[3 * x], t[3 * x + 1], t[3 * x + 2]};
        if (!plain) rr.displayStages(e3);
        if (m.vig != 0) {
          const float cx = m.d.fullW * 0.5f, cy = m.d.fullH * 0.5f;
          const float px = m.d.ox0 + (float(x) + 0.5f) * m.d.stepX - 0.5f,
                      py = m.d.oy0 + (float(y) + 0.5f) * m.d.stepY - 0.5f;
          applyVignette(e3, (px - cx) / cx, (py - cy) / cy, m.vig);
        }
        t[3 * x] = e3[0], t[3 * x + 1] = e3[1], t[3 * x + 2] = e3[2];
      }
    }
    for (int i = 0; i < W; ++i) {  // clamp + encode (vectorises: fixed trip count, no branches)
      const float rr_ = std::min(std::max(t[3 * i], 0.f), 1.f), g = std::min(std::max(t[3 * i + 1], 0.f), 1.f),
                  b = std::min(std::max(t[3 * i + 2], 0.f), 1.f);
      o[4 * i] = std::uint8_t(rr_ * 255.f + 0.5f);
      o[4 * i + 1] = std::uint8_t(g * 255.f + 0.5f);
      o[4 * i + 2] = std::uint8_t(b * 255.f + 0.5f);
      o[4 * i + 3] = 255;
    }
  };
  auto tramp = [](void* u, int y, float* t, int W) { (*static_cast<decltype(sink)*>(u))(y, t, W); };
  auto fetchRowFn = [&](int y, float* row) {
    for (int x = 0; x < w; ++x) m.fetchPixel(x, y, row + 3 * x);
  };
  auto fetchTramp = [](void* u, int y, float* row) { (*static_cast<decltype(fetchRowFn)*>(u))(y, row); };
  const bool resampled = m.geo.active || m.region;
  return lease->renderRows(m.in, w, h, gainEv, resampled ? fetchTramp : nullptr, &fetchRowFn, tramp, &sink);
}

bool FrameKernels::runFilm(ImageF& F, const local::AuxTarget* aux) {
  Impl& m = *impl_;
  m.filmRan = true;
  if (!m.fr.stock) return false;
  const int w = m.d.w, h = m.d.h;
  float g[3];
  if (m.exact) {
    for (int c = 0; c < 3; ++c) g[c] = m.exactOp->wb[c] * m.exactOp->exposureGain;
  } else {
    for (int c = 0; c < 3; ++c) g[c] = m.gain[c];
  }
  const SpatialQuality q = m.exact ? (m.opt.spatial == SpatialQuality::Off ? SpatialQuality::Off : SpatialQuality::Full)
                                   : m.opt.spatial;
  const local::Frame* lf = m.local.get();
  m.filmOkV = runFilmStage(
      m.r.p_, m.fr, g, m.in, m.useGeo || lf, w, h, m.pitch, q,
      [&](int y, float* row) {
        if (m.useGeo) {
          for (int x = 0; x < w; ++x) m.fetchPixel(x, y, row + 3 * x);
        } else {
          std::memcpy(row, m.in.row(y), std::size_t(w) * 3 * sizeof(float));
        }
        if (!lf) return;
        // local adjustments, stage L, on the film's scene input (docs section 40.7): in the gained domain, back to the film's input domain
        const bool wantK = aux && aux->data && lf->anyWindow;
        float K[kLocalWindowSlots];
        for (int x = 0; x < w; ++x) {
          float* p = row + 3 * x;
          const float src[3] = {p[0], p[1], p[2]};
          float lin[3] = {std::max(0.f, p[0] * g[0]), std::max(0.f, p[1] * g[1]), std::max(0.f, p[2] * g[2])};
          float u, v;
          m.srcUV(x, y, u, v);
          local::applyLinear(*lf, u, v, src, lin, wantK ? K : nullptr);
          for (int c = 0; c < 3; ++c) p[c] = g[c] > 0.f ? lin[c] / g[c] : 0.f;
          if (wantK) {
            const std::size_t o = std::size_t(y - aux->oy) * aux->w + std::size_t(x - aux->ox);
            for (int k = 0; k < kLocalWindowSlots; ++k)
              if (aux->slot[k] >= 0) aux->data[std::size_t(aux->slot[k]) * aux->plane + o] = K[k];
          }
        }
      },
      F, &m.opt);
  return m.filmOkV;
}

void FrameKernels::finalizeSpatial() {
  Impl& m = *impl_;
  if (!m.exact || m.opt.spatial == SpatialQuality::Off) return;
  // renderFloat: a film at full strength brings its own grain, the look's grain would double it
  EditParams pe = m.r.p_;
  if (m.filmOkV && m.fr.strength >= 1.f) pe.look = "std";
  m.sp = makeSpatial(m.r.p_, m.source.sceneReferred, m.filmOkV && m.fr.strength >= 1.f ? makeStyle(pe) : m.exactOp->style);
}

struct ScratchFrame::Impl {
  ScratchImage s;
};
ScratchFrame::ScratchFrame() : impl_(std::make_unique<Impl>()) {}
ScratchFrame::~ScratchFrame() = default;
ImageF& ScratchFrame::get(int w, int h) { return impl_->s.get(w, h); }

}  // namespace dapple
