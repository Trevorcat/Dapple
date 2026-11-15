#pragma once
// Internal: the neighbourhood operations of the renderer. They run on the display-referred,
// sRGB-encoded float image after the per-pixel pipeline and act on luminance (or on the
// R-Y / B-Y chroma planes) so colour is not shifted. Radii are given for a 2048 px long edge and
// scaled with the image, so a preview looks like the export.

#include <vector>

#include "dapple/image.h"

namespace dapple {

namespace engine::detail { struct Block; }  // core/engine/nodes/detail/detail.h

// ---- the neighbourhood stages one by one (engine CPU nodes). A stage works on a window `img` at (ox, oy) of a frame of fw x fh
// pixels; frame-global quantities (dehaze airlight / transmission, guided-filter coefficients) come from a solve made on
// the whole frame's 1/4 grid, which a tiled run accumulates tile by tile (tile origins and sizes multiples of 4).
struct SpatialScratch {
  std::vector<float> Y, tmp;  // luminance plane (kept between the stages of a chain) and a scratch plane
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
// noise reduction: the numbers come from the NR blocks (core/engine/nodes/nr/nr.h)
void stageNRLuma(ImageF& img, SpatialScratch& sc, int radius, float blend, float inv2);
void stageNRChroma(ImageF& img, SpatialScratch& sc, int factor, int lowRadius, int lumaRadius, float blend, float inv2);
void dehazeLowGrid(const ImageF& img, std::vector<float>& low, int& lw, int& lh);  // 4x4 area average, 3 channels
// dehaze: the numbers come from the Dehaze block (core/engine/nodes/dehaze/dehaze.h)
void dehazeSolveGrid(const std::vector<float>& low, int lw, int lh, int minRadius, int boxRadius, float omega, DehazeSolve& sv);
void stageDehazeApply(ImageF& img, SpatialScratch& sc, int ox, int oy, int fw, int fh, const DehazeSolve& sv);  // amount > 0
void stageDehazeVeil(ImageF& img, SpatialScratch& sc, float k, float air);                                      // amount < 0
// 1/4 grid of the luminance plane the stages kept (taken from `img` when none) over the rect [x0,x1) x [y0,y1) of img
void lumaGrid(const ImageF& img, SpatialScratch& sc, int x0, int y0, int x1, int y1, std::vector<float>& low, int& lw, int& lh);
// clarity / texture / sharpening: the numbers come from the Detail block (core/engine/nodes/detail/detail.h)
void guidedSolveGrid(const std::vector<float>& low, int lw, int lh, int radius, float eps, GuidedSolve& gs);
void stageDetail(ImageF& img, SpatialScratch& sc, const engine::detail::Block& p, const GuidedSolve* gs, int ox, int oy, int fw,
                 int fh);  // gs == null: solve from this image (whole picture)
void stageGrain(ImageF& img, float period, float inv, float amp, int ox, int oy);  // numbers of the Grain block (core/engine/nodes/grain)
}  // namespace dapple
