#pragma once
// Internal: the neighbourhood operations of the renderer. They run on the display-referred,
// sRGB-encoded float image after the per-pixel pipeline and act on luminance (or on the
// R-Y / B-Y chroma planes) so colour is not shifted. Radii are given for a 2048 px long edge and
// scaled with the image, so a preview looks like the export.

#include <vector>

#include "dapple/image.h"

namespace dapple {

struct SpatialParams {
  float nrLuma = 0.f;    // 0..1
  float nrColor = 0.f;   // 0..1
  float dehaze = 0.f;    // -1..1
  float clarity = 0.f;   // -1..1
  float texture = 0.f;   // -1..1
  float sharpen = 0.f;   // 0..1.5
  float sharpRadius = 1.f;  // pixels at 2048 px (0.5..3.0)
  float grain = 0.f;     // 0..1
  float scale = 0.f;     // radii scale (full-picture long edge / 2048); 0 = this image's own long edge / 2048

  bool any() const {
    return nrLuma > 0 || nrColor > 0 || dehaze != 0 || clarity != 0 || texture != 0 || sharpen > 0 || grain > 0;
  }
};

// Applies the stages in this order: noise reduction, dehaze, clarity + texture + sharpening
// (computed from the same luminance and applied together), film grain. Clamps to [0, 1].
void applySpatial(ImageF& img, const SpatialParams& p);

// ---- the same stages one by one (engine CPU nodes). A stage works on a window `img` at (ox, oy) of a frame of fw x fh
// pixels; frame-global quantities (dehaze airlight / transmission, guided-filter coefficients) come from a solve made on
// the whole frame's 1/4 grid, which a tiled run accumulates tile by tile (tile origins and sizes multiples of 4).
struct SpatialScratch {
  std::vector<float> Y, tmp;  // luminance plane (kept between stages like applySpatial does) and a scratch plane
  bool yValid = false;
};
struct DehazeSolve {
  bool valid = false;
  float A[3] = {1.f, 1.f, 1.f};
  int lw = 0, lh = 0;
  std::vector<float> t;
};
struct GuidedSolve {
  bool valid = false;
  int lw = 0, lh = 0;
  std::vector<float> a, b;
};
void stageNRLuma(ImageF& img, SpatialScratch& sc, float amount, float scale);
void stageNRChroma(ImageF& img, SpatialScratch& sc, float amount, float scale);
void dehazeLowGrid(const ImageF& img, std::vector<float>& low, int& lw, int& lh);  // 4x4 area average, 3 channels
void dehazeSolveGrid(const std::vector<float>& low, int lw, int lh, float amount, float scale, DehazeSolve& sv);
void stageDehazeApply(ImageF& img, SpatialScratch& sc, float amount, int ox, int oy, int fw, int fh, const DehazeSolve* sv);
// 1/4 grid of the luminance plane the stages kept (taken from `img` when none) over the rect [x0,x1) x [y0,y1) of img
void lumaGrid(const ImageF& img, SpatialScratch& sc, int x0, int y0, int x1, int y1, std::vector<float>& low, int& lw, int& lh);
void guidedSolveGrid(const std::vector<float>& low, int lw, int lh, float scale, GuidedSolve& gs);
void stageDetail(ImageF& img, SpatialScratch& sc, const SpatialParams& p, float scale, const GuidedSolve* gs, int ox, int oy,
                 int fw, int fh);  // gs == null: solve from this image (whole picture)
void stageGrain(ImageF& img, float amount, float scale, int ox, int oy);
}  // namespace dapple
