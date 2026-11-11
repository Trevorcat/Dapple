#pragma once

#include <array>
#include <memory>
#include <vector>

#include "dapple/camera.h"
#include "dapple/decoder.h"
#include "dapple/edit_params.h"
#include "dapple/image.h"
#include "dapple/lut.h"

namespace dapple {

// CPU reference implementation of the develop pipeline:
//
//   linear ProPhoto (scene) -> white balance -> exposure -> tone (log domain)
//   -> base shoulder curve -> OKLab colour (vibrance/sat/HSL/grading)
//   -> style/pad/grading -> sRGB gamut map -> sRGB encode -> curves -> LUT -> vignette
//   -> noise reduction -> dehaze -> clarity/texture/sharpen -> grain -> 8 bit
//   (geometry - rotate, straighten, crop, flip - is a resampling front end of the first stage)
//
// The GPU implementations for Android/iOS must match this output; it is the
// ground truth used by regression tests.
class Renderer {
 public:
  // `lut` may be null. It must outlive the renderer.
  Renderer(const EditParams& params, const CameraProfile& camera, const Lut3D* lut);

  // Widens the limits of the tonal-class move (EditParams::toneTarget); > 1 for previews.
  void setToneReach(float reach) { toneReach_ = reach; }

  Image8 render(const SourceImage& source) const;

  // Same as render() but keeps float sRGB-encoded values (0..1), for tests. The output size
  // follows the geometry (crop / rotate / straighten). `spatial` = false skips the neighbourhood
  // stages (noise reduction, dehaze, clarity, texture, sharpening, grain), used by the tone analysis.
  ImageF renderFloat(const SourceImage& source, bool spatial = true) const;

 private:
  struct Curve1D {
    std::vector<float> table;  // samples over [0,1]
    bool identity = true;
    float operator()(float x) const;
  };

  EditParams p_;
  const CameraProfile& camera_;
  const Lut3D* lut_;
  Curve1D master_, red_, green_, blue_;
  float wbGain_[3]{1, 1, 1};
  float toneReach_ = 1.f;
};

}  // namespace dapple
