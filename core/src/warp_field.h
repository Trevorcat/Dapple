#pragma once
// Internal: the displacement field of the portrait reshaping (dapple/reshape.h; docs/engine/pipeline-architecture.md section 45).
//
// One whole-photo BACKWARD field B on a regular grid of nodes over the photo's normalised space: the content shown at photo point p
// of the warped picture comes from p + B(p) of the original. Units: long edges of the photo (isotropic). The engine folds it into the
// geometry fetch (engine::point::CpuFrame / point.comp / film_src.comp / local_film_in.comp): output pixel -> geometry -> source position s ->
// s + B(s) -> resample. Where the four grid nodes around s are exactly zero nothing changes (bit-identical to no reshaping).
//
// The CPU sampler below and warp.glsl are twins: keep the arithmetic identical.
#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dapple/reshape.h"

namespace dapple {
namespace jsonlite { struct JValue; }
bool reshapeFromValue(const jsonlite::JValue& v, ReshapeParams& r);  // reshape.cpp
}  // namespace dapple

namespace dapple::warp {

struct Field {
  int gw = 0, gh = 0;          // nodes (>= 2 each); node (i, j) at u = i / (gw - 1), v = j / (gh - 1)
  int photoW = 0, photoH = 0;  // the aspect the field was built for
  std::vector<float> d;        // (dx, dy) per node, row-major, long-edge units (layered: the two layers blended at the nodes: GPU twin, sampleUV)
  // Layered mode (docs/engine/pipeline-architecture.md section 48): the person layer's backward field `dp`, the background fill field `bg`
  // (zero outside the old silhouette; inside it the mirror read from just outside) and the person plane `mp` (mw x mh, 0..255). CPU displace()
  // chooses per pixel between the two by the warped person plane, so the background outside the silhouette is read exactly where it is.
  bool layered = false;
  std::vector<float> dp, bg;
  std::vector<float> bs;  // optional whole-frame stretch (long legs A / B), applied first: s -> s + bs(s), then the layers at the stretched position
  std::vector<std::uint8_t> mp;
  int mw = 0, mh = 0;
  float maxDisp = 0;           // max |B| over the nodes, long-edge units
  float maxGrad = 0;           // max |grad F| of the forward field after limiting (< 1: fold-free)
  float maxMove = 0;           // max |B| of the person layer (layered) or of the whole field, long-edge units (maxDisp also counts the background fill reads)
  float rawGrad = 0;           // max |grad F| before limiting
  bool gradLimited = false;    // the operators asked for more and were scaled down
  float delta = 0;             // background protection band (long-edge units), 0 = no matte
  bool matteUsed = false;
  bool structure = false;      // mode C was used (one continuous field, d)
  float cFoldFrac = 0;         // mode C: share of nodes blended toward the smooth field to stay fold-free
  bool matteMissing = false;   // a matte id was given but the plane is not in the raster store
  std::uint64_t id = 0;        // content key (params hash, grid)
  std::size_t bytes() const { return (d.size() + dp.size() + bg.size() + bs.size()) * sizeof(float) + mp.size(); }
};

// Source-pixel mapping of a frame: grid coordinate = s * a + b (s in source pixels, pixel i spans [i, i + 1)); displacement in source
// pixels = d * lpx.
struct Map {
  float ax = 1, bx = 0, ay = 1, by = 0, lpx = 1;
};
// photoW / photoH: the size of the picture the source pixels belong to (a region window's parent); x0, y0: the source's origin in it.
Map makeMap(const Field& f, int srcW, int srcH, int photoW, int photoH, int x0, int y0);

// Person plane (layered mode) at photo-normalised (u, v), 0..1, bilinear, clamped.
inline float matteAt(const Field& f, float u, float v) {
  const float x = std::min(std::max(u * float(f.mw) - 0.5f, 0.f), float(f.mw - 1)), y = std::min(std::max(v * float(f.mh) - 0.5f, 0.f), float(f.mh - 1));
  const int i = std::min(int(x), f.mw - 2), j = std::min(int(y), f.mh - 2);
  const float fx = x - float(i), fy = y - float(j);
  const std::uint8_t* a = f.mp.data() + std::size_t(j) * f.mw + i;
  const std::uint8_t* c = a + f.mw;
  return ((float(a[0]) * (1.f - fx) + float(a[1]) * fx) * (1.f - fy) + (float(c[0]) * (1.f - fx) + float(c[1]) * fx) * fy) * (1.f / 255.f);
}
inline float smooth01(float a, float b, float x) {
  const float t = std::min(std::max((x - a) / (b - a), 0.f), 1.f);
  return t * t * (3.f - 2.f * t);
}

// Bilinear sample of the field at source position (sx, sy); false (and no change) when the four nodes are exactly zero.
inline bool displace(const Field& f, const Map& m, float& sx, float& sy) {
  float gx = sx * m.ax + m.bx, gy = sy * m.ay + m.by;
  gx = std::min(std::max(gx, 0.f), float(f.gw - 1));
  gy = std::min(std::max(gy, 0.f), float(f.gh - 1));
  bool stretched = false;
  float s0x = sx, s0y = sy;
  if (!f.bs.empty()) {  // stage 1: the whole-frame stretch
    const int i = std::min(int(gx), f.gw - 2), j = std::min(int(gy), f.gh - 2);
    const float fx = gx - float(i), fy = gy - float(j);
    const float* a = f.bs.data() + 2 * (std::size_t(j) * f.gw + i);
    const float* c = a + 2 * std::size_t(f.gw);
    const float dx = (a[0] * (1.f - fx) + a[2] * fx) * (1.f - fy) + (c[0] * (1.f - fx) + c[2] * fx) * fy;
    const float dy = (a[1] * (1.f - fx) + a[3] * fx) * (1.f - fy) + (c[1] * (1.f - fx) + c[3] * fx) * fy;
    if (dx != 0.f || dy != 0.f) {
      s0x += dx * m.lpx, s0y += dy * m.lpx;
      stretched = true;
      gx = std::min(std::max(s0x * m.ax + m.bx, 0.f), float(f.gw - 1));
      gy = std::min(std::max(s0y * m.ay + m.by, 0.f), float(f.gh - 1));
    }
  }
  const int i = std::min(int(gx), f.gw - 2), j = std::min(int(gy), f.gh - 2);
  const float fx = gx - float(i), fy = gy - float(j);
  const std::size_t o = 2 * (std::size_t(j) * f.gw + i), r = 2 * std::size_t(f.gw);
  if (f.layered) {
    const float* a = f.dp.data() + o;
    const float* c = a + r;
    const float* g = f.bg.data() + o;
    const float* h = g + r;
    if (a[0] == 0.f && a[1] == 0.f && a[2] == 0.f && a[3] == 0.f && c[0] == 0.f && c[1] == 0.f && c[2] == 0.f && c[3] == 0.f && g[0] == 0.f && g[1] == 0.f &&
        g[2] == 0.f && g[3] == 0.f && h[0] == 0.f && h[1] == 0.f && h[2] == 0.f && h[3] == 0.f) {
      sx = s0x, sy = s0y;
      return stretched;
    }
    auto bil = [&](const float* p, const float* q, int k) {
      return (p[k] * (1.f - fx) + p[k + 2] * fx) * (1.f - fy) + (q[k] * (1.f - fx) + q[k + 2] * fx) * fy;
    };
    const float px = bil(a, c, 0), py = bil(a, c, 1);
    const float u = gx / float(f.gw - 1), v = gy / float(f.gh - 1);
    const float L = m.lpx;
    const float t = smooth01(0.55f, 0.85f, matteAt(f, u + px * L / float(f.photoW), v + py * L / float(f.photoH)));
    const float inS = smooth01(0.02f, 0.06f, matteAt(f, u, v));
    const float bx = bil(g, h, 0) * inS, by = bil(g, h, 1) * inS;
    const float lx = bx + t * (px - bx), ly = by + t * (py - by);
    sx = s0x + lx * L;
    sy = s0y + ly * L;
    return stretched || lx != 0.f || ly != 0.f;  // exactly 0 here: the exact (not resampled) path, bit-identical to no reshaping
  }
  const float* a = f.d.data() + o;
  const float* c = a + r;
  if (a[0] == 0.f && a[1] == 0.f && a[2] == 0.f && a[3] == 0.f && c[0] == 0.f && c[1] == 0.f && c[2] == 0.f && c[3] == 0.f) return false;
  const float dx = (a[0] * (1.f - fx) + a[2] * fx) * (1.f - fy) + (c[0] * (1.f - fx) + c[2] * fx) * fy;
  const float dy = (a[1] * (1.f - fx) + a[3] * fx) * (1.f - fy) + (c[1] * (1.f - fx) + c[3] * fx) * fy;
  sx += dx * m.lpx;
  sy += dy * m.lpx;
  return true;
}

// Long edge of the grid by tier: exact 1024 cells, interactive 256 cells (the coarse mesh while a control moves).
constexpr int kGridExact = 1024, kGridInteractive = 512;

// The field of `r` for a photo of photoW x photoH (cached: process-wide LRU of 8 fields / 64 MB). Null when nothing moves.
std::shared_ptr<const Field> bind(const ReshapeParams& r, int photoW, int photoH, bool interactive);

// Backward field at photo-normalised (u, v), long-edge units (bilinear; 0 outside the support).
void sampleUV(const Field& f, double u, double v, double& dx, double& dy);
// Where the original photo point (u, v) appears in the warped picture (inverse of p -> p + B(p), fixed-point iteration).
void forwardUV(const Field& f, double u, double v, double& uo, double& vo);
// Where the content shown at warped (u, v) comes from: (u, v) + B(u, v), photo-normalised.
void backwardUV(const Field& f, double u, double v, double& uo, double& vo);

struct Counters {
  std::uint64_t builds = 0, hits = 0;
  double lastBuildMs = 0;
  // structure-aware mode (section 49), of the last field built in that mode
  double cAnalysisMs = 0;   // structure tensor of the photo (0 when it came from the cache)
  double cSolveMs = 0;      // the sparse solve on the coarse grid
  int cIters = 0;           // conjugate-gradient iterations used
  double cResidual = 0;     // final residual relative to the start
  double cFoldFrac = 0;     // share of nodes that violated the fold limit and were blended toward the smooth field
  int cRounds = 0;          // reweighted re-solves it took to keep the solution fold-free
  int cFoldPasses = 0;      // blend passes it took (0 = the solved field was fold-free as it was)
  bool cUsed = false;       // false: the build did not use mode C (no slimming operator)
};
// Reshape modes of the slimming operators (瘦腰 瘦手臂 瘦腿 小头), internal switch (not in the UI):
//   Smooth    (S) one smooth field, protected by a decay band around the person plane (the background near the person is dragged; default)
//   Layered   (L) person warped, background frozen, the uncovered strip filled from just outside it (section 48)
//   Structure (C) one continuous field, the minimiser of a quadratic energy that keeps straight photo structures straight (section 49)
enum class Mode { Smooth = 0, Layered = 1, Structure = 2 };
void setMode(Mode m);
Mode mode();
// Test / evidence hook: false = the earlier single smooth field (background dragged by the band), for before / after sheets.
void setLayered(bool on);  // true = Layered, false = Smooth
// Internal 长腿 switch (not in the UI): 0 = W windowed stretch with the ground compressed (earlier), 1 = windowed stretch on the person layer, feet anchored
// (layered; default), 2 = A whole-frame stretch along the leg axis, 3 = B whole-frame vertical stretch.
void setLegMode(int mode);
// Evidence hook: override the protection band of the (non-layered) field, long-edge units; 0 = default.
void setBandOverride(double band);
Counters counters();
void resetCache();

}  // namespace dapple::warp
