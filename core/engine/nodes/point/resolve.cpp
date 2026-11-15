// The point family's resolves (docs/engine/pipeline-architecture.md section 50): the parameters of an edit (resolve), the gains, the
// colour stage operator (makePointOp), the keys, the frame's blocks (resolveFrame: geometry, warp, gain, film glue, display) and the
// single-image HDR binding (hdrBind). Pure functions of their inputs; the per-pixel code is in kernels.h.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "dapple/film_match.h"
#include "engine/nodes/film/film_nodes.h"
#include "engine/nodes/point/kernels.h"
#include "hdr_field.h"
#include "warp_field.h"

namespace dapple::engine::point {

namespace {
bool allZero(const float amt[4]) { return amt[0] == 0 && amt[1] == 0 && amt[2] == 0 && amt[3] == 0; }

}  // namespace

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

void whiteBalanceGain(const EditParams& p, float out[3]) {
  constexpr float kTemp = 0.006f, kTint = 0.004f;
  out[0] = std::exp2(kTemp * p.temperature);
  out[1] = std::exp2(-kTint * p.tint);
  out[2] = std::exp2(-kTemp * p.temperature);
  const float norm = proPhotoLuma(out);
  for (int c = 0; c < 3; ++c) out[c] /= norm;
}

void inputGain(const EditParams& params, const CameraProfile& camera, bool sceneReferred, float out[3]) {
  EditParams p = params;
  p.clamp();
  float wb[3];
  whiteBalanceGain(p, wb);
  const float expo = std::exp2(p.exposure + (sceneReferred ? camera.baselineExposureEv : 0.f));
  for (int c = 0; c < 3; ++c) out[c] = wb[c] * expo;
}

Params resolve(const EditParams& params, const CameraProfile& camera, const Lut3D* lut) {
  Params rp;
  rp.p = params;
  rp.camera = &camera;
  rp.lut = lut;
  rp.p.clamp();
  whiteBalanceGain(rp.p, rp.wb);
  // Film frames are authored for sRGB output (their LUTs, grain, paper mapping): they never render in P3; the C layer converts the finished sRGB frame.
  if (rp.p.outputSpace == 1 && filmActive(rp.p)) rp.p.outputSpace = 0;

  // Each curve = parametric (four regions) followed by the point curve.
  const EditParams& p = rp.p;
  auto build = [&](Curve1D& c, const float amt[4], const std::vector<CurvePoint>& pts) {
    const bool para = !allZero(amt);
    const bool point = !pts.empty();
    if (!para && !point) return;
    const std::vector<float> pc =
        para ? sampleParametric(amt, p.parametricShadowSplit, p.parametricMidtoneSplit, p.parametricHighlightSplit)
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
  const float master[4] = {p.parametricShadows, p.parametricDarks, p.parametricLights, p.parametricHighlights};
  build(rp.curves.master, master, p.toneCurve);
  build(rp.curves.red, p.channelParametric[0].data(), p.toneCurveRed);
  build(rp.curves.green, p.channelParametric[1].data(), p.toneCurveGreen);
  build(rp.curves.blue, p.channelParametric[2].data(), p.toneCurveBlue);
  return rp;
}

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


PointOp makePointOp(const Params& rp, const SourceImage& source, const BakedLut* reuse) {
  PointOp op;
  const bool scene = source.sceneReferred;
  op.scene = scene;
  for (int c = 0; c < 3; ++c) op.wb[c] = rp.wb[c];
  op.exposureGain = std::exp2(rp.p.exposure + (scene ? rp.camera->baselineExposureEv : 0.f));
  op.contrast = (rp.p.contrast + (scene ? rp.camera->baselineContrast : 0.f)) / 100.f;
  op.hi = rp.p.highlights / 100.f;
  op.sh = rp.p.shadows / 100.f;
  op.wh = rp.p.whites / 100.f;
  op.bl = rp.p.blacks / 100.f;
  op.sat = rp.p.saturation / 100.f;
  op.vib = rp.p.vibrance / 100.f;

  // User HSL plus the camera's base colour rendering (scene-referred sources only).
  op.hslEff = rp.p.hsl;
  if (scene)
    for (int i = 0; i < kHslBandCount; ++i) {
      op.hslEff[i].hue += rp.camera->baselineHsl[i].hue;
      op.hslEff[i].saturation += rp.camera->baselineHsl[i].saturation;
      op.hslEff[i].luminance += rp.camera->baselineHsl[i].luminance;
    }
  for (const auto& b : op.hslEff) op.anyHsl |= (b.hue != 0 || b.saturation != 0 || b.luminance != 0);

  op.gS = makeGrade(rp.p.gradeShadows);
  op.gM = makeGrade(rp.p.gradeMidtones);
  op.gH = makeGrade(rp.p.gradeHighlights);
  op.gG = makeGrade(rp.p.gradeGlobal);
  op.zoneGrade = op.gS.active || op.gM.active || op.gH.active;
  op.anyGrade = op.zoneGrade || op.gG.active;
  // zone cross-over: blend widens the soft edges, balance moves them (positive = more highlight)
  op.gWidth = 0.05f + 0.30f * rp.p.gradeBlend / 100.f;
  const float gShift = -rp.p.gradeBalance / 100.f * 0.2f;
  op.gC1 = 0.30f + gShift;
  op.gC2 = 0.70f + gShift;

  op.style = makeStyle(rp.p);
  op.match = &rp.camera->colorMatch;
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
  } else if (rp.p.toneStrength > 0.f) {
    op.analysis = analyzeTone(rp.toneProxy ? *rp.toneProxy : source, *rp.camera, rp.p);
    op.hasAnalysis = true;
    const auto pts = toneTargetCurve(op.analysis, rp.p.toneTarget, rp.p.toneStrength / 100.f, rp.toneReach);
    if (!pts.empty()) {
      op.toneMove.table = sampleMonotoneCurve(pts);
      op.toneMove.identity = false;
    }
  }

  if (const ModernKind mk = modernKind(rp.p.filmStock); mk != ModernKind::None && rp.p.filmStrength > 0.f) {
    const ModernRecipe& r = modernRecipe(mk);
    const float s = std::clamp(rp.p.filmStrength / 100.f, 0.f, 1.f);
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
      const SourceImage& img = rp.modernProxy ? *rp.modernProxy : source;
      const int W = img.linear.width, H = img.linear.height;
      const int stride = std::max(1, std::max(W, H) / 256);
      const float baseGain = std::exp2(scene ? rp.camera->baselineExposureEv : 0.f);
      std::vector<float> Y, S;
      Y.reserve(std::size_t((W / stride + 1) * (H / stride + 1)));
      S.reserve(Y.capacity());
      for (int y = 0; y < H; y += stride) {
        const int y1 = std::min(H - 1, y + stride / 2);
        for (int x = 0; x < W; x += stride) {
          const int x1 = std::min(W - 1, x + stride / 2);
          const float* a = img.linear.row(y1) + 3 * x1;
          const float v[3] = {a[0] * rp.wb[0] * baseGain, a[1] * rp.wb[1] * baseGain, a[2] * rp.wb[2] * baseGain};
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
  op.lut = rp.lut;
  op.lutMix = std::clamp(rp.p.lutAmount / 100.f, 0.f, 1.f);
  op.fade = op.style.fade;
  op.p3 = rp.p.outputSpace == 1;
  op.master = &rp.curves.master;
  op.red = &rp.curves.red;
  op.green = &rp.curves.green;
  op.blue = &rp.curves.blue;
  return op;
}

void modernGridSize(int W, int H, int& gw, int& gh) {
  const int stride = std::max(1, std::max(W, H) / 256);
  gw = (W + stride - 1) / stride;
  gh = (H + stride - 1) / stride;
}
void modernGridPixel(int W, int H, int i, int j, int& x, int& y) {  // the samples makePointOp's modern end points read (below)
  const int stride = std::max(1, std::max(W, H) / 256);
  x = std::min(W - 1, i * stride + stride / 2);
  y = std::min(H - 1, j * stride + stride / 2);
}

float modernSdrWhite(const Params& rp, const SourceImage& source) {
  const PointOp op = makePointOp(rp, source, nullptr);
  if (!op.mod.tone) return 1.f;
  return std::pow(std::max(modernDecode(op.mod.pw), 1e-4f), op.mod.s);
}

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

bool filmActive(const EditParams& p) {
  if (p.filmStock.size() > 64 || p.filmStock == "none") return false;  // what EditParams::clamp clears
  return resolveFilm(p).stock != nullptr;
}

void sceneGain(const Params& rp, const SourceImage& source, float gain[3]) {
  const float expo = std::exp2(rp.p.exposure + (source.sceneReferred ? rp.camera->baselineExposureEv : 0.f));
  for (int c = 0; c < 3; ++c) gain[c] = rp.wb[c] * expo;
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


// The strength of the single-image HDR the frame asks for: the user's `hdr`, at least the modern rendering's own share (section 37).
float effectiveHdr(const EditParams& p) {
  float h = p.hdr;
  if (const ModernKind mk = modernKind(p.filmStock); mk != ModernKind::None)
    h = std::max(h, modernRecipe(mk).hdr * std::clamp(p.filmStrength / 100.f, 0.f, 1.f));
  return h;
}

// The user's 100 is internal strength 0.65 (round 2 review: the internal 100 greys foliage and darkens shadows); the stored value stays 0..100.
constexpr float kHdrUserMax = 0.65f;

void hdrBind(const Params& rp, const SourceImage& source, engine::point::Hdr& out, const std::vector<float>* tone) {
  out = engine::point::Hdr{};
  if (!(effectiveHdr(rp.p) > 0.f) || !source.sceneReferred) return;
  const FilmRun fr = resolveFilm(rp.p);
  if (fr.stock && fr.strength > 0.f) return;  // the film branch takes the source before this stage (not implemented there)
  const float expoLog2 = rp.p.exposure + rp.camera->baselineExposureEv;
  float gain[3];
  sceneGain(rp, source, gain);
  const std::shared_ptr<const HdrField> f = hdr::fieldFor(rp.hdrProxy ? *rp.hdrProxy : source, rp.wb);  // streaming export: the 512 px copy (section 42)
  if (!f) return;
  const float T = tone ? hdrShownRange(*tone) : 8.f;
  const HdrParams hp = hdr::resolve(*f, kHdrUserMax * std::clamp(effectiveHdr(rp.p), 0.f, 100.f) / 100.f, expoLog2, source.meta.iso, gain, T);
  out.range = hp.hi - hp.lo, out.shown = T;
  if (!hp.active()) return;  // the scene fits the range the tone curve shows: nothing to compress
  out.s = hp.s, out.ev = hp.ev, out.key = hp.key, out.yn = hp.yn, out.yclip = hp.yclip, out.lo = hp.lo, out.hi = hp.hi, out.ls = hp.ls, out.hp = hp.hp;
  out.field = f;
}


// Framing (rotate 90 -> straighten (+ ratio crop without empty corners) -> flip) of a w x h source, then the region `R` of the picture.
// `scale`: the radii scale of the frame (full-picture long edge / 2048; a region at its own pixel pitch).
Geometry resolveGeometry(const EditParams& p, int w, int h, const RenderRegion& R, float* scale) {
  Geometry g;
  g.srcW = w;
  g.srcH = h;
  g.rot = (p.rotate / 90) % 4;
  g.flip = p.flip;
  g.fw = float(g.rot % 2 ? h : w);
  g.fh = float(g.rot % 2 ? w : h);
  const float fw = g.fw, fh = g.fh;
  const bool tilted = p.straighten != 0.f;
  float ar = fw / fh;
  if (p.crop == "1:1") ar = 1.f;
  else if (p.crop == "4:5") ar = 4.f / 5.f;
  else if (p.crop == "3:4") ar = 3.f / 4.f;
  else if (p.crop == "16:9") ar = 16.f / 9.f;
  else if (p.crop == "9:16") ar = 9.f / 16.f;
  const bool cropped = p.crop != "orig";
  const bool framed = g.rot != 0 || g.flip || tilted || cropped;
  int pw = w, ph = h;  // size of the whole (geometry-transformed) picture
  if (framed) {
    const float th = p.straighten * kPi / 180.f;
    g.cs = std::cos(th);
    g.sn = std::sin(th);
    const float c = std::fabs(g.cs), s = std::fabs(g.sn);
    // Largest box of ratio ar, rotated by th, that stays inside the fw x fh frame.
    float bw = std::min(fw / (c + s / ar), fh / (s + c / ar));
    if (tilted) bw -= 2.5f;  // keep the 4x4 resampling kernel off the image border
    bw = std::max(1.f, bw);
    pw = std::clamp(int(tilted ? std::floor(bw) : std::lround(bw)), 1, int(fw));
    ph = std::clamp(int(tilted ? std::floor(bw / ar) : std::lround(bw / ar)), 1, int(fh));
  }
  g.fullW = float(pw);
  g.fullH = float(ph);
  const float fx0 = std::clamp(R.x0, 0.f, 1.f), fy0 = std::clamp(R.y0, 0.f, 1.f);
  const float fx1 = std::clamp(R.x1, fx0 + 1e-4f, 1.f), fy1 = std::clamp(R.y1, fy0 + 1e-4f, 1.f);
  g.region = R.active();
  if (!g.region) {
    g.outW = pw;
    g.outH = ph;
  } else {
    const float rw = (fx1 - fx0) * g.fullW, rh = (fy1 - fy0) * g.fullH;
    const float k = R.outLongEdge > 0 ? float(R.outLongEdge) / std::max(rw, rh) : 1.f;
    g.outW = std::max(1, int(std::lround(rw * k)));
    g.outH = std::max(1, int(std::lround(rh * k)));
    g.ox0 = fx0 * g.fullW;
    g.oy0 = fy0 * g.fullH;
    g.stepX = rw / float(g.outW);
    g.stepY = rh / float(g.outH);
  }
  g.useGeo = framed || g.region;
  g.exact = !tilted && !g.region;
  if (scale)
    *scale = g.region ? std::max(float(g.outW) / (fx1 - fx0), float(g.outH) / (fy1 - fy0)) / 2048.f : float(std::max(g.outW, g.outH)) / 2048.f;
  return g;
}

std::string pointKey(const Params& rp) {
  EditParams k = rp.p;
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
  std::snprintf(buf, sizeof buf, "|%p|%s|%.3f|%d|%d", static_cast<const void*>(rp.lut), rp.camera->id.c_str(), rp.toneReach,
                rp.toneProxy ? 1 : 0, rp.p.outputSpace);
  return toJson(k) + buf;
}

std::string colourKey(const Params& rp) {
  Params r = rp;
  r.p.exposure = r.p.contrast = r.p.highlights = r.p.shadows = r.p.whites = r.p.blacks = 0.f;
  r.p.temperature = r.p.tint = 0.f;
  r.p.toneTarget.clear();
  r.p.toneStrength = 0.f;
  // the lattice is a function of the colour stage alone: neither the framing nor the vignette reaches it
  r.p.vignette = 0.f;
  r.p.crop = "orig";
  r.p.straighten = 0.f;
  r.p.rotate = 0;
  r.p.flip = false;
  return pointKey(r);
}


void fastSize(const Params& rp, const SourceImage& source, const RenderRegion& region, int& w, int& h) {
  const Geometry g = resolveGeometry(rp.p, source.linear.width, source.linear.height, region);
  w = g.outW;
  h = g.outH;
}

// ---- the point family's blocks of a frame (both executors) -----------------------------------------------------------------------
Blocks resolveFrame(const Params& rp, int srcW, int srcH, bool sceneReferred, const FastOptions& opt, const SourceImage* src,
                                             bool exactVariant) {
  engine::point::Blocks fp;
  const FilmRun filmRun = resolveFilm(rp.p);
  float scale = 1.f;
  fp.geo = resolveGeometry(rp.p, srcW, srcH, exactVariant ? RenderRegion{} : opt.region, &scale);
  const Geometry& g = fp.geo;
  const bool par = src && src->parent && src->parent->fullW > 0 && src->parent->fullH > 0;
  if (par && srcW > 0 && srcH > 0)  // a region source (docs section 44): the radii are fractions of the PARENT picture (like the export's)
    scale *= float(std::max(src->parent->fullW, src->parent->fullH)) / float(std::max(srcW, srcH));
  fp.spatialScale = scale;
  if (src && rp.p.reshape.active() && srcW > 0 && srcH > 0 && !src->linear.empty()) {
    // portrait reshaping (section 45): the field lives in the photo's normalised space (a region window samples it in its parent's
    // coordinates), mapped to the frame's source pixels (the GPU store may be a resized copy of `src`: kx, ky source px of `src` per frame px)
    const int PW = par ? src->parent->fullW : src->linear.width, PH = par ? src->parent->fullH : src->linear.height;
    fp.warp.field = warp::bind(rp.p.reshape, PW, PH, !exactVariant && opt.spatial == SpatialQuality::Interactive);
    if (fp.warp.field) {
      const warp::Field& f = *fp.warp.field;
      const float kx = float(src->linear.width) / float(srcW), ky = float(src->linear.height) / float(srcH);
      const warp::Map m = warp::makeMap(f, src->linear.width, src->linear.height, PW, PH, par ? src->parent->x0 : 0, par ? src->parent->y0 : 0);
      fp.warp.ax = m.ax * kx, fp.warp.bx = m.bx, fp.warp.ay = m.ay * ky, fp.warp.by = m.by, fp.warp.lpx = m.lpx / kx;
      fp.warp.pad = int(std::ceil(f.maxDisp * fp.warp.lpx)) + 1;
      fp.geo.useGeo = true;
    }
  }
  const float expo = std::exp2(rp.p.exposure + (sceneReferred ? rp.camera->baselineExposureEv : 0.f));
  for (int c = 0; c < 3; ++c) fp.gain[c] = rp.wb[c] * expo;
  // the look's grain: a film at full strength brings its own (the exact variant settles it after the film ran, point::CpuFrame::finalizeSpatial)
  EditParams pStyle = rp.p;
  if (!exactVariant && filmRun.stock && filmRun.strength >= 1.f) pStyle.look = "std";
  fp.lookGrain = makeStyle(pStyle).grain;
  if (filmRun.stock) {  // the film stage's arguments (the GPU film nodes)
    engine::point::Film& f = fp.film;
    f.active = true;
    f.displayPlain = rp.curves.master.identity && rp.curves.red.identity && rp.curves.green.identity && rp.curves.blue.identity && !(rp.lut && rp.p.lutAmount > 0);
    f.strength = filmRun.strength;
    if (!f.displayPlain) {  // the display stages after the film: the curve tables and the look LUT go to the backend as they are
      engine::point::Display& d = fp.display;
      const Curve1D* cs[4] = {&rp.curves.master, &rp.curves.red, &rp.curves.green, &rp.curves.blue};
      d.curveSamples = kCurveSamples;
      for (int c = 0; c < 4; ++c)
        if (!cs[c]->identity && int(cs[c]->table.size()) == kCurveSamples) d.curveMask |= 1u << c;
      if (d.curveMask) {
        d.curves.assign(std::size_t(4) * kCurveSamples, 0.f);
        for (int c = 0; c < 4; ++c)
          if ((d.curveMask >> c) & 1u) std::copy(cs[c]->table.begin(), cs[c]->table.end(), d.curves.begin() + std::ptrdiff_t(c) * kCurveSamples);
      }
      if (rp.lut && rp.p.lutAmount > 0) {
        d.lut = rp.lut;
        d.lutMix = std::clamp(rp.p.lutAmount / 100.f, 0.f, 1.f);
        d.lutId = rp.lut->contentId();
      }
    }
    f.stock = rp.p.filmStock;
    f.paper = filmRun.paper;
    f.filmEv = rp.p.filmExposure + rp.p.filmKeyEv;
    f.printEv = rp.p.filmPrintEv;
    f.black = rp.p.filmBlack;
    {
      double sx, sy, sd;
      filmInputAdaptation(rp.p, &sx, &sy, &sd);
      f.sceneX = float(sx), f.sceneY = float(sy), f.adapt = float(sd);
    }
    f.warmth = rp.p.filmWarmth;
    f.tint = rp.p.filmTint;
    f.grainPct = rp.p.filmGrain;
    f.halationPct = rp.p.filmHalation;
    f.formatScale = g.pitch() * float(std::max(g.outW, g.outH)) / float(std::max(srcW, srcH));
    f.quality = opt.spatial == SpatialQuality::Off ? 0 : (opt.spatial == SpatialQuality::Interactive ? 1 : 2);
    f.keepGrain = opt.keepGrain;
    f.coarse = opt.coarseFilm;
    f.fullLongEdge = opt.fullLongEdge;
    f.frameIndex = opt.frameIndex;
    if (!engine::filmnode::enabled()) {
      fp.supported = false;
      fp.why = "film simulation as nodes is switched off (DAPPLE_FILM_NODES=0)";
    }
  }
  return fp;
}

FrameFacts facts(const Blocks& b, const FastOptions& opt, bool sceneReferred) {
  engine::FrameFacts f;
  f.geo = b.geo;
  f.spatialScale = b.spatialScale;
  f.quality = int(opt.spatial);
  f.noise = opt.noise;
  f.keepGrain = opt.keepGrain;
  f.lookGrain = b.lookGrain;
  f.resizeSharpen = b.resizeSharpen;
  f.sceneReferred = sceneReferred;
  return f;
}

// ---- NodeBehaviour of the Geometry and Hdr nodes (resolved: Blocks) ----------------------------------------------------------------
Rect sourceWindow(const Blocks& b, const Rect& out) {
  const Geometry& g = b.geo;
  const int W = g.srcW, H = g.srcH;
  if (!g.useGeo) return Rect{std::clamp(out.x0, 0, W), std::clamp(out.y0, 0, H), std::clamp(out.x1, 0, W), std::clamp(out.y1, 0, H)};
  // the mapping is affine (quarter turns, straighten, flip, scale): the extremes are at the corner pixel centres
  float lx = 1e30f, ly = 1e30f, hx = -1e30f, hy = -1e30f;
  for (int cy : {out.y0, out.y1 - 1})
    for (int cx : {out.x0, out.x1 - 1}) {
      float sx, sy;
      mapPixel(g, cx, cy, sx, sy);
      lx = std::min(lx, sx), hx = std::max(hx, sx), ly = std::min(ly, sy), hy = std::max(hy, sy);
    }
  // the nearest pixel / the 4 x 4 Catmull-Rom taps, and a pixel of float slack; a reshaping field moves reads by up to `pad` pixels
  const int m = b.warp.active() ? 6 + b.warp.pad : (g.exact ? 2 : 4);  // Lanczos-3 reads 3 px either side
  return Rect{std::clamp(int(std::floor(lx)) - m, 0, W), std::clamp(int(std::floor(ly)) - m, 0, H), std::clamp(int(std::ceil(hx)) + m, 0, W),
              std::clamp(int(std::ceil(hy)) + m, 0, H)};
}

namespace {
const Blocks& blocksOf(const void* r) { return *static_cast<const Blocks*>(r); }
bool geometryIdentity(const void* r) { return !blocksOf(r).geo.useGeo; }
bool hdrIdentity(const void* r) { return !blocksOf(r).hdr.active(); }
Halo noHalo(const void*) { return {}; }
Rect geometryInput(const void* r, const Rect& out) { return sourceWindow(blocksOf(r), out); }
// resolvedBytes 0: the blocks are not copied into a node slot (the CPU nodes bind the frame's CpuFrame, the GPU records from GpuRun::frame)
const NodeBehaviour kGeometry{0, nullptr, geometryIdentity, noHalo, geometryInput};
const NodeBehaviour kHdr{0, nullptr, hdrIdentity, noHalo, nullptr};
}  // namespace

const NodeBehaviour& geometryBehaviour() { return kGeometry; }
const NodeBehaviour& hdrBehaviour() { return kHdr; }

}  // namespace dapple::engine::point
