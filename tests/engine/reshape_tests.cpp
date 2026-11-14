// Portrait reshaping: the geometric warp stage and its operators (docs/engine/pipeline-architecture.md section 45).
//   dapple_reshape_tests <id>   exit 77: no GPU (parity)
//     json         "reshape" round trip byte-identical, unknown keys kept, clamping, an edit without it unchanged (no key written)
//     identity     zero strengths: no field, bit-identical frame; with a field, pixels outside its support bit-identical; the old Renderer ignores it
//     bounded      every operator at 100 on a symmetric synthetic face / body: displacement within its bound, fold-free (max |grad F| < 1, not limited),
//                  mirror-symmetric (B_x(mirror) = -B_x, B_y equal); strengths add up without folding
//     protect      background protection: with the person matte, the field is exactly 0 beyond the band delta outside the person; line bending
//     push         push / restore strokes: content follows the drag, restore brings it back, fold-free
//     tiles        CPU tiled == whole (max 1 code) with reshaping alone and with crop / rotate / straighten / flip + local adjustments + clarity
//     region       a region window (SourceImage::parent) renders the reshaping like the same pixels of the whole frame
//     interactive  interactive (coarse grid 256) vs exact frame (grid 1024): mean difference bound
//     mappoint     forward(backward(p)) = p; heal spots and masks follow the content
//     sharpness    a pure sub-pixel translation keeps >= 90 % of the gradient energy; zero displacement = the same bits
//     parity       GPU vs CPU frame, GPU tiled export vs one tile
//   not in ctest: bench (field build, CPU / GPU frames at 1080 interactive and 24 MP exact)
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "dapple/camera.h"
#include "dapple/decoder.h"
#include "dapple/edit_params.h"
#include "dapple/renderer.h"
#include "engine/exec/cpu_executor.h"
#include "engine/exec/engine_session.h"
#include "ai_masks.h"
#include "frame_kernels.h"
#include "json_value.h"
#include "local_eval.h"
#include "warp_field.h"
#ifdef DAPPLE_RESHAPE_GPU
#include "engine/exec/gpu_executor.h"
#include "gpu_rig.h"
#include "parity.h"
#endif

namespace {

using namespace dapple;
using namespace dapple::engine;

int g_failures = 0;
#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("  CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                         \
    }                                                                       \
  } while (0)

const CameraProfile& cam() {
  static const CameraRegistry reg = CameraRegistry::withBuiltins();
  return reg.resolve("", "");
}

float hash01(std::uint32_t x, std::uint32_t y) {
  std::uint32_t h = x * 0x8DA6B343u ^ y * 0xD8163841u;
  h ^= h >> 13, h *= 0x5BD1E995u, h ^= h >> 15;
  return float(h & 0xFFFFFF) / float(0xFFFFFF);
}

// A synthetic scene: a wall with straight vertical and horizontal lines (the background), a figure (head ellipse + torso + legs) in front,
// per-pixel texture. Scene-linear.
SourceImage makeScene(int W, int H) {
  SourceImage s;
  s.sceneReferred = true;
  s.meta.make = "Synthetic";
  s.linear = ImageF(W, H);
  const float L = float(std::max(W, H));
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      const float X = (x + 0.5f) / L, Y = (y + 0.5f) / L, ax = float(W) / L;
      float r = 0.32f, g = 0.30f, b = 0.27f;
      if (std::fmod(X, 0.05f) < 0.004f || std::fmod(Y, 0.05f) < 0.004f) r = g = b = 0.06f;  // lines
      const float cx = 0.5f * ax;
      const float dh = std::hypot((X - cx) / 0.075f, (Y - 0.22f) / 0.1f);
      const bool torso = std::fabs(X - cx) < 0.11f && Y > 0.31f && Y < 0.62f;
      const bool legs = (std::fabs(X - cx - 0.045f) < 0.035f || std::fabs(X - cx + 0.045f) < 0.035f) && Y >= 0.62f && Y < 0.93f;
      if (dh < 1.f) r = 0.55f, g = 0.38f, b = 0.30f;
      if (torso) r = 0.12f, g = 0.18f, b = 0.40f;
      if (legs) r = 0.20f, g = 0.20f, b = 0.22f;
      // eyes, nose
      if (std::hypot(X - cx + 0.025f, Y - 0.205f) < 0.008f || std::hypot(X - cx - 0.025f, Y - 0.205f) < 0.008f) r = g = b = 0.05f;
      const float n = 0.85f + 0.3f * hash01(std::uint32_t(x), std::uint32_t(y));
      float* p = s.linear.row(y) + 3 * std::size_t(x);
      p[0] = r * n, p[1] = g * n, p[2] = b * n;
    }
  return s;
}

// The person matte of the scene, stored in the raster store (photo space, long edge 512)
void putMatte(const char* id, int W, int H) {
  const int L = 512, w = W >= H ? L : int(std::lround(L * double(W) / H)), h = H >= W ? L : int(std::lround(L * double(H) / W));
  std::vector<std::uint8_t> px(std::size_t(w) * h, 0);
  const float Lp = float(std::max(W, H));
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const float X = (x + 0.5f) / w * W / Lp, Y = (y + 0.5f) / h * H / Lp, cx = 0.5f * W / Lp;
      const bool head = std::hypot((X - cx) / 0.075f, (Y - 0.22f) / 0.1f) < 1.f;
      const bool neck = std::fabs(X - cx) < 0.03f && Y > 0.3f && Y < 0.33f;
      const bool torso = std::fabs(X - cx) < 0.11f && Y > 0.31f && Y < 0.62f;
      const bool legs = (std::fabs(X - cx - 0.045f) < 0.035f || std::fabs(X - cx + 0.045f) < 0.035f) && Y >= 0.62f && Y < 0.93f;
      px[std::size_t(y) * w + x] = (head || neck || torso || legs) ? 255 : 0;
    }
  local::rasterSet(id, w, h, px.data(), w);
}

// Face anchors of the synthetic head (photo-normalised), mirror-symmetric about x = 0.5
ReshapeFace synthFace(int W, int H) {
  const double L = std::max(W, H), cx = 0.5, cyL = 0.22, aL = 0.075, bL = 0.1;
  ReshapeFace f;
  f.index = 1;
  f.pts.assign(2 * kReshapeFacePts, 0.f);
  auto put = [&](int k, double XL, double YL) { f.pts[2 * k] = float(XL * L / W), f.pts[2 * k + 1] = float(YL * L / H); };
  const double cxL = cx * W / L;
  for (int i = 0; i < 36; ++i) {
    const double ph = 2 * 3.14159265358979 * i / 36;
    put(i, cxL - aL * std::sin(ph), cyL - bL * std::cos(ph));
  }
  put(36, cxL - 0.025, 0.205), put(37, cxL + 0.025, 0.205);
  put(38, cxL - 0.037, 0.205), put(39, cxL - 0.013, 0.205), put(40, cxL + 0.013, 0.205), put(41, cxL + 0.037, 0.205);
  put(42, cxL - 0.025, 0.199), put(43, cxL - 0.025, 0.211), put(44, cxL + 0.025, 0.199), put(45, cxL + 0.025, 0.211);
  put(46, cxL - 0.012, 0.245), put(47, cxL + 0.012, 0.245), put(48, cxL, 0.24), put(49, cxL, 0.2);
  put(50, cxL - 0.022, 0.27), put(51, cxL + 0.022, 0.27), put(52, cxL, 0.268), put(53, cxL, 0.272);
  return f;
}
// Pose anchors of the synthetic figure
ReshapeBody synthBody(int W, int H) {
  const double L = std::max(W, H), cxL = 0.5 * W / L;
  ReshapeBody b;
  b.pts.assign(3 * kReshapeBodyPts, 0.f);
  auto put = [&](int k, double XL, double YL) { b.pts[3 * k] = float(XL * L / W), b.pts[3 * k + 1] = float(YL * L / H), b.pts[3 * k + 2] = 0.99f; };
  put(0, cxL, 0.24), put(7, cxL - 0.06, 0.21), put(8, cxL + 0.06, 0.21);
  put(11, cxL - 0.09, 0.34), put(12, cxL + 0.09, 0.34);
  put(13, cxL - 0.10, 0.46), put(14, cxL + 0.10, 0.46), put(15, cxL - 0.10, 0.58), put(16, cxL + 0.10, 0.58);
  put(23, cxL - 0.045, 0.62), put(24, cxL + 0.045, 0.62), put(25, cxL - 0.045, 0.77), put(26, cxL + 0.045, 0.77);
  put(27, cxL - 0.045, 0.91), put(28, cxL + 0.045, 0.91);
  return b;
}

bool exactRender(EngineSession& es, const EditParams& p, const SourceImage& src, ExecMode mode, int tile, Image8& out) {
  const Renderer r(p, cam(), nullptr);
  EngineSession::ExactOptions o;
  o.mode = mode;
  o.tile = tile;
  o.sessionCache = false;
  std::string err;
  const bool ok = es.renderExact(p, r, src, o, out, &err, nullptr);
  if (!ok) std::printf("  renderExact failed: %s\n", err.c_str());
  return ok;
}
bool cpuFrame(EngineSession& es, const EditParams& p, const SourceImage& src, bool interactive, FrameResult& res) {
  Renderer r(p, cam(), nullptr);
  FrameJob job{p, r, src, nullptr, cam(), nullptr, {}, {}, {}};
  job.request.bakeSize = 33;
  job.request.fastAnalysis = false;
  job.request.noise = false;
  job.request.interactive = interactive;
  job.fastOptions.spatial = interactive ? SpatialQuality::Interactive : SpatialQuality::Full;
  job.fastOptions.noise = false;
  return es.renderFrame(job, res);
}
double maxDiff(const Image8& a, const Image8& b, std::size_t* differing = nullptr) {
  if (a.width != b.width || a.height != b.height) return 1e9;
  int m = 0;
  std::size_t d = 0;
  for (std::size_t i = 0; i < a.data.size(); ++i) {
    const int e = std::abs(int(a.data[i]) - int(b.data[i]));
    m = std::max(m, e), d += e != 0;
  }
  if (differing) *differing = d;
  return m;
}

EditParams faceEdit(int W, int H, float slim, float small, float chin, float eyes, float nose) {
  EditParams p;
  ReshapeFace f = synthFace(W, H);
  f.slim = slim, f.small = small, f.chin = chin, f.eyes = eyes, f.nose = nose;
  p.reshape.faces = {f};
  return p;
}
EditParams richEdit(int W, int H, bool matte) {
  EditParams p = faceEdit(W, H, 60, 30, 40, 50, 50);
  ReshapeBody b = synthBody(W, H);
  b.waist = 60, b.arms = 60, b.legs = 50, b.longLegs = 60, b.head = 40;
  p.reshape.bodies = {b};
  ReshapePush s;
  s.r = 0.05f;
  s.pts = {0.3f, 0.5f, 0.33f, 0.52f, 0.36f, 0.5f};
  p.reshape.push = {s};
  if (matte) p.reshape.matte = "rs_matte";
  return p;
}

// ---- json ------------------------------------------------------------------------------------------------------------------------------------
void testJson() {
  EditParams p = richEdit(600, 800, true);
  p.reshape.push.push_back(ReshapePush{0.03f, true, {0.31f, 0.5f, 0.35f, 0.5f}});
  const std::string a = toJson(p);
  auto q = fromJson(a);
  CHECK(bool(q));
  if (q) {
    const std::string b = toJson(*q);
    CHECK(a == b);
    CHECK(reshapeHash(q->reshape) == reshapeHash(p.reshape));
  }
  // an edit without the key: nothing written, unchanged
  EditParams e;
  CHECK(toJson(e).find("reshape") == std::string::npos);
  // unknown keys kept verbatim; clamping; a face with the wrong number of points is inert
  auto r = fromJson(R"({"reshape":{"v":2,"future":{"a":[1,2]},"faces":[{"slim":250,"chin":-400,"pts":[0.1,0.2]}],"push":[{"r":9,"pts":[0.1,0.1,0.2,0.2,0.3]}]}})");
  CHECK(bool(r));
  if (r) {
    const std::string w = reshapeToJson(r->reshape);
    std::printf("  written: %s\n", w.c_str());
    CHECK(w.find("\"future\":{\"a\":[1,2]}") != std::string::npos);
    CHECK(r->reshape.faces.size() == 1 && r->reshape.faces[0].slim == 100.f && r->reshape.faces[0].chin == -100.f && r->reshape.faces[0].pts.empty());
    CHECK(!r->reshape.faces[0].active());
    CHECK(r->reshape.push.size() == 1 && r->reshape.push[0].r == 0.25f && r->reshape.push[0].pts.size() == 4);
  }
  // the AI faces meta carries the 54 anchors ("shape", photo normalised) that the app copies into "pts"
  {
    aimask::FaceFeatures ff;
    for (int k = 0; k < kReshapeFacePts; ++k) ff.shape[k][0] = 10.f + k, ff.shape[k][1] = 20.f + 2 * k;
    const std::string meta = aimask::facesMetaJson({ff}, 400, 500);
    const std::size_t a = meta.find("\"shape\":[");
    CHECK(a != std::string::npos);
    if (a != std::string::npos) {
      jsonlite::JValue v;
      const std::string arrText = meta.substr(a + 8, meta.find(']', a) - a - 7);
      jsonlite::JParser jp(arrText);
      CHECK(jp.parse(v) && v.items.size() == 2u * kReshapeFacePts && std::fabs(v.items[3].number - 22.0 / 500) < 1e-5);
    }
  }
  // inactive (landmarks only): hash 0 -> no cache key change
  EditParams z = faceEdit(600, 800, 0, 0, 0, 0, 0);
  CHECK(!z.reshape.active() && reshapeHash(z.reshape) == 0);
}

// ---- identity ---------------------------------------------------------------------------------------------------------------------------------
void testIdentity() {
  const int W = 480, H = 640;
  const SourceImage src = makeScene(W, H);
  EngineSession es;
  Image8 base, z;
  CHECK(exactRender(es, EditParams{}, src, ExecMode::Whole, 0, base));
  EditParams pz = faceEdit(W, H, 0, 0, 0, 0, 0);
  CHECK(exactRender(es, pz, src, ExecMode::Whole, 0, z));
  CHECK(maxDiff(base, z) == 0);
  {  // the frame plan has no field and no geometry
    Renderer r(pz, cam(), nullptr);
    FastOptions fo;
    FrameKernels fk(r, src, nullptr, fo);
    CHECK(!fk.useGeo() && !fk.framePlan().warp.active());
  }
  // eyes only: everything outside the field's support is bit-identical, inside changes
  EditParams pe = faceEdit(W, H, 0, 0, 0, 100, 0);
  Image8 e;
  CHECK(exactRender(es, pe, src, ExecMode::Whole, 0, e));
  auto f = warp::bind(pe.reshape, W, H, false);
  CHECK(f != nullptr);
  std::size_t inside = 0, outsideDiff = 0, insideDiff = 0;
  if (f) {
    // the default sharpening reads 1-2 px around a pixel: "outside" = more than 3 px from any moved pixel
    const warp::Map m = warp::makeMap(*f, W, H, W, H, 0, 0);
    std::vector<std::uint8_t> mv(std::size_t(W) * H, 0), nearMv(std::size_t(W) * H, 0);
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) {
        float sx = x + 0.5f, sy = y + 0.5f;
        mv[std::size_t(y) * W + x] = warp::displace(*f, m, sx, sy);
      }
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x)
        if (mv[std::size_t(y) * W + x])
          for (int j = std::max(0, y - 3); j <= std::min(H - 1, y + 3); ++j)
            for (int i = std::max(0, x - 3); i <= std::min(W - 1, x + 3); ++i) nearMv[std::size_t(j) * W + i] = 1;
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) {
        bool diff = false;
        for (int c = 0; c < 3; ++c) diff |= base.data[(std::size_t(y) * W + x) * 3 + c] != e.data[(std::size_t(y) * W + x) * 3 + c];
        if (mv[std::size_t(y) * W + x]) ++inside, insideDiff += diff;
        else if (!nearMv[std::size_t(y) * W + x]) outsideDiff += diff;
      }
  }
  std::printf("  eyes 100: %zu px in the support (%zu changed), %zu changed more than 3 px outside it\n", inside, insideDiff, outsideDiff);
  CHECK(outsideDiff == 0 && insideDiff > 100);
  // the old Renderer ignores the reshaping
  const Renderer r0(EditParams{}, cam(), nullptr), r1(pe, cam(), nullptr);
  CHECK(r0.render(src).data == r1.render(src).data);
}

// ---- bounded and symmetric ----------------------------------------------------------------------------------------------------------------------
struct FieldStats {
  double maxDisp = 0, asym = 0;
  float grad = 0;
  bool limited = false;
};
FieldStats fieldStats(const ReshapeParams& r, int W, int H) {
  FieldStats s;
  auto f = warp::bind(r, W, H, false);
  if (!f) return s;
  s.maxDisp = f->maxDisp, s.grad = f->maxGrad, s.limited = f->gradLimited;
  for (int j = 0; j < 200; ++j)
    for (int i = 0; i < 200; ++i) {
      const double u = (i + 0.5) / 200, v = (j + 0.5) / 200;
      double dx, dy, mx, my;
      warp::sampleUV(*f, u, v, dx, dy);
      warp::sampleUV(*f, 1.0 - u, v, mx, my);
      s.asym = std::max(s.asym, std::max(std::fabs(dx + mx), std::fabs(dy - my)));
    }
  return s;
}
void testBounded() {
  const int W = 900, H = 1200;
  const double L = 1200;
  struct Op {
    const char* name;
    std::function<void(ReshapeParams&)> set;
    double boundPx;  // bound of |B| in px of a 1200 px long edge
  };
  ReshapeFace f0 = synthFace(W, H);
  ReshapeBody b0 = synthBody(W, H);
  // face width 0.15 L = 180 px: slim 0.055 fw = 9.9 px, small 0.07 x (radius) ~ 0.07 x 0.1 L x 1.6 = 13 px, chin 0.06 fh = 14.4, eyes 0.1 x 0.5 iod ~ 1.5, nose ~0.4
  const Op ops[] = {
      {"slim 100", [&](ReshapeParams& r) { ReshapeFace f = f0; f.slim = 100; r.faces = {f}; }, 12},
      {"small 100", [&](ReshapeParams& r) { ReshapeFace f = f0; f.small = 100; r.faces = {f}; }, 16},
      {"chin +100", [&](ReshapeParams& r) { ReshapeFace f = f0; f.chin = 100; r.faces = {f}; }, 16},
      {"chin -100", [&](ReshapeParams& r) { ReshapeFace f = f0; f.chin = -100; r.faces = {f}; }, 16},
      {"eyes 100", [&](ReshapeParams& r) { ReshapeFace f = f0; f.eyes = 100; r.faces = {f}; }, 3},
      {"nose 100", [&](ReshapeParams& r) { ReshapeFace f = f0; f.nose = 100; r.faces = {f}; }, 3},
      {"face all 100", [&](ReshapeParams& r) { ReshapeFace f = f0; f.slim = f.small = f.chin = f.eyes = f.nose = 100; r.faces = {f}; }, 40},
      {"waist 100", [&](ReshapeParams& r) { ReshapeBody b = b0; b.waist = 100; r.bodies = {b}; }, 14},
      {"arms 100", [&](ReshapeParams& r) { ReshapeBody b = b0; b.arms = 100; r.bodies = {b}; }, 8},
      {"legs 100", [&](ReshapeParams& r) { ReshapeBody b = b0; b.legs = 100; r.bodies = {b}; }, 8},
      {"long legs 100", [&](ReshapeParams& r) { ReshapeBody b = b0; b.longLegs = 100; r.bodies = {b}; }, 45},
      {"head 100", [&](ReshapeParams& r) { ReshapeBody b = b0; b.head = 100; r.bodies = {b}; }, 20},
      {"body all 100", [&](ReshapeParams& r) { ReshapeBody b = b0; b.waist = b.arms = b.legs = b.longLegs = b.head = 100; r.bodies = {b}; }, 60},
  };
  for (const Op& o : ops) {
    ReshapeParams r;
    o.set(r);
    const FieldStats s = fieldStats(r, W, H);
    std::printf("  %-14s max |B| %5.2f px, max |grad F| %.3f%s, asymmetry %.2e px\n", o.name, s.maxDisp * L, s.grad, s.limited ? " (limited)" : "", s.asym * L);
    CHECK(s.maxDisp > 0);
    CHECK(s.maxDisp * L <= o.boundPx);
    CHECK(s.grad < 0.75f || (s.limited && s.grad <= 0.75f));
    CHECK(!s.limited || std::strstr(o.name, "all"));
    CHECK(s.asym * L < 0.02);
  }
}

// ---- background protection ----------------------------------------------------------------------------------------------------------------------
void testProtect() {
  const int W = 900, H = 1200;
  putMatte("rs_matte", W, H);
  auto mt = local::rasterGet("rs_matte");
  for (int withMatte = 0; withMatte < 2; ++withMatte) {
    ReshapeParams r = richEdit(W, H, withMatte != 0).reshape;
    r.push.clear();
    for (auto& b : r.bodies) b.longLegs = 0;  // the band stretch is not protected (lines stay straight by construction)
    auto f = warp::bind(r, W, H, false);
    CHECK(f != nullptr);
    if (!f || !mt) continue;
    // background pixels by distance from the person (from the matte), max |B| in px
    double far = 0, nearMax = 0;
    double bendV = 0;
    const double L = std::max(W, H);
    for (int y = 0; y < H; y += 2)
      for (int x = 0; x < W; x += 2) {
        const int mx = std::min(mt->w - 1, int((x + 0.5) / W * mt->w)), my = std::min(mt->h - 1, int((y + 0.5) / H * mt->h));
        if (mt->px[std::size_t(my) * mt->w + mx] >= 128) continue;
        // distance to the person (brute force on the matte grid, coarse)
        double dmin = 1e9;
        for (int j = std::max(0, my - 40); j < std::min(mt->h, my + 41); ++j)
          for (int i = std::max(0, mx - 40); i < std::min(mt->w, mx + 41); ++i)
            if (mt->px[std::size_t(j) * mt->w + i] >= 128) dmin = std::min(dmin, std::hypot(double(i - mx), double(j - my)) / mt->w * W / L);
        double dx, dy;
        warp::sampleUV(*f, (x + 0.5) / W, (y + 0.5) / H, dx, dy);
        const double d = std::hypot(dx, dy) * L;
        if (dmin > f->delta + 0.004 || dmin > 1e8) far = std::max(far, d);
        else nearMax = std::max(nearMax, d);
        bendV = std::max(bendV, std::fabs(dx) * L);
      }
    std::printf("  %s: delta %.4f (%.1f px), background max |B| near the person %.2f px, beyond delta %.3f px, max horizontal bend of vertical lines %.2f px\n",
                withMatte ? "with matte" : "no matte  ", f->delta, f->delta * L, nearMax, far, bendV);
    if (withMatte) CHECK(far == 0.0);
  }
  // a missing matte plane: no protection, logged, export rule lists it (c_api), the frame still renders
  ReshapeParams r = richEdit(W, H, false).reshape;
  r.matte = "rs_missing";
  auto f = warp::bind(r, W, H, false);
  CHECK(f && f->matteMissing && !f->matteUsed);
}

// ---- body operators: long legs for the main person only, gating, limbs pressed together ---------------------------------------------------------
// A landscape photo with two figures 0.5 long edges apart; only the first has long legs (the app gives the engine the main person only).
ReshapeBody figure(int W, int H, double xL, double sc) {
  ReshapeBody b = synthBody(W, H);
  const double L = std::max(W, H), cxL = 0.5 * W / L;
  for (int k = 0; k < kReshapeBodyPts; ++k) {
    b.pts[3 * k] = float(((b.pts[3 * k] * W / L - cxL) * sc + xL) * L / W);
    b.pts[3 * k + 1] = float(b.pts[3 * k + 1] * H / L * sc * L / H);
  }
  return b;
}
void testBody() {
  const int W = 1800, H = 1000;
  const double L = 1800, B = double(H) / L;
  ReshapeBody a = figure(W, H, 0.30, 0.55);
  a.longLegs = 100;
  ReshapeParams r;
  r.bodies = {a};
  auto f = warp::bind(r, W, H, false);
  CHECK(f != nullptr);
  if (!f) return;
  double legsMax = 0, farMax = 0, bottom = 0, jump = 0, slope = 0;
  const double footRow = 0.55 * 0.91;
  for (int j = 0; j < 500; ++j) {
    const double v = (j + 0.5) / 500;
    double prev = 0;
    for (int i = 0; i < 1800; i += 2) {
      const double u = (i + 0.5) / 1800;
      double dx, dy;
      warp::sampleUV(*f, u, v, dx, dy);
      const double x = u;  // isotropic: AX = 1
      if (std::fabs(x - 0.30) < 0.04 && std::fabs(v * B - footRow) < 0.03) legsMax = std::max(legsMax, std::fabs(dy));
      if (x > 0.70) farMax = std::max(farMax, std::hypot(dx, dy));
      if (j == 499) {
        double bx, by;
        warp::sampleUV(*f, u, 1.0, bx, by);
        bottom = std::max(bottom, std::fabs(by));
      }
      if (i) jump = std::max(jump, std::fabs(dy - prev)), slope = std::max(slope, std::fabs(dy - prev) / (2.0 / 1800));
      prev = dy;
    }
  }
  std::printf("  long legs, main person only: |dy| at the legs %.2f px, beyond the window 0 (%.2e px), bottom row %.2e px, max step between samples %.3f px, max slope along a row %.3f\n",
              legsMax * L, farMax * L, bottom * L, jump * L, slope);
  CHECK(legsMax * L > 8);
  CHECK(farMax == 0.0);  // the second figure (x = 0.8) and everything right of the falloff: exactly untouched
  CHECK(bottom * L < 0.01);
  CHECK(jump * L < 1.5);  // smooth over 2 px steps: no seam
  CHECK(slope < 0.30);
  CHECK(f->maxGrad < 0.75f && !f->gradLimited);

  // gating: a body without what the operator needs moves nothing
  auto vis = [&](ReshapeBody b, std::initializer_list<int> ks, float v) {
    for (int k : ks) b.pts[3 * k + 2] = v;
    return b;
  };
  auto moves = [&](const ReshapeBody& b) {
    ReshapeParams p;
    p.bodies = {b};
    auto g = warp::bind(p, W, H, false);
    return g && g->maxDisp > 0;
  };
  ReshapeBody full = figure(W, H, 0.30, 0.55);
  ReshapeBody t = full;
  t.longLegs = 100;
  CHECK(moves(t));
  CHECK(!moves(vis(t, {27, 28, 29, 30, 31, 32}, 0.1f)));  // no feet
  t = full, t.head = 100;
  CHECK(moves(t));
  CHECK(!moves(vis(t, {23, 24}, 0.1f)));                  // head and shoulders only
  t = full, t.legs = 100;
  CHECK(moves(t));
  CHECK(!moves(vis(t, {25, 26, 27, 28}, 0.1f)));          // no legs
  t = full, t.waist = 100;
  CHECK(!moves(vis(t, {23, 24}, 0.1f)));                  // no hips
  t = full, t.arms = 100;
  CHECK(!moves(vis(t, {13, 14, 15, 16}, 0.1f)));          // no arms

  // a person plane that does not cover the posed body (the segmenter missed it): the protected operators are off
  {
    std::vector<std::uint8_t> zero(64 * 64, 0);
    local::rasterSet("rs_empty", 64, 64, zero.data(), 64);
    ReshapeParams p;
    t = full, t.waist = 100, t.arms = 100;
    p.bodies = {t};
    p.matte = "rs_empty";
    auto g = warp::bind(p, W, H, false);
    CHECK(g == nullptr || g->maxDisp == 0);
  }

  // legs pressed together and an arm against the torso: the inner edge stays (the neighbour is not pulled), the outer edge moves in
  {
    const int W2 = 900, H2 = 1200;
    ReshapeBody b = synthBody(W2, H2);
    b.legs = 100;
    ReshapeParams p;
    p.bodies = {b};
    auto g = warp::bind(p, W2, H2, false);
    CHECK(g != nullptr);
    if (g) {
      const double L2 = 1200, cx = 0.5, y = 0.70 * L2 / H2;  // mid thigh
      double inner = 0, outer = 0, dx, dy;
      for (double d = 0.0; d <= 0.02; d += 0.002) {  // the gap between the legs (edges at +-0.01 L around the centre)
        warp::sampleUV(*g, cx + d * L2 / W2, y, dx, dy);
        inner = std::max(inner, std::fabs(dx) * L2);
      }
      for (double d = 0.07; d <= 0.085; d += 0.002) {  // the outer edge of the right leg (x = 0.045 + 0.035 = 0.08 L)
        warp::sampleUV(*g, cx + d * L2 / W2, y, dx, dy);
        outer = std::max(outer, std::fabs(dx) * L2);
      }
      std::printf("  legs together at 100: |B| in the gap %.2f px, at the outer edge %.2f px\n", inner, outer);
      CHECK(outer > 1.0);
      CHECK(inner < 0.2 * outer);
    }
  }
}

// ---- push / restore ----------------------------------------------------------------------------------------------------------------------------
void testPush() {
  const int W = 800, H = 800;
  ReshapeParams r;
  ReshapePush s;
  s.r = 0.06f;
  s.pts = {0.40f, 0.50f, 0.43f, 0.50f, 0.46f, 0.50f};  // drag 0.06 to the right
  r.push = {s};
  auto f = warp::bind(r, W, H, false);
  CHECK(f != nullptr);
  if (!f) return;
  // the content that was at the start of the drag is now near its end
  double u, v;
  warp::forwardUV(*f, 0.40, 0.50, u, v);
  std::printf("  push: (0.400, 0.500) -> (%.4f, %.4f), max |grad F| n/a, max |B| %.4f\n", u, v, f->maxDisp);
  CHECK(u > 0.44 && u < 0.47 && std::fabs(v - 0.5) < 1e-3);
  // fold-free: the backward map is monotone along the drag line
  double prev = -1;
  bool mono = true;
  for (int i = 0; i <= 400; ++i) {
    double bu, bv;
    warp::backwardUV(*f, 0.3 + 0.4 * i / 400.0, 0.5, bu, bv);
    if (bu <= prev) mono = false;
    prev = bu;
  }
  CHECK(mono);
  // restore over the whole area brings the field close to zero
  ReshapePush rs;
  rs.r = 0.12f;
  rs.restore = true;
  for (int k = 0; k < 12; ++k) rs.pts.insert(rs.pts.end(), {0.35f + 0.01f * k, 0.5f});
  ReshapeParams r2 = r;
  for (int k = 0; k < 6; ++k) r2.push.push_back(rs);
  auto g = warp::bind(r2, W, H, false);
  CHECK(g && g->maxDisp < 0.25 * f->maxDisp);
  if (g) std::printf("  restore x6: max |B| %.4f -> %.4f\n", f->maxDisp, g->maxDisp);
  // a long fast drag is split into fold-free dabs
  ReshapeParams r3;
  ReshapePush l;
  l.r = 0.03f;
  l.pts = {0.2f, 0.5f, 0.6f, 0.5f};
  r3.push = {l};
  auto h = warp::bind(r3, W, H, false);
  prev = -1, mono = true;
  for (int i = 0; i <= 800 && h; ++i) {
    double bu, bv;
    warp::backwardUV(*h, 0.1 + 0.6 * i / 800.0, 0.5, bu, bv);
    if (bu < prev - 1e-9) mono = false;
    prev = bu;
  }
  CHECK(h && mono);
}

// ---- tiles ------------------------------------------------------------------------------------------------------------------------------------
void testTiles() {
  const int W = 600, H = 800;
  const SourceImage src = makeScene(W, H);
  putMatte("rs_matte", W, H);
  struct Case {
    const char* name;
    std::function<void(EditParams&)> set;
  };
  const Case cases[] = {{"reshape", [](EditParams&) {}},
                        {"+ crop 4:5, rotate 90, straighten 4, flip", [](EditParams& p) { p.crop = "4:5", p.rotate = 90, p.straighten = 4.f, p.flip = true; }},
                        {"+ clarity 30, local radial + brush", [](EditParams& p) {
                           p.clarity = 30.f;
                           auto q = fromJson(R"({"local":{"v":1,"items":[{"id":"a","strength":100,"mask":[{"kind":"radial","op":"add","cx":0.5,"cy":0.25,"rx":0.15,"ry":0.12}],"adjust":{"exposure":30,"clarity":40}}]}})");
                           if (q) p.local = q->local;
                         }}};
  for (const Case& c : cases) {
    EditParams p = richEdit(W, H, true);
    c.set(p);
    EngineSession es;
    Image8 whole;
    CHECK(exactRender(es, p, src, ExecMode::Whole, 0, whole));
    for (int tile : {96, 200}) {
      Image8 t;
      std::size_t d = 0;
      CHECK(exactRender(es, p, src, ExecMode::TiledExport, tile, t));
      const double m = maxDiff(whole, t, &d);
      std::printf("  %-44s tile %3d: max code %.0f, differing bytes %zu of %zu\n", c.name, tile, m, d, whole.data.size());
      CHECK(m <= 1);
    }
  }
}

// ---- region ---------------------------------------------------------------------------------------------------------------------------------
void testRegion() {
  const int W = 600, H = 800;
  const SourceImage src = makeScene(W, H);
  putMatte("rs_matte", W, H);
  auto photo = std::make_shared<SourceImage>(src);
  EditParams p = richEdit(W, H, true);
  EngineSession es;
  Image8 whole;
  CHECK(exactRender(es, p, src, ExecMode::Whole, 0, whole));
  struct Win {
    int x0, y0, x1, y1;
  };
  for (const Win& w : {Win{120, 80, 480, 520}, Win{0, 300, 300, 800}}) {
    SourceImage ws;
    ws.meta = src.meta, ws.sceneReferred = true;
    ws.linear = ImageF(w.x1 - w.x0, w.y1 - w.y0);
    for (int y = w.y0; y < w.y1; ++y) std::memcpy(ws.linear.row(y - w.y0), src.linear.row(y) + 3 * std::size_t(w.x0), std::size_t(w.x1 - w.x0) * 3 * sizeof(float));
    auto rp = std::make_shared<RegionParent>();
    rp->fullW = W, rp->fullH = H, rp->x0 = w.x0, rp->y0 = w.y0;
    rp->photo = [photo] { return photo; };
    ws.parent = rp;
    EngineSession es2;
    Image8 rg;
    CHECK(exactRender(es2, p, ws, ExecMode::Whole, 0, rg));
    if (rg.width != w.x1 - w.x0) continue;
    // reads that the field moves out of the window are clamped to its edge: compare from `margin` px in (the field's max displacement + 4)
    auto f = warp::bind(p.reshape, W, H, false);
    const int margin = f ? int(std::ceil(f->maxDisp * std::max(W, H))) + 4 : 4;
    int mx = 0;
    std::size_t over = 0;
    for (int y = margin; y < rg.height - margin; ++y)
      for (int x = margin; x < rg.width - margin; ++x)
        for (int c = 0; c < 3; ++c) {
          const int e = std::abs(int(whole.data[(std::size_t(y + w.y0) * W + x + w.x0) * 3 + c]) - int(rg.data[(std::size_t(y) * rg.width + x) * 3 + c]));
          mx = std::max(mx, e), over += e > 1;
        }
    std::printf("  window (%d,%d) %dx%d, %d px in: max code %d, values over 1: %zu\n", w.x0, w.y0, rg.width, rg.height, margin, mx, over);
    CHECK(mx <= 1);
  }
}

// ---- interactive ---------------------------------------------------------------------------------------------------------------------------------
void testInteractive() {
  const int W = 900, H = 1200;
  const SourceImage src = makeScene(W, H);
  putMatte("rs_matte", W, H);
  EditParams p = richEdit(W, H, true);
  EngineSession es;
  FrameResult a, b;
  CHECK(cpuFrame(es, p, src, true, a));
  CHECK(cpuFrame(es, p, src, false, b));
  if (a.rgba && b.rgba && a.w == b.w && a.h == b.h) {
    double s = 0;
    int mx = 0;
    for (std::size_t i = 0; i < std::size_t(a.w) * a.h * 4; ++i) {
      if (i % 4 == 3) continue;
      const int e = std::abs(int(a.rgba[i]) - int(b.rgba[i]));
      s += e, mx = std::max(mx, e);
    }
    s /= double(a.w) * a.h * 3;
    std::printf("  interactive (grid %d) vs exact (grid %d) frame %dx%d: mean %.3f levels, max %d\n", warp::kGridInteractive, warp::kGridExact, a.w, a.h, s, mx);
    CHECK(s < 1.0);
  }
  auto fi = warp::bind(p.reshape, W, H, true), fe = warp::bind(p.reshape, W, H, false);
  if (fi && fe) {
    double m = 0;
    for (int j = 0; j <= 400; ++j)
      for (int i = 0; i <= 300; ++i) {
        double a0, a1, b0, b1;
        warp::sampleUV(*fi, i / 300.0, j / 400.0, a0, a1);
        warp::sampleUV(*fe, i / 300.0, j / 400.0, b0, b1);
        m = std::max(m, std::hypot(a0 - b0, a1 - b1));
      }
    std::printf("  field hand-over: max |B_int - B_exact| %.3f px of the 1200 px long edge\n", m * 1200);
    CHECK(m * 1200 < 1.0);
  }
  std::free(a.rgba);
  std::free(b.rgba);
}

// ---- mapping -------------------------------------------------------------------------------------------------------------------------------------
void testMapPoint() {
  const int W = 900, H = 1200;
  putMatte("rs_matte", W, H);
  ReshapeParams r = richEdit(W, H, true).reshape;
  auto f = warp::bind(r, W, H, false);
  CHECK(f != nullptr);
  if (!f) return;
  double worst = 0, wu = 0, wv = 0;
  for (int j = 1; j < 40; ++j)
    for (int i = 1; i < 30; ++i) {
      const double u = i / 30.0, v = j / 40.0;
      double fu, fv, bu, bv;
      warp::forwardUV(*f, u, v, fu, fv);
      warp::backwardUV(*f, fu, fv, bu, bv);
      const double e = std::hypot((bu - u) * W, (bv - v) * H);
      if (e > worst) worst = e, wu = u, wv = v;
    }
  std::printf("  backward(forward(p)) - p: max %.4f px (at %.3f, %.3f)\n", worst, wu, wv);
  CHECK(worst < 0.05);
  // a radial mask placed on the eye follows the eye: the mask centre stays on the same content
  const SourceImage src = makeScene(W, H);
  EditParams p;
  p.reshape = r;
  ReshapeFace& fc = p.reshape.faces[0];
  fc.eyes = 100;
  Renderer rr(p, cam(), nullptr);
  FastOptions fo;
  FrameKernels fk(rr, src, nullptr, fo);
  // the output pixel showing the eye centre maps back to the eye centre's source position
  double eu, ev;
  auto fe = warp::bind(p.reshape, W, H, false);
  warp::forwardUV(*fe, fc.pts[2 * 36], fc.pts[2 * 36 + 1], eu, ev);
  float su, sv;
  fk.srcUV(int(eu * W), int(ev * H), su, sv);
  const double err = std::hypot((su - fc.pts[2 * 36]) * W, (sv - fc.pts[2 * 36 + 1]) * H);
  std::printf("  eye centre through srcUV: %.3f px off (pixel quantisation <= 0.71 x local scale)\n", err);
  CHECK(err < 1.0);
}

// ---- sharpness ---------------------------------------------------------------------------------------------------------------------------------
// smooth value noise with period `cell` px (band-limited texture, like a demosaicked photo's detail)
float vnoise(float x, float y, float cell) {
  const float fx = x / cell, fy = y / cell;
  const int ix = int(std::floor(fx)), iy = int(std::floor(fy));
  float tx = fx - ix, ty = fy - iy;
  tx = tx * tx * (3 - 2 * tx), ty = ty * ty * (3 - 2 * ty);
  auto v = [&](int a, int b) { return hash01(std::uint32_t(a + 9999), std::uint32_t(b + 9999)); };
  const float a = v(ix, iy) + (v(ix + 1, iy) - v(ix, iy)) * tx, b = v(ix, iy + 1) + (v(ix + 1, iy + 1) - v(ix, iy + 1)) * tx;
  return a + (b - a) * ty;
}
void testSharpness() {
  const int W = 512, H = 512;
  for (int tex = 0; tex < 2; ++tex) {
    SourceImage src = makeScene(W, H);
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) {
        float* p = src.linear.row(y) + 3 * std::size_t(x);
        const float t = tex == 0 ? 0.6f + 0.8f * hash01(std::uint32_t(x) * 7u, std::uint32_t(y) * 13u) : 0.5f + vnoise(float(x), float(y), 3.f);
        p[0] *= t, p[1] *= t, p[2] *= t;
      }
    EngineSession es;
    Image8 a;
    CHECK(exactRender(es, EditParams{}, src, ExecMode::Whole, 0, a));
    auto grad = [&](const Image8& im, int x0, int y0, int n) {
      double g = 0;
      for (int y = y0; y < y0 + n; ++y)
        for (int x = x0; x < x0 + n; ++x)
          for (int c = 0; c < 3; ++c) {
            const double d = double(im.data[(std::size_t(y) * W + x + 1) * 3 + c]) - im.data[(std::size_t(y) * W + x) * 3 + c];
            const double e = double(im.data[(std::size_t(y + 1) * W + x) * 3 + c]) - im.data[(std::size_t(y) * W + x) * 3 + c];
            g += d * d + e * e;
          }
      return g;
    };
    for (float shift : {0.25f, 0.5f, 3.5f}) {
      EditParams p;
      ReshapePush s;
      s.r = 0.25f;
      s.pts = {0.5f, 0.5f, 0.5f + shift / W, 0.5f};
      p.reshape.push = {s};
      Image8 b;
      CHECK(exactRender(es, p, src, ExecMode::Whole, 0, b));
      const double r = grad(b, 246, 246, 20) / grad(a, 246, 246, 20);
      std::printf("  %-34s shift %.2f px at the brush centre: gradient energy %.3f of the original (Lanczos-3)\n",
                  tex == 0 ? "white noise per pixel (worst case)" : "band-limited texture (3 px cells)", shift, r);
      if (tex == 1) CHECK(r > 0.8);
      // corner (outside the brush): the same bits
      bool same = true;
      for (int y = 0; y < 30; ++y)
        for (int x = 0; x < 30; ++x)
          for (int c = 0; c < 3; ++c) same &= a.data[(std::size_t(y) * W + x) * 3 + c] == b.data[(std::size_t(y) * W + x) * 3 + c];
      CHECK(same);
    }
  }
}

#ifdef DAPPLE_RESHAPE_GPU
std::shared_ptr<engine::gpu::Context> gpuContext() {
  std::string err;
  engine::gpu::ContextOptions co;
  co.device.allowSoftware = std::getenv("DAPPLE_GPU_SOFTWARE") != nullptr;
  auto ctx = engine::gpu::Context::create(co, err);
  if (!ctx) {
    std::printf("no usable GPU context: %s\n", err.c_str());
    std::exit(77);
  }
  std::printf("device: %s\n", ctx->core().dev().caps().text().c_str());
  return ctx;
}
void parityImpl(int W, int H, int exportTile) {
  auto ctx = gpuContext();
  const SourceImage src = makeScene(W, H);
  putMatte("rs_matte", W, H);
  struct Case {
    const char* name;
    std::function<void(EditParams&)> set;
    bool interactive;
  };
  const std::vector<Case> cases = {
      {"face ops", [&](EditParams& p) { p = faceEdit(W, H, 100, 50, 60, 100, 100); }, false},
      {"all ops + push + matte", [&](EditParams& p) { p = richEdit(W, H, true); }, false},
      {"all ops interactive", [&](EditParams& p) { p = richEdit(W, H, true); }, true},
      {"all ops + geometry", [&](EditParams& p) { p = richEdit(W, H, true); p.crop = "4:5", p.rotate = 90, p.straighten = 4.f, p.flip = true; }, false},
      {"all ops + hdr + local", [&](EditParams& p) {
         p = richEdit(W, H, true);
         p.hdr = 60.f;
         auto q = fromJson(R"({"local":{"v":1,"items":[{"id":"a","strength":100,"mask":[{"kind":"radial","op":"add","cx":0.5,"cy":0.25,"rx":0.15,"ry":0.12}],"adjust":{"exposure":30,"clarity":40}}]}})");
         if (q) p.local = q->local;
       }, false},
  };
  dapple::gpu::Rig rig(ctx, src, cam());
  EngineSession cpuEs;
  for (const Case& c : cases) {
    EditParams p;
    c.set(p);
    Renderer r(p, cam(), nullptr);
    FastOptions fo;
    fo.spatial = c.interactive ? SpatialQuality::Interactive : SpatialQuality::Full;
    fo.noise = false;
    std::string err;
    int gw = 0, gh = 0;
    engine::GpuStats st;
    std::uint8_t* gpuPx = rig.render(p, r, src, fo, 33, gw, gh, &st, err);
    FrameResult res;
    const bool okCpu = cpuFrame(cpuEs, p, src, c.interactive, res);
    if (!gpuPx || !okCpu || res.w != gw || res.h != gh) {
      std::printf("  %-28s render failed (%s %s)\n", c.name, err.c_str(), res.error.c_str());
      ++g_failures;
    } else {
      const dapple::gpu::DeStats d = dapple::gpu::compareRgba8(gpuPx, res.rgba, std::size_t(gw) * gh);
      std::printf("  %-28s %4dx%-4d mean %.4f p99 %.3f max %.2f  differing %.1f%%  (%s)\n", c.name, gw, gh, d.mean, d.p99, d.max, d.differing * 100, res.executor.c_str());
      CHECK(d.mean <= 0.15 && d.p99 <= 1.5);
    }
    std::free(gpuPx);
    std::free(res.rgba);
  }
  EditParams p = richEdit(W, H, true);
  Renderer r(p, cam(), nullptr);
  std::vector<std::uint8_t> a, b;
  int ew = 0, eh = 0;
  for (int pass = 0; pass < 2; ++pass) {
    std::vector<std::uint8_t>& dst = pass ? b : a;
    std::string err;
    engine::GpuStats st;
    const bool ok = rig.exportTiled(
        p, r, src, 0, 65, pass ? exportTile : 4096, pass == 1, 0.f, [&](int w, int h) { ew = w, eh = h, dst.assign(std::size_t(w) * h * 4, 0); },
        [&](int x0, int y0, int tw, int th, const std::uint8_t* px, std::size_t stride) {
          for (int j = 0; j < th; ++j) std::memcpy(&dst[(std::size_t(y0 + j) * ew + x0) * 4], px + std::size_t(j) * stride, std::size_t(tw) * 4);
          return true;
        },
        &st, err);
    if (!ok) std::printf("  export failed: %s\n", err.c_str());
    CHECK(ok);
  }
  std::size_t diff = 0;
  int mx = 0;
  for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
    const int d = std::abs(int(a[i]) - int(b[i]));
    diff += d != 0, mx = std::max(mx, d);
  }
  std::printf("  GPU tiled export (%d px tiles, streamed source windows) vs one tile %dx%d: max code %d, differing bytes %zu\n", exportTile, ew, eh, mx, diff);
  CHECK(mx <= 1);
}
void testParity() { parityImpl(480, 640, 128); }
#endif

// ---- bench (not in ctest) -------------------------------------------------------------------------------------------------------------------------
double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v.empty() ? -1 : v[v.size() / 2];
}
void testBench() {
  using Clock = std::chrono::steady_clock;
  const int W = 4000, H = 6000;
  const SourceImage big = makeScene(W, H);
  SourceImage small = big;
  small.linear = resizeToLongEdge(big.linear, 1080);
  putMatte("rs_matte", W, H);
  EditParams none;
  EditParams face = faceEdit(W, H, 60, 30, 30, 40, 40);
  face.reshape.matte = "rs_matte";
  EditParams all = richEdit(W, H, true);
  // field build times
  for (int tier = 0; tier < 2; ++tier) {
    std::vector<double> v;
    for (int i = 0; i < 5; ++i) {
      warp::resetCache();
      const auto t0 = Clock::now();
      warp::bind(all.reshape, W, H, tier == 0);
      v.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
    }
    std::printf("  field build (all ops + push + matte), %s grid %d: %.1f ms\n", tier == 0 ? "interactive" : "exact", tier == 0 ? warp::kGridInteractive : warp::kGridExact,
                median(v));
  }
#ifdef DAPPLE_RESHAPE_GPU
  auto ctx = gpuContext();
  dapple::gpu::Rig rig(ctx, big, cam());
#endif
  std::printf("  %-26s | %-15s | %-14s | %-11s | %-18s\n", "edit", "GPU 1080 int ms", "GPU 24 MP ms", "CPU 1080 ms", "CPU exact 24 MP ms");
  const std::pair<const char*, const EditParams*> eds[] = {{"none", &none}, {"face ops", &face}, {"all ops + push + matte", &all}};
  for (const auto& e : eds) {
    const EditParams& p = *e.second;
    double gi = -1, gf = -1, ci = -1, cf = -1;
#ifdef DAPPLE_RESHAPE_GPU
    Renderer rd(p, cam(), nullptr);
    for (int mode = 0; mode < 2; ++mode) {
      const SourceImage& s = mode ? big : small;
      FastOptions fo;
      fo.spatial = mode ? SpatialQuality::Full : SpatialQuality::Interactive;
      fo.noise = false;
      std::vector<double> wall;
      for (int i = 0; i < 7; ++i) {
        std::string err;
        engine::GpuStats st;
        int w = 0, h = 0;
        std::uint8_t* px = rig.render(p, rd, s, fo, 33, w, h, &st, err, 0);
        if (!px) {
          std::printf("  gpu render failed: %s\n", err.c_str());
          break;
        }
        std::free(px);
        if (i >= 2) wall.push_back(st.wallMs);
      }
      (mode ? gf : gi) = median(wall);
    }
#endif
    {
      EngineSession es;
      std::vector<double> v;
      for (int i = 0; i < 7; ++i) {
        FrameResult res;
        const auto t0 = Clock::now();
        if (!cpuFrame(es, p, small, true, res)) break;
        v.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
        std::free(res.rgba);
      }
      if (v.size() > 2) v.erase(v.begin(), v.begin() + 2);
      ci = median(v);
    }
    {
      EngineSession es;
      Image8 out;
      const auto t0 = Clock::now();
      exactRender(es, p, big, ExecMode::TiledExport, 0, out);
      cf = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    }
    std::printf("  %-26s | %15.2f | %14.2f | %11.1f | %18.0f\n", e.first, gi, gf, ci, cf);
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  struct T {
    const char* id;
    void (*fn)();
  };
  const T tests[] = {{"json", testJson},       {"identity", testIdentity}, {"bounded", testBounded},         {"protect", testProtect},
                     {"push", testPush},       {"tiles", testTiles},       {"region", testRegion},           {"interactive", testInteractive},
                     {"mappoint", testMapPoint}, {"sharpness", testSharpness}, {"body", testBody},
#ifdef DAPPLE_RESHAPE_GPU
                     {"parity", testParity},
#endif
  };
  const std::string id = argc > 1 ? argv[1] : "";
  if (id == "bench") {
    testBench();
    return g_failures ? 1 : 0;
  }
  bool ran = false;
  for (const T& t : tests)
    if (id.empty() || id == t.id) {
      std::printf("[%s]\n", t.id);
      t.fn();
      ran = true;
    }
  if (!ran) {
    std::printf("unknown test id '%s'\n", id.c_str());
    return 2;
  }
  std::printf(g_failures ? "FAILED (%d)\n" : "OK\n", g_failures);
  return g_failures ? 1 : 0;
}
