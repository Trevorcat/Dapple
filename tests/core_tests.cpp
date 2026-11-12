// Dependency-free unit tests for the Dapple core. Run with ctest.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

#include "color.h"
#include "colour_metrics.h"
#include "parallel.h"
#include "sysperf.h"
#include "dapple/camera.h"
#include "dapple/dapple_c.h"
#include "dapple/decoder.h"
#include "dapple/edit_params.h"
#include "dapple/ingest.h"
#include "dapple/look.h"
#include "dapple/lut.h"
#include "dapple/renderer.h"
#include "dapple/tone.h"
#include "dapple/auto_adjust.h"
#include "dapple/xmp_preset.h"

namespace fs = std::filesystem;
using namespace dapple;

namespace {

int g_checks = 0, g_failures = 0, g_skipped = 0;

#define CHECK(cond)                                                              \
  do {                                                                           \
    ++g_checks;                                                                  \
    if (!(cond)) {                                                               \
      ++g_failures;                                                              \
      std::fprintf(stderr, "  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);     \
    }                                                                            \
  } while (0)

#define CHECK_NEAR(a, b, tol)                                                                          \
  do {                                                                                                 \
    ++g_checks;                                                                                        \
    const double va_ = (a), vb_ = (b);                                                                 \
    if (!(std::fabs(va_ - vb_) <= (tol))) {                                                            \
      ++g_failures;                                                                                    \
      std::fprintf(stderr, "  FAIL %s:%d  %s = %.6g vs %s = %.6g (tol %g)\n", __FILE__, __LINE__, #a, va_, #b, \
                   vb_, double(tol));                                                                  \
    }                                                                                                  \
  } while (0)

std::string identityCubeText(int n) {
  std::ostringstream o;
  o << "# test\nTITLE \"id\"\nLUT_3D_SIZE " << n << "\n";
  for (int b = 0; b < n; ++b)
    for (int g = 0; g < n; ++g)
      for (int r = 0; r < n; ++r) o << r / float(n - 1) << " " << g / float(n - 1) << " " << b / float(n - 1) << "\n";
  return o.str();
}

// ------------------------------------------------------------------ LUT ---
void testLutIdentity() {
  auto lut = Lut3D::parseCube(identityCubeText(5));
  CHECK(lut.has_value());
  if (!lut) return;
  CHECK(lut->size() == 5);
  CHECK(lut->title() == "id");
  std::mt19937 rng(1);
  std::uniform_real_distribution<float> u(0.f, 1.f);
  float maxErr = 0;
  for (int i = 0; i < 2000; ++i) {
    const float in[3] = {u(rng), u(rng), u(rng)};
    float out[3];
    lut->apply(in, out);
    for (int c = 0; c < 3; ++c) maxErr = std::max(maxErr, std::fabs(out[c] - in[c]));
  }
  CHECK(maxErr < 1e-5f);
  // Out-of-domain input is clamped.
  const float hi[3] = {2.f, -1.f, 0.5f};
  float out[3];
  lut->apply(hi, out);
  CHECK_NEAR(out[0], 1.0, 1e-6);
  CHECK_NEAR(out[1], 0.0, 1e-6);
  CHECK_NEAR(out[2], 0.5, 1e-6);
  // Built-in identity() agrees.
  const Lut3D id = Lut3D::identity(9);
  const float p[3] = {0.31f, 0.62f, 0.93f};
  id.apply(p, out);
  CHECK_NEAR(out[0], p[0], 1e-5);
  CHECK_NEAR(out[2], p[2], 1e-5);
  CHECK(!Lut3D::parseCube("LUT_3D_SIZE 2\n0 0 0\n").has_value());  // truncated
  CHECK(!Lut3D::parseCube("LUT_1D_SIZE 4\n").has_value());
}

void testLutTetrahedral() {
  // A random LUT: grid points must come back exactly.
  const int n = 4;
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> u(0.f, 1.f);
  std::vector<float> table;
  std::ostringstream o;
  o << "LUT_3D_SIZE " << n << "\n";
  o.precision(9);
  for (int i = 0; i < n * n * n * 3; ++i) table.push_back(u(rng));
  for (int i = 0; i < n * n * n; ++i) o << table[3 * i] << " " << table[3 * i + 1] << " " << table[3 * i + 2] << "\n";
  auto lut = Lut3D::parseCube(o.str());
  CHECK(lut.has_value());
  if (!lut) return;
  float maxErr = 0;
  for (int b = 0; b < n; ++b)
    for (int g = 0; g < n; ++g)
      for (int r = 0; r < n; ++r) {
        const float in[3] = {r / float(n - 1), g / float(n - 1), b / float(n - 1)};
        float out[3];
        lut->apply(in, out);
        const float* ref = &table[3 * (std::size_t(b) * n * n + std::size_t(g) * n + r)];
        for (int c = 0; c < 3; ++c) maxErr = std::max(maxErr, std::fabs(out[c] - ref[c]));
      }
  CHECK(maxErr < 1e-6f);

  // Tetrahedral interpolation reproduces any affine map exactly between grid points.
  std::ostringstream a;
  a.precision(9);
  a << "LUT_3D_SIZE 5\n";
  auto affine = [](const float in[3], float out[3]) {
    out[0] = 0.1f + 0.5f * in[0] + 0.2f * in[1] + 0.1f * in[2];
    out[1] = 0.05f + 0.1f * in[0] + 0.6f * in[1] + 0.2f * in[2];
    out[2] = 0.2f + 0.0f * in[0] + 0.3f * in[1] + 0.5f * in[2];
  };
  for (int b = 0; b < 5; ++b)
    for (int g = 0; g < 5; ++g)
      for (int r = 0; r < 5; ++r) {
        const float in[3] = {r / 4.f, g / 4.f, b / 4.f};
        float out[3];
        affine(in, out);
        a << out[0] << " " << out[1] << " " << out[2] << "\n";
      }
  auto alut = Lut3D::parseCube(a.str());
  CHECK(alut.has_value());
  if (!alut) return;
  float affErr = 0;
  for (int i = 0; i < 2000; ++i) {
    const float in[3] = {u(rng), u(rng), u(rng)};
    float got[3], want[3];
    alut->apply(in, got);
    affine(in, want);
    for (int c = 0; c < 3; ++c) affErr = std::max(affErr, std::fabs(got[c] - want[c]));
  }
  CHECK(affErr < 2e-6f);
}

// ------------------------------------------------------------------ XMP ---
const char* kXmpFixture = R"(<?xml version="1.0" encoding="UTF-8"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/">
 <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
  <rdf:Description rdf:about=""
    xmlns:crs="http://ns.adobe.com/camera-raw-settings/1.0/"
    crs:Exposure2012="+0.50"
    crs:Contrast2012="+12"
    crs:Highlights2012="-35"
    crs:Vibrance="+20"
    crs:Clarity2012="+15"
    crs:HueAdjustmentOrange="-10"
    crs:SaturationAdjustmentBlue="-25"
    crs:SplitToningShadowHue="220"
    crs:SplitToningShadowSaturation="30"
    crs:ParametricShadows="5">
   <crs:Name><rdf:Alt><rdf:li xml:lang="x-default">Warm Film</rdf:li></rdf:Alt></crs:Name>
   <crs:Shadows2012>+18</crs:Shadows2012>
   <crs:ToneCurvePV2012>
    <rdf:Seq>
     <rdf:li>0, 12</rdf:li>
     <rdf:li>64, 58</rdf:li>
     <rdf:li>255, 255</rdf:li>
    </rdf:Seq>
   </crs:ToneCurvePV2012>
   <crs:ToneCurvePV2012Red>
    <rdf:Seq><rdf:li>0, 0</rdf:li><rdf:li>255, 255</rdf:li></rdf:Seq>
   </crs:ToneCurvePV2012Red>
  </rdf:Description>
 </rdf:RDF>
</x:xmpmeta>)";

void testXmp() {
  auto preset = parseXmpPreset(kXmpFixture);
  CHECK(preset.has_value());
  if (!preset) return;
  const EditParams& p = preset->params;
  CHECK(preset->name == "Warm Film");
  CHECK_NEAR(p.exposure, 0.5, 1e-6);          // attribute form
  CHECK_NEAR(p.contrast, 12, 1e-6);
  CHECK_NEAR(p.highlights, -35, 1e-6);
  CHECK_NEAR(p.shadows, 18, 1e-6);            // element form
  CHECK_NEAR(p.vibrance, 20, 1e-6);
  CHECK_NEAR(p.hsl[kOrange].hue, -10, 1e-6);
  CHECK_NEAR(p.hsl[kBlue].saturation, -25, 1e-6);
  CHECK_NEAR(p.gradeShadows.hue, 220, 1e-6);
  CHECK_NEAR(p.gradeShadows.saturation, 30, 1e-6);
  CHECK_NEAR(p.parametricShadows, 5, 1e-6);
  CHECK(p.toneCurve.size() == 3);               // tone curve rdf:Seq
  if (p.toneCurve.size() == 3) {
    CHECK_NEAR(p.toneCurve[0].second, 12, 1e-6);
    CHECK_NEAR(p.toneCurve[1].first, 64, 1e-6);
    CHECK_NEAR(p.toneCurve[1].second, 58, 1e-6);
  }
  CHECK(p.toneCurveRed.empty());                // identity curve dropped
  CHECK(std::find(preset->unsupported.begin(), preset->unsupported.end(), "Clarity2012") == preset->unsupported.end());
  CHECK_NEAR(preset->params.clarity, 15, 1e-6);
  CHECK(!parseXmpPreset("<x:xmpmeta/>").has_value());
}

// ----------------------------------------------------------------- JSON ---
void testJsonRoundTrip() {
  EditParams p;
  p.temperature = 12.5f;
  p.tint = -3.25f;
  p.exposure = 0.333333f;
  p.contrast = 17;
  p.highlights = -42.125f;
  p.shadows = 21;
  p.whites = 4;
  p.blacks = -6;
  p.vibrance = 11;
  p.saturation = -7;
  p.parametricDarks = 8;
  p.parametricHighlightSplit = 80;
  p.hsl[kGreen] = {5, -20, 12};
  p.gradeShadows = {220, 30};
  p.gradeHighlights = {40, 15};
  p.gradeBalance = -10;
  p.vignette = -25;
  p.lutPath = "D:\\luts\\\"quoted\" \xE5\x9B\xBE.cube";  // backslash, quote, UTF-8
  p.lutAmount = 65;
  p.toneTarget = "low-long";
  p.toneStrength = 35;
  p.toneCurve = {{0, 5}, {128, 140.5f}, {255, 250}};
  p.toneCurveBlue = {{0, 0}, {255, 240}};

  const std::string json = toJson(p);
  auto back = fromJson(json);
  CHECK(back.has_value());
  if (!back) return;
  CHECK(toJson(*back) == json);
  CHECK_NEAR(back->exposure, p.exposure, 1e-7);
  CHECK(back->lutPath == p.lutPath);
  CHECK(back->toneCurve == p.toneCurve);
  CHECK(back->toneCurveBlue == p.toneCurveBlue);
  CHECK(back->hsl[kGreen].luminance == 12);
  CHECK(back->toneTarget == "low-long" && back->toneStrength == 35);
  CHECK_NEAR(back->parametricHighlightSplit, 80, 0);

  // Tolerance: unknown keys, missing keys, quoted numbers, wrapper object.
  auto t = fromJson(R"({"params": {"exposure": "0.5", "future": [1,2,{"a":null}], "contrast": 10}, "name": "x"})");
  CHECK(t.has_value());
  if (t) {
    CHECK_NEAR(t->exposure, 0.5 / 20, 1e-9);  // JSON exposure is in UI units (EV * 20)
    CHECK_NEAR(t->contrast, 10, 1e-9);
    CHECK_NEAR(t->lutAmount, 100, 1e-9);
  }
  auto empty = fromJson("{}");
  CHECK(empty.has_value() && toJson(*empty) == toJson(EditParams{}));
  CHECK(!fromJson("{\"exposure\": ").has_value());
  CHECK(!fromJson("[1,2]").has_value());
  CHECK(!fromJson("").has_value());
}

// -------------------------------------------------------------- Renderer ---
SourceImage greyRamp(int steps, bool scene, bool linearValues) {
  SourceImage s;
  s.sceneReferred = scene;
  s.linear = ImageF(steps, 1);
  for (int i = 0; i < steps; ++i) {
    float v = i / float(steps - 1);
    if (!linearValues) v = color::srgbDecode(v);
    for (int c = 0; c < 3; ++c) s.linear.row(0)[3 * i + c] = v;
  }
  return s;
}

void testRenderer() {
  const CameraRegistry cams = CameraRegistry::withBuiltins();
  const CameraProfile& cam = cams.resolve("Sony", "ILCE-7RM5");

  // Display-referred (camera JPEG like) source, zero params: near identity.
  {
    SourceImage src = greyRamp(256, false, false);
    // add some colour patches as extra rows' worth: use a 2D image instead
    src.linear = ImageF(16, 16);
    const float patches[4][3] = {{0.5f, 0.5f, 0.5f}, {0.8f, 0.4f, 0.3f}, {0.2f, 0.5f, 0.7f}, {0.1f, 0.1f, 0.1f}};
    for (int y = 0; y < 16; ++y)
      for (int x = 0; x < 16; ++x) {
        const float* pc = patches[(x / 4) % 4];
        float lin[3] = {color::srgbDecode(pc[0]), color::srgbDecode(pc[1]), color::srgbDecode(pc[2])};
        color::srgbToProPhoto().apply(lin, src.linear.row(y) + 3 * x);
      }
    Renderer r(EditParams{}, cam, nullptr);
    const ImageF out = r.renderFloat(src);
    double maxErr = 0;
    for (int x = 0; x < 16; ++x)
      for (int c = 0; c < 3; ++c) maxErr = std::max(maxErr, std::fabs(double(out.row(3)[3 * x + c]) - patches[(x / 4) % 4][c]));
    CHECK(maxErr < 2.0 / 255.0);
  }

  // Mid-grey JPEG exactly.
  {
    SourceImage src = greyRamp(2, false, false);
    src.linear = ImageF(1, 1);
    for (int c = 0; c < 3; ++c) src.linear.data[c] = color::srgbDecode(0.5f);
    Renderer r(EditParams{}, cam, nullptr);
    const Image8 o = r.render(src);
    CHECK(std::abs(int(o.data[0]) - 128) <= 1 && std::abs(int(o.data[1]) - 128) <= 1 && std::abs(int(o.data[2]) - 128) <= 1);
  }

  // Monotonic tone for a scene-referred ramp, with and without edits.
  auto luma = [](const ImageF& im, int x) { return 0.2126 * im.row(0)[3 * x] + 0.7152 * im.row(0)[3 * x + 1] + 0.0722 * im.row(0)[3 * x + 2]; };
  std::vector<EditParams> variants(5);
  variants[1].contrast = 60;
  variants[2].highlights = -80;
  variants[2].shadows = 80;
  variants[3].exposure = 1.5f;
  variants[3].whites = 50;
  variants[3].blacks = -50;
  variants[4].toneCurve = {{0, 20}, {100, 80}, {200, 230}, {255, 255}};
  variants[4].parametricLights = 40;
  for (std::size_t v = 0; v < variants.size(); ++v) {
    SourceImage src = greyRamp(512, true, true);
    for (float& f : src.linear.data) f *= 4.f;  // headroom above clip: values to 4.0
    Renderer r(variants[v], cam, nullptr);
    const ImageF out = r.renderFloat(src);
    bool monotone = true;
    for (int x = 1; x < out.width; ++x) monotone &= luma(out, x) + 1e-4 >= luma(out, x - 1);
    CHECK(monotone);
    CHECK(luma(out, out.width - 1) <= 1.0001);
  }
  // Exposure +1 EV brightens mid grey.
  {
    SourceImage src = greyRamp(2, true, true);
    src.linear = ImageF(1, 1);
    for (int c = 0; c < 3; ++c) src.linear.data[c] = 0.18f;
    EditParams up;
    up.exposure = 1.f;
    const float a = Renderer(EditParams{}, cam, nullptr).renderFloat(src).data[1];
    const float b = Renderer(up, cam, nullptr).renderFloat(src).data[1];
    CHECK(b > a + 0.05f);
  }
  // Calibration lock: a neutral patch at 0.18 (scene-linear) renders like the Sony
  // camera JPEG (median code ~0.72 at that exposure, measured on 50 ARWs).
  {
    SourceImage src;
    src.sceneReferred = true;
    src.linear = ImageF(1, 1);
    for (int c = 0; c < 3; ++c) src.linear.data[c] = 0.18f;
    const ImageF o = Renderer(EditParams{}, cam, nullptr).renderFloat(src);
    CHECK_NEAR(o.data[1], 0.72, 0.05);
    // The fitted camera profile corrects the camera JPEG's slightly cooler neutral axis (a ProPhoto
    // grey is ~4 b* too yellow without it), so allow a small R/B offset.
    CHECK_NEAR(o.data[0], o.data[2], 0.06);
  }
  // LUT at 0% amount is a no-op; identity LUT is a no-op.
  {
    const Lut3D id = Lut3D::identity(17);
    SourceImage src = greyRamp(64, true, true);
    EditParams withLut;
    withLut.lutAmount = 100;
    const ImageF a = Renderer(EditParams{}, cam, nullptr).renderFloat(src);
    const ImageF b = Renderer(withLut, cam, &id).renderFloat(src);
    double maxErr = 0;
    for (std::size_t i = 0; i < a.data.size(); ++i) maxErr = std::max(maxErr, std::fabs(double(a.data[i]) - b.data[i]));
    CHECK(maxErr < 1e-4);
  }
}

// A display-referred test image whose luminance is spread uniformly over [lo, hi] (encoded).
ImageF toneImage(float lo, float hi) {
  ImageF im(64, 64);
  for (int y = 0; y < 64; ++y)
    for (int x = 0; x < 64; ++x)
      for (int c = 0; c < 3; ++c) im.row(y)[3 * x + c] = lo + (hi - lo) * float(x + 64 * y) / (64.f * 64.f - 1.f);
  return im;
}

void testTone() {
  CHECK(toneClasses().size() == 9);
  CHECK(toneClassIndex("low-long") >= 0 && toneClassIndex("mid-mid") >= 0 && toneClassIndex("nope") == -1);
  for (const auto& c : toneClasses()) {
    CHECK(toneClassIndex(c.name) >= 0);
    CHECK(std::string(c.name) == std::string(c.key) + "-" + c.range);
  }
  // Every prototype classifies as itself (piecewise linear distribution through p5, p50, p95).
  for (const auto& c : toneClasses()) {
    ImageF im(100, 1);
    for (int x = 0; x < 100; ++x) {
      const float q = (x + 0.5f) / 100.f;
      float v = q < 0.05f   ? c.p5 * q / 0.05f
                : q < 0.5f  ? c.p5 + (c.p50 - c.p5) * (q - 0.05f) / 0.45f
                : q < 0.95f ? c.p50 + (c.p95 - c.p50) * (q - 0.5f) / 0.45f
                            : c.p95 + (1 - c.p95) * (q - 0.95f) / 0.05f;
      for (int ch = 0; ch < 3; ++ch) im.data[3 * x + ch] = v;
    }
    const ToneAnalysis a = analyzeToneImage(im);
    CHECK(a.nearest == c.name);
  }
  const ToneAnalysis dark = analyzeToneImage(toneImage(0.0f, 0.4f));
  CHECK(dark.key == "low" && dark.histogram.size() == 64);
  float sum = 0;
  for (float h : dark.histogram) sum += h;
  CHECK_NEAR(sum, 1.0, 1e-3);
  CHECK(dark.p5 < dark.p50 && dark.p50 < dark.p95);
  CHECK(analyzeToneImage(toneImage(0.5f, 1.0f)).key == "high");

  // Curve: endpoints fixed, monotone; strength 0 / unknown target = identity.
  const auto pts = toneTargetCurve(dark, "mid-mid", 1.f);
  CHECK(pts.size() >= 2 && pts.front() == std::make_pair(0.f, 0.f) && pts.back() == std::make_pair(255.f, 255.f));
  bool mono = true;
  for (std::size_t i = 1; i < pts.size(); ++i) mono &= pts[i].first > pts[i - 1].first && pts[i].second > pts[i - 1].second;
  CHECK(mono);
  CHECK(toneTargetCurve(dark, "mid-mid", 0.f).empty());
  CHECK(toneTargetCurve(dark, "bogus", 1.f).empty());

  // Renderer: a mid-mid target brightens a dark display-referred image; strength 0 does nothing.
  const CameraRegistry cams = CameraRegistry::withBuiltins();
  const CameraProfile& cam = cams.resolve("Sony", "ILCE-7RM5");
  SourceImage src;
  src.sceneReferred = false;
  const ImageF e = toneImage(0.05f, 0.45f);
  src.linear = ImageF(64, 64);
  for (std::size_t i = 0; i < e.data.size(); ++i) src.linear.data[i] = color::srgbDecode(e.data[i]);
  auto meanOf = [&](const EditParams& p) {
    const ImageF o = Renderer(p, cam, nullptr).renderFloat(src);
    double m = 0;
    for (float v : o.data) m += v;
    return m / double(o.data.size());
  };
  EditParams p0, p1, p2;
  p1.toneTarget = "mid-mid";
  p1.toneStrength = 100;
  p2 = p1;
  p2.toneStrength = 0;
  CHECK(meanOf(p1) > meanOf(p0) + 0.005);
  CHECK_NEAR(meanOf(p2), meanOf(p0), 1e-9);
}

void testColorMatch() {
  const CameraRegistry cams = CameraRegistry::withBuiltins();
  const CameraProfile& cam = cams.resolve("Sony", "ILCE-7RM5");
  CHECK(cam.colorMatch.hasLut() && cam.colorMatch.hasMatrix);
  CHECK(!cams.resolve("Fujifilm", "X-T50").colorMatch.enabled());
  for (int r = 0; r < 3; ++r)  // rows sum to 1: neutrals stay neutral through the matrix
    CHECK_NEAR(cam.colorMatch.matrix[3 * r] + cam.colorMatch.matrix[3 * r + 1] + cam.colorMatch.matrix[3 * r + 2], 1.0, 1e-3);
  SourceImage src;
  src.sceneReferred = true;
  src.linear = ImageF(4, 4);
  for (float& v : src.linear.data) v = 0.18f;
  const ImageF o = Renderer(EditParams{}, cam, nullptr).renderFloat(src);
  CHECK(std::fabs(o.data[0] - o.data[1]) < 0.05f && std::fabs(o.data[1] - o.data[2]) < 0.05f);
}

void testAutoDoesNoHarm() {
  const CameraRegistry cams = CameraRegistry::withBuiltins();
  const CameraProfile& cam = cams.resolve("Sony", "ILCE-7RM5");
  SourceImage src;
  src.sceneReferred = true;
  src.meta.asShotWb[0] = 2.4f;
  src.meta.asShotWb[2] = 1.8f;
  src.linear = ImageF(64, 64);
  for (int i = 0; i < 64 * 64; ++i) {
    const float v = 0.02f + 0.3f * float(i) / (64.f * 64.f);
    src.linear.data[3 * i] = v * 1.05f;  // slight warm cast on a neutral area
    src.linear.data[3 * i + 1] = v;
    src.linear.data[3 * i + 2] = v * 0.95f;
  }
  const AutoResult r = computeAutoAdjust(src, cam);
  CHECK(r.correction.temperature == 0.f && r.correction.tint == 0.f);  // plausible as-shot WB is never touched
  CHECK(r.correction.exposure == 0.f);
  CHECK(!r.tone.nearest.empty() && r.tone.histogram.size() == 64);
}

void testCameraRegistry() {
  const CameraRegistry cams = CameraRegistry::withBuiltins();
  CHECK(cams.resolve("Sony", "ILCE-7RM5").id == "sony.ilce-7rm5");
  CHECK(cams.resolve("sony", "ilce-7rm5").id == "sony.ilce-7rm5");
  CHECK(cams.resolve("Fujifilm", "X-T50").id == "fujifilm.x-t50");
  CHECK(cams.resolve("Sony", "ILCE-9M3").id == "sony.generic");
  CHECK(cams.resolve("Canon", "EOS R5").id == "generic");
  CHECK(cams.isRawExtension(".arw") && cams.isRawExtension(".raf") && !cams.isRawExtension(".jpg"));
}

void testIngest() {
  const fs::path dir = fs::temp_directory_path() / "dapple_ingest_test";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  { std::ofstream(dir / "old.arw") << "old"; }
  FolderWatchSource::Options o;
  o.settleTime = std::chrono::milliseconds(0);
  FolderWatchSource src(dir, [](const fs::path& p) { return p.extension() == ".arw"; }, o);
  { std::ofstream(dir / "new.arw") << "new data"; }
  { std::ofstream(dir / "skip.txt") << "x"; }
  int delivered = 0;
  std::string last;
  for (int i = 0; i < 3; ++i)
    src.poll([&](const IncomingPhoto& p) {
      ++delivered;
      last = p.path.filename().string();
    });
  CHECK(delivered == 1);
  CHECK(last == "new.arw");
  fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------- C API ---
std::string u8(const fs::path& p) {
  const auto s = p.u8string();
  return std::string(s.begin(), s.end());
}

void testCApiPreviews(DappleSession* s);

void testCApi() {
  CHECK(std::string(dapple_version()) == "0.1.0");
  CHECK(dapple_session_open("this/does/not/exist.ARW", DAPPLE_QUALITY_PREVIEW, 0) == nullptr);
  CHECK(std::string(dapple_last_error()).size() > 0);
  CHECK(dapple_is_supported_file("x.ARW") == 1 && dapple_is_supported_file("x.RAF") == 1 &&
        dapple_is_supported_file("x.jpg") == 1 && dapple_is_supported_file("x.txt") == 0);
  CHECK(dapple_probe_json("nope.txt") == nullptr);

  // XMP -> JSON through the C API using a temp file.
  const fs::path tmp = fs::temp_directory_path() / "dapple_capi_test";
  std::error_code ec;
  fs::create_directories(tmp, ec);
  const fs::path xmp = tmp / "preset.xmp";
  { std::ofstream(xmp, std::ios::binary) << kXmpFixture; }
  char* xj = dapple_xmp_to_json(u8(xmp).c_str());
  CHECK(xj != nullptr);
  if (xj) {
    auto parsed = fromJson(xj);
    CHECK(parsed.has_value() && std::fabs(parsed->exposure - 0.5f) < 1e-6f);
    CHECK(std::string(xj).find("\"clarity\": 15") != std::string::npos);
    dapple_free_string(xj);
  }

  // Real photo, skipped when the sample folder is absent.
  fs::path root = fs::path(std::u8string(u8"D:/图片"));
  if (const char* env = std::getenv("DAPPLE_SAMPLE_DIR")) root = fs::path(std::u8string(env, env + std::strlen(env)));
  fs::path arw;
  if (fs::is_directory(root, ec)) {
    for (const auto& e : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec)) {
      std::string ext = e.path().extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
      if (e.is_regular_file(ec) && ext == ".arw") {
        arw = e.path();
        break;
      }
    }
  }
  if (arw.empty()) {
    ++g_skipped;
    std::printf("  (no sample ARW found, skipping real-photo C API test)\n");
    fs::remove_all(tmp, ec);
    return;
  }
  const std::string arwPath = u8(arw);
  char* probe = dapple_probe_json(arwPath.c_str());
  CHECK(probe != nullptr);
  if (probe) {
    CHECK(std::string(probe).find("\"make\":\"Sony\"") != std::string::npos);
    CHECK(std::string(probe).find("sony.ilce-7rm5") != std::string::npos);
    dapple_free_string(probe);
  }
  DappleSession* s = dapple_session_open(arwPath.c_str(), DAPPLE_QUALITY_PREVIEW, 1024);
  CHECK(s != nullptr);
  if (!s) {
    std::fprintf(stderr, "  open failed: %s\n", dapple_last_error());
    return;
  }
  char* info = dapple_session_info_json(s);
  CHECK(info && std::string(info).find("ILCE-7RM5") != std::string::npos);
  dapple_free_string(info);
  char* aj = dapple_session_auto_json(s, 1.f);
  CHECK(aj && std::string(aj).find("\"correction\"") != std::string::npos && std::string(aj).find("\"stats\"") != std::string::npos);
  dapple_free_string(aj);

  char* tj = dapple_session_analyze_json(s);
  CHECK(tj != nullptr);
  if (tj) {
    const std::string js = tj;
    CHECK(js.find("\"histogram\":[") != std::string::npos && js.find("\"p50\"") != std::string::npos);
    CHECK(js.find("\"nearest\":\"") != std::string::npos && js.find("\"key\"") != std::string::npos &&
          js.find("\"range\"") != std::string::npos);
    dapple_free_string(tj);
  }
  char* tj2 = dapple_session_analyze_params_json(s, "{\"exposure\": 20}");
  CHECK(tj2 != nullptr);
  dapple_free_string(tj2);
  CHECK(dapple_session_analyze_params_json(s, "{bad") == nullptr);
  testCApiPreviews(s);
  CHECK(dapple_session_analyze_json(nullptr) == nullptr);

  DappleImage img{};
  CHECK(dapple_session_render(s, nullptr, nullptr, 0, &img) == DAPPLE_OK);
  CHECK(img.rgba != nullptr && std::max(img.width, img.height) == 1024);
  double mean = 0;
  if (img.rgba) {
    for (int i = 0; i < img.width * img.height; ++i) mean += img.rgba[4 * i + 1];
    mean /= double(img.width) * img.height;
    CHECK(img.rgba[3] == 255);
  }
  CHECK(mean > 5 && mean < 250);
  dapple_free_image(&img);
  CHECK(img.rgba == nullptr && img.width == 0);

  // Params + LUT (cached by path) + smaller output.
  const fs::path cube = tmp / "id.cube";
  { std::ofstream(cube) << identityCubeText(9); }
  const std::string cubePath = u8(cube);
  CHECK(dapple_session_render(s, "{\"exposure\": 20}", cubePath.c_str(), 256, &img) == DAPPLE_OK);
  CHECK(std::max(img.width, img.height) == 256);
  double mean2 = 0;
  if (img.rgba) {
    for (int i = 0; i < img.width * img.height; ++i) mean2 += img.rgba[4 * i + 1];
    mean2 /= double(img.width) * img.height;
  }
  CHECK(mean2 > mean);  // +1 EV is brighter
  dapple_free_image(&img);
  CHECK(dapple_session_render(s, "{not json", nullptr, 0, &img) == DAPPLE_ERROR);
  CHECK(std::string(dapple_last_error()).find("params") != std::string::npos);

  // interactive frame API: image + analysis in one call, region render
  {
    DappleFrameRequest rq{};
    rq.size = sizeof rq;
    rq.long_edge = 512;
    rq.flags = DAPPLE_FRAME_INTERACTIVE | DAPPLE_FRAME_ANALYSIS;
    DappleImage fr{};
    char* an = nullptr;
    CHECK(dapple_session_render_frame(s, "{\"exposure\": 10, \"clarity\": 30}", nullptr, &rq, &fr, &an) == DAPPLE_OK);
    CHECK(fr.rgba && std::max(fr.width, fr.height) == 512 && fr.rgba[3] == 255);
    CHECK(an && std::string(an).find("\"histogram\":[") != std::string::npos);
    dapple_free_image(&fr);
    dapple_free_string(an);
    rq.flags = 0;  // final quality, region at the lower right quarter
    rq.region[0] = 0.5f; rq.region[1] = 0.5f; rq.region[2] = 1.f; rq.region[3] = 1.f;
    rq.region_long_edge = 300;
    CHECK(dapple_session_render_frame(s, "{\"exposure\": 10}", nullptr, &rq, &fr, nullptr) == DAPPLE_OK);
    CHECK(fr.rgba && std::max(fr.width, fr.height) == 300);
    dapple_free_image(&fr);
    rq = DappleFrameRequest{};
    rq.size = sizeof rq;
    rq.flags = DAPPLE_FRAME_EXACT | DAPPLE_FRAME_ANALYSIS;
    rq.long_edge = 256;
    CHECK(dapple_session_render_frame(s, "", nullptr, &rq, &fr, &an) == DAPPLE_OK && fr.rgba && an);
    dapple_free_image(&fr);
    dapple_free_string(an);
    CHECK(dapple_session_render_frame(s, "{bad", nullptr, &rq, &fr, nullptr) == DAPPLE_ERROR);
  }

  const fs::path jpg = tmp / "out.jpg";
  CHECK(dapple_session_export_jpeg(s, "", nullptr, 512, 90, u8(jpg).c_str()) == DAPPLE_OK);
  CHECK(fs::exists(jpg, ec) && fs::file_size(jpg, ec) > 1000);
  dapple_session_close(s);
  fs::remove_all(tmp, ec);
}

// ---------------------------------------------------- full parameter set (chapter 6) ---

// The 74 numeric keys of docs/design/requirements.md 6.3, spelled out independently of the code.
std::vector<std::string> contractKeys() {
  std::vector<std::string> k = {"exposure", "contrast", "highlights", "shadows", "whites", "blacks", "temp", "tint",
                                "vibrance", "saturation", "clarity", "texture", "dehaze", "vignette", "grain",
                                "sharpen", "sharpRadius", "nrLuma", "nrColor", "gr_blend", "gr_balance", "straighten"};
  for (const char* b : {"red", "orange", "yellow", "green", "aqua", "blue", "purple", "magenta"})
    for (const char* c : {"h", "s", "l"}) k.push_back(std::string("hsl_") + b + "_" + c);
  for (const char* ch : {"rgb", "r", "g", "b"})
    for (const char* z : {"H", "L", "D", "S"}) k.push_back(std::string("cv_") + ch + "_" + z);
  for (const char* zone : {"shadows", "mid", "highlights", "global"})
    for (const char* c : {"h", "s", "l"}) k.push_back(std::string("gr_") + zone + "_" + c);
  return k;
}

struct SchemaEntry {
  std::string key;
  double lo = 0, hi = 0, def = 0;
};

std::vector<SchemaEntry> schemaEntries() {
  const std::string s = paramsSchemaJson();
  std::vector<SchemaEntry> out;
  std::size_t pos = 0;
  const std::string kKey = "{\"key\":\"";
  while ((pos = s.find(kKey, pos)) != std::string::npos) {
    SchemaEntry e;
    pos += kKey.size();
    e.key = s.substr(pos, s.find('"', pos) - pos);
    auto field = [&](const char* name) {
      const std::size_t p = s.find(name, pos);
      return std::strtod(s.c_str() + p + std::strlen(name), nullptr);
    };
    e.lo = field("\"min\":");
    e.hi = field("\"max\":");
    e.def = field("\"default\":");
    out.push_back(e);
  }
  return out;
}

// Value of `"key": number` in a toJson() string; NaN when absent.
double jsonNumber(const std::string& json, const std::string& key) {
  const std::string needle = "\"" + key + "\": ";
  const std::size_t p = json.find(needle);
  if (p == std::string::npos) return std::nan("");
  return std::strtod(json.c_str() + p + needle.size(), nullptr);
}

void testAllKeys() {
  const auto entries = schemaEntries();
  CHECK(entries.size() == 83);  // 74 contract keys + lookStrength, toneStrength, lutAmount + 6 film keys
  for (const std::string& k : contractKeys()) {
    const bool found = std::any_of(entries.begin(), entries.end(), [&](const SchemaEntry& e) { return e.key == k; });
    CHECK(found);
    if (!found) std::fprintf(stderr, "  missing contract key %s\n", k.c_str());
  }
  CHECK(contractKeys().size() == 74);

  const std::string defJson = toJson(EditParams{});
  for (const SchemaEntry& e : entries) {
    // defaults
    CHECK_NEAR(jsonNumber(defJson, e.key), e.def, 1e-6);
    // min, max, mid round trip through JSON
    for (double v : {e.lo, e.hi, (e.lo + e.hi) / 2 + 0.25 * (e.hi - e.lo) * 0.1}) {
      std::ostringstream in;
      in << "{\"" << e.key << "\": " << v << "}";
      auto p = fromJson(in.str());
      CHECK(p.has_value());
      if (!p) continue;
      CHECK_NEAR(jsonNumber(toJson(*p), e.key), v, 1e-4 * std::fabs(v) + 1e-5);
      // a single key never disturbs another one
      const std::string diff = toJsonDiff(*p);
      std::size_t keys = 0;
      for (std::size_t q = diff.find("\": "); q != std::string::npos; q = diff.find("\": ", q + 1)) ++keys;
      CHECK(keys <= 2);  // the key itself (+ cv_splits is never emitted unless changed)
    }
    // clamping
    for (double v : {e.hi * 10 + 1000, e.lo * 10 - 1000}) {
      std::ostringstream in;
      in << "{\"" << e.key << "\": " << v << "}";
      auto p = fromJson(in.str());
      CHECK(p.has_value());
      if (p) {
        const double got = jsonNumber(toJson(*p), e.key);
        CHECK(got >= e.lo - 1e-6 && got <= e.hi + 1e-6);
      }
    }
  }

  // non-numeric keys
  EditParams p;
  p.crop = "4:5";
  p.rotate = 270;
  p.flip = true;
  p.look = "teal";
  p.lookStrength = 70;
  p.hasPalette = true;
  p.paletteX = 0.2f;
  p.paletteY = 0.9f;
  p.toneTarget = "high-short";
  p.toneStrength = 55;
  p.straighten = -12.5f;
  p.exposure = 1.25f;
  p.gradeGlobal = {200, 30, -20};
  p.gradeBlend = 80;
  p.channelParametric[1][3] = 22;
  const std::string j = toJson(p);
  auto back = fromJson(j);
  CHECK(back.has_value());
  if (back) {
    CHECK(toJson(*back) == j);
    CHECK(back->crop == "4:5" && back->rotate == 270 && back->flip && back->look == "teal");
    CHECK(back->hasPalette && std::fabs(back->paletteX - 0.2f) < 1e-6f && std::fabs(back->paletteY - 0.9f) < 1e-6f);
    CHECK_NEAR(back->exposure, 1.25, 1e-6);
    CHECK_NEAR(jsonNumber(j, "exposure"), 25.0, 1e-5);  // UI units: EV * 20
    CHECK_NEAR(back->channelParametric[1][3], 22, 1e-6);
    CHECK_NEAR(jsonNumber(j, "cv_g_H"), 22, 1e-6);
    CHECK_NEAR(back->gradeGlobal.luminance, -20, 1e-6);
  }

  // palette absent unless overridden; diff of defaults is empty; diff restores the params
  CHECK(toJson(EditParams{}).find("palette") == std::string::npos);
  CHECK(toJsonDiff(EditParams{}).find("\": ") == std::string::npos);
  auto d = fromJson(toJsonDiff(p));
  CHECK(d.has_value() && toJson(*d) == j);

  // tolerance and legacy spellings
  auto t = fromJson(R"({"temp": 12, "unknown": {"x": [1, 2]}, "hsl_red_s": "-30", "temperature": 5, "exposureEv": 0.5})");
  CHECK(t.has_value());
  if (t) {
    CHECK_NEAR(t->temperature, 12, 1e-6);  // flat key wins over the legacy one
    CHECK_NEAR(t->hsl[kRed].saturation, -30, 1e-6);
    CHECK_NEAR(t->exposure, 0.5, 1e-6);
    CHECK_NEAR(t->sharpen, 40, 0);  // missing = default (not 0)
    CHECK_NEAR(t->nrColor, 25, 0);
    CHECK_NEAR(t->gradeBlend, 50, 0);
    CHECK_NEAR(t->lookStrength, 100, 0);
  }

  // range clamping of the non-numeric ones
  auto c = fromJson(R"({"crop": "7:3", "rot": 450, "flip": 1, "look": "nope", "palette": {"x": 2, "y": -1}})");
  CHECK(c.has_value());
  if (c) {
    CHECK(c->crop == "orig" && c->rotate == 90 && c->flip && c->look == "std");
    CHECK_NEAR(c->paletteX, 0.96, 1e-6);
    CHECK_NEAR(c->paletteY, 0.04, 1e-6);
  }
  auto neg = fromJson(R"({"rot": -90})");
  CHECK(neg.has_value() && neg->rotate == 270);

  // += treats defaults as "no change"
  EditParams a;
  a.sharpen = 60;
  EditParams delta;
  delta.contrast = 10;
  a += delta;
  CHECK_NEAR(a.sharpen, 60, 0);
  CHECK_NEAR(a.contrast, 10, 0);
  EditParams delta2;
  delta2.sharpen = 50;
  a += delta2;
  CHECK_NEAR(a.sharpen, 70, 0);
}

// ----------------------------------------------------------- synthetic sources ---

// Display-referred source (like a camera JPEG): `fn(x, y, rgb)` gives sRGB-encoded values.
template <class Fn>
SourceImage makeDisplay(int w, int h, Fn fn) {
  SourceImage s;
  s.sceneReferred = false;
  s.linear = ImageF(w, h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      float enc[3];
      fn(x, y, enc);
      float lin[3] = {color::srgbDecode(enc[0]), color::srgbDecode(enc[1]), color::srgbDecode(enc[2])};
      color::srgbToProPhoto().apply(lin, s.linear.row(y) + 3 * x);
    }
  return s;
}

SourceImage colourScene(int w = 96, int h = 64) {
  return makeDisplay(w, h, [&](int x, int y, float* o) {
    o[0] = 0.15f + 0.7f * float(x) / float(w - 1);
    o[1] = 0.2f + 0.6f * float(y) / float(h - 1);
    o[2] = 0.7f - 0.5f * float(x) / float(w - 1);
  });
}

double meanOf(const ImageF& im, int ch = -1) {
  double s = 0;
  std::size_t n = 0;
  for (std::size_t i = 0; i < im.data.size(); ++i) {
    if (ch >= 0 && int(i % 3) != ch) continue;
    s += im.data[i];
    ++n;
  }
  return s / double(n);
}

double maxAbsDiff(const ImageF& a, const ImageF& b) {
  if (a.width != b.width || a.height != b.height) return 1e9;
  double m = 0;
  for (std::size_t i = 0; i < a.data.size(); ++i) m = std::max(m, double(std::fabs(a.data[i] - b.data[i])));
  return m;
}

double meanChroma(const ImageF& im) {  // mean |max - min| of the encoded channels
  double s = 0;
  for (std::size_t i = 0; i + 2 < im.data.size(); i += 3)
    s += std::max({im.data[i], im.data[i + 1], im.data[i + 2]}) - std::min({im.data[i], im.data[i + 1], im.data[i + 2]});
  return s / double(im.data.size() / 3);
}

void testIdentityAtDefaults() {
  const CameraRegistry cams = CameraRegistry::withBuiltins();
  const CameraProfile& cam = cams.resolve("Sony", "ILCE-7RM5");
  const SourceImage disp = colourScene();
  const ImageF ref = Renderer(EditParams{}, cam, nullptr).renderFloat(disp);

  // Display-referred: all defaults (sharpen 40, nrColor 25 are the no-op level) change nothing,
  // whatever the style does at strength 0.
  EditParams z;
  z.look = "bw";
  z.lookStrength = 0;
  z.hasPalette = true;
  z.paletteX = 0.96f;
  z.paletteY = 0.04f;
  CHECK(maxAbsDiff(ref, Renderer(z, cam, nullptr).renderFloat(disp)) == 0.0);
  EditParams neutralGeo;
  neutralGeo.crop = "orig";
  neutralGeo.rotate = 360;  // clamps to 0
  CHECK(maxAbsDiff(ref, Renderer(neutralGeo, cam, nullptr).renderFloat(disp)) == 0.0);
  CHECK(ref.width == disp.linear.width && ref.height == disp.linear.height);

  // Scene-referred: sharpen 0 / nrColor 0 is the previous untouched render; the style at
  // strength 0 and the geometry defaults add nothing on top of that.
  SourceImage scene = disp;
  scene.sceneReferred = true;
  EditParams plain;
  plain.sharpen = 0;
  plain.nrColor = 0;
  const ImageF a = Renderer(plain, cam, nullptr).renderFloat(scene);
  EditParams withStyleOff = plain;
  withStyleOff.look = "film";
  withStyleOff.lookStrength = 0;
  CHECK(maxAbsDiff(a, Renderer(withStyleOff, cam, nullptr).renderFloat(scene)) == 0.0);
  // The default develop baseline (sharpen 40 + chroma NR 25) does act on RAW, but only slightly.
  const ImageF b = Renderer(EditParams{}, cam, nullptr).renderFloat(scene);
  CHECK(maxAbsDiff(a, b) < 0.1);
  // deterministic
  CHECK(maxAbsDiff(b, Renderer(EditParams{}, cam, nullptr).renderFloat(scene)) == 0.0);
}

void testSliderMonotonicity() {
  const CameraRegistry cams = CameraRegistry::withBuiltins();
  const CameraProfile& cam = cams.resolve("Sony", "ILCE-7RM5");
  // Horizontal grey ramp, display-referred.
  const SourceImage ramp = makeDisplay(128, 8, [](int x, int, float* o) { o[0] = o[1] = o[2] = 0.04f + 0.9f * x / 127.f; });
  auto meanWith = [&](const char* key, double v, int ch = -1) {
    std::ostringstream in;
    in << "{\"" << key << "\": " << v << "}";
    return meanOf(Renderer(*fromJson(in.str()), cam, nullptr).renderFloat(ramp), ch);
  };
  // exposure, highlights, shadows, whites, blacks: brighter with a higher value
  for (const char* k : {"exposure", "highlights", "shadows", "whites", "blacks"}) {
    const double lo = meanWith(k, -60), mid = meanWith(k, 0), hi = meanWith(k, 60);
    CHECK(lo < mid && mid < hi);
    if (!(lo < mid && mid < hi)) std::fprintf(stderr, "  not monotone: %s %.4f %.4f %.4f\n", k, lo, mid, hi);
  }
  // identity at the default value
  CHECK_NEAR(meanWith("exposure", 0), meanOf(Renderer(EditParams{}, cam, nullptr).renderFloat(ramp)), 0);
  // temperature: red up / blue down
  CHECK(meanWith("temp", -60, 0) < meanWith("temp", 0, 0) && meanWith("temp", 0, 0) < meanWith("temp", 60, 0));
  CHECK(meanWith("temp", -60, 2) > meanWith("temp", 0, 2) && meanWith("temp", 0, 2) > meanWith("temp", 60, 2));
  // tint: more magenta = less green
  CHECK(meanWith("tint", -60, 1) > meanWith("tint", 0, 1) && meanWith("tint", 0, 1) > meanWith("tint", 60, 1));
  // curves: parametric zone amounts lift, per channel too
  CHECK(meanWith("cv_rgb_L", -60) < meanWith("cv_rgb_L", 0) && meanWith("cv_rgb_L", 0) < meanWith("cv_rgb_L", 60));
  CHECK(meanWith("cv_rgb_S", -60) < meanWith("cv_rgb_S", 60));
  CHECK(meanWith("cv_r_D", -60, 0) < meanWith("cv_r_D", 60, 0));
  CHECK_NEAR(meanWith("cv_r_D", 60, 1), meanWith("cv_r_D", 0, 1), 1e-6);  // only the red channel moves
  CHECK(meanWith("cv_b_H", -60, 2) < meanWith("cv_b_H", 60, 2));

  // saturation / vibrance: more chroma
  const SourceImage col = colourScene();
  auto chroma = [&](const char* key, double v) {
    std::ostringstream in;
    in << "{\"" << key << "\": " << v << "}";
    return meanChroma(Renderer(*fromJson(in.str()), cam, nullptr).renderFloat(col));
  };
  for (const char* k : {"saturation", "vibrance"}) CHECK(chroma(k, -60) < chroma(k, 0) && chroma(k, 0) < chroma(k, 60));
  CHECK(chroma("saturation", -100) < 0.01);
  CHECK(chroma("hsl_blue_s", -100) < chroma("hsl_blue_s", 0));
  // vignette: negative darkens the corners, positive lightens them, centre untouched
  auto corner = [&](double v) {
    std::ostringstream in;
    in << "{\"vignette\": " << v << "}";
    const ImageF o = Renderer(*fromJson(in.str()), cam, nullptr).renderFloat(ramp);
    return std::pair<double, double>{o.row(0)[0], o.row(4)[3 * 64]};
  };
  CHECK(corner(-80).first < corner(0).first && corner(80).first > corner(0).first);
  CHECK_NEAR(corner(-80).second, corner(0).second, 1e-3);
  // grading: a hue + saturation in the shadows tints dark pixels, global tints everything
  {
    const ImageF base = Renderer(EditParams{}, cam, nullptr).renderFloat(ramp);
    const ImageF sh = Renderer(*fromJson(R"({"gr_shadows_h": 240, "gr_shadows_s": 80})"), cam, nullptr).renderFloat(ramp);
    const ImageF gl = Renderer(*fromJson(R"({"gr_global_h": 30, "gr_global_s": 80})"), cam, nullptr).renderFloat(ramp);
    auto bMinusR = [](const ImageF& im, int x) { return double(im.row(0)[3 * x + 2] - im.row(0)[3 * x]); };
    CHECK(bMinusR(sh, 6) > bMinusR(base, 6) + 0.01);               // dark end turns blue
    CHECK_NEAR(bMinusR(sh, 120), bMinusR(base, 120), 0.01);        // bright end is left alone
    CHECK(bMinusR(gl, 120) < bMinusR(base, 120) - 0.01 && bMinusR(gl, 6) < bMinusR(base, 6));  // warm everywhere
    const ImageF lum = Renderer(*fromJson(R"({"gr_global_l": 60})"), cam, nullptr).renderFloat(ramp);
    CHECK(meanOf(lum) > meanOf(base));
  }
}

void testLocalOperations() {
  const CameraRegistry cams = CameraRegistry::withBuiltins();
  const CameraProfile& cam = cams.resolve("Sony", "ILCE-7RM5");
  std::mt19937 rng(7);
  std::normal_distribution<float> gauss(0.f, 1.f);
  const int W = 256, H = 192;
  // texture of period p on a mid-grey with a hard step edge in the middle
  auto textured = [&](float period, float amp, float noise) {
    return makeDisplay(W, H, [&](int x, int y, float* o) {
      float v = 0.5f + amp * std::sin(6.2831853f * float(x) / period) * std::sin(6.2831853f * float(y) / period);
      if (x > W / 2) v += 0.2f;
      v += noise * gauss(rng);
      o[0] = o[1] = o[2] = std::clamp(v, 0.f, 1.f);
    });
  };
  auto render = [&](const SourceImage& s, const char* json) {
    return Renderer(*fromJson(json), cam, nullptr).renderFloat(s);
  };
  // variance of luma in the left (flat-ish) half
  auto leftVariance = [&](const ImageF& im) {
    double m = 0, v = 0;
    std::size_t n = 0;
    for (int y = 8; y < H - 8; ++y)
      for (int x = 8; x < W / 2 - 8; ++x) {
        m += im.row(y)[3 * x + 1];
        ++n;
      }
    m /= double(n);
    for (int y = 8; y < H - 8; ++y)
      for (int x = 8; x < W / 2 - 8; ++x) v += std::pow(im.row(y)[3 * x + 1] - m, 2);
    return v / double(n);
  };

  const SourceImage mid = textured(24.f, 0.05f, 0.f);   // mid-frequency pattern
  const SourceImage fine = textured(5.f, 0.04f, 0.f);   // fine pattern
  CHECK(leftVariance(render(mid, R"({"clarity": 70})")) > leftVariance(render(mid, "{}")));
  CHECK(leftVariance(render(mid, R"({"clarity": -70})")) < leftVariance(render(mid, "{}")));
  CHECK(leftVariance(render(fine, R"({"texture": 70})")) > leftVariance(render(fine, "{}")));
  CHECK(leftVariance(render(fine, R"({"texture": -70})")) < leftVariance(render(fine, "{}")));
  // sharpen above its JPEG-source baseline (40) increases fine contrast; below it nothing changes
  CHECK(leftVariance(render(fine, R"({"sharpen": 120})")) > leftVariance(render(fine, "{}")));
  CHECK_NEAR(leftVariance(render(fine, R"({"sharpen": 0})")), leftVariance(render(fine, "{}")), 0);

  // halos: a strong clarity/texture/sharpen on a step edge overshoots only a little
  {
    const SourceImage edge = textured(1e9f, 0.f, 0.f);  // flat 0.5 | 0.7 step
    for (const char* json : {R"({"clarity": 100})", R"({"texture": 100})", R"({"sharpen": 150, "sharpRadius": 30})",
                             R"({"dehaze": 100})"}) {
      const ImageF o = render(edge, json);
      float mx = 0.f, mn = 1.f;
      for (int x = 0; x < W; ++x) {
        mx = std::max(mx, o.row(H / 2)[3 * x + 1]);
        mn = std::min(mn, o.row(H / 2)[3 * x + 1]);
      }
      const ImageF ref = render(edge, "{}");
      float rmx = 0.f, rmn = 1.f;
      for (int x = 0; x < W; ++x) {
        rmx = std::max(rmx, ref.row(H / 2)[3 * x + 1]);
        rmn = std::min(rmn, ref.row(H / 2)[3 * x + 1]);
      }
      if (std::string(json).find("dehaze") == std::string::npos) {
        CHECK(mx - rmx < 0.14f && rmn - mn < 0.14f);
      }
      CHECK(mx <= 1.f && mn >= 0.f);
    }
  }

  // noise reduction removes noise, grain adds it (deterministically)
  const SourceImage noisy = textured(1e9f, 0.f, 0.03f);
  const double v0 = leftVariance(render(noisy, "{}"));
  CHECK(leftVariance(render(noisy, R"({"nrLuma": 80})")) < v0 * 0.6);
  // chroma noise: random colour noise on grey
  {
    const SourceImage cn = makeDisplay(W, H, [&](int, int, float* o) {
      o[0] = std::clamp(0.5f + 0.05f * gauss(rng), 0.f, 1.f);
      o[1] = 0.5f;
      o[2] = std::clamp(0.5f + 0.05f * gauss(rng), 0.f, 1.f);
    });
    SourceImage cs = cn;
    cs.sceneReferred = true;
    EditParams a, b;
    a.nrColor = 0;
    a.sharpen = 0;
    b.nrColor = 80;
    b.sharpen = 0;
    CHECK(meanChroma(Renderer(b, cam, nullptr).renderFloat(cs)) < meanChroma(Renderer(a, cam, nullptr).renderFloat(cs)) * 0.7);
  }
  const SourceImage flat = textured(1e9f, 0.f, 0.f);
  const ImageF g1 = render(flat, R"({"grain": 60})"), g2 = render(flat, R"({"grain": 60})");
  CHECK(maxAbsDiff(g1, g2) == 0.0);
  CHECK(leftVariance(g1) > 1e-4 && leftVariance(g1) > leftVariance(render(flat, R"({"grain": 20})")));
  CHECK_NEAR(leftVariance(render(flat, "{}")), 0, 1e-9);

  // dehaze: flat bright haze gains contrast; negative adds haze (raises black, lowers contrast)
  const SourceImage haze = makeDisplay(W, H, [&](int x, int, float* o) {
    o[0] = o[1] = o[2] = 0.55f + 0.2f * float(x) / float(W - 1);
  });
  auto spread = [&](const ImageF& im) { return double(im.row(10)[3 * (W - 1) + 1] - im.row(10)[1]); };
  CHECK(spread(render(haze, R"({"dehaze": 80})")) > spread(render(haze, "{}")));
  CHECK(spread(render(haze, R"({"dehaze": -80})")) < spread(render(haze, "{}")));
}

void testGeometry() {
  const CameraRegistry cams = CameraRegistry::withBuiltins();
  const CameraProfile& cam = cams.resolve("Sony", "ILCE-7RM5");
  const int W = 120, H = 80;
  // R grows with x, G with y
  const SourceImage src = makeDisplay(W, H, [&](int x, int y, float* o) {
    o[0] = 0.1f + 0.8f * float(x) / float(W - 1);
    o[1] = 0.1f + 0.8f * float(y) / float(H - 1);
    o[2] = 0.3f;
  });
  auto render = [&](const char* json) { return Renderer(*fromJson(json), cam, nullptr).renderFloat(src); };
  auto dims = [&](const char* json) {
    const ImageF o = render(json);
    return std::pair<int, int>{o.width, o.height};
  };
  CHECK((dims("{}") == std::pair<int, int>{120, 80}));
  CHECK((dims(R"({"crop": "1:1"})") == std::pair<int, int>{80, 80}));
  CHECK((dims(R"({"crop": "4:5"})") == std::pair<int, int>{64, 80}));
  CHECK((dims(R"({"crop": "3:4"})") == std::pair<int, int>{60, 80}));
  CHECK((dims(R"({"crop": "9:16"})") == std::pair<int, int>{45, 80}));
  CHECK((dims(R"({"crop": "16:9"})") == std::pair<int, int>{120, 68}));
  CHECK((dims(R"({"rot": 90})") == std::pair<int, int>{80, 120}));
  CHECK((dims(R"({"rot": 180})") == std::pair<int, int>{120, 80}));
  CHECK((dims(R"({"rot": 270, "crop": "1:1"})") == std::pair<int, int>{80, 80}));
  CHECK((dims(R"({"flip": true})") == std::pair<int, int>{120, 80}));

  // flip mirrors horizontally; rot 90 turns clockwise; centre crop keeps the centre
  {
    const ImageF o = render("{}"), f = render(R"({"flip": true})");
    CHECK_NEAR(f.row(10)[3 * 5], o.row(10)[3 * (W - 1 - 5)], 1e-6);
    CHECK_NEAR(f.row(10)[3 * 5 + 1], o.row(10)[3 * (W - 1 - 5) + 1], 1e-6);
    const ImageF r = render(R"({"rot": 90})");  // 80 x 120
    // source top-left -> top-right, source top-right -> bottom-right
    CHECK_NEAR(r.row(0)[3 * 79], o.row(0)[0], 1e-6);
    CHECK_NEAR(r.row(119)[3 * 79], o.row(0)[3 * (W - 1)], 1e-6);
    CHECK_NEAR(r.row(0)[3 * 0], o.row(H - 1)[0], 1e-6);
    const ImageF r4 = render(R"({"rot": 90})"), r5 = render(R"({"rot": 450})");
    CHECK(maxAbsDiff(r4, r5) == 0.0);
    const ImageF c = render(R"({"crop": "1:1"})");
    CHECK_NEAR(c.row(40)[3 * 40], o.row(40)[3 * 60], 1e-6);
    const ImageF r2 = render(R"({"rot": 180})");
    CHECK_NEAR(r2.row(0)[0], o.row(H - 1)[3 * (W - 1)], 1e-6);
  }

  // straighten: auto-fill crop never shows the border and keeps the aspect ratio
  {
    SourceImage framed = makeDisplay(200, 120, [&](int x, int y, float* o) {
      const bool border = x < 1 || y < 1 || x >= 199 || y >= 119;
      o[0] = o[1] = o[2] = border ? 0.f : 0.6f;
    });
    for (const char* json : {R"({"straighten": 15})", R"({"straighten": -30})", R"({"straighten": 45, "crop": "1:1"})",
                             R"({"straighten": 10, "rot": 90})", R"({"straighten": 12, "flip": true, "crop": "16:9"})"}) {
      const ImageF o = Renderer(*fromJson(json), cam, nullptr).renderFloat(framed);
      float mn = 1.f;
      for (float v : o.data) mn = std::min(mn, v);
      CHECK(mn > 0.35f);
      if (mn <= 0.35f) std::fprintf(stderr, "  border visible with %s (min %.3f)\n", json, mn);
      CHECK(o.width <= 200 && o.height <= 200 && o.width > 20 && o.height > 20);
    }
    const ImageF o = Renderer(*fromJson(R"({"straighten": 15})"), cam, nullptr).renderFloat(framed);
    CHECK_NEAR(double(o.width) / o.height, 200.0 / 120.0, 0.05);
    // sign: opposite angles give mirrored results, 0 is the identity
    const ImageF a = render(R"({"straighten": 8})"), b = render(R"({"straighten": -8})");
    CHECK(a.width == b.width && a.height == b.height && maxAbsDiff(a, b) > 0.01);
    const ImageF z = render(R"({"straighten": 0.0})");
    CHECK(maxAbsDiff(z, render("{}")) == 0.0);
  }
}

void testStyles() {
  const CameraRegistry cams = CameraRegistry::withBuiltins();
  const CameraProfile& cam = cams.resolve("Sony", "ILCE-7RM5");
  const SourceImage col = colourScene();
  auto render = [&](const std::string& json) { return Renderer(*fromJson(json), cam, nullptr).renderFloat(col); };
  const ImageF base = render("{}");
  CHECK(looks().size() == 9);
  const char* ids[] = {"std", "warm", "amber", "rose", "neutral", "cool", "teal", "film", "bw"};
  for (int i = 0; i < 9; ++i) {
    CHECK(lookIndex(ids[i]) == i);
    const ImageF o = render(std::string("{\"look\": \"") + ids[i] + "\"}");
    const double d = maxAbsDiff(base, o);
    if (i == 0) CHECK(d == 0.0);
    else CHECK(d > 0.005);
    // strength 0 removes the style entirely, 50 sits between
    CHECK(maxAbsDiff(base, render(std::string("{\"look\": \"") + ids[i] + "\", \"lookStrength\": 0}")) == 0.0);
    const double half = maxAbsDiff(base, render(std::string("{\"look\": \"") + ids[i] + "\", \"lookStrength\": 50}"));
    CHECK(i == 0 || (half > 0.0 && half < d));
    // everything stays in gamut
    for (float v : o.data) CHECK(v >= 0.f && v <= 1.f);
  }
  CHECK(lookIndex("nope") == -1);
  CHECK(looksJson().find("\"id\":\"teal\"") != std::string::npos);
  // black & white is neutral; warm/cool move the red-blue balance in opposite directions
  {
    const ImageF bw = render(R"({"look": "bw"})");
    float spread = 0.f;
    for (std::size_t i = 0; i + 2 < bw.data.size(); i += 3)
      spread = std::max(spread, std::fabs(bw.data[i] - bw.data[i + 1]) + std::fabs(bw.data[i + 2] - bw.data[i + 1]));
    CHECK(spread < 0.02f);
    const double rb = meanOf(base, 0) - meanOf(base, 2);
    CHECK(meanOf(render(R"({"look": "warm"})"), 0) - meanOf(render(R"({"look": "warm"})"), 2) > rb);
    CHECK(meanOf(render(R"({"look": "cool"})"), 0) - meanOf(render(R"({"look": "cool"})"), 2) < rb);
    CHECK(meanChroma(render(R"({"look": "neutral"})")) < meanChroma(base));
  }
  // pad: x scales colour richness, y moves warm <-> cool; inactive at strength 0
  {
    const double pale = meanChroma(render(R"({"palette": {"x": 0.04, "y": 0.5}})"));
    const double rich = meanChroma(render(R"({"palette": {"x": 0.96, "y": 0.5}})"));
    CHECK(pale < meanChroma(base) && meanChroma(base) < rich);
    auto rb = [&](const char* json) { return meanOf(render(json), 0) - meanOf(render(json), 2); };
    CHECK(rb(R"({"palette": {"x": 0.5, "y": 0.04}})") > rb("{}"));
    CHECK(rb(R"({"palette": {"x": 0.5, "y": 0.96}})") < rb("{}"));
    CHECK(maxAbsDiff(base, render(R"({"lookStrength": 0, "palette": {"x": 0.96, "y": 0.04}})")) == 0.0);
    // the pad is bounded: values outside are clamped, so these two are the same picture
    CHECK(maxAbsDiff(render(R"({"palette": {"x": 5, "y": -5}})"), render(R"({"palette": {"x": 0.96, "y": 0.04}})")) == 0.0);
  }
  // film: lifted blacks (fade) and grain
  {
    const SourceImage dark = makeDisplay(64, 64, [](int, int, float* o) { o[0] = o[1] = o[2] = 0.f; });
    const ImageF f = Renderer(*fromJson(R"({"look": "film"})"), cam, nullptr).renderFloat(dark);
    CHECK(meanOf(f) > 0.02);
  }
}

void testCApiPreviews(DappleSession* s) {
  char* lj = dapple_looks_json();
  CHECK(lj != nullptr);
  if (lj) {
    for (const char* id : {"std", "warm", "amber", "rose", "neutral", "cool", "teal", "film", "bw"})
      CHECK(std::string(lj).find(std::string("\"id\":\"") + id + "\"") != std::string::npos);
    dapple_free_string(lj);
  }
  char* sj = dapple_params_schema_json();
  CHECK(sj && std::string(sj).find("\"key\":\"straighten\"") != std::string::npos);
  dapple_free_string(sj);

  DappleImage looksOut[9];
  CHECK(dapple_session_render_look_previews(s, "{\"look\":\"bw\"}", 96, looksOut) == DAPPLE_OK);
  for (auto& im : looksOut) {
    CHECK(im.rgba != nullptr && std::max(im.width, im.height) == 96);
  }
  if (looksOut[0].rgba && looksOut[8].rgba) {
    CHECK(std::memcmp(looksOut[0].rgba, looksOut[8].rgba, std::size_t(looksOut[0].width) * looksOut[0].height * 4) != 0);
  }
  for (auto& im : looksOut) dapple_free_image(&im);

  DappleImage tone[9];
  CHECK(dapple_session_render_tone_previews(s, nullptr, 96, -1.f, tone) == DAPPLE_OK);
  for (auto& im : tone) CHECK(im.rgba != nullptr && std::max(im.width, im.height) == 96);
  if (tone[0].rgba && tone[8].rgba) {  // low-short is darker than high-long
    double lo = 0, hi = 0;
    const int n = tone[0].width * tone[0].height;
    for (int i = 0; i < n; ++i) {
      lo += tone[0].rgba[4 * i + 1];
      hi += tone[8].rgba[4 * i + 1];
    }
    CHECK(lo < hi);
  }
  for (auto& im : tone) dapple_free_image(&im);
  CHECK(dapple_session_render_tone_previews(nullptr, nullptr, 96, -1.f, tone) == DAPPLE_ERROR);

  // cropping through the C API changes the output size
  DappleImage img{};
  CHECK(dapple_session_render(s, "{\"crop\": \"1:1\", \"straighten\": 5}", nullptr, 400, &img) == DAPPLE_OK);
  CHECK(img.rgba && img.width == img.height);
  dapple_free_image(&img);
}


// -------------------------------------------------- baked (lattice) path vs direct path ---

// Scene-referred test image covering many stops and saturated colours (deterministic).
SourceImage bakeScene(int w, int h) {
  SourceImage s;
  s.sceneReferred = true;
  s.linear = ImageF(w, h);
  std::mt19937 rng(12345);
  std::uniform_real_distribution<float> U(0.f, 1.f);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      float* p = s.linear.row(y) + 3 * x;
      const float level = std::exp2(-9.f + 9.5f * (0.5f * U(rng) + 0.5f * float(x) / float(w)));  // 2^-9 .. 2^0.5
      float c[3] = {U(rng), U(rng), U(rng)};
      if (U(rng) < 0.25f) c[int(U(rng) * 3) % 3] = 0.f;  // fully saturated colours
      const float luma = 0.288f * c[0] + 0.712f * c[1] + 0.0001f;
      for (int k = 0; k < 3; ++k) p[k] = std::min(1.2f, level * c[k] / luma * 0.6f);
    }
  return s;
}

struct DeStats {
  double mean = 0, p99 = 0, max = 0;
};

DeStats deltaE(const ImageF& a, const ImageF& b) {
  std::vector<double> de;
  for (std::size_t i = 0; i + 2 < a.data.size(); i += 3) {
    const auto la = metrics::srgbToLab(&a.data[i]);
    const auto lb = metrics::srgbToLab(&b.data[i]);
    de.push_back(metrics::deltaE2000(la, lb));
  }
  std::sort(de.begin(), de.end());
  DeStats r;
  for (double d : de) r.mean += d;
  r.mean /= double(de.size());
  r.p99 = de[std::size_t(double(de.size()) * 0.99)];
  r.max = de.back();
  return r;
}

void testBakedPath() {
  const CameraRegistry cams = CameraRegistry::withBuiltins();
  const CameraProfile& cam = cams.resolve("Sony", "ILCE-7RM5");
  const SourceImage scene = bakeScene(320, 240);
  const SourceImage disp = colourScene(160, 120);

  std::vector<std::pair<const char*, EditParams>> cases;
  cases.push_back({"defaults", EditParams{}});
  {
    EditParams p;
    p.exposure = 0.5f;
    p.contrast = 20;
    p.shadows = 35;
    p.highlights = -40;
    p.toneStrength = 60;
    cases.push_back({"auto-like", p});
  }
  {
    EditParams p;
    p.temperature = 30;
    p.tint = -10;
    p.vibrance = 40;
    p.saturation = 20;
    p.hsl[kOrange].saturation = 30;
    p.hsl[kBlue].hue = -30;
    p.hsl[kGreen].luminance = 25;
    p.gradeShadows = {220, 40, -5};
    p.gradeHighlights = {40, 30, 5};
    p.gradeGlobal = {10, 10, 0};
    p.look = "teal";
    p.hasPalette = true;
    p.paletteX = 0.8f;
    p.paletteY = 0.3f;
    p.toneCurve = {{0, 0}, {64, 50}, {192, 210}, {255, 255}};
    p.toneCurveBlue = {{0, 10}, {255, 245}};
    p.parametricLights = 30;
    cases.push_back({"heavy colour", p});
  }
  {
    EditParams p;
    p.look = "film";
    p.exposure = -1.f;
    p.whites = 30;
    p.blacks = -30;
    cases.push_back({"film look", p});
  }
  {
    EditParams p;
    p.look = "bw";
    p.vignette = -40;
    cases.push_back({"bw + vignette", p});
  }

  for (const SourceImage* src : {&scene, &disp})
    for (const auto& [name, p] : cases) {
      Renderer r(p, cam, nullptr);
      const ImageF ref = r.renderFloat(*src, false);
      for (int n : {41, 65}) {
        const auto lut = r.bake(*src, n);
        FastOptions fo;
        fo.spatial = SpatialQuality::Off;
        const ImageF got = r.renderBakedFloat(*src, *lut, fo);
        CHECK(got.width == ref.width && got.height == ref.height);
        const DeStats d = deltaE(ref, got);
        if (d.mean >= 0.5 || (n == 65 && d.mean >= 0.3))
          std::fprintf(stderr, "  %s n=%d (%s): mean dE %.3f p99 %.2f max %.2f\n", name, n, src == &scene ? "scene" : "display", d.mean, d.p99, d.max);
        CHECK(d.mean < 0.5);
        if (n == 65) CHECK(d.mean < 0.3);
      }
    }

  // geometry + vignette + cropping through the baked path match the direct path
  {
    EditParams p;
    p.crop = "1:1";
    p.straighten = 4.f;
    p.vignette = -50.f;
    p.look = "warm";
    Renderer r(p, cam, nullptr);
    const ImageF ref = r.renderFloat(scene, false);
    const auto lut = r.bake(scene, 65);
    FastOptions fo;
    fo.spatial = SpatialQuality::Off;
    const ImageF got = r.renderBakedFloat(scene, *lut, fo);
    CHECK(got.width == ref.width && got.height == ref.height);
    CHECK(deltaE(ref, got).mean < 0.5);
  }

  // region render: the full region at natural scale equals the plain render; half the width halves the size
  {
    EditParams p;
    p.look = "warm";
    Renderer r(p, cam, nullptr);
    const auto lut = r.bake(scene, 33);
    FastOptions fo;
    fo.spatial = SpatialQuality::Off;
    const ImageF whole = r.renderBakedFloat(scene, *lut, fo);
    fo.region.outLongEdge = std::max(whole.width, whole.height);
    fo.region.x1 = 1.f;
    fo.region.y1 = 1.f;
    fo.region.x0 = 1e-3f;  // forces the region path
    const ImageF reg = r.renderBakedFloat(scene, *lut, fo);
    CHECK(std::abs(reg.width - whole.width) <= 1 && std::abs(reg.height - whole.height) <= 1);
    FastOptions half;
    half.spatial = SpatialQuality::Off;
    half.region = {0.f, 0.f, 0.5f, 1.f, 0};
    int w = 0, h = 0;
    r.fastSize(scene, half.region, w, h);
    CHECK(w == whole.width / 2 && h == whole.height);
    const ImageF left = r.renderBakedFloat(scene, *lut, half);
    CHECK(left.width == w && left.height == h);
    // pixels of the left half agree with the plain render (cubic sampling at 1:1 vs exact copy)
    double md = 0;
    for (int y = 4; y < h - 4; ++y)
      for (int x = 4; x < w - 4; ++x)
        for (int c = 0; c < 3; ++c) md += std::fabs(left.row(y)[3 * x + c] - whole.row(y)[3 * x + c]);
    md /= double((h - 8) * (w - 8) * 3);
    CHECK(md < 0.02);
  }

  // C API frame
  {
    CHECK(dapple_session_render_frame(nullptr, "", nullptr, nullptr, nullptr, nullptr) == DAPPLE_ERROR);
    DappleImage none{};
    CHECK(dapple_session_render_frame(nullptr, "", nullptr, nullptr, &none, nullptr) == DAPPLE_ERROR);
  }
}

// big.LITTLE classification and the interactive pool's cpu set; the pool itself (spin-then-block) must give
// the same result as a serial loop for many back-to-back small jobs.
void testSchedulingPolicy() {
  using dapple::sys::classify;
  using dapple::sys::poolCpus;
  // Snapdragon 680: 4x A73 (cap 1024) + 4x A53 (~400) -> pool on the 4 big cores
  auto sd680 = classify({400, 400, 400, 400, 1024, 1024, 1024, 1024});
  CHECK(sd680.hetero() && sd680.perf.size() == 4 && sd680.efficient.size() == 4);
  CHECK(poolCpus(sd680) == std::vector<int>({4, 5, 6, 7}));
  // Helio G85: 2x A75 + 6x A55 -> too few big cores: every cpu takes part
  auto g85 = classify({430, 430, 430, 430, 430, 430, 1024, 1024});
  CHECK(g85.hetero() && g85.perf.size() == 2);
  CHECK(poolCpus(g85).size() == 8);
  // 8 Elite (2 prime + 6 performance, all >= 60% of the prime): homogeneous for our purpose, no pinning
  auto elite = classify({870, 870, 870, 870, 870, 870, 1024, 1024});
  CHECK(!elite.hetero() && poolCpus(elite).size() == 8);
  // unknown capacities (emulator, desktop): no split
  auto unk = classify({-1, -1, -1, -1});
  CHECK(!unk.hetero() && poolCpus(unk).size() == 4);
  CHECK(classify({}).total == 0);
  // many tiny jobs back to back (exercises the spin / wake hand-off)
  std::vector<int> v(997, 0);
  long long want = 0;
  for (int rep = 0; rep < 300; ++rep) {
    dapple::parallelFor(int(v.size()), [&](int i) { v[std::size_t(i)] += i % 7; }, 3);
    for (std::size_t i = 0; i < v.size(); ++i) want += int(i) % 7;
  }
  long long got = 0;
  for (int x : v) got += x;
  CHECK(got == want);
}

}  // namespace

int main() {
  struct T {
    const char* name;
    void (*fn)();
  };
  const T tests[] = {{"lut identity", testLutIdentity}, {"lut tetrahedral", testLutTetrahedral},
                     {"xmp", testXmp},                  {"json round trip", testJsonRoundTrip},
                     {"renderer", testRenderer},        {"camera registry", testCameraRegistry},
                     {"tone classes", testTone}, {"colour match", testColorMatch}, {"auto no harm", testAutoDoesNoHarm},
                     {"ingest", testIngest},            {"c api", testCApi},
                     {"all keys", testAllKeys},         {"identity at defaults", testIdentityAtDefaults},
                     {"slider monotonicity", testSliderMonotonicity}, {"local operations", testLocalOperations},
                     {"geometry", testGeometry},        {"styles", testStyles},
                     {"baked path", testBakedPath},  {"scheduling policy", testSchedulingPolicy}};
  for (const T& t : tests) {
    const int before = g_failures;
    t.fn();
    std::printf("[%s] %s\n", g_failures == before ? " ok " : "FAIL", t.name);
  }
  std::printf("%d checks, %d failures, %d skipped\n", g_checks, g_failures, g_skipped);
  return g_failures ? 1 : 0;
}
