#pragma once
// Everything a GPU (or any other) backend needs to reproduce Renderer::renderBakedRgba() for one frame,
// resolved by the CPU reference so the numbers cannot drift apart: geometry, gain, vignette and the
// neighbourhood parameters. See Renderer::framePlan() and docs/engine/gpu-backend.md.
#include <string>

namespace dapple {

struct FramePlan {
  bool supported = true;      // false: the CPU path must render this frame (reason in `why`)
  std::string why;
  int srcW = 0, srcH = 0;     // dimensions of the source the plan was made for
  int outW = 0, outH = 0;     // frame size (region and output long edge applied)

  // geometry: output pixel centre (ox0 + (x + .5) * stepX, ...) -> rotate 90 -> straighten -> flip -> source
  bool useGeo = false;        // false: output pixel (x, y) is source pixel (x, y)
  bool exact = false;         // pure index mapping, no resampling (else Catmull-Rom)
  int rot = 0;                // quarter turns
  bool flip = false;
  float cs = 1.f, sn = 0.f, fw = 0.f, fh = 0.f;
  float ox0 = 0.f, oy0 = 0.f, stepX = 1.f, stepY = 1.f, fullW = 0.f, fullH = 0.f;

  float gain[3] = {1.f, 1.f, 1.f};  // white balance * exposure, applied to the source before the tone stage
  float vignette = 0.f;             // -1..1

  // neighbourhood stages, after the quality level / region were applied (see SpatialParams)
  float nrLuma = 0.f, nrColor = 0.f, dehaze = 0.f, clarity = 0.f, texture = 0.f, sharpen = 0.f, sharpRadius = 1.f,
        grain = 0.f;
  float spatialScale = 1.f;   // radii scale (full-picture long edge / 2048), resolved
  bool anySpatial() const {
    return nrLuma > 0 || nrColor > 0 || dehaze != 0 || clarity != 0 || texture != 0 || sharpen > 0 || grain > 0;
  }
};

}  // namespace dapple
