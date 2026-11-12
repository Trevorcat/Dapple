#pragma once
// Internal: the neighbourhood operations of the renderer. They run on the display-referred,
// sRGB-encoded float image after the per-pixel pipeline and act on luminance (or on the
// R-Y / B-Y chroma planes) so colour is not shifted. Radii are given for a 2048 px long edge and
// scaled with the image, so a preview looks like the export.

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

}  // namespace dapple
