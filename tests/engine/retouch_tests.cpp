// Portrait retouch as local operations (docs/engine/pipeline-architecture.md section 43).
//   dapple_retouch_tests <id>   exit 77: no GPU (parity)
//     json         retouch ops / "retouch" object round trip, version 1 kept for edits without them, unknown ops and keys kept verbatim, clamping
//     bands        band identity (L_c + (L_f - L_c) + (Y - L_f) = Y), the high band is not touched by smoothing (bound), the mid band is reduced
//     scale        the same face at two resolutions gives the same retouch after resampling (bound)
//     blobs        blemish found on clean skin; freckles kept at the default sensitivity (found at the highest)
//     heal         heal spots: deterministic, tile-exact (CPU tiled == whole), the spot takes the surrounding tone and keeps texture
//     whiten       the whitening lift never passes its ceiling; warm chroma goes, other hues stay
//     identity     zero-strength retouch: no Local node, bit-identical; the old Renderer ignores retouch ops; legacy channel layout unchanged
//     tiles        CPU tiled == whole with every retouch op + heal + a legacy op, plain and under crop / rotate / straighten / flip
//     interactive  interactive frame vs the exact frame downsampled (bound)
//     undereye     the under-eye crescent plane builder (AI service) on a synthetic mesh
//     parity       GPU executor vs CPU executor, and a tiled GPU export vs one tile
//   not in ctest: bench (added cost per op, 1080 interactive and 24 MP, GPU and CPU)
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "ai_masks.h"
#include "dapple/camera.h"
#include "dapple/decoder.h"
#include "dapple/edit_params.h"
#include "dapple/local_adjust.h"
#include "dapple/renderer.h"
#include "engine/exec/cpu_executor.h"
#include "engine/exec/engine_session.h"
#include "frame_kernels.h"
#include "local_eval.h"
#include "local_retouch.h"
#ifdef DAPPLE_RETOUCH_GPU
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

float hash01(std::uint32_t x, std::uint32_t y, std::uint32_t s) {
  std::uint32_t h = x * 0x8DA6B343u ^ y * 0xD8163841u ^ s * 0xCB1AB31Fu;
  h ^= h >> 13, h *= 0x5BD1E995u, h ^= h >> 15;
  return float(h & 0xFFFFFF) / float(0xFFFFFF);
}
// smooth value noise in photo space (period `cell`, metric units)
float vnoise(float X, float Y, float cell, std::uint32_t seed) {
  const float x = X / cell, y = Y / cell;
  const int ix = int(std::floor(x)), iy = int(std::floor(y));
  float tx = x - float(ix), ty = y - float(iy);
  tx = tx * tx * (3 - 2 * tx), ty = ty * ty * (3 - 2 * ty);
  auto v = [&](int a, int b) { return hash01(std::uint32_t(a + 4096), std::uint32_t(b + 4096), seed) * 2.f - 1.f; };
  const float a = v(ix, iy) + (v(ix + 1, iy) - v(ix, iy)) * tx, b = v(ix, iy + 1) + (v(ix + 1, iy + 1) - v(ix, iy + 1)) * tx;
  return a + (b - a) * ty;
}

// A synthetic portrait (scene-linear, photo space): face ellipse with shading, pores (per pixel), mottling and red blotches (value noise at a
// fraction of the inter-ocular distance), three blemishes, eyes, mouth, darker under-eye crescents, a shiny forehead patch. iod = 0.16 of the
// long edge, face centre (0.5, 0.52) of a 4:5 portrait.
struct Portrait {
  float cx = 0.5f, cy = 0.52f, iod = 0.16f;
  float ax() const { return 1.05f * iod; }
  float ay() const { return 1.40f * iod; }
};
const Portrait kP;

SourceImage makePortrait(int W, int H, bool blemishes = true, std::uint32_t seed = 3) {
  SourceImage s;
  s.sceneReferred = true;
  s.meta.iso = 100.f;
  s.linear = ImageF(W, H);
  const float L = float(std::max(W, H)), sx = float(W) / L, sy = float(H) / L;
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      const float X = (float(x) + 0.5f) / L, Y = (float(y) + 0.5f) / L;  // metric
      const float fx = (X - kP.cx * sx) / kP.ax(), fy = (Y - kP.cy * sy) / kP.ay();
      const float e = fx * fx + fy * fy;
      float r = 0.10f + 0.05f * Y, g = 0.12f + 0.04f * Y, b = 0.16f;  // background
      if (e < 1.f) {
        const float shade = 0.62f + 0.38f * (1.f - 0.8f * e) * (0.85f + 0.15f * (-fx));  // 3D form, light from the left
        const float mott = 1.f + 0.035f * vnoise(X, Y, 0.09f * kP.iod, seed);
        const float red = 0.04f * std::max(0.f, vnoise(X, Y, 0.12f * kP.iod, seed + 7));
        const float pore = 1.f + 0.05f * (hash01(std::uint32_t(x), std::uint32_t(y), seed + 1) - 0.5f);
        float k = shade * mott * pore;
        r = 0.46f * k * (1.f + 2.f * red), g = 0.29f * k * (1.f - red), b = 0.21f * k;
        // under-eye crescents (darker, bluish)
        for (int side = -1; side <= 1; side += 2) {
          const float ex = (X - (kP.cx * sx + side * 0.5f * kP.iod)) / (0.28f * kP.iod), ey = (Y - (kP.cy * sy - 0.20f * kP.iod)) / (0.10f * kP.iod);
          if (ex * ex + ey * ey < 1.f) {
            const float w = 1.f - (ex * ex + ey * ey);
            r *= 1.f - 0.25f * w, g *= 1.f - 0.22f * w, b *= 1.f - 0.08f * w;
          }
          // eye
          const float qx = (X - (kP.cx * sx + side * 0.5f * kP.iod)) / (0.22f * kP.iod), qy = (Y - (kP.cy * sy - 0.36f * kP.iod)) / (0.09f * kP.iod);
          if (qx * qx + qy * qy < 1.f) r = 0.05f, g = 0.04f, b = 0.04f;
        }
        // forehead shine
        const float hx = (X - kP.cx * sx) / (0.25f * kP.iod), hy = (Y - (kP.cy * sy - 0.95f * kP.iod)) / (0.12f * kP.iod);
        if (hx * hx + hy * hy < 1.f) {
          const float w = 1.f - (hx * hx + hy * hy);
          r += 0.35f * w, g += 0.33f * w, b += 0.32f * w;
        }
        // mouth
        const float mx = (X - kP.cx * sx) / (0.35f * kP.iod), my = (Y - (kP.cy * sy + 0.75f * kP.iod)) / (0.08f * kP.iod);
        if (mx * mx + my * my < 1.f) r = 0.35f, g = 0.10f, b = 0.10f;
        if (blemishes) {
          const float bl[3][2] = {{-0.45f, 0.25f}, {0.40f, 0.35f}, {-0.15f, 1.05f}};
          for (const auto& q : bl) {
            const float dx = X - (kP.cx * sx + q[0] * kP.iod), dy = Y - (kP.cy * sy + q[1] * kP.iod), rr = 0.03f * kP.iod;
            const float d2 = (dx * dx + dy * dy) / (rr * rr);
            if (d2 < 4.f) {
              const float w = std::exp(-d2);
              r *= 1.f - 0.10f * w, g *= 1.f - 0.30f * w, b *= 1.f - 0.25f * w;  // a red, darker spot
            }
          }
        }
      }
      float* p = s.linear.row(y) + 3 * std::size_t(x);
      p[0] = r, p[1] = g, p[2] = b;
    }
  return s;
}

// face-skin raster (the ellipse minus eyes and mouth, soft), 512 px long edge, photo space
void putSkinRaster(const char* id, int W, int H) {
  const int L = 512, w = W >= H ? L : int(std::lround(512.f * W / H)), h = H >= W ? L : int(std::lround(512.f * H / W));
  const float sx = float(W) / float(std::max(W, H)), sy = float(H) / float(std::max(W, H));
  std::vector<std::uint8_t> px(std::size_t(w) * h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const float X = (x + 0.5f) / w * sx, Y = (y + 0.5f) / h * sy;
      const float fx = (X - kP.cx * sx) / kP.ax(), fy = (Y - kP.cy * sy) / kP.ay();
      float m = 1.f - std::clamp((std::sqrt(fx * fx + fy * fy) - 0.93f) / 0.07f, 0.f, 1.f);
      for (int side = -1; side <= 1; side += 2) {
        const float qx = (X - (kP.cx * sx + side * 0.5f * kP.iod)) / (0.30f * kP.iod), qy = (Y - (kP.cy * sy - 0.36f * kP.iod)) / (0.15f * kP.iod);
        if (qx * qx + qy * qy < 1.f) m = 0.f;
      }
      const float mx = (X - kP.cx * sx) / (0.45f * kP.iod), my = (Y - (kP.cy * sy + 0.75f * kP.iod)) / (0.14f * kP.iod);
      if (mx * mx + my * my < 1.f) m = 0.f;
      px[std::size_t(y) * w + x] = std::uint8_t(m * 255.f + 0.5f);
    }
  local::rasterSet(id, w, h, px.data(), w);
}
// under-eye raster (two crescents)
void putUnderEyeRaster(const char* id, int W, int H) {
  const int L = 512, w = W >= H ? L : int(std::lround(512.f * W / H)), h = H >= W ? L : int(std::lround(512.f * H / W));
  const float sx = float(W) / float(std::max(W, H)), sy = float(H) / float(std::max(W, H));
  std::vector<std::uint8_t> px(std::size_t(w) * h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const float X = (x + 0.5f) / w * sx, Y = (y + 0.5f) / h * sy;
      float m = 0.f;
      for (int side = -1; side <= 1; side += 2) {
        const float ex = (X - (kP.cx * sx + side * 0.5f * kP.iod)) / (0.32f * kP.iod), ey = (Y - (kP.cy * sy - 0.20f * kP.iod)) / (0.13f * kP.iod);
        m = std::max(m, std::clamp(1.2f * (1.f - (ex * ex + ey * ey)), 0.f, 1.f));
      }
      px[std::size_t(y) * w + x] = std::uint8_t(m * 255.f + 0.5f);
    }
  local::rasterSet(id, w, h, px.data(), w);
}

LocalAdjustment rasterAdj(const char* id, const char* ref) {
  LocalAdjustment a;
  a.id = id;
  MaskComponent m;
  m.kind = MaskKind::Raster;
  m.ref = ref;
  m.refine = 40.f;
  a.mask.push_back(m);
  a.face = kP.iod;
  return a;
}

// the retouch edit used by most engine tests: skin (smooth, even, redness, shine, heal, texture), under-eye, face light, a manual spot
std::vector<LocalAdjustment> retouchEdit(bool legacyToo = true) {
  std::vector<LocalAdjustment> L;
  LocalAdjustment skin = rasterAdj("skin", "rt_skin");
  skin.ops.v[kLoSmooth] = 60.f;
  skin.ops.v[kLoEven] = 50.f;
  skin.ops.v[kLoRedness] = 50.f;
  skin.ops.v[kLoShine] = 60.f;
  skin.ops.v[kLoHeal] = 100.f;
  if (legacyToo) skin.ops.v[kLoTexture] = 15.f;
  L.push_back(skin);
  LocalAdjustment ue = rasterAdj("ue", "rt_ue");
  ue.ops.v[kLoUnderEye] = 70.f;
  L.push_back(ue);
  LocalAdjustment face = rasterAdj("light", "rt_skin");
  face.ops.v[kLoFaceLight] = 40.f;
  face.ops.v[kLoTemp] = 8.f;
  L.push_back(face);
  LocalAdjustment spot;
  spot.id = "spot";
  HealSpot h;
  h.x = (kP.cx * 0.8f + 0.3f * kP.iod) / 0.8f, h.y = kP.cy + 0.3f * kP.iod, h.r = 0.006f;  // on the cheek (4:5 portraits)
  spot.spots.push_back(h);
  L.push_back(spot);
  return L;
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

void putRasters(int W, int H) {
  putSkinRaster("rt_skin", W, H);
  putUnderEyeRaster("rt_ue", W, H);
}

// display-referred skin patch for the unit tests of the bands: sRGB-encoded values of the portrait
ImageF displayPatch(const SourceImage& s) {
  ImageF o(s.linear.width, s.linear.height);
  for (int y = 0; y < o.height; ++y)
    for (int x = 0; x < o.width * 3; ++x) {
      const float v = std::clamp(s.linear.row(y)[x] * 1.6f, 0.f, 1.f);
      o.row(y)[x] = v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f;
    }
  return o;
}

// a Frame + one amount plane for retouchCombineWindow: op `op` at amount `k` everywhere inside the face ellipse (soft), 0 outside
struct UnitFrame {
  local::Frame f;
  local::AuxTarget aux;
  std::vector<float> data;
};
void unitFrame(UnitFrame& u, const ImageF& img, int op, float k) {
  u.f = local::Frame{};
  u.f.chanOp[0] = op;
  u.f.winUsed[op - kLocalWindowFirst] = true;
  u.f.retouchW = true;
  u.f.anyWindow = true;
  const int w = img.width, h = img.height;
  u.data.assign(std::size_t(w) * h, 0.f);
  const float L = float(std::max(w, h)), sx = float(w) / L, sy = float(h) / L;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const float X = (x + 0.5f) / L, Y = (y + 0.5f) / L;
      const float fx = (X - kP.cx * sx) / kP.ax(), fy = (Y - kP.cy * sy) / kP.ay();
      const float e = std::sqrt(fx * fx + fy * fy);
      u.data[std::size_t(y) * w + x] = k * std::clamp((0.9f - e) / 0.1f, 0.f, 1.f);
    }
  u.aux.data = u.data.data();
  u.aux.ox = 0, u.aux.oy = 0, u.aux.w = w, u.aux.h = h;
  u.aux.plane = std::size_t(w) * h;
  for (int& s : u.aux.slot) s = -1;
  u.aux.slot[0] = 0;
}

std::vector<float> lumaOf(const ImageF& im) {
  std::vector<float> Y(std::size_t(im.width) * im.height);
  for (int y = 0; y < im.height; ++y)
    for (int x = 0; x < im.width; ++x) {
      const float* p = im.row(y) + 3 * std::size_t(x);
      Y[std::size_t(y) * im.width + x] = 0.2126729f * p[0] + 0.7151522f * p[1] + 0.0721750f * p[2];
    }
  return Y;
}

// ---- json ---------------------------------------------------------------------------------------------------------------------------------
void testJson() {
  std::vector<LocalAdjustment> L = retouchEdit();
  L[0].healSens = 40.f;
  L[3].spots.push_back({0.3f, 0.4f, 0.005f, 0.01f, -0.005f, true});
  L.push_back(rasterAdj("teeth", "rt_skin"));
  L.back().ops.v[kLoWhiten] = 45.f;
  const std::string j = localToJson(L);
  std::vector<LocalAdjustment> back;
  CHECK(localFromJson(j, back));
  CHECK(localToJson(back) == j);
  CHECK(j.rfind("{\"v\":2,", 0) == 0);
  CHECK(back.size() == L.size() && back[3].spots.size() == 2 && back[3].spots[1].src && !back[3].spots[0].src);
  CHECK(std::fabs(back[0].face - kP.iod) < 1e-6f && back[0].healSens == 40.f && back[4].ops.v[kLoWhiten] == 45.f);
  CHECK(localHash(back) == localHash(L));
  std::printf("  retouch json %zu bytes, round trip identical\n", j.size());
  // an edit without retouch stays version 1 and its hash does not see the new fields
  std::vector<LocalAdjustment> old(1);
  old[0].id = "a";
  MaskComponent m;
  old[0].mask.push_back(m);
  old[0].ops.v[kLoExposure] = 0.5f;
  const std::string jo = localToJson(old);
  CHECK(jo.rfind("{\"v\":1,", 0) == 0 && jo.find("retouch") == std::string::npos);
  // a newer build's op and item key: kept verbatim, written back
  const std::string newer =
      R"({"v":3,"items":[{"id":"x","strength":100,"mask":[{"kind":"radial","op":"add"}],"adjust":{"smooth":30,"glow":12,"future_op":{"a":1}},"halo":[1,2]}]})";
  std::vector<LocalAdjustment> nb;
  CHECK(localFromJson(newer, nb) && nb.size() == 1);
  const std::string wb = localToJson(nb);
  CHECK(wb.find("\"glow\":12") != std::string::npos && wb.find("\"future_op\":{\"a\":1}") != std::string::npos &&
        wb.find("\"halo\":[1,2]") != std::string::npos && nb[0].ops.v[kLoSmooth] == 30.f);
  // clamping: radius, offset within 4 radii, sensitivity, spot cap
  std::vector<LocalAdjustment> c;
  CHECK(localFromJson(R"({"v":2,"items":[{"id":"c","adjust":{"heal":250},"retouch":{"face":-1,"sens":900,"spots":[[0.5,0.5,1,0.5,0.5],[0.1,0.1]]}}]})", c));
  CHECK(c.size() == 1 && c[0].ops.v[kLoHeal] == 100.f && c[0].healSens == 100.f && c[0].face == 0.f && c[0].spots.size() == 1);
  if (!c.empty() && !c[0].spots.empty()) CHECK(c[0].spots[0].r == 0.02f && std::hypot(c[0].spots[0].dx, c[0].spots[0].dy) <= 0.08f + 1e-6f);
}

// ---- bands -------------------------------------------------------------------------------------------------------------------------------
double rms(const std::vector<float>& a, const std::vector<float>& b, const std::vector<float>* mask = nullptr) {
  double s = 0, n = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const double w = mask ? (*mask)[i] : 1.0;
    s += w * double(a[i] - b[i]) * (a[i] - b[i]), n += w;
  }
  return std::sqrt(s / std::max(n, 1e-9));
}

void testBands() {
  const SourceImage s = makePortrait(800, 1000, false);
  const ImageF in = displayPatch(s);
  UnitFrame u;
  unitFrame(u, in, kLoSmooth, 1.f);
  const local::RetouchRadii R = local::retouchRadii(kP.iod * 1000.f);
  local::RtPlanes P0;
  local::retouchPlanes(u.f, in, u.aux, R, P0);
  const std::vector<float> Y0 = lumaOf(in);
  double idErr = 0;
  std::vector<float> H0(Y0.size()), M0(Y0.size()), inner(Y0.size());
  for (int y = 0; y < in.height; ++y)
    for (int x = 0; x < in.width; ++x) {
      const std::size_t i = std::size_t(y) * in.width + x;
      const float Lc = P0.Lc(x, y), Lf = P0.Lf[i];
      H0[i] = Y0[i] - Lf, M0[i] = Lf - Lc;
      idErr = std::max(idErr, double(std::fabs(Lc + M0[i] + H0[i] - Y0[i])));
      inner[i] = u.data[i] >= 1.f ? 1.f : 0.f;  // the fully selected skin
    }
  std::printf("  band identity: max |L_c + M + H - Y| = %.2e (radii rf %d rc %d rs %d at iod %.0f px)\n", idErr, R.rf, R.rc, R.rs, R.iodPx);
  CHECK(idErr < 1e-5);
  for (float k : {0.5f, 1.f}) {
    UnitFrame uk;
    unitFrame(uk, in, kLoSmooth, k);
    ImageF out = in;
    local::retouchCombineWindow(uk.f, out, uk.aux, R);
    // decompose the output with the same filters (its own L_f; the reference L_c of the input)
    local::RtPlanes P1;
    local::retouchPlanes(uk.f, out, uk.aux, R, P1);
    const std::vector<float> Y1 = lumaOf(out);
    std::vector<float> H1(Y1.size()), M1(Y1.size()), zero(Y1.size(), 0.f);
    for (int y = 0; y < out.height; ++y)
      for (int x = 0; x < out.width; ++x) {
        const std::size_t i = std::size_t(y) * out.width + x;
        H1[i] = Y1[i] - P1.Lf[i], M1[i] = P1.Lf[i] - P0.Lc(x, y);
      }
    const double hIn = rms(H0, zero, &inner), hDiff = rms(H1, H0, &inner), mIn = rms(M0, zero, &inner), mOut = rms(M1, zero, &inner);
    std::printf("  smooth %3.0f: high band rms %.5f, change %.5f (%.1f %%); mid band rms %.5f -> %.5f (%.0f %% removed)\n", k * 100, hIn, hDiff,
                100 * hDiff / hIn, mIn, mOut, 100 * (1 - mOut / mIn));
    CHECK(hDiff / hIn < 0.12);
    CHECK(mOut / mIn < (k >= 1.f ? 0.35 : 0.7));
  }
  // zero amount: untouched (bit-exact)
  UnitFrame z;
  unitFrame(z, in, kLoSmooth, 0.f);
  ImageF out = in;
  local::retouchCombineWindow(z.f, out, z.aux, R);
  CHECK(std::memcmp(out.row(0), in.row(0), std::size_t(in.width) * in.height * 3 * sizeof(float)) == 0);
}

// ---- complexion invariance (section 43, round 2) --------------------------------------------------------------------------------------------
// several ops in one frame, each on its own channel, amount k inside the face ellipse
void unitFrameOps(UnitFrame& u, const ImageF& img, const std::vector<std::pair<int, float>>& ops) {
  u.f = local::Frame{};
  u.f.retouchW = true;
  u.f.anyWindow = true;
  const int w = img.width, h = img.height;
  const std::size_t N = std::size_t(w) * h;
  u.data.assign(N * ops.size(), 0.f);
  const float L = float(std::max(w, h)), sx = float(w) / L, sy = float(h) / L;
  for (int& s : u.aux.slot) s = -1;
  for (std::size_t c = 0; c < ops.size(); ++c) {
    u.f.chanOp[c] = ops[c].first;
    u.f.winUsed[ops[c].first - kLocalWindowFirst] = true;
    u.aux.slot[c] = int(c);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        const float X = (x + 0.5f) / L, Y = (y + 0.5f) / L;
        const float fx = (X - kP.cx * sx) / kP.ax(), fy = (Y - kP.cy * sy) / kP.ay();
        u.data[c * N + std::size_t(y) * w + x] = ops[c].second * std::clamp((0.9f - std::sqrt(fx * fx + fy * fy)) / 0.1f, 0.f, 1.f);
      }
  }
  u.aux.data = u.data.data();
  u.aux.ox = 0, u.aux.oy = 0, u.aux.w = w, u.aux.h = h;
  u.aux.plane = N;
}

// OKLab of an sRGB-encoded pixel
void okLab(const float* p, float o[3]) {
  float l[3];
  for (int c = 0; c < 3; ++c) l[c] = p[c] <= 0.04045f ? p[c] / 12.92f : std::pow((p[c] + 0.055f) / 1.055f, 2.4f);
  const float L = std::cbrt(0.4122214708f * l[0] + 0.5363325363f * l[1] + 0.0514459929f * l[2]);
  const float M = std::cbrt(0.2119034982f * l[0] + 0.6806995451f * l[1] + 0.1073969566f * l[2]);
  const float S = std::cbrt(0.0883024619f * l[0] + 0.2817188376f * l[1] + 0.6299787005f * l[2]);
  o[0] = 0.2104542553f * L + 0.7936177850f * M - 0.0040720468f * S;
  o[1] = 1.9779984951f * L - 2.4285922050f * M + 0.4505937099f * S;
  o[2] = 0.0259040371f * L + 0.7827717662f * M - 0.8086757660f * S;
}
struct Complexion {
  double L = 0, a = 0, b = 0, sat = 0, n = 0;
};
// mean OKLab and mean saturation (chroma / lightness) over the pixels where `w` >= 0.99 (and optionally inside a disc to skip)
Complexion complexionOf(const ImageF& im, const std::vector<float>& w, float skipX = -1, float skipY = -1, float skipR = 0) {
  Complexion c;
  for (int y = 0; y < im.height; ++y)
    for (int x = 0; x < im.width; ++x) {
      if (w[std::size_t(y) * im.width + x] < 0.99f) continue;
      if (skipR > 0 && (x - skipX) * (x - skipX) + (y - skipY) * (y - skipY) < skipR * skipR) continue;
      float o[3];
      okLab(im.row(y) + 3 * std::size_t(x), o);
      c.L += o[0], c.a += o[1], c.b += o[2], c.sat += std::sqrt(o[1] * o[1] + o[2] * o[2]) / std::max(o[0], 0.05f), c.n += 1;
    }
  if (c.n > 0) c.L /= c.n, c.a /= c.n, c.b /= c.n, c.sat /= c.n;
  return c;
}
double dE(const Complexion& p, const Complexion& q) { return 100.0 * std::sqrt((p.L - q.L) * (p.L - q.L) + (p.a - q.a) * (p.a - q.a) + (p.b - q.b) * (p.b - q.b)); }

// tan skin: shading, a cheek blush (low frequency, part of the face), pores, mild mottling and two red blotches; display-encoded
ImageF tanPatch(int w, int h, float iodPx, bool blotches) {
  ImageF im(w, h);
  const float cx = 0.5f * w, cy = 0.5f * h;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const float dx = (x - cx) / iodPx, dy = (y - cy) / iodPx;
      const float shade = 1.f - 0.10f * dx - 0.05f * (dx * dx + dy * dy);
      const float blush = std::exp(-((dx - 0.3f) * (dx - 0.3f) + (dy - 0.2f) * (dy - 0.2f)) / (2 * 0.3f * 0.3f));
      float r = 0.80f * shade + 0.05f * blush, g = 0.60f * shade - 0.02f * blush, b = 0.47f * shade - 0.01f * blush;
      const float n = 0.02f * (hash01(std::uint32_t(x), std::uint32_t(y), 21) - 0.5f) + 0.012f * vnoise(float(x), float(y), 0.1f * iodPx, 22);
      r += n, g += n, b += n;
      if (blotches) {
        const float bl[2][2] = {{-0.35f, -0.2f}, {0.15f, 0.35f}};
        for (const auto& q : bl) {
          const float e = ((dx - q[0]) * (dx - q[0]) + (dy - q[1]) * (dy - q[1])) / (0.05f * 0.05f);
          const float wgt = std::exp(-0.5f * e);
          r += 0.04f * wgt, g -= 0.07f * wgt, b -= 0.04f * wgt;
        }
      }
      float* p = im.row(y) + 3 * std::size_t(x);
      p[0] = std::clamp(r, 0.f, 1.f), p[1] = std::clamp(g, 0.f, 1.f), p[2] = std::clamp(b, 0.f, 1.f);
    }
  return im;
}
// red excess (OKLab a) of a disc relative to a ring around it
double redExcess(const ImageF& im, float cx, float cy, float r) {
  double in = 0, ring = 0;
  int ni = 0, nr = 0;
  for (int y = int(cy - 2.5f * r); y <= int(cy + 2.5f * r); ++y)
    for (int x = int(cx - 2.5f * r); x <= int(cx + 2.5f * r); ++x) {
      if (x < 0 || y < 0 || x >= im.width || y >= im.height) continue;
      const float d = std::hypot(x - cx, y - cy);
      float o[3];
      okLab(im.row(y) + 3 * std::size_t(x), o);
      if (d < r) in += o[1], ++ni;
      else if (d > 1.8f * r && d < 2.5f * r) ring += o[1], ++nr;
    }
  return ni && nr ? in / ni - ring / nr : 0;
}

// per-channel pixel noise (sensor / demosaic chroma noise and the colour texture of real skin: pores are slightly redder, fine capillaries)
void addChromaNoise(ImageF& im, float amp) {
  for (int y = 0; y < im.height; ++y)
    for (int x = 0; x < im.width; ++x)
      for (int c = 0; c < 3; ++c) {
        float& v = im.row(y)[3 * x + c];
        v = std::clamp(v + amp * (hash01(std::uint32_t(x), std::uint32_t(y), 40u + std::uint32_t(c)) - 0.5f), 0.f, 1.f);
      }
}

void testComplexion() {
  // (a) tan patch: the blotches go, the blush and the tan stay
  const int w = 600, h = 600;
  const float iod = 300.f;
  for (float k : {0.5f, 1.f}) {
    ImageF in = tanPatch(w, h, iod, true), clean = tanPatch(w, h, iod, false);
    addChromaNoise(in, 0.05f), addChromaNoise(clean, 0.05f);
    std::vector<float> all(std::size_t(w) * h, 1.f);
    UnitFrame u;
    u.f = local::Frame{};
    unitFrameOps(u, in, {{kLoEven, k}, {kLoRedness, k}});
    std::fill(u.data.begin(), u.data.end(), k);  // the whole patch is skin
    ImageF out = in;
    local::retouchCombineWindow(u.f, out, u.aux, local::retouchRadii(iod));
    const Complexion c0 = complexionOf(in, all), c1 = complexionOf(out, all), cc = complexionOf(clean, all);
    const float bx = 0.5f * w + (-0.35f) * iod, by = 0.5f * h + (-0.2f) * iod, br = 0.05f * iod;
    const double ex0 = redExcess(in, bx, by, br), ex1 = redExcess(out, bx, by, br);
    const float ux = 0.5f * w + 0.3f * iod, uy = 0.5f * h + 0.2f * iod;  // blush centre vs the far side of the patch
    float ob0[3], ob1[3], of0[3], of1[3];
    okLab(in.row(int(uy)) + 3 * std::size_t(ux), ob0), okLab(out.row(int(uy)) + 3 * std::size_t(ux), ob1);
    okLab(in.row(int(0.5f * h - 0.3f * iod)) + 3 * std::size_t(0.5f * w - 0.6f * iod), of0);
    okLab(out.row(int(0.5f * h - 0.3f * iod)) + 3 * std::size_t(0.5f * w - 0.6f * iod), of1);
    const double blush0 = ob0[1] - of0[1], blush1 = ob1[1] - of1[1];
    std::printf("  tan patch, even + redness %3.0f: skin mean dE %.2f (vs the clean patch %.2f), saturation ratio %.4f; blotch red excess %.4f -> %.4f (%.0f %% "
                "removed); blush (a) %.4f -> %.4f\n",
                k * 100, dE(c0, c1), dE(cc, c1), c1.sat / c0.sat, ex0, ex1, 100 * (1 - ex1 / ex0), blush0, blush1);
    CHECK(dE(c0, c1) <= 1.0);
    CHECK(std::fabs(c1.sat / c0.sat - 1.0) <= 0.02);
    CHECK(ex1 < (k >= 1.f ? 0.4 : 0.7) * ex0);
    CHECK(blush1 > 0.8 * blush0);
  }
  // (b) the synthetic portrait at the app's default recipe and at the maxima: each op alone and together
  const SourceImage s = makePortrait(800, 1000, false);
  ImageF in = displayPatch(s);
  addChromaNoise(in, 0.05f);
  const local::RetouchRadii R = local::retouchRadii(kP.iod * 1000.f);
  struct Case {
    const char* name;
    std::vector<std::pair<int, float>> ops;
  };
  const Case cases[] = {{"default skin recipe (30 / 25 / 30)", {{kLoSmooth, 0.3f}, {kLoEven, 0.25f}, {kLoRedness, 0.3f}}},
                        {"maxima (100 / 60 / 70)", {{kLoSmooth, 1.f}, {kLoEven, 0.6f}, {kLoRedness, 0.7f}}},
                        {"even 100", {{kLoEven, 1.f}}},
                        {"redness 100", {{kLoRedness, 1.f}}},
                        {"smooth 100", {{kLoSmooth, 1.f}}},
                        {"shine 80 (skin outside the forehead shine)", {{kLoShine, 0.8f}}}};
  for (const Case& c : cases) {
    UnitFrame u;
    unitFrameOps(u, in, c.ops);
    ImageF out = in;
    local::retouchCombineWindow(u.f, out, u.aux, R);
    std::vector<float> inner(u.aux.plane);
    for (std::size_t i = 0; i < inner.size(); ++i) inner[i] = u.data[i] >= c.ops[0].second * 0.999f ? 1.f : 0.f;
    // shine: measure the skin outside the specular patch (forehead, 0.95 iod above the centre)
    const float sx = kP.cx * 800.f, sy = (kP.cy - 0.95f * kP.iod) * 1000.f, sr = c.ops[0].first == kLoShine ? 0.4f * kP.iod * 1000.f : 0.f;
    const Complexion c0 = complexionOf(in, inner, sx, sy, sr), c1 = complexionOf(out, inner, sx, sy, sr);
    std::printf("  portrait, %-44s skin mean dE %.2f, L %+.4f, saturation ratio %.4f\n", c.name, dE(c0, c1), c1.L - c0.L, c1.sat / c0.sat);
    CHECK(dE(c0, c1) <= 1.0);
    CHECK(std::fabs(c1.sat / c0.sat - 1.0) <= 0.02);
  }
}

// ---- one retouch scale for a near and a far face (section 43.1) --------------------------------------------------------------------------------
// A near face (iod 160 px) and a far face (iod 53 px, a third) retouched at their own scale, at the near face's and at the far face's scale:
// the high-band change (texture) and the mid-band reduction (the retouch) of each face. Writes build/retouch/r2/twoscales.ppm (rows: near,
// far; columns: original | own scale | near-face scale | far-face scale) when DAPPLE_RETOUCH_SHEETS is set.
struct BandEffect {
  double hChange = 0, mRemoved = 0;
};
BandEffect smoothAt(const ImageF& in, float iodRun, float iodTruth, ImageF* outImg) {
  UnitFrame u;
  unitFrame(u, in, kLoSmooth, 1.f);
  ImageF out = in;
  local::retouchCombineWindow(u.f, out, u.aux, local::retouchRadii(iodRun));
  // measured with the face's TRUE bands (its own scale)
  const local::RetouchRadii T = local::retouchRadii(iodTruth);
  local::RtPlanes P0, P1;
  local::retouchPlanes(u.f, in, u.aux, T, P0);
  local::retouchPlanes(u.f, out, u.aux, T, P1);
  const std::vector<float> Y0 = lumaOf(in), Y1 = lumaOf(out);
  double h2 = 0, dh2 = 0, m0 = 0, m1 = 0, n = 0;
  for (int y = 0; y < in.height; ++y)
    for (int x = 0; x < in.width; ++x) {
      const std::size_t i = std::size_t(y) * in.width + x;
      if (u.data[i] < 1.f) continue;
      const double H0 = Y0[i] - P0.Lf[i], H1 = Y1[i] - P1.Lf[i], M0 = P0.Lf[i] - P0.Lc(x, y), M1 = P1.Lf[i] - P0.Lc(x, y);
      h2 += H0 * H0, dh2 += (H1 - H0) * (H1 - H0), m0 += M0 * M0, m1 += M1 * M1, n += 1;
    }
  if (outImg) *outImg = out;
  BandEffect e;
  e.hChange = std::sqrt(dh2 / h2), e.mRemoved = 1.0 - std::sqrt(m1 / m0);
  return e;
}
void testTwoScales() {
  const SourceImage nearS = makePortrait(800, 1000, false), farS = makePortrait(267, 333, false);
  ImageF nearI = displayPatch(nearS), farI = displayPatch(farS);
  addChromaNoise(nearI, 0.03f), addChromaNoise(farI, 0.03f);
  const float iN = kP.iod * 1000.f, iF = kP.iod * 333.f;
  ImageF tiles[2][4];
  tiles[0][0] = nearI, tiles[1][0] = farI;
  const float runs[3] = {0, iN, iF};
  const char* names[3] = {"own scale", "near-face scale", "far-face scale"};
  BandEffect e[2][3];
  for (int face = 0; face < 2; ++face)
    for (int r = 0; r < 3; ++r) {
      const ImageF& in = face ? farI : nearI;
      const float truth = face ? iF : iN;
      e[face][r] = smoothAt(in, r == 0 ? truth : runs[r], truth, &tiles[face][r + 1]);
      std::printf("  %s face (iod %3.0f px) smoothed 100 at the %-16s: high band changed %5.1f %%, mid band removed %5.1f %%\n", face ? "far " : "near", truth,
                  names[r], 100 * e[face][r].hChange, 100 * e[face][r].mRemoved);
    }
  // the rule (the largest face): the near face is exact; the far face keeps its texture and is retouched less
  CHECK(e[0][1].hChange < 0.05 && e[1][1].hChange < 0.05);
  CHECK(e[1][1].mRemoved > 0.4);
  if (std::getenv("DAPPLE_RETOUCH_SHEETS")) {  // a sheet: rows near / far (far upscaled 3x nearest), columns original | own | near | far
    const int tw = 800, th = 1000;
    std::vector<std::uint8_t> px(std::size_t(4 * tw) * (2 * th) * 3, 255);
    for (int face = 0; face < 2; ++face)
      for (int c = 0; c < 4; ++c) {
        const ImageF& im = tiles[face][c];
        const int sc = face ? 3 : 1;
        for (int y = 0; y < th; ++y)
          for (int x = 0; x < tw; ++x) {
            const int sx = x / sc, sy = y / sc;
            if (sx >= im.width || sy >= im.height) continue;
            for (int k = 0; k < 3; ++k)
              px[(std::size_t(face * th + y) * 4 * tw + std::size_t(c * tw + x)) * 3 + k] = std::uint8_t(std::clamp(im.row(sy)[3 * sx + k], 0.f, 1.f) * 255.f + 0.5f);
          }
      }
    std::FILE* fp = std::fopen("build/retouch/r2/twoscales.ppm", "wb");
    if (fp) {
      std::fprintf(fp, "P6\n%d %d\n255\n", 4 * tw, 2 * th);
      std::fwrite(px.data(), 1, px.size(), fp);
      std::fclose(fp);
    }
  }
}

// ---- luminance-range mask coherence (section 40.10; the phone report: "blocky / dithered" range masks on foliage) --------------------------
// A foliage-like texture (pixel-scale luminance swinging +-1 EV around the range's edge) below a bright sky: mask of a luminance range with a
// narrow feather, per pixel (refine 0) vs coherent (refine 50). Metrics: pixel-scale change / 4-px-scale change of the mask (~1 for a
// pixel-binary dither, << 1 for coherent regions), the share of the change on 2- and 3-px block boundaries (1 = no block structure), the
// sky / tree edge position.
double scaleRatio(const std::vector<float>& m, int w, int h, int x0, int x1, int y0, int y1) {
  double e1 = 0, e4 = 0;
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x + 4 < x1; ++x) {
      const float a = m[std::size_t(y) * w + x];
      e1 += std::fabs(m[std::size_t(y) * w + x + 1] - a), e4 += std::fabs(m[std::size_t(y) * w + x + 4] - a);
    }
  return e4 > 0 ? e1 / e4 : 0;
}
double blockShare(const std::vector<float>& m, int w, int h, int n) {
  std::vector<double> e(std::size_t(n), 0.0);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x + 1 < w; ++x) e[std::size_t((x + 1) % n)] += std::fabs(m[std::size_t(y) * w + x + 1] - m[std::size_t(y) * w + x]);
  double tot = 0, mx = 0;
  for (double v : e) tot += v, mx = std::max(mx, v);
  return tot > 0 ? mx / tot * n : 1.0;
}
void testRangeSmooth() {
  const int W = 1024, H = 768;
  SourceImage src;
  src.sceneReferred = true;
  src.linear = ImageF(W, H);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      float Y;
      if (y < H / 3) Y = 0.6f;  // sky
      else Y = 0.035f * std::exp2(2.f * (hash01(std::uint32_t(x), std::uint32_t(y), 7) - 0.5f) + 0.6f * vnoise(float(x), float(y), 40.f, 8));
      float* p = src.linear.row(y) + 3 * std::size_t(x);
      p[0] = Y * 0.9f, p[1] = Y * 1.05f, p[2] = Y * (y < H / 3 ? 1.3f : 0.6f);
    }
  double e1Pixel = 0;
  for (float refine : {0.f, 50.f}) {
    std::vector<LocalAdjustment> L(1);
    L[0].id = "lum";
    MaskComponent m;
    m.kind = MaskKind::Luminance;
    m.lo = 22.f, m.hi = 100.f, m.lumFeather = 4.f, m.refine = refine;  // a narrow feather: the threshold sits inside the foliage's swing
    L[0].mask.push_back(m);
    L[0].ops.v[kLoExposure] = 1.f;
    const std::shared_ptr<const local::Frame> f = local::bind(L, src, false);
    CHECK(f != nullptr);
    if (!f) return;
    std::vector<float> mk(std::size_t(W) * H);
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) {
        const float* p = src.linear.row(y) + 3 * std::size_t(x);
        mk[std::size_t(y) * W + x] = local::maskOf(*f, 0, (x + 0.5f) / W, (y + 0.5f) / H, p, p);
      }
    const double ratio = scaleRatio(mk, W, H, 0, W, H / 2, H);
    double e1 = 0, mean = 0;
    for (int y = H / 2; y < H; ++y)
      for (int x = 0; x + 1 < W; ++x) e1 += std::fabs(mk[std::size_t(y) * W + x + 1] - mk[std::size_t(y) * W + x]), mean += mk[std::size_t(y) * W + x];
    std::printf("  refine %3.0f: foliage mean mask %.3f, mean pixel-to-pixel change %.4f\n", refine, mean / (double(W - 1) * (H / 2)), e1 / (double(W - 1) * (H / 2)));
    const double b2 = blockShare(mk, W, H, 2), b3 = blockShare(mk, W, H, 3);
    // the sky / tree edge: the row where the column mean drops below 0.5 (sky = selected)
    int edge = -1;
    for (int y = 0; y < H && edge < 0; ++y) {
      double s = 0;
      for (int x = 0; x < W; ++x) s += mk[std::size_t(y) * W + x];
      if (s / W < 0.5) edge = y;
    }
    std::printf("  refine %3.0f: pixel / 4-px change %.2f (foliage), block share n=2 %.2f n=3 %.2f, sky edge at row %d (true %d)\n", refine, ratio, b2, b3, edge,
                H / 3);
    CHECK(b2 < 1.1 && b3 < 1.1);  // no 2x2 / 3x3 block structure
    if (refine == 0.f) e1Pixel = e1;
    if (refine > 0.f) {
      CHECK(e1 < 0.25 * e1Pixel);  // the mask changes pixel to pixel at least 4x less: regions, not a per-pixel dither
    }
  }
}

// ---- scale invariance ------------------------------------------------------------------------------------------------------------------
ImageF downsample2(const ImageF& a) {
  ImageF o(a.width / 2, a.height / 2);
  for (int y = 0; y < o.height; ++y)
    for (int x = 0; x < o.width; ++x)
      for (int c = 0; c < 3; ++c)
        o.row(y)[3 * x + c] = 0.25f * (a.row(2 * y)[6 * x + c] + a.row(2 * y)[6 * x + 3 + c] + a.row(2 * y + 1)[6 * x + c] + a.row(2 * y + 1)[6 * x + 3 + c]);
  return o;
}

void testScale() {
  const SourceImage big = makePortrait(1600, 2000, false);
  const ImageF hi = displayPatch(big), lo = downsample2(hi);
  for (int op : {int(kLoSmooth), int(kLoEven), int(kLoUnderEye), int(kLoShine), int(kLoFaceLight)}) {
    ImageF oh = hi, ol = lo;
    UnitFrame uh, ul;
    unitFrame(uh, hi, op, 1.f);
    unitFrame(ul, lo, op, 1.f);
    local::retouchCombineWindow(uh.f, oh, uh.aux, local::retouchRadii(kP.iod * 2000.f));
    local::retouchCombineWindow(ul.f, ol, ul.aux, local::retouchRadii(kP.iod * 1000.f));
    const ImageF ohd = downsample2(oh);
    double dl = 0, diff = 0;
    for (int y = 0; y < lo.height; ++y)
      for (int x = 0; x < lo.width * 3; ++x) {
        const double a = ol.row(y)[x] - lo.row(y)[x], b = ohd.row(y)[x] - lo.row(y)[x];
        dl += std::fabs(a), diff += std::fabs(a - b);
      }
    const double rel = diff / std::max(dl, 1e-9);
    std::printf("  %-10s mean |change| %.5f, |hi->lo - lo| / |change| = %.3f\n", localOps()[std::size_t(op)].key, dl / (lo.width * lo.height * 3.0), rel);
    CHECK(rel < 0.25);
  }
}

// ---- blobs ------------------------------------------------------------------------------------------------------------------------------
std::vector<float> skinPatch(int w, int h, std::uint32_t seed, float iodPx) {
  std::vector<float> rgb(std::size_t(w) * h * 3);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const float k = (0.9f + 0.1f * float(y) / h) * (1.f + 0.04f * (hash01(std::uint32_t(x), std::uint32_t(y), seed) - 0.5f)) *
                      (1.f + 0.03f * vnoise(float(x), float(y), 0.15f * iodPx, seed + 3));
      float* p = rgb.data() + 3 * (std::size_t(y) * w + x);
      p[0] = 0.40f * k, p[1] = 0.26f * k, p[2] = 0.19f * k;
    }
  return rgb;
}
void addSpot(std::vector<float>& rgb, int w, int h, float cx, float cy, float r, float dark, float redden) {
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const float d2 = ((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy)) / (r * r);
      if (d2 > 6.f) continue;
      const float wgt = std::exp(-d2);
      float* p = rgb.data() + 3 * (std::size_t(y) * w + x);
      p[0] *= 1.f - (dark - redden) * wgt, p[1] *= 1.f - (dark + redden) * wgt, p[2] *= 1.f - dark * wgt;
    }
}

void testBlobs() {
  const int w = 400, h = 400;
  const float iod = 300.f;  // a head-shot crop: a blemish of 0.03 iod is 9 px across
  // (a) clean skin + one blemish (red, darker) + one faint spot under the default threshold
  std::vector<float> a = skinPatch(w, h, 5, iod);
  addSpot(a, w, h, 150.f, 220.f, 0.03f * iod, 0.10f, 0.12f);
  addSpot(a, w, h, 300.f, 120.f, 0.02f * iod, 0.015f, 0.f);
  const std::vector<local::Blob> ba = local::detectBlobs(a.data(), nullptr, w, h, iod, 25.f, true);
  bool found = false;
  for (const local::Blob& b : ba)
    if (std::hypot(b.x - 150.f, b.y - 220.f) < 4.f) found = true;
  std::printf("  clean skin + blemish: %zu spot(s), blemish found %s", ba.size(), found ? "yes" : "no");
  for (const local::Blob& b : ba) std::printf(" (%.0f,%.0f r %.1f z %.1f conf %.2f src %+.0f,%+.0f)", b.x, b.y, b.r, b.z, b.conf, b.sx, b.sy);
  std::printf("\n");
  CHECK(found && ba.size() <= 2);
  // (b) a freckled face: 60 freckles of the same contrast and size as the blemish
  std::vector<float> f = skinPatch(w, h, 9, iod);
  std::mt19937 rng(4);
  std::uniform_real_distribution<float> U(20.f, 380.f), S(0.8f, 1.2f);
  for (int i = 0; i < 60; ++i) addSpot(f, w, h, U(rng), U(rng), 0.025f * iod * S(rng), 0.10f, 0.05f);
  const std::vector<local::Blob> bf = local::detectBlobs(f.data(), nullptr, w, h, iod, 25.f, false);
  const std::vector<local::Blob> bf100 = local::detectBlobs(f.data(), nullptr, w, h, iod, 100.f, false);
  std::printf("  freckled skin (60 freckles): %zu spot(s) at sensitivity 25 (default), %zu at 100\n", bf.size(), bf100.size());
  CHECK(bf.size() <= 3);
  CHECK(bf100.size() >= 20);
  // (c) the blemish among freckles is not singled out at the default either (it looks like one more freckle): documented behaviour
  // (d) source of the blemish: a clean patch (no blob), inside the patch
  if (found)
    for (const local::Blob& b : ba)
      if (std::hypot(b.x - 150.f, b.y - 220.f) < 4.f) CHECK(std::hypot(b.sx, b.sy) >= 2.4f * b.r && std::hypot(b.sx, b.sy) <= 3.6f * b.r);
}

// ---- heal --------------------------------------------------------------------------------------------------------------------------------
double meanLumaDisc(const Image8& im, float cx, float cy, float r) {
  double s = 0;
  int n = 0;
  for (int y = std::max(0, int(cy - r)); y <= std::min(im.height - 1, int(cy + r)); ++y)
    for (int x = std::max(0, int(cx - r)); x <= std::min(im.width - 1, int(cx + r)); ++x) {
      if ((x - cx) * (x - cx) + (y - cy) * (y - cy) > r * r) continue;
      const std::uint8_t* p = im.data.data() + (std::size_t(y) * im.width + x) * 3;
      s += 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2], ++n;
    }
  return n ? s / n : 0;
}
double maxDiff(const Image8& a, const Image8& b, std::size_t* differing = nullptr) {
  int mx = 0;
  std::size_t d = 0;
  for (std::size_t i = 0; i < a.data.size() && i < b.data.size(); ++i) {
    const int v = std::abs(int(a.data[i]) - int(b.data[i]));
    mx = std::max(mx, v), d += v != 0;
  }
  if (differing) *differing = d;
  return a.data.size() == b.data.size() ? mx : 999;
}

void testHeal() {
  const int W = 800, H = 1000;
  const SourceImage src = makePortrait(W, H, true);
  putRasters(W, H);
  EditParams p;
  LocalAdjustment skin = rasterAdj("skin", "rt_skin");
  skin.ops.v[kLoHeal] = 100.f;
  p.local = {skin};
  const std::shared_ptr<const local::Frame> f = local::bind(p.local, src, false);
  CHECK(f != nullptr);
  if (!f) return;
  std::printf("  automatic spots: %zu\n", f->spots.size());
  const float bl[3][2] = {{-0.45f, 0.25f}, {0.40f, 0.35f}, {-0.15f, 1.05f}};
  int hit = 0;
  for (const auto& q : bl) {
    const float X = kP.cx * 0.8f + q[0] * kP.iod, Y = kP.cy + q[1] * kP.iod;
    for (const local::SpotM& s : f->spots)
      if (std::hypot(s.x - X, s.y - Y) < 0.012f * kP.iod + 0.002f) {
        ++hit;
        break;
      }
  }
  for (const local::SpotM& s : f->spots) std::printf("    spot (%.4f, %.4f) r %.4f src %+.4f %+.4f alpha %.2f\n", s.x, s.y, s.r, s.ox, s.oy, s.alpha);
  std::printf("  blemishes found: %d of 3\n", hit);
  CHECK(hit == 3);
  CHECK(f->spots.size() <= 6);
  EngineSession es;
  Image8 a, b, base, t1, t2;
  EditParams p0;
  CHECK(exactRender(es, p, src, ExecMode::Whole, 0, a));
  CHECK(exactRender(es, p, src, ExecMode::Whole, 0, b));
  CHECK(exactRender(es, p0, src, ExecMode::Whole, 0, base));
  CHECK(maxDiff(a, b) == 0);
  for (int tile : {96, 200}) {
    Image8 t;
    std::size_t d = 0;
    CHECK(exactRender(es, p, src, ExecMode::TiledExport, tile, t));
    const double m = maxDiff(a, t, &d);
    std::printf("  tile %d vs whole: max code %.0f, differing bytes %zu\n", tile, m, d);
    CHECK(m <= 1);
  }
  // each blemish: before = darker than its ring; after = within 2 levels of the ring
  for (const auto& q : bl) {
    const float X = (kP.cx * 0.8f + q[0] * kP.iod) * float(std::max(W, H)), Y = (kP.cy + q[1] * kP.iod) * float(std::max(W, H));
    const float r = 0.03f * kP.iod * float(std::max(W, H));
    const double in0 = meanLumaDisc(base, X, Y, r), ring0 = meanLumaDisc(base, X, Y, 3 * r) * 9 / 8 - in0 / 8;
    const double in1 = meanLumaDisc(a, X, Y, r), ring1 = meanLumaDisc(a, X, Y, 3 * r) * 9 / 8 - in1 / 8;
    std::printf("  blemish at (%.0f, %.0f): centre - ring %.1f -> %.1f levels\n", X, Y, in0 - ring0, in1 - ring1);
    CHECK(in0 - ring0 < -3.0);
    CHECK(std::fabs(in1 - ring1) < 2.5);
  }
  // manual spots: deterministic and tile-exact under geometry
  EditParams pm;
  pm.local = retouchEdit(false);
  pm.local.resize(4);
  pm.local[0].ops = LocalOps{};
  pm.local[0].ops.v[kLoHeal] = 60.f;
  pm.crop = "4:5";
  pm.straighten = 3.f;
  Image8 w1, w2;
  CHECK(exactRender(es, pm, src, ExecMode::Whole, 0, w1));
  CHECK(exactRender(es, pm, src, ExecMode::TiledExport, 128, w2));
  std::size_t d = 0;
  const double m = maxDiff(w1, w2, &d);
  std::printf("  manual + automatic spots, crop 4:5 straighten 3: tile 128 vs whole max code %.0f, differing bytes %zu\n", m, d);
  CHECK(m <= 1);
}

// ---- whiten ---------------------------------------------------------------------------------------------------------------------------------
void testWhiten() {
  std::vector<LocalAdjustment> L(1);
  L[0].id = "t";
  MaskComponent m;
  m.kind = MaskKind::Radial;
  m.cx = m.cy = 0.5f, m.rx = m.ry = 2.f, m.feather = 0.f;
  L[0].mask.push_back(m);
  L[0].ops.v[kLoWhiten] = 100.f;
  SourceImage src;
  src.linear = ImageF(16, 16);
  const std::shared_ptr<const local::Frame> f = local::bind(L, src, false);
  CHECK(f && f->anyLinear);
  if (!f) return;
  double worst = 0;
  int bad = 0;
  for (float Y0 : {0.05f, 0.18f, 0.3f, 0.4f, 0.47f, 0.6f, 1.2f})
    for (int hue = 0; hue < 6; ++hue) {
      // yellowish teeth (hue 1), reddish sclera (hue 0), neutral (2), blue (4)...
      const float cols[6][3] = {{1.15f, 0.95f, 0.85f}, {1.08f, 1.0f, 0.70f}, {1.f, 1.f, 1.f}, {0.9f, 1.05f, 0.9f}, {0.85f, 0.95f, 1.3f}, {1.05f, 0.9f, 1.1f}};
      float px[3] = {Y0 * cols[hue][0], Y0 * cols[hue][1], Y0 * cols[hue][2]};
      const float yin = 0.2880402f * px[0] + 0.7118741f * px[1] + 0.0000857f * px[2];
      const float src3[3] = {px[0], px[1], px[2]};
      local::applyLinear(*f, 0.5f, 0.5f, src3, px, nullptr);
      const float yout = 0.2880402f * px[0] + 0.7118741f * px[1] + 0.0000857f * px[2];
      const float cap = std::max(yin, local::kWhitenCeiling);
      if (yout > cap * 1.0005f) ++bad;
      worst = std::max(worst, double(yout / cap));
      if (hue == 1 && Y0 == 0.3f) {
        const float b0 = 0.70f / 1.0f, b1 = px[2] / px[1];
        std::printf("  teeth-like pixel Y %.3f -> %.3f (+%.2f EV), blue/green %.3f -> %.3f\n", yin, yout, std::log2(yout / yin), b0, b1);
        CHECK(b1 > b0 + 0.1f && yout > yin);
      }
      if (hue == 4 && Y0 == 0.3f) {  // a blue pixel keeps its colour
        const float r0 = 1.3f / 0.95f, r1 = px[2] / px[1];
        CHECK(std::fabs(r1 - r0) < 0.02f);
      }
    }
  std::printf("  ceiling: worst output / max(input, ceiling %.3f) = %.4f, over %d\n", local::kWhitenCeiling, worst, bad);
  CHECK(bad == 0);
}

// ---- identity ---------------------------------------------------------------------------------------------------------------------------
void testIdentity() {
  const SourceImage src = makePortrait(400, 500);
  putRasters(400, 500);
  EngineSession es;
  EditParams p0;
  Image8 base, z, off0, off1;
  CHECK(exactRender(es, p0, src, ExecMode::Whole, 0, base));
  EditParams pz;
  pz.local = retouchEdit();
  for (LocalAdjustment& a : pz.local) a.strength = 0.f;
  CHECK(exactRender(es, pz, src, ExecMode::Whole, 0, z));
  CHECK(maxDiff(base, z) == 0);
  // the old Renderer (DAPPLE_ENGINE=off) never reads the local list
  EditParams pr;
  pr.local = retouchEdit();
  {
    const Renderer r0(p0, cam(), nullptr), r1(pr, cam(), nullptr);
    Image8 o0 = r0.render(src), o1 = r1.render(src);
    CHECK(o0.data == o1.data);
  }
  // a legacy edit keeps the identity channel layout (bit-identical frames: the golden corpus checks the pixels)
  EditParams pl;
  LocalAdjustment a = rasterAdj("l", "rt_skin");
  a.ops.v[kLoTexture] = -30.f;
  a.ops.v[kLoDehaze] = 20.f;
  pl.local = {a};
  const std::shared_ptr<const local::Frame> f = local::bind(pl.local, src, false);
  CHECK(f && !f->retouchW && f->chanOp[0] == kLoClarity && f->chanOp[4] == kLoDehaze && f->chanOp[5] == -1);
  local::AuxTarget t;
  if (f) CHECK(local::auxLayout(*f, t) == 2 && t.slot[1] == 0 && t.slot[4] == 1);
  // with a retouch op the used ops are packed
  pr.local = retouchEdit();
  const std::shared_ptr<const local::Frame> g = local::bind(pr.local, src, false);
  CHECK(g && g->retouchW && g->legacyW);
  if (g) {
    int n = 0;
    for (int c = 0; c < kLocalWindowSlots; ++c) n += g->chanOp[c] >= 0;
    std::printf("  retouch edit: %d amount channels:", n);
    for (int c = 0; c < kLocalWindowSlots; ++c)
      if (g->chanOp[c] >= 0) std::printf(" %s", localOps()[std::size_t(g->chanOp[c])].key);
    std::printf(", %zu heal spots, iod %.3f\n", g->spots.size(), g->iod);
    CHECK(n == 7);
  }
}

// ---- tiles ------------------------------------------------------------------------------------------------------------------------------
void testTiles() {
  const SourceImage src = makePortrait(800, 1000);
  putRasters(800, 1000);
  struct Case {
    const char* name;
    std::function<void(EditParams&)> set;
  };
  const Case cases[] = {{"plain", [](EditParams&) {}},
                        {"crop 4:5, rotate 90, straighten 4, flip", [](EditParams& p) { p.crop = "4:5", p.rotate = 90, p.straighten = 4.f, p.flip = true; }},
                        {"global clarity + local clarity on the eyes", [](EditParams& p) {
                           p.clarity = 15.f;
                           p.local[2].ops.v[kLoClarity] = 30.f;
                         }}};
  for (const Case& c : cases) {
    EditParams p;
    p.local = retouchEdit();
    c.set(p);
    EngineSession es;
    Image8 whole;
    CHECK(exactRender(es, p, src, ExecMode::Whole, 0, whole));
    for (int tile : {96, 256}) {
      Image8 t;
      std::size_t d = 0;
      CHECK(exactRender(es, p, src, ExecMode::TiledExport, tile, t));
      const double m = maxDiff(whole, t, &d);
      std::printf("  %-44s tile %3d: max code %.0f, differing bytes %zu of %zu\n", c.name, tile, m, d, whole.data.size());
      CHECK(m <= 1);
    }
  }
}

// ---- interactive ---------------------------------------------------------------------------------------------------------------------------
double meanDiffDown(const std::uint8_t* small, int sw, int sh, const Image8& big) {
  // big (exact) area-averaged to small's size, mean absolute difference in levels over RGB
  double s = 0;
  for (int y = 0; y < sh; ++y)
    for (int x = 0; x < sw; ++x) {
      const int x0 = x * big.width / sw, x1 = std::max(x0 + 1, (x + 1) * big.width / sw);
      const int y0 = y * big.height / sh, y1 = std::max(y0 + 1, (y + 1) * big.height / sh);
      for (int c = 0; c < 3; ++c) {
        double a = 0;
        for (int yy = y0; yy < y1; ++yy)
          for (int xx = x0; xx < x1; ++xx) a += big.data[(std::size_t(yy) * big.width + xx) * 3 + c];
        a /= double((y1 - y0) * (x1 - x0));
        s += std::fabs(a - small[(std::size_t(y) * sw + x) * 4 + c]);
      }
    }
  return s / (double(sw) * sh * 3);
}

void testInteractive() {
  const SourceImage big = makePortrait(1600, 2000);
  SourceImage small = big;
  small.linear = resizeToLongEdge(big.linear, 1080);
  putRasters(1600, 2000);
  EngineSession es;
  double base = 0, with = 0;
  for (int pass = 0; pass < 2; ++pass) {
    EditParams p;
    if (pass) p.local = retouchEdit();
    FrameResult res;
    CHECK(cpuFrame(es, p, small, true, res));
    Image8 ex;
    CHECK(exactRender(es, p, big, ExecMode::Whole, 0, ex));
    const double d = meanDiffDown(res.rgba, res.w, res.h, ex);
    (pass ? with : base) = d;
    std::free(res.rgba);
  }
  std::printf("  interactive (1080) vs exact (2000, area-averaged): mean |diff| %.3f levels without the retouch, %.3f with it\n", base, with);
  CHECK(with - base < 0.6);
}

// ---- many raster masks (section 40.4: 12 per edit) ------------------------------------------------------------------------------------------------
// 12 distinct 1024 px planes (horizontal bands of the photo) in 12 adjustments, one of them also used twice; band i gets exposure +0.5 EV
std::vector<LocalAdjustment> twelveRasters(int W, int H) {
  std::vector<LocalAdjustment> L;
  const int w = W >= H ? 1024 : int(std::lround(1024.f * W / H)), h = H >= W ? 1024 : int(std::lround(1024.f * H / W));
  for (int i = 0; i < 12; ++i) {
    std::vector<std::uint8_t> px(std::size_t(w) * h, 0);
    for (int y = h * i / 12; y < h * (i + 1) / 12; ++y)
      for (int x = 0; x < w; ++x) px[std::size_t(y) * w + x] = 255;
    const std::string id = "rt_band" + std::to_string(i);
    local::rasterSet(id, w, h, px.data(), w);
    LocalAdjustment a;
    a.id = id;
    MaskComponent m;
    m.kind = MaskKind::Raster;
    m.ref = id;
    m.refine = 30.f;
    a.mask.push_back(m);
    a.ops.v[kLoExposure] = 0.5f;
    if (i == 3) a.ops.v[kLoSmooth] = 40.f, a.face = 0.12f;
    L.push_back(a);
  }
  L[5].mask.push_back(L[5].mask[0]);  // the same plane twice (subtract of itself = nothing; then add back): stored once
  L[5].mask[1].op = MaskOp::Intersect;
  return L;
}

void testRasters() {
  const int W = 1200, H = 1500;
  const SourceImage src = makePortrait(W, H);
  std::vector<LocalAdjustment> L = twelveRasters(W, H);
  for (bool interactive : {false, true}) {
    local::resetPlaneCache();
    const std::shared_ptr<const local::Frame> f = local::bind(L, src, interactive);
    CHECK(f && f->adj.size() == 12);
    if (!f) return;
    int planes = 0, withPlane = 0;
    std::vector<std::uint64_t> keys;
    for (const local::Comp& c : f->comp)
      if (c.plane) {
        ++withPlane;
        if (std::find(keys.begin(), keys.end(), c.plane->key) == keys.end()) keys.push_back(c.plane->key), ++planes;
      }
    const double mb = double(f->planeFloats) * 4 / 1048576.0;
    std::printf("  %s: 13 raster components, %d with a plane, %d distinct, grid cap %d, plane store %.1f MB\n", interactive ? "interactive" : "exact", withPlane,
                planes, local::rasterGridCap(L, interactive), mb);
    CHECK(withPlane == 13 && planes == 12);
    CHECK(mb <= (interactive ? 12 * 512 * 512 * 16 / 1048576.0 : 12 * 640 * 640 * 16 / 1048576.0) + 0.01);
  }
  // every band is lifted (all 12 masks evaluate), on the CPU and through the engine
  EngineSession es;
  Image8 base, out;
  EditParams p0, p;
  p.local = L;
  CHECK(exactRender(es, p0, src, ExecMode::Whole, 0, base));
  CHECK(exactRender(es, p, src, ExecMode::Whole, 0, out));
  int lifted = 0;
  for (int i = 0; i < 12; ++i) {
    const float cy = (float(i) + 0.5f) / 12.f * float(H), cx = 0.08f * float(W);  // the background column: no face
    const double d = meanLumaDisc(out, cx, cy, 10.f) - meanLumaDisc(base, cx, cy, 10.f);
    lifted += d > 3.0;
  }
  std::printf("  bands lifted: %d of 12\n", lifted);
  CHECK(lifted == 12);
  Image8 t;
  CHECK(exactRender(es, p, src, ExecMode::TiledExport, 200, t));
  std::size_t d = 0;
  const double m = maxDiff(out, t, &d);
  std::printf("  12 rasters: tile 200 vs whole max code %.0f, differing bytes %zu\n", m, d);
  CHECK(m <= 1);
}

// ---- under-eye plane ---------------------------------------------------------------------------------------------------------------------
void testUnderEye() {
  aimask::FaceMesh m;
  // a frontal synthetic mesh: iris centres, lid chains, mouth; everything else at the face centre
  for (int i = 0; i < 478; ++i) m.xy[i][0] = 200.f, m.xy[i][1] = 220.f;
  m.xy[468][0] = 160.f, m.xy[468][1] = 180.f;  // person's right iris (image left)
  m.xy[473][0] = 240.f, m.xy[473][1] = 180.f;
  m.xy[13][0] = 200.f, m.xy[13][1] = 260.f, m.xy[14][0] = 200.f, m.xy[14][1] = 264.f;
  const int lidR[9] = {33, 7, 163, 144, 145, 153, 154, 155, 133}, lidL[9] = {263, 249, 390, 373, 374, 380, 381, 382, 362};
  for (int i = 0; i < 9; ++i) {
    const float t = float(i) / 8.f, bow = 4.f * std::sin(3.14159f * t);
    m.xy[lidR[i]][0] = 140.f + 40.f * t, m.xy[lidR[i]][1] = 182.f + bow;  // outer (x 140) -> inner (x 180)
    m.xy[lidL[i]][0] = 260.f - 40.f * t, m.xy[lidL[i]][1] = 182.f + bow;
  }
  const aimask::Plane p = aimask::underEyePlane(m, 400, 400, aimask::Plane{});
  CHECK(!p.empty());
  if (p.empty()) return;
  auto at = [&](int x, int y) { return int(p.px[std::size_t(y) * 400 + x]); };
  // inside the crescent (below the lid, deepest near t = 0.6: x = 164 / 236), nothing above the eye, nothing at the mouth, symmetric
  const int inR = at(164, 195), inL = at(236, 195), above = at(164, 170), mouth = at(200, 262), cheek = at(164, 215);
  std::printf("  under-eye plane: in %d / %d, above the lid %d, mouth %d, 35 px below the lid %d\n", inR, inL, above, mouth, cheek);
  CHECK(inR > 200 && inL > 200 && std::abs(inR - inL) <= 8 && above == 0 && mouth == 0 && cheek < 40);
}

#ifdef DAPPLE_RETOUCH_GPU
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

void testParity() {
  auto ctx = gpuContext();
  struct Case {
    const char* name;
    std::function<void(EditParams&)> set;
    bool interactive;
  };
  auto only = [](int op, float v) {
    return [op, v](EditParams& p) {
      LocalAdjustment a = rasterAdj("o", op == kLoUnderEye ? "rt_ue" : "rt_skin");
      a.ops.v[std::size_t(op)] = v;
      p.local = {a};
    };
  };
  const std::vector<Case> cases = {
      {"smooth 100", only(kLoSmooth, 100.f), false},
      {"even 100", only(kLoEven, 100.f), false},
      {"redness 100", only(kLoRedness, 100.f), false},
      {"undereye 100", only(kLoUnderEye, 100.f), false},
      {"shine 100", only(kLoShine, 100.f), false},
      {"facelight 100", only(kLoFaceLight, 100.f), false},
      {"whiten 100", only(kLoWhiten, 100.f), false},
      {"heal (auto) 100", only(kLoHeal, 100.f), false},
      {"all retouch + texture", [](EditParams& p) { p.local = retouchEdit(); }, false},
      {"all retouch, crop / straighten / flip", [](EditParams& p) { p.local = retouchEdit(); p.crop = "4:5"; p.straighten = 4.f; p.flip = true; p.rotate = 90; }, false},
      {"all retouch interactive", [](EditParams& p) { p.local = retouchEdit(); }, true},
      {"all retouch + global clarity / dehaze", [](EditParams& p) { p.local = retouchEdit(); p.clarity = 20.f; p.dehaze = 10.f; }, false},
      {"12 raster masks", [](EditParams& p) { p.local = twelveRasters(1200, 1500); }, false},
      {"12 raster masks interactive", [](EditParams& p) { p.local = twelveRasters(1200, 1500); }, true},
  };
  for (const std::pair<int, int>& sz : {std::pair<int, int>{640, 800}, std::pair<int, int>{1200, 1500}}) {
    const SourceImage src = makePortrait(sz.first, sz.second);
    putRasters(sz.first, sz.second);
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
        std::printf("  %-40s %dx%d: render failed (%s %s)\n", c.name, sz.first, sz.second, err.c_str(), res.error.c_str());
        ++g_failures;
      } else {
        const dapple::gpu::DeStats d = dapple::gpu::compareRgba8(gpuPx, res.rgba, std::size_t(gw) * gh);
        std::printf("  %-40s %4dx%-4d mean %.4f p99 %.3f max %.2f  differing %.1f%%  (%s)\n", c.name, gw, gh, d.mean, d.p99, d.max, d.differing * 100,
                    res.executor.c_str());
        CHECK(d.mean <= 0.15 && d.p99 <= 1.5);
      }
      std::free(gpuPx);
      std::free(res.rgba);
    }
    EditParams p;
    p.local = retouchEdit();
    Renderer r(p, cam(), nullptr);
    std::vector<std::uint8_t> a, b;
    int ew = 0, eh = 0;
    for (int pass = 0; pass < 2; ++pass) {
      std::vector<std::uint8_t>& dst = pass ? b : a;
      std::string err;
      engine::GpuStats st;
      const bool ok = rig.exportTiled(
          p, r, src, 0, 65, pass ? 192 : 4096, false, 0.f, [&](int w, int h) { ew = w, eh = h, dst.assign(std::size_t(w) * h * 4, 0); },
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
    std::printf("  GPU tiled export (192 px tiles) vs one tile %dx%d: max code %d, differing bytes %zu\n", ew, eh, mx, diff);
    CHECK(mx <= 1);
  }
}
#endif

// ---- bench (not in ctest) ----------------------------------------------------------------------------------------------------------------
double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v.empty() ? -1 : v[v.size() / 2];
}
void testBench() {
  using Clock = std::chrono::steady_clock;
  const SourceImage big = makePortrait(4800, 6000);  // 28.8 MP portrait (a 24 MP class frame in 4:5)
  SourceImage small = big;
  small.linear = resizeToLongEdge(big.linear, 1080);
  putRasters(4800, 6000);
#ifdef DAPPLE_RETOUCH_GPU
  auto ctx = gpuContext();
  dapple::gpu::Rig rig(ctx, big, cam());
#endif
  struct Op {
    const char* name;
    std::function<void(EditParams&)> set;
  };
  auto one = [](int op, float v) {
    return [op, v](EditParams& p) {
      LocalAdjustment a = rasterAdj("o", op == kLoUnderEye ? "rt_ue" : "rt_skin");
      a.ops.v[std::size_t(op)] = v;
      p.local = {a};
    };
  };
  const Op ops[] = {{"none (raster exposure)", [](EditParams& p) { LocalAdjustment a = rasterAdj("o", "rt_skin"); a.ops.v[kLoExposure] = 0.3f; p.local = {a}; }},
                    {"smooth", one(kLoSmooth, 50)}, {"even", one(kLoEven, 50)}, {"redness", one(kLoRedness, 50)},
                    {"undereye", one(kLoUnderEye, 50)}, {"shine", one(kLoShine, 50)}, {"facelight", one(kLoFaceLight, 50)},
                    {"whiten", one(kLoWhiten, 50)}, {"heal auto", one(kLoHeal, 100)},
                    {"all (retouchEdit)", [](EditParams& p) { p.local = retouchEdit(); }},
                    {"12 raster masks (exposure)", [](EditParams& p) { p.local = twelveRasters(4800, 6000); }}};
  std::printf("  %-24s | %-16s | %-16s | %-12s | %-14s\n", "op", "GPU 1080 int ms", "GPU 28.8 MP ms", "CPU 1080 ms", "CPU exact 28.8 MP ms");
  for (const Op& o : ops) {
    EditParams p;
    o.set(p);
    double gi = -1, gf = -1, ci = -1, cf = -1;
#ifdef DAPPLE_RETOUCH_GPU
    Renderer rd(p, cam(), nullptr);
    for (int mode = 0; mode < 2; ++mode) {
      const SourceImage& s = mode ? big : small;
      FastOptions fo;
      fo.spatial = mode ? SpatialQuality::Full : SpatialQuality::Interactive;
      fo.noise = false;
      std::vector<double> wall;
      for (int i = 0; i < 9; ++i) {
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
    std::printf("  %-24s | %16.2f | %16.2f | %12.1f | %14.0f\n", o.name, gi, gf, ci, cf);
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  struct T {
    const char* id;
    void (*fn)();
  };
  const T tests[] = {{"json", testJson},   {"bands", testBands},       {"scale", testScale},       {"blobs", testBlobs},
                     {"heal", testHeal},   {"whiten", testWhiten},     {"identity", testIdentity}, {"tiles", testTiles},
                     {"interactive", testInteractive}, {"undereye", testUnderEye}, {"rasters", testRasters}, {"complexion", testComplexion}, {"twoscales", testTwoScales}, {"rangesmooth", testRangeSmooth},
#ifdef DAPPLE_RETOUCH_GPU
                     {"parity", testParity},
#endif
  };
  const std::string id = argc > 1 ? argv[1] : "";
  if (id == "bench") {
    std::printf("[bench]\n");
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
