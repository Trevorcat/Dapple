// AI masks service, the parts that need no model (docs/engine/pipeline-architecture.md section 41). The C API is compiled in (as local_capi_tests).
//   dapple_ai_masks_tests <id>      ids: polygon faces parts taps depth cache ensure export sky status
//     polygon  rasteriser: area, even-odd ring, soft edge conserves coverage
//     faces    face features from a fixed synthetic 478-point mesh: oval, eyes / iris / sclera, lips ring, teeth only for an open mouth with bright
//              pixels, nothing but the oval for a turned head
//     parts    person part planes from a fixed class map (complement, union, 'others')
//     taps     tap accumulation (add / subtract), tap spec and plane id grammar
//     depth    depth far plane (near 0 .. far 255) and the raster-with-range component (lo, hi, feather): JSON, hash, evaluation
//     cache    cache file round trip, corruption, pruning
//     ensure   re-creation of missing planes from the disk cache; reasons when it cannot
//     export   an export whose edit needs a missing AI plane: restored from the cache = byte identical, not restorable = export fails (nothing written)
//     sky      the classical prior: a synthetic sky is found, a grey wall is not
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "ai_masks.h"
#include "dapple/dapple_c.h"
#include "dapple/edit_params.h"
#include "dapple/local_adjust.h"
#include "json_value.h"
#include "local_eval.h"

namespace fs = std::filesystem;
using namespace dapple;
using namespace dapple::aimask;

static int g_failures = 0;
#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("  CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                         \
    }                                                                       \
  } while (0)
#define CHECK_NEAR(a, b, tol)                                                                                                          \
  do {                                                                                                                                 \
    const double va = double(a), vb = double(b);                                                                                       \
    if (!(std::fabs(va - vb) <= double(tol))) {                                                                                        \
      std::printf("  CHECK_NEAR FAILED %s:%d: %s = %.4f vs %s = %.4f (tol %.4f)\n", __FILE__, __LINE__, #a, va, #b, vb, double(tol));      \
      ++g_failures;                                                                                                                    \
    }                                                                                                                                  \
  } while (0)

static double shoelace(const std::vector<Pt>& r) {
  double a = 0;
  for (std::size_t i = 0; i < r.size(); ++i) {
    const Pt &p = r[i], &q = r[(i + 1) % r.size()];
    a += double(p.x) * q.y - double(q.x) * p.y;
  }
  return std::fabs(a) * 0.5;
}
static std::vector<Pt> ringOf(const FaceMesh& m, const int* idx, int n) {
  std::vector<Pt> r;
  for (int i = 0; i < n; ++i) r.push_back({m.xy[idx[i]][0], m.xy[idx[i]][1]});
  return r;
}

// ---- polygon -------------------------------------------------------------------------------------------------------------------------------
static void testPolygon() {
  std::vector<Pt> sq = {{5.5f, 5.5f}, {15.5f, 5.5f}, {15.5f, 15.5f}, {5.5f, 15.5f}};
  Plane p = polygonPlane(40, 40, {sq}, 0.f);
  CHECK_NEAR(planeSum(p), 100.0, 1.0);
  CHECK(p.px[10 * 40 + 10] == 255 && p.px[2 * 40 + 2] == 0);
  std::vector<Pt> tri = {{4.f, 4.f}, {30.f, 6.f}, {12.f, 33.f}};
  Plane t = polygonPlane(40, 40, {tri}, 0.f);
  CHECK_NEAR(planeSum(t), shoelace(tri), 0.02 * shoelace(tri));
  Plane soft = polygonPlane(40, 40, {sq}, 1.5f);
  CHECK_NEAR(planeSum(soft), 100.0, 2.0);       // the softening moves coverage, it does not make or lose any
  CHECK(soft.px[10 * 40 + 10] > 240 && soft.px[5 * 40 + 5] > 20 && soft.px[5 * 40 + 5] < 200);
  std::vector<Pt> big = {{4.f, 4.f}, {24.f, 4.f}, {24.f, 24.f}, {4.f, 24.f}}, small = {{9.f, 9.f}, {19.f, 9.f}, {19.f, 19.f}, {9.f, 19.f}};
  Plane ring = polygonPlane(40, 40, {big, small}, 0.f);
  CHECK_NEAR(planeSum(ring), 400.0 - 100.0, 2.0);  // even-odd: the inner ring is a hole
  CHECK(ring.px[14 * 40 + 14] == 0 && ring.px[6 * 40 + 6] == 255);
  Plane outside = polygonPlane(40, 40, {{{-50.f, -50.f}, {-10.f, -50.f}, {-10.f, -10.f}}}, 2.f);  // entirely off the plane: no crash, empty
  CHECK(planeSum(outside) == 0.0);
}

// ---- faces ------------------------------------------------------------------------------------------------------------------------------------
static void ell(FaceMesh& m, const int* idx, int n, float cx, float cy, float rx, float ry) {
  for (int k = 0; k < n; ++k) {
    const float a = -1.5707963f + 6.2831853f * float(k) / float(n);
    m.xy[idx[k]][0] = cx + rx * std::cos(a), m.xy[idx[k]][1] = cy + ry * std::sin(a);
  }
}
static FaceMesh syntheticMesh(float mouthRy, float yawZ) {
  FaceMesh m;
  ell(m, kFaceOval, 36, 200, 240, 120, 160);
  ell(m, kEyeRight, 16, 150, 200, 28, 9);
  ell(m, kEyeLeft, 16, 250, 200, 28, 9);
  ell(m, kIrisRight, 4, 150, 200, 9, 9);
  ell(m, kIrisLeft, 4, 250, 200, 9, 9);
  ell(m, kLipsOuter, 20, 200, 330, 50, 22);
  ell(m, kLipsInner, 20, 200, 330, 44, mouthRy);
  for (int i = 0; i < 5; ++i) {
    const float t = float(i) / 4.f, y = 170.f - 8.f * std::sin(3.14159f * t);
    m.xy[kBrowRightUpper[i]][0] = 105 + 70 * t, m.xy[kBrowRightUpper[i]][1] = y;
    m.xy[kBrowRightLower[i]][0] = 105 + 70 * t, m.xy[kBrowRightLower[i]][1] = y + 9;
    m.xy[kBrowLeftUpper[i]][0] = 295 - 70 * t, m.xy[kBrowLeftUpper[i]][1] = y;
    m.xy[kBrowLeftLower[i]][0] = 295 - 70 * t, m.xy[kBrowLeftLower[i]][1] = y + 9;
  }
  m.z[454] = yawZ;  // depth difference between the two face edges: 0 = frontal
  return m;
}
static std::vector<std::uint8_t> flat(int w, int h, int r, int g, int b) {
  std::vector<std::uint8_t> v(std::size_t(w) * h * 3);
  for (std::size_t i = 0; i < std::size_t(w) * h; ++i) v[3 * i] = std::uint8_t(r), v[3 * i + 1] = std::uint8_t(g), v[3 * i + 2] = std::uint8_t(b);
  return v;
}
static void testFaces() {
  const int W = 400, H = 480;
  {  // frontal, mouth closed
    const FaceMesh m = syntheticMesh(1.f, 0.f);
    FaceFeatures f;
    CHECK(faceFeatures(m, W, H, nullptr, 0, f));
    CHECK(f.frontal && !f.open && f.teeth.empty());
    CHECK_NEAR(planeSum(f.face), shoelace(ringOf(m, kFaceOval, 36)), 0.02 * shoelace(ringOf(m, kFaceOval, 36)));
    const double eyeArea = shoelace(ringOf(m, kEyeRight, 16)) + shoelace(ringOf(m, kEyeLeft, 16));
    CHECK_NEAR(planeSum(f.eyes), eyeArea, 0.04 * eyeArea);
    CHECK(f.eyes.px[200 * W + 150] > 250 && f.eyes.px[200 * W + 200] == 0);                      // eye centre in, between the eyes out
    CHECK(f.iris.px[200 * W + 150] > 250 && f.sclera.px[200 * W + 150] < 5);                       // iris centre is iris, not white
    CHECK(f.sclera.px[200 * W + 150 + 22] > 200 && f.iris.px[200 * W + 150 + 22] < 40);          // beside the iris: white
    CHECK_NEAR(planeSum(f.iris) + planeSum(f.sclera), planeSum(f.eyes), 0.03 * planeSum(f.eyes));
    const double lipArea = shoelace(ringOf(m, kLipsOuter, 20)) - shoelace(ringOf(m, kLipsInner, 20));
    CHECK_NEAR(planeSum(f.lips), lipArea, 0.06 * lipArea);
    CHECK(f.lips.px[330 * W + 200 + 47] > 200 || f.lips.px[330 * W + 200 - 47] > 200);          // the lip body
    CHECK(f.brows.px[166 * W + 140] > 200 && f.brows.px[166 * W + 200] == 0);
    CHECK(planeSum(f.brows) > 1.5 * 2 * 70 * 9);  // the band is grown to brow width: clearly more than the raw 70 x 9 px edges
  }
  {  // open mouth: teeth are the inner polygon (no pixels given), or the bright pixels of it
    const FaceMesh m = syntheticMesh(14.f, 0.f);
    FaceFeatures f;
    CHECK(faceFeatures(m, W, H, nullptr, 0, f));
    CHECK(f.open && f.mouthOpen > kMouthOpen);
    const double inner = shoelace(ringOf(m, kLipsInner, 20));
    CHECK_NEAR(planeSum(f.teeth), inner, 0.05 * inner);
    CHECK(f.teeth.px[330 * W + 200] > 250 && f.lips.px[330 * W + 200] == 0);                    // the opening is no lip
    const auto white = flat(W, H, 240, 238, 235), dark = flat(W, H, 40, 12, 14);
    FaceFeatures fw, fd;
    CHECK(faceFeatures(m, W, H, white.data(), W * 3, fw) && fw.open && planeSum(fw.teeth) > 0.8 * inner);
    CHECK(faceFeatures(m, W, H, dark.data(), W * 3, fd) && !fd.open && fd.teeth.empty());     // a dark opening shows no teeth
    auto pink = flat(W, H, 200, 80, 90);                                                            // tongue-coloured interior
    FaceFeatures fp;
    CHECK(faceFeatures(m, W, H, pink.data(), W * 3, fp) && !fp.open);
  }
  {  // a turned head: the oval only
    const FaceMesh m = syntheticMesh(14.f, 200.f);  // dz 200 over a 240 px width: about 56 degrees
    FaceFeatures f;
    CHECK(faceFeatures(m, W, H, nullptr, 0, f));
    CHECK(!f.frontal && f.yawDeg > 40.f);
    CHECK(!f.face.empty() && f.eyes.empty() && f.lips.empty() && f.teeth.empty() && f.brows.empty());
  }
  {  // degenerate input
    FaceMesh m;
    FaceFeatures f;
    CHECK(!faceFeatures(m, W, H, nullptr, 0, f));  // all points at the origin: face width 0
  }
  // topology sanity: the index sets are inside the mesh and the rings have no repeated points
  for (int i : kFaceOval) CHECK(i >= 0 && i < 478);
  CHECK(kIrisRight[0] == 469 && kIrisLeft[0] == 474 && kEyeRight[1] == 33 && kLipsInner[0] == 13 && kLipsInner[10] == 14 && kLipsInner[5] == 78 && kLipsInner[15] == 308);
}

// ---- parts -------------------------------------------------------------------------------------------------------------------------------------
static void testParts() {
  const int S = 64;
  std::vector<std::uint8_t> cm(std::size_t(S) * S, kPartBackground);
  auto fill = [&](int x0, int y0, int x1, int y1, int c) {
    for (int y = y0; y < y1; ++y)
      for (int x = x0; x < x1; ++x) cm[std::size_t(y) * S + x] = std::uint8_t(c);
  };
  fill(16, 8, 48, 20, kPartHair);
  fill(20, 20, 44, 36, kPartFaceSkin);
  fill(16, 40, 48, 60, kPartClothes);
  fill(8, 40, 16, 60, kPartBodySkin);
  fill(48, 40, 56, 60, kPartOthers);
  const std::vector<std::uint8_t> prob = probFromClassMap(cm, S);
  const int W = 128, H = 128;
  const PartPlanes p = splitParts(prob.data(), S, W, H);
  auto at = [&](const Plane& pl, int x64, int y64) { return int(pl.px[std::size_t(y64 * 2 + 1) * W + x64 * 2 + 1]); };
  CHECK(at(p.hair, 30, 14) == 255 && at(p.hair, 30, 30) == 0 && at(p.hair, 2, 2) == 0);
  CHECK(at(p.faceSkin, 30, 28) == 255 && at(p.faceSkin, 30, 14) == 0);
  CHECK(at(p.clothes, 30, 50) == 255 && at(p.bodySkin, 12, 50) == 255 && at(p.bodySkin, 30, 50) == 0);
  CHECK(at(p.person, 30, 14) == 255 && at(p.person, 52, 50) == 255 && at(p.person, 2, 2) == 0);   // person = 255 - background: 'others' belongs to it
  CHECK(at(p.clothes, 52, 50) == 0 && at(p.hair, 52, 50) == 0);                                  // ... but to no named part
  CHECK(at(p.background, 2, 2) == 255 && at(p.background, 30, 14) == 0);
  // a small blob of "hair" in the background (network noise) is dropped, a second person of comparable size is kept
  {
    std::vector<std::uint8_t> cm2 = cm;
    for (int y = 2; y < 6; ++y)
      for (int x = 56; x < 60; ++x) cm2[std::size_t(y) * S + x] = kPartHair;  // 16 cells against a person of about 1900
    const PartPlanes q = splitParts(probFromClassMap(cm2, S).data(), S, W, H);
    CHECK(at(q.hair, 58, 4) == 0 && at(q.person, 58, 4) == 0 && at(q.background, 58, 4) == 255);
    CHECK(at(q.hair, 30, 14) == 255);
    std::vector<std::uint8_t> cm3(std::size_t(S) * S, kPartBackground);
    for (int y = 10; y < 40; ++y)
      for (int x = 4; x < 24; ++x) cm3[std::size_t(y) * S + x] = kPartClothes;
    for (int y = 14; y < 40; ++y)
      for (int x = 40; x < 60; ++x) cm3[std::size_t(y) * S + x] = kPartClothes;
    const PartPlanes r = splitParts(probFromClassMap(cm3, S).data(), S, W, H);
    CHECK(at(r.clothes, 10, 20) == 255 && at(r.clothes, 50, 20) == 255);
  }
  // complement and union hold pixel by pixel
  for (std::size_t i = 0; i < p.person.px.size(); ++i) {
    CHECK(int(p.person.px[i]) + int(p.background.px[i]) == 255);
    const int named = int(p.hair.px[i]) + p.faceSkin.px[i] + p.bodySkin.px[i] + p.clothes.px[i];
    CHECK(named <= int(p.person.px[i]) + 4);
    if (g_failures > 20) return;
  }
}

// ---- taps ----------------------------------------------------------------------------------------------------------------------------------------
static Plane rect(int w, int h, int x0, int y0, int x1, int y1) {
  Plane p;
  p.w = w, p.h = h, p.px.assign(std::size_t(w) * h, 0);
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x) p.px[std::size_t(y) * w + x] = 255;
  return p;
}
static void testTaps() {
  const int W = 40, H = 30;
  const Plane A = rect(W, H, 2, 2, 20, 20), B = rect(W, H, 18, 5, 38, 25), C = rect(W, H, 8, 8, 14, 14);
  std::vector<TapMask> t = {{true, A}};
  CHECK_NEAR(planeSum(accumulateTaps(t, W, H)), 18 * 18, 0.5);
  t.push_back({true, B});
  CHECK_NEAR(planeSum(accumulateTaps(t, W, H)), 18 * 18 + 20 * 20 - 2 * 15, 0.5);  // union: overlap 2 x 15
  t.push_back({false, C});
  const Plane acc = accumulateTaps(t, W, H);
  CHECK_NEAR(planeSum(acc), 18 * 18 + 20 * 20 - 30 - 36, 0.5);
  CHECK(acc.px[10 * W + 10] == 0 && acc.px[3 * W + 3] == 255 && acc.px[10 * W + 30] == 255);
  t.pop_back();  // undo of the last tap = accumulate the earlier ones: the subtracted object is back
  CHECK(accumulateTaps(t, W, H).px[10 * W + 10] == 255);
  std::vector<TapMask> soft = {{true, A}};
  soft[0].mask.px[3 * W + 3] = 100;
  soft.push_back({false, rect(W, H, 0, 0, W, H)});
  soft.back().mask.px.assign(std::size_t(W) * H, 128);  // subtracting a soft mask scales: a * (1 - 128/255)
  CHECK(std::abs(int(accumulateTaps(soft, W, H).px[3 * W + 3]) - 50) <= 1 && std::abs(int(accumulateTaps(soft, W, H).px[10 * W + 10]) - 127) <= 1);
  // spec grammar
  std::vector<TapPoint> pts = {{0.412f, 0.531f, true}, {0.2f, 0.7f, false}, {1.f, 0.f, true}};
  const std::string spec = tapSpec(pts);
  CHECK(spec == "+411,530;-200,699;+999,0" || spec == "+412,531;-200,699;+999,0" || spec.size() > 10);
  std::vector<TapPoint> back;
  CHECK(parseTapSpec(spec, back) && back.size() == 3 && !back[1].add && std::fabs(back[0].x - 0.412f) < 0.002f);
  CHECK(!parseTapSpec("", back) && !parseTapSpec("+5", back) && !parseTapSpec("*1,2", back) && !parseTapSpec("+1,2;;+3,4", back) && !parseTapSpec("+1000,2", back));
  std::string many;
  for (int i = 0; i < kMaxTaps + 1; ++i) many += (i ? ";+1,2" : "+1,2");
  CHECK(!parseTapSpec(many, back));
  // ids: every id the service makes parses back to its group, and the tap id fits the raster store's 128 character limit at 10 taps
  const std::string ph = "0123456789abcdef";
  CHECK(parseId(makeId(ph, "hair")).group == "person" && parseId(makeId(ph, "background")).group == "person");
  CHECK(parseId(makeId(ph, "depth")).group == "depth" && parseId(makeId(ph, "sky")).group == "sky");
  const ParsedId f = parseId(makeId(ph, "f3:teeth"));
  CHECK(f.ok && f.group == "faces" && f.face == 3 && f.feature == "teeth");
  CHECK(!parseId(makeId(ph, "f5:face")).ok && !parseId(makeId(ph, "f1:nose")).ok && !parseId("ai:xyz:hair").ok && !parseId("manual:1").ok && !parseId(makeId(ph, "tap:bad")).ok);
  std::vector<TapPoint> ten(10, TapPoint{0.123f, 0.456f, true});
  const std::string id = makeId(ph, "tap:" + tapSpec(ten));
  CHECK(id.size() <= 128 && parseId(id).group == "tap" && parseId(id).taps.size() == 10);
  CHECK(isAiId(id) && !isAiId("brush1"));
}

// ---- depth + raster with range ------------------------------------------------------------------------------------------------------------------
static float evalMask(const std::vector<LocalAdjustment>& L, const SourceImage& src, float u, float v) {
  const auto f = local::bind(L, src, false);
  if (!f) return -1.f;
  const float px[3] = {0.2f, 0.2f, 0.2f};
  return local::maskOf(*f, 0, u, v, px, px);
}
static void testDepth() {
  const int dw = 64, dh = 48;
  std::vector<float> d(std::size_t(dw) * dh);
  for (int y = 0; y < dh; ++y)
    for (int x = 0; x < dw; ++x) d[std::size_t(y) * dw + x] = 0.5f + 10.f * float(x) / float(dw - 1);  // inverse depth grows to the right: near on the right
  const Plane far = depthFarPlane(d.data(), dw, dh, 200, 150);
  CHECK(far.w == 200 && far.h == 150);
  CHECK(far.px[75 * 200 + 2] > 245 && far.px[75 * 200 + 197] < 10);  // far on the left, near on the right
  bool mono = true;
  for (int x = 1; x < 200; ++x) mono &= far.px[75 * 200 + x] <= far.px[75 * 200 + x - 1];
  CHECK(mono);
  CHECK(far.px[10 * 200 + 100] == far.px[100 * 200 + 100]);  // constant down a column
  // flat depth map: no division by zero, a valid plane
  std::vector<float> flatd(std::size_t(dw) * dh, 3.f);
  CHECK(!depthFarPlane(flatd.data(), dw, dh, 20, 15).empty());

  local::rasterSet("t:depth", far.w, far.h, far.px.data(), far.w);
  SourceImage src;
  src.linear = ImageF(64, 48);
  for (float& v : src.linear.data) v = 0.2f;
  auto make = [&](bool range, float lo, float hi, float fe) {
    std::vector<LocalAdjustment> L(1);
    LocalAdjustment& a = L[0];
    a.id = "d";
    a.ops.v[kLoExposure] = 1.f;
    MaskComponent r;
    r.kind = MaskKind::Raster;
    r.ref = "t:depth";
    r.refine = 0.f;
    if (range) r.rangeLo = lo, r.rangeHi = hi, r.rangeFeather = fe;
    a.mask.push_back(r);
    return L;
  };
  const float plain = evalMask(make(false, 0, 100, 10), src, 0.1f, 0.5f), plainNear = evalMask(make(false, 0, 100, 10), src, 0.9f, 0.5f);
  CHECK(plain > 0.9f && plainNear < 0.1f);
  const auto band = make(true, 40.f, 70.f, 5.f);  // the middle depth band
  CHECK(evalMask(band, src, 0.5f, 0.5f) > 0.9f);                                   // depth about 50 %
  CHECK(evalMask(band, src, 0.05f, 0.5f) < 0.05f && evalMask(band, src, 0.95f, 0.5f) < 0.05f);  // far end and near end are out
  const auto farOnly = make(true, 75.f, 100.f, 5.f);
  CHECK(evalMask(farOnly, src, 0.05f, 0.5f) > 0.9f && evalMask(farOnly, src, 0.5f, 0.5f) < 0.05f);
  const auto nearOnly = make(true, 0.f, 25.f, 5.f);
  CHECK(evalMask(nearOnly, src, 0.95f, 0.5f) > 0.9f && evalMask(nearOnly, src, 0.5f, 0.5f) < 0.05f);
  // the default range is the identity, exactly
  auto idr = make(true, 0.f, 100.f, 10.f);
  for (float u : {0.05f, 0.3f, 0.5f, 0.77f, 0.95f}) CHECK(evalMask(idr, src, u, 0.5f) == evalMask(make(false, 0, 100, 10), src, u, 0.5f));
  // JSON: written only when active, read back, tolerant of a missing key; the hash sees the range only when active
  const std::string jPlain = localToJson(make(false, 0, 100, 10)), jRange = localToJson(band);
  CHECK(jPlain.find("\"lo\"") == std::string::npos && jRange.find("\"lo\":40") != std::string::npos && jRange.find("\"hi\":70") != std::string::npos);
  std::vector<LocalAdjustment> back;
  CHECK(localFromJson(jRange, back) && back.size() == 1 && back[0].mask[0].rangeLo == 40.f && back[0].mask[0].rangeHi == 70.f && back[0].mask[0].rangeFeather == 5.f);
  CHECK(localFromJson(jPlain, back) && !back[0].mask[0].hasRange());
  CHECK(localHash(make(true, 0, 100, 10)) == localHash(make(false, 0, 100, 10)));  // identity range: the hash of existing edits does not change
  CHECK(localHash(band) != localHash(make(true, 40.f, 71.f, 5.f)) && localHash(band) != localHash(make(false, 0, 100, 10)));
  std::vector<LocalAdjustment> bad = make(true, 90.f, 20.f, 999.f);  // out of order, out of range: clamped
  bad[0].mask[0].rangeLo = 90.f, bad[0].mask[0].rangeHi = 20.f, bad[0].mask[0].rangeFeather = 999.f;
  clampLocal(bad);
  CHECK(bad[0].mask[0].rangeLo == 20.f && bad[0].mask[0].rangeHi == 90.f && bad[0].mask[0].rangeFeather == 50.f);
}

// ---- cache ---------------------------------------------------------------------------------------------------------------------------------------
static fs::path tmpRoot() {
  static const fs::path p = fs::temp_directory_path() / ("dapple_ai_masks_test_" + std::to_string(std::uint64_t(std::time(nullptr)) ^ std::uint64_t(std::rand())));
  return p;
}
static PlaneSet sampleSet() {
  PlaneSet s;
  Plane a = rect(120, 80, 10, 10, 90, 60), b;
  b.w = 120, b.h = 80;
  b.px.resize(120 * 80);
  std::uint32_t r = 12345;
  for (auto& v : b.px) r = r * 1664525u + 1013904223u, v = std::uint8_t(r >> 24);
  s.entries = {{"hair", a}, {"f1:lips", b}, {"person", rect(7, 3, 0, 0, 7, 3)}};
  s.meta = "{\"found\":true,\"gate\":\"face\",\"faces\":1}";
  return s;
}
static void testCache() {
  const PlaneSet s = sampleSet();
  std::vector<std::uint8_t> bytes = packSet(s);
  PlaneSet back;
  CHECK(unpackSet(bytes.data(), bytes.size(), back));
  CHECK(back.entries.size() == 3 && back.meta == s.meta);
  for (std::size_t i = 0; i < 3; ++i) CHECK(back.entries[i].key == s.entries[i].key && back.entries[i].plane.w == s.entries[i].plane.w && back.entries[i].plane.px == s.entries[i].plane.px);
  CHECK(bytes.size() < 120 * 80 * 2);  // the constant planes compress, the noise plane does not
  // corruption: flip every 37th byte in turn, truncate, extend
  int rejected = 0, tried = 0;
  for (std::size_t i = 0; i < bytes.size(); i += 37) {
    std::vector<std::uint8_t> c = bytes;
    c[i] ^= 0x5A;
    ++tried;
    PlaneSet x;
    rejected += !unpackSet(c.data(), c.size(), x);
  }
  CHECK(rejected == tried);
  PlaneSet x;
  CHECK(!unpackSet(bytes.data(), bytes.size() - 1, x) && !unpackSet(bytes.data(), 10, x) && !unpackSet(nullptr, 0, x));
  // disk: configure, store, a second configure to the same dir still reads it; another photo misses
  const fs::path root = tmpRoot();
  fs::remove_all(root);
  configure("", root.string());
  CHECK(storeGroup("00000000000000a1", "person", s));
  CHECK(fs::exists(root / "ai_masks" / "00000000000000a1" / "person.dpl"));
  // pruning: three photos, the oldest goes first
  for (int i = 2; i <= 4; ++i) {
    char id[24];
    std::snprintf(id, sizeof id, "00000000000000a%d", i);
    CHECK(storeGroup(id, "person", s));
    std::error_code ec;
    fs::last_write_time(root / "ai_masks" / id / "person.dpl", fs::file_time_type::clock::now() - std::chrono::hours(10 - i), ec);
  }
  std::error_code ec;
  fs::last_write_time(root / "ai_masks" / "00000000000000a1" / "person.dpl", fs::file_time_type::clock::now() - std::chrono::hours(100), ec);
  const std::uint64_t one = fs::file_size(root / "ai_masks" / "00000000000000a2" / "person.dpl");
  pruneCache(one * 2 + one / 2);
  int left = 0;
  for (auto& d : fs::directory_iterator(root / "ai_masks")) left += d.is_directory();
  CHECK(left == 2 && !fs::exists(root / "ai_masks" / "00000000000000a1") && fs::exists(root / "ai_masks" / "00000000000000a4"));
}

// ---- mouth plane, faces JSON, meta cache round trip -------------------------------------------------------------------------------------------
static void testMouth() {
  const int W = 400, H = 480;
  const FaceMesh open = syntheticMesh(14.f, 0.f), closed = syntheticMesh(1.f, 0.f);
  FaceFeatures fo, fc, fd;
  const auto dark = flat(W, H, 40, 12, 14);
  CHECK(faceFeatures(open, W, H, dark.data(), W * 3, fo) && fo.mouth.w == W && fo.mouth.h == H);
  CHECK(!fo.open && fo.teeth.empty());  // a dark opening shows no teeth, the mouth plane exists nevertheless
  const double inner = shoelace(ringOf(open, kLipsInner, 20));
  CHECK_NEAR(planeSum(fo.mouth), inner, 0.05 * inner);
  CHECK(fo.mouth.px[330 * W + 200] > 250 && fo.mouth.px[330 * W + 200 + 60] == 0 && fo.mouth.px[250 * W + 200] == 0 && fo.mouth.px[330 * W + 200 - 52] < 5);
  CHECK(faceFeatures(closed, W, H, nullptr, 0, fc) && fc.mouth.empty());
  CHECK(faceFeatures(syntheticMesh(14.f, 200.f), W, H, nullptr, 0, fd) && fd.mouth.empty());  // turned head: no mouth plane
  const ParsedId pid = parseId(makeId("00000000000000a1", "f2:mouth"));
  CHECK(pid.ok && pid.group == "faces" && pid.face == 2 && pid.feature == "mouth");
  CHECK(kEpoch == 6);
}

// a mesh whose unused points sit at the face centre and with iris centres (468 / 473), as a real mesh has
static FaceMesh fullMesh(float mouthRy, float yawZ) {
  FaceMesh m = syntheticMesh(mouthRy, yawZ);
  for (int i = 0; i < 478; ++i)
    if (m.xy[i][0] == 0.f && m.xy[i][1] == 0.f) m.xy[i][0] = 200.f, m.xy[i][1] = 240.f;
  m.xy[468][0] = 150.f, m.xy[468][1] = 200.f, m.xy[473][0] = 250.f, m.xy[473][1] = 200.f;
  return m;
}
static void testFacesJson() {
  using dapple::jsonlite::JParser;
  using dapple::jsonlite::JValue;
  const int W = 400, H = 480;
  FaceFeatures a, b, c;
  CHECK(faceFeatures(fullMesh(14.f, 0.f), W, H, nullptr, 0, a));     // open, frontal (teeth = inner polygon)
  CHECK(faceFeatures(fullMesh(1.f, 0.f), W, H, nullptr, 0, b));      // closed, frontal
  CHECK(faceFeatures(fullMesh(14.f, 200.f), W, H, nullptr, 0, c));   // turned
  const std::string meta = facesMetaJson({a, b, c}, W, H);
  JValue v;
  JParser p(meta);
  CHECK(p.parse(v));
  const JValue* cnt = v.get("count");
  const JValue* list = v.get("list");
  CHECK(cnt && cnt->number == 3 && list && list->kind == JValue::Array && list->items.size() == 3);
  if (!list || list->items.size() != 3) return;
  const JValue& f1 = list->items[0];
  CHECK(f1.get("index") && f1.get("index")->number == 1 && list->items[2].get("index")->number == 3);
  const JValue* box = f1.get("box");
  CHECK(box && box->items.size() == 4 && box->items[0].number < box->items[2].number && box->items[1].number < box->items[3].number);
  CHECK(box && box->items[0].number >= 0 && box->items[2].number <= 1 && box->items[3].number <= 1);
  // the synthetic oval spans x 80..320, y 80..400 of 400 x 480
  CHECK_NEAR(box->items[0].number, 80.0 / W, 0.02);
  CHECK_NEAR(box->items[3].number, 400.0 / H, 0.02);
  CHECK_NEAR(f1.get("iod")->number, 100.0 / 480.0, 0.002);  // iris centres 100 px apart, long edge 480
  const JValue* ctr = f1.get("center");
  CHECK(ctr && ctr->items.size() == 2);
  CHECK_NEAR(ctr->items[0].number, 200.0 / W, 0.002);
  CHECK_NEAR(ctr->items[1].number, 200.0 / H, 0.002);
  CHECK(f1.get("frontal")->boolean && !list->items[2].get("frontal")->boolean && f1.get("yaw") && f1.get("mouth_open") && f1.get("teeth"));
  auto keys = [&](const JValue& f) {
    std::string k;
    for (const JValue& x : f.get("planes")->items) k += x.str + ",";
    return k;
  };
  CHECK(keys(f1) == "face,eyes,iris,sclera,lips,teeth,mouth,brows,undereye,");
  CHECK(keys(list->items[1]) == "face,eyes,iris,sclera,lips,brows,undereye,");  // closed mouth: no teeth, no mouth
  CHECK(keys(list->items[2]) == "face,");
  CHECK(facesListOfMeta(meta).front() == '[' && facesListOfMeta(meta).back() == ']' && facesListOfMeta("{\"found\":true}") == "[]");
  JValue lv;
  const std::string listText = facesListOfMeta(meta);
  JParser lp(listText);
  CHECK(lp.parse(lv) && lv.items.size() == 3);
  JValue ev;
  const std::string emptyText = facesMetaJson({}, W, H);
  JParser ep(emptyText);
  CHECK(ep.parse(ev) && ev.get("count")->number == 0 && ev.get("list")->items.empty());
}

static void testFacesCache() {
  FaceFeatures a, b;
  CHECK(faceFeatures(syntheticMesh(14.f, 0.f), 400, 480, nullptr, 0, a) && faceFeatures(syntheticMesh(1.f, 0.f), 400, 480, nullptr, 0, b));
  PlaneSet s;
  s.entries = {{"f1:face", a.face}, {"f1:mouth", a.mouth}, {"f2:face", b.face}};
  s.meta = facesMetaJson({a, b}, 400, 480);
  std::vector<std::uint8_t> bytes = packSet(s);
  PlaneSet back;
  CHECK(unpackSet(bytes.data(), bytes.size(), back) && back.meta == s.meta && back.entries.size() == 3);
  CHECK(facesListOfMeta(back.meta) == facesListOfMeta(s.meta));
  const fs::path root = tmpRoot();
  fs::remove_all(root);
  configure("", root.string());
  CHECK(storeGroup("00000000000000b1", "faces", s));
  std::ifstream in(root / "ai_masks" / "00000000000000b1" / "faces.dpl", std::ios::binary);
  std::vector<std::uint8_t> disk((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  PlaneSet fromDisk;
  CHECK(!disk.empty() && unpackSet(disk.data(), disk.size(), fromDisk) && fromDisk.meta == s.meta);
}

// ---- ensure ----------------------------------------------------------------------------------------------------------------------------------------
static void testEnsure() {
  const fs::path root = tmpRoot();
  fs::remove_all(root);
  configure("", root.string());
  const std::string ph = "00000000000000b1";
  PlaneSet s;
  s.entries = {{"hair", rect(64, 48, 0, 0, 32, 48)}, {"person", rect(64, 48, 0, 0, 64, 24)}, {"background", rect(64, 48, 0, 24, 64, 48)}};
  s.meta = "{\"found\":true,\"gate\":\"face\",\"faces\":1}";
  CHECK(storeGroup(ph, "person", s));
  Source src;
  src.photoId = ph;
  bool rendered = false;
  src.render = [&](Rendition&, std::string& err) {
    rendered = true;
    err = "test: no picture";
    return false;
  };
  Control ctl;
  ctl.yield = false;
  const std::string hair = makeId(ph, "hair"), person = makeId(ph, "person");
  local::rasterRemove(hair), local::rasterRemove(person);
  std::string why;
  const auto still = ensure(src, {hair, person}, ctl, &why);
  CHECK(still.empty() && local::rasterHas(hair) && local::rasterHas(person));
  CHECK(!rendered);  // restored from the cache: no picture, no model
  const auto r = local::rasterGet(hair);
  CHECK(r && r->w == 64 && r->h == 48 && r->px[5] == 255 && r->px[40] == 0);
  // already present: nothing to do
  CHECK(ensure(src, {hair}, ctl, &why).empty());
  // not in the cache and no model: the id is reported, with a reason
  why.clear();
  const std::string depth = makeId(ph, "depth");
  local::rasterRemove(depth);
  const auto m2 = ensure(src, {depth}, ctl, &why);
  CHECK(m2.size() == 1 && m2[0] == depth && !why.empty());
  std::printf("  why (depth): %s\n", why.c_str());
  // another photo's id, a hand-made id
  why.clear();
  const auto m3 = ensure(src, {makeId("00000000000000b2", "hair"), "brush:1"}, ctl, &why);
  CHECK(m3.size() == 2 && !why.empty());
  // a feature of a face that does not exist (cache says one face, none of "f2")
  PlaneSet fs1;
  fs1.entries = {{"f1:face", rect(64, 48, 10, 10, 50, 40)}};
  fs1.meta = "{\"count\":1}";
  CHECK(storeGroup(ph, "faces", fs1));
  const std::string f2 = makeId(ph, "f2:face"), f1 = makeId(ph, "f1:face");
  local::rasterRemove(f1), local::rasterRemove(f2);
  const auto m4 = ensure(src, {f1, f2}, ctl, &why);
  CHECK(m4.size() == 1 && m4[0] == f2 && local::rasterHas(f1));
  // a cache file of another epoch / damaged: a miss, not a crash
  {
    std::fstream f(root / "ai_masks" / ph / "person.dpl", std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(30);
    f.put('\x7f');
  }
  local::rasterRemove(hair);
  CHECK(ensure(src, {hair}, ctl, &why).size() == 1);
}

// ---- export -----------------------------------------------------------------------------------------------------------------------------------------
static std::vector<std::uint8_t> readAll(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static double meanLuma(DappleImage& im, bool left) {
  double s = 0;
  int n = 0;
  for (int y = 0; y < im.height; ++y)
    for (int x = left ? 0 : im.width / 2; x < (left ? im.width / 2 : im.width); ++x) {
      const std::uint8_t* p = im.rgba + (std::size_t(y) * im.width + x) * 4;
      s += 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2], ++n;
    }
  return s / std::max(1, n);
}
static void testExport() {
  const fs::path root = tmpRoot() / "export";
  fs::remove_all(root);
  fs::create_directories(root);
  const int W = 320, H = 200;
  std::vector<std::uint8_t> px(std::size_t(W) * H * 3);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      std::uint8_t* p = &px[(std::size_t(y) * W + x) * 3];
      p[0] = std::uint8_t(60 + x * 120 / W), p[1] = std::uint8_t(70 + y * 100 / H), p[2] = std::uint8_t(90 + (x + y) * 50 / (W + H));
    }
  const std::string jpg = (root / "src.jpg").string();
  CHECK(stbi_write_jpg(jpg.c_str(), W, H, 3, px.data(), 95) != 0);
  configure("", (root / "cache").string());
  DappleSession* s = dapple_session_open(jpg.c_str(), DAPPLE_QUALITY_PREVIEW, 0);
  CHECK(s != nullptr);
  if (!s) return;
  char* pid = dapple_session_ai_photo_id(s);
  const std::string ph = pid ? pid : "";
  dapple_free_string(pid);
  CHECK(ph.size() == 16);
  const std::string id = makeId(ph, "person");
  PlaneSet set;
  set.entries = {{"person", rect(64, 40, 0, 0, 32, 40)}};  // the left half
  set.meta = "{\"found\":true}";
  CHECK(storeGroup(ph, "person", set));
  const std::string withMask = std::string("{\"local\":{\"v\":1,\"items\":[{\"id\":\"p\",\"strength\":100,\"mask\":[{\"kind\":\"raster\",\"op\":\"add\",\"ref\":\"") + id +
                               "\",\"refine\":0}],\"adjust\":{\"exposure\":40}}]}}";
  const std::string withBrush = "{\"local\":{\"v\":1,\"items\":[{\"id\":\"p\",\"strength\":100,\"mask\":[{\"kind\":\"raster\",\"op\":\"add\",\"ref\":\"manual:1\",\"refine\":0}],\"adjust\":{\"exposure\":40}}]}}";
  const fs::path outA = root / "a.jpg", outB = root / "b.jpg", outC = root / "c.jpg", outD = root / "d.jpg", outN = root / "n.jpg";
  // the plane is there: the reference export
  CHECK(dapple_ai_status_json() != nullptr);
  CHECK(local::rasterSet(id, 64, 40, set.entries[0].plane.px.data(), 64));
  CHECK(dapple_session_export_jpeg(s, withMask.c_str(), nullptr, 0, 92, outA.string().c_str()) == DAPPLE_OK);
  CHECK(dapple_session_export_jpeg(s, "{}", nullptr, 0, 92, outN.string().c_str()) == DAPPLE_OK);
  // the plane is gone (cache wipe of the process store / another device) but the disk cache has it: the export restores it, byte for byte
  local::rasterRemove(id);
  CHECK(dapple_mask_raster_has(id.c_str()) == 0);
  {
    char* j = dapple_session_ai_missing_json(s, withMask.c_str());
    const std::string mj = j ? j : "";
    dapple_free_string(j);
    CHECK(mj.find(id) != std::string::npos && mj.find("\"ai\":true") != std::string::npos && mj.find("\"photo\":true") != std::string::npos);
  }
  CHECK(dapple_session_export_jpeg(s, withMask.c_str(), nullptr, 0, 92, outB.string().c_str()) == DAPPLE_OK);
  CHECK(dapple_mask_raster_has(id.c_str()) == 1);
  CHECK(readAll(outA) == readAll(outB) && !readAll(outA).empty());
  CHECK(readAll(outA) != readAll(outN));  // and the mask does something
  // the plane is gone and the cache too: an interactive / plain render proceeds with an empty mask, the export FAILS and writes nothing
  local::rasterRemove(id);
  fs::remove_all(root / "cache");
  DappleImage plain{}, masked{};
  CHECK(dapple_session_render(s, "{}", nullptr, 0, &plain) == DAPPLE_OK);
  CHECK(dapple_session_render(s, withMask.c_str(), nullptr, 0, &masked) == DAPPLE_OK);
  CHECK_NEAR(meanLuma(masked, true), meanLuma(plain, true), 0.01);  // empty mask: nothing changes
  dapple_free_image(&plain), dapple_free_image(&masked);
  CHECK(dapple_session_export_jpeg(s, withMask.c_str(), nullptr, 0, 92, outC.string().c_str()) == DAPPLE_ERROR);
  CHECK(!fs::exists(outC));
  const std::string err = dapple_last_error() ? dapple_last_error() : "";
  CHECK(err.find(id) != std::string::npos && err.find("nothing was written") != std::string::npos);
  std::printf("  error: %s\n", err.c_str());
  // a hand-made raster that is not in the store: same rule
  CHECK(dapple_session_export_jpeg(s, withBrush.c_str(), nullptr, 0, 92, outD.string().c_str()) == DAPPLE_ERROR && !fs::exists(outD));
  // supplied again by the caller: fine
  CHECK(local::rasterSet("manual:1", 64, 40, set.entries[0].plane.px.data(), 64));
  CHECK(dapple_session_export_jpeg(s, withBrush.c_str(), nullptr, 0, 92, outD.string().c_str()) == DAPPLE_OK);
  // the HDR export path obeys the same rule
  local::rasterRemove(id);
  CHECK(dapple_session_export_jpeg_hdr(s, withMask.c_str(), nullptr, 0, 92, (root / "h.jpg").string().c_str(), 0.f, nullptr) == DAPPLE_ERROR);
  // disabled adjustments do not need their plane
  const std::string disabled = std::string("{\"local\":{\"v\":1,\"items\":[{\"id\":\"p\",\"on\":false,\"strength\":100,\"mask\":[{\"kind\":\"raster\",\"op\":\"add\",\"ref\":\"") + id +
                               "\"}],\"adjust\":{\"exposure\":40}}]}}";
  CHECK(dapple_session_export_jpeg(s, disabled.c_str(), nullptr, 0, 92, (root / "e.jpg").string().c_str()) == DAPPLE_OK);
  // closing a session with a job in flight waits for it
  dapple_session_close(s);
}

// ---- raster store eviction and on-demand re-creation ---------------------------------------------------------------------------------------------
// Planes of photos that are no longer open go first (least recently used photo first); the photo in use loses planes only when nothing else is left,
// and then the ones its edit does not read.
static void testStore() {
  const std::vector<std::uint8_t> px(16 * 8, 100);
  auto pid = [](char ph, int k) { return std::string("ai:") + std::string(16, ph) + ":k" + std::to_string(k); };
  auto put = [&](const std::string& id) { CHECK(local::rasterSet(id, 16, 8, px.data(), 16)); };
  for (char c : {'a', 'b', 'c', 'd'})
    for (int k = 0; k < 70; ++k) local::rasterRemove(pid(c, k));
  for (int k = 0; k < 30; ++k) put(pid('a', k));
  for (int k = 0; k < 30; ++k) put(pid('b', k));
  CHECK(local::rasterGet(pid('a', 5)) != nullptr);  // a is now more recent than b
  for (int k = 0; k < 5; ++k) put(pid('c', k));       // 65 planes: one goes, from the least recently used photo (b), its oldest plane
  CHECK(!local::rasterHas(pid('b', 0)));
  int na = 0, nb = 0, nc = 0;
  for (int k = 0; k < 30; ++k) na += local::rasterHas(pid('a', k)), nb += local::rasterHas(pid('b', k));
  for (int k = 0; k < 5; ++k) nc += local::rasterHas(pid('c', k));
  CHECK(na == 30 && nb == 29 && nc == 5);
  // analysing one more photo pushes out b entirely before a loses anything
  for (int k = 0; k < 30; ++k) put(pid('d', k));
  nb = 0, na = 0;
  for (int k = 0; k < 30; ++k) nb += local::rasterHas(pid('b', k)), na += local::rasterHas(pid('a', k));
  int nd = 0;
  for (int k = 0; k < 30; ++k) nd += local::rasterHas(pid('d', k));
  std::printf("  after d: a %d b %d d %d\n", na, nb, nd);
  CHECK(nd == 30 && nb == 0 && na == 29 && !local::rasterHas(pid('a', 0)));  // b went entirely, then a's oldest plane; c (newer than a) is whole
  nc = 0;
  for (int k = 0; k < 5; ++k) nc += local::rasterHas(pid('c', k));
  CHECK(nc == 5);
  // the open photo alone over the cap: the planes the edit reads (touched at every render) survive, the unread ones go
  for (char c : {'a', 'b', 'c', 'd'})
    for (int k = 0; k < 70; ++k) local::rasterRemove(pid(c, k));
  put(pid('a', 0));
  for (int k = 1; k < 64; ++k) put(pid('a', k));
  for (int k = 64; k < 70; ++k) {
    CHECK(local::rasterGet(pid('a', 0)) != nullptr);  // the edit reads plane 0 at every render
    put(pid('a', k));
  }
  CHECK(local::rasterHas(pid('a', 0)));
  CHECK(local::rasterHas(pid('a', 69)));
  int count = 0;
  local::rasterStoreBytes(&count);
  CHECK(count <= 64);
  for (int k = 0; k < 70; ++k) local::rasterRemove(pid('a', k));
}

// A foreground exact frame re-creates a plane the edit references and the store lost (disk cache), so the mask is not silently empty.
static void testRecreate() {
  const fs::path root = tmpRoot() / "recreate";
  fs::remove_all(root);
  fs::create_directories(root);
  const int W = 320, H = 200;
  std::vector<std::uint8_t> px(std::size_t(W) * H * 3);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      std::uint8_t* p = &px[(std::size_t(y) * W + x) * 3];
      p[0] = std::uint8_t(60 + x * 120 / W), p[1] = std::uint8_t(70 + y * 100 / H), p[2] = std::uint8_t(90 + (x + y) * 50 / (W + H));
    }
  const std::string jpg = (root / "src.jpg").string();
  CHECK(stbi_write_jpg(jpg.c_str(), W, H, 3, px.data(), 95) != 0);
  configure("", (root / "cache").string());
  DappleSession* s = dapple_session_open(jpg.c_str(), DAPPLE_QUALITY_PREVIEW, 0);
  CHECK(s != nullptr);
  if (!s) return;
  char* pid = dapple_session_ai_photo_id(s);
  const std::string ph = pid ? pid : "";
  dapple_free_string(pid);
  const std::string id = makeId(ph, "person");
  PlaneSet set;
  set.entries = {{"person", rect(64, 40, 0, 0, 32, 40)}};
  set.meta = "{\"found\":true}";
  CHECK(storeGroup(ph, "person", set));
  const std::string withMask = std::string("{\"local\":{\"v\":1,\"items\":[{\"id\":\"p\",\"strength\":100,\"mask\":[{\"kind\":\"raster\",\"op\":\"add\",\"ref\":\"") + id +
                               "\",\"refine\":0}],\"adjust\":{\"exposure\":40}}]}}";
  CHECK(local::rasterSet(id, 64, 40, set.entries[0].plane.px.data(), 64));
  DappleFrameRequest rq{};
  rq.size = int32_t(sizeof rq);
  rq.flags = DAPPLE_FRAME_EXACT;
  DappleImage ref{}, again{}, none{};
  CHECK(dapple_session_render_frame(s, withMask.c_str(), nullptr, &rq, &ref, nullptr) == DAPPLE_OK);
  // evicted: the next exact frame brings it back from the disk cache and renders the same picture
  local::rasterRemove(id);
  CHECK(dapple_mask_raster_has(id.c_str()) == 0);
  CHECK(dapple_session_render_frame(s, withMask.c_str(), nullptr, &rq, &again, nullptr) == DAPPLE_OK);
  CHECK(dapple_mask_raster_has(id.c_str()) == 1);
  CHECK_NEAR(meanLuma(again, true), meanLuma(ref, true), 0.01);
  // an interactive frame does not stop to re-create (the exact frame that follows does)
  local::rasterRemove(id);
  rq.flags = DAPPLE_FRAME_INTERACTIVE;
  CHECK(dapple_session_render_frame(s, withMask.c_str(), nullptr, &rq, &none, nullptr) == DAPPLE_OK);
  CHECK(dapple_mask_raster_has(id.c_str()) == 0);
  dapple_free_image(&ref), dapple_free_image(&again), dapple_free_image(&none);
  dapple_session_close(s);
}

// ---- sky -----------------------------------------------------------------------------------------------------------------------------------------
static void testSky() {
  const int W = 320, H = 240;
  std::vector<std::uint8_t> sky(std::size_t(W) * H * 3), wall(std::size_t(W) * H * 3);
  std::uint32_t r = 7;
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      r = r * 1664525u + 1013904223u;
      const int n = int(r >> 28) - 8;  // +-8 of sensor-like noise
      std::uint8_t* p = &sky[(std::size_t(y) * W + x) * 3];
      if (y < H * 55 / 100) {  // blue gradient sky
        p[0] = std::uint8_t(90 + y / 3 + n / 2), p[1] = std::uint8_t(140 + y / 3 + n / 2), p[2] = std::uint8_t(225 + n / 2);
      } else {  // textured dark ground
        const int t = ((x / 3) * 7 + (y / 3) * 13) % 60;
        p[0] = std::uint8_t(40 + t), p[1] = std::uint8_t(60 + t), p[2] = std::uint8_t(30 + t / 2);
      }
      std::uint8_t* q = &wall[(std::size_t(y) * W + x) * 3];
      q[0] = q[1] = q[2] = std::uint8_t(170 + n);
    }
  float conf = 0;
  const Plane s = skyPrior(sky.data(), W, H, W * 3, nullptr, &conf);
  CHECK(!s.empty() && conf > 0.2f);
  if (!s.empty()) {
    CHECK(s.px[(H / 5) * W + W / 2] > 230 && s.px[(H * 85 / 100) * W + W / 2] < 10);
    CHECK_NEAR(planeSum(s) / double(W * H), 0.55, 0.06);
  }
  float c2 = 0;
  CHECK(skyPrior(wall.data(), W, H, W * 3, nullptr, &c2).empty() && c2 == 0.f);  // a grey wall is not a sky
  // a blue wall low in the frame (not connected to the top) is not a sky either
  std::vector<std::uint8_t> low = sky;
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      std::uint8_t* p = &low[(std::size_t(y) * W + x) * 3];
      if (y < H / 2) p[0] = 60, p[1] = 50, p[2] = 40, p[0] += std::uint8_t((x * 5 + y * 11) % 50);  // textured top
    }
  CHECK(skyPrior(low.data(), W, H, W * 3, nullptr, &c2).empty());
}

static void testStatus() {
  char* j = dapple_ai_status_json();
  const std::string s = j ? j : "";
  dapple_free_string(j);
  std::printf("  status: %s\n", s.c_str());
  CHECK(s.find("\"compiled\":") != std::string::npos && s.find("\"tasks\":") != std::string::npos);
  DappleSession* none = nullptr;
  CHECK(dapple_session_ai_start(none, 1, 0, 4) == -1);
  char* job = dapple_ai_job_json(9999);
  CHECK(job && std::string(job).find("unknown") != std::string::npos);
  dapple_free_string(job);
}

int main(int argc, char** argv) {
  struct T {
    const char* id;
    void (*fn)();
  };
  const T tests[] = {{"polygon", testPolygon}, {"faces", testFaces}, {"parts", testParts}, {"taps", testTaps}, {"depth", testDepth},
                     {"cache", testCache},     {"ensure", testEnsure}, {"export", testExport}, {"sky", testSky}, {"status", testStatus},
                     {"mouth", testMouth}, {"facesjson", testFacesJson}, {"facescache", testFacesCache}, {"store", testStore}, {"recreate", testRecreate}};
  if (argc < 2) {
    for (const T& t : tests) std::printf("%s\n", t.id);
    return 0;
  }
  for (const T& t : tests)
    if (std::string(argv[1]) == t.id) {
      std::printf("[%s]\n", t.id);
      t.fn();
      std::error_code ec;
      fs::remove_all(tmpRoot(), ec);
      std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ok", g_failures, g_failures == 1 ? "" : "s");
      return g_failures ? 1 : 0;
    }
  std::printf("unknown test id %s\n", argv[1]);
  return 2;
}
