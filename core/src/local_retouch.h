#pragma once
// Portrait retouch as local operations (docs/engine/pipeline-architecture.md section 43). Internal header (core/src).
//
// Bind time (local::bind): the retouch scale of an adjustment (inter-ocular distance, photo metric), the automatic heal spots (multi-scale
// blob detection on a photo-space grid inside the mask) and the source patches of manual spots. Frame time (LocalDetail, CPU twin of the
// shaders local_heal.comp, local_rt_down.comp, local_retouch.comp): the heal pass, the shared retouch planes (fine low-pass L_f, edge-aware
// coarse base L_c, skin reference S) and the per-pixel combine.
#include <cmath>
#include <cstdint>
#include <vector>

#include "dapple/frame_plan.h"
#include "local_eval.h"

namespace dapple::local {

// ---- constants shared with the shaders (keep identical) ----------------------------------------------------------------------------
constexpr float kRtFine = 0.010f;     // L_f box radius (2 passes) = 1 % of the inter-ocular distance (pores, fine hair stay above it)
constexpr float kRtCoarse = 0.12f;    // L_c guided-filter box radius on the 1/4 grid, in inter-ocular distances
constexpr float kRtRef = 0.20f;       // skin reference: box radius (2 passes) on the 1/4 grid, in inter-ocular distances
constexpr float kRtBlot = 0.025f;     // blotch-scale colour mean: box radius (2 passes) on the 1/4 grid, in inter-ocular distances
constexpr float kRtYmin = 0.05f;      // relative chroma q = (R - Y, B - Y) / max(Y, kRtYmin)
constexpr float kRtEps = 0.04f * 0.04f;  // guided filter eps of L_c (display luminance units)
// whiten (stage L, scene-linear after the global gain): the lift (at most +0.5 EV) never takes a pixel above 1.4 EV over mid grey
constexpr float kWhitenCeiling = 0.18f * 2.6390158f;

// ---- bind time ------------------------------------------------------------------------------------------------------------------------
// Retouch scale of adjustment `a` (Frame adjustment index `adj`, already in f.adj): a.face, else an estimate from the mask's area.
float retouchIodOf(const LocalAdjustment& a, const Frame& f, int adj, const SourceImage& src);
// Long edge of the heal detection grid of an adjustment on a W x H photo (also asked for by the streaming export's guide capture).
int healGridLong(const LocalAdjustment& a, int W, int H);
// Heal spots of adjustment `a`: the manual ones (their own opacity = strength) and, when the heal op is > 0, the automatic ones inside the mask
// of Frame adjustment `adj` (-1: the adjustment has no mask). Appends in composition order (manual first).
void healSpotsOf(const LocalAdjustment& a, const Frame& f, int adj, const SourceImage& src, float iod, std::vector<SpotM>& out);

// Detection internals, exposed for the tests: blobs of a linear RGB patch (w x h, 3 floats a pixel) with mask (null = 1), inter-ocular distance
// in patch pixels and sensitivity 0..100. Returns (x, y, radius, confidence) in patch pixels.
struct Blob {
  float x = 0, y = 0, r = 0, conf = 0, z = 0;
  float sx = 0, sy = 0;  // source offset (patch pixels)
};
std::vector<Blob> detectBlobs(const float* rgb, const float* mask, int w, int h, float iodPx, float sens, bool searchSources);

// ---- frame time -------------------------------------------------------------------------------------------------------------------------
// Frame pixel index (x, y) -> photo metric (units of the photo's long edge), the mapping of FrameKernels::srcUV (affine: crop, rotate, flip,
// straighten, region).
struct Affine {
  double m[6] = {1, 0, 0, 0, 1, 0};  // X = m0 x + m1 y + m2, Y = m3 x + m4 y + m5
};
Affine frameAffine(const FramePlan& P);
double affinePpu(const Affine& A);  // frame pixels per photo metric unit
// The pixels per metric unit the halo of a frame is sized for (an upper bound of affinePpu for every frame the engine renders) and the one the
// retouch radii use (the true one, never above the halo's).
float haloPpu(const Frame& f, float scale);
float retouchPpu(const Frame& f, float scale, const FramePlan& P);

struct RetouchRadii {
  int rf = 1, rc = 1, rs = 1, rb = 1;  // L_f box (full resolution, 2 passes), L_c guided box, skin reference box, blotch-scale box (1/4 grid)
  float iodPx = 0;
};
RetouchRadii retouchRadii(float iodPx);
int retouchPlanesHalo(const RetouchRadii& r);
int healHalo(const Frame& f, float ppu);

// A heal spot in window pixels.
struct SpotPx {
  float cx = 0, cy = 0, R = 0, alpha = 0;
  int ox = 0, oy = 0, stride = 1;
};
// The spots whose disc touches the window (wx, wy, ww, wh) of the frame, in window coordinates, in composition order.
// `warp` (portrait reshaping, section 45): the spots follow the reshaped picture (centre and source forward-mapped through the field).
void spotsForWindow(const Frame& f, const Affine& A, int wx, int wy, int ww, int wh, std::vector<SpotPx>& out, const warp::Field* warp = nullptr);
// Heal pass: every spot fills its disc with the source patch plus a smooth correction fitted on a ring (reads the input only).
void healWindow(const std::vector<SpotPx>& spots, ImageF& img);
// The shared retouch planes of a window: L_f (full resolution), the guided coefficients of L_c and the skin reference S (1/4 grid).
struct RtPlanes {
  int w = 0, h = 0, lw = 0, lh = 0;
  std::vector<float> Lf, ab, S, B;  // ab: (a, b) a cell, L_c = a L_f + b; S / B: (w Y, w qr, w qb, w) at the reference / blotch radius
  float Lc(int x, int y) const;
};
void retouchPlanes(const Frame& f, const ImageF& img, const AuxTarget& aux, const RetouchRadii& R, RtPlanes& out);
// Retouch ops of a window (after healing): planes + combine.
void retouchCombineWindow(const Frame& f, ImageF& img, const AuxTarget& aux, const RetouchRadii& R);
// Heal + retouch of a LocalDetail window (CPU), before the legacy ops.
void retouchDetailWindow(const Frame& f, ImageF& img, const AuxTarget& aux, float scale, const FramePlan& P);

// The skin weight of a pixel (normalised convolution of the skin reference) from its retouch amounts.
inline float skinWeight(float ks, float ke, float kr, float kh, float ku) {
  const float s = std::fmin(std::fmax(ks + ke + kr + kh, 0.f), 1.f), u = std::fmin(std::fmax(ku, 0.f), 1.f);
  return (s + 0.02f) * (1.f - u);
}

}  // namespace dapple::local
