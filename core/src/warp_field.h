#pragma once
// Internal: the displacement field of the portrait reshaping (dapple/reshape.h; docs/engine/pipeline-architecture.md section 45).
//
// One whole-photo BACKWARD field B on a regular grid of nodes over the photo's normalised space: the content shown at photo point p
// of the warped picture comes from p + B(p) of the original. Units: long edges of the photo (isotropic). The engine folds it into the
// geometry fetch (FrameKernels / point.comp / film_src.comp / local_film_in.comp): output pixel -> geometry -> source position s ->
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
  std::vector<float> d;        // (dx, dy) per node, row-major, long-edge units
  float maxDisp = 0;           // max |B| over the nodes, long-edge units
  float maxGrad = 0;           // max |grad F| of the forward field after limiting (< 1: fold-free)
  bool gradLimited = false;    // the operators asked for more and were scaled down
  float delta = 0;             // background protection band (long-edge units), 0 = no matte
  bool matteUsed = false;
  bool matteMissing = false;   // a matte id was given but the plane is not in the raster store
  std::uint64_t id = 0;        // content key (params hash, grid)
  std::size_t bytes() const { return d.size() * sizeof(float); }
};

// Source-pixel mapping of a frame: grid coordinate = s * a + b (s in source pixels, pixel i spans [i, i + 1)); displacement in source
// pixels = d * lpx.
struct Map {
  float ax = 1, bx = 0, ay = 1, by = 0, lpx = 1;
};
// photoW / photoH: the size of the picture the source pixels belong to (a region window's parent); x0, y0: the source's origin in it.
Map makeMap(const Field& f, int srcW, int srcH, int photoW, int photoH, int x0, int y0);

// Bilinear sample of the field at source position (sx, sy); false (and no change) when the four nodes are exactly zero.
inline bool displace(const Field& f, const Map& m, float& sx, float& sy) {
  float gx = sx * m.ax + m.bx, gy = sy * m.ay + m.by;
  gx = std::min(std::max(gx, 0.f), float(f.gw - 1));
  gy = std::min(std::max(gy, 0.f), float(f.gh - 1));
  const int i = std::min(int(gx), f.gw - 2), j = std::min(int(gy), f.gh - 2);
  const float fx = gx - float(i), fy = gy - float(j);
  const float* a = f.d.data() + 2 * (std::size_t(j) * f.gw + i);
  const float* c = a + 2 * std::size_t(f.gw);
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
};
Counters counters();
void resetCache();

}  // namespace dapple::warp
