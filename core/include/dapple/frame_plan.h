#pragma once
// Everything a GPU (or any other) backend needs to reproduce Renderer::renderBakedRgba() for one frame,
// resolved by the CPU reference so the numbers cannot drift apart: geometry, gain, vignette and the
// neighbourhood parameters. See Renderer::framePlan() and docs/engine/gpu-backend.md.
#include <cstdint>
#include <string>
#include <vector>

namespace dapple {

class Lut3D;

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
  // GPU export of a downscaled picture: extra output sharpening (same radius as `sharpen`) after the Lanczos resize. Set by the
  // GPU path only (EngineSession::exportGpu); the CPU executors never see it. Makes the graph contain a Detail node.
  float resizeSharpen = 0.f;
  // Film simulation (GPU film frames, docs/engine/pipeline-architecture.md section 20): set when the frame is a film frame the GPU can
  // run (full strength; `gain` above is then the film's input gain). The executor builds the film node arguments from these.
  struct Film {
    bool active = false;
    bool displayPlain = true;       // DisplayStages (master / RGB curves, look LUT) are the identity
    float strength = 1.f;           // film strength 0..1 (the film frame is blended with the normal render when < 1)
    std::string stock, paper;
    float filmEv = 0.f;             // film_exp + the film exposure of the automatic match
    float printEv = 0.f;            // print exposure (EditParams::filmPrintEv)
    float black = 0.f;              // paper black (EditParams::filmBlack)
    float warmth = 0.f, tint = 0.f, grainPct = 100.f, halationPct = 100.f;
    float formatScale = 1.f;
    int quality = 2;                // 0 off, 1 interactive, 2 full
    bool keepGrain = false;
    int fullLongEdge = 0;
    unsigned frameIndex = 0;
  } film;
  // DisplayStages after a film (Renderer::displayStages): master / red / green / blue curves and the look LUT. Filled only for a film frame
  // with `film.displayPlain == false`; the curves are the 1024-sample tables of the renderer (identity curves are not stored).
  struct Display {
    unsigned curveMask = 0;      // bit c: curve c (0 master, 1 red, 2 green, 3 blue) is not the identity
    int curveSamples = 0;
    std::vector<float> curves;   // [4][curveSamples]
    const Lut3D* lut = nullptr;  // look LUT (null = none); owned by the caller of Renderer, valid for the frame
    float lutMix = 0.f;          // 0..1
    std::uint64_t lutId = 0;     // Lut3D::contentId()
  } display;
  bool anySpatial() const {
    return nrLuma > 0 || nrColor > 0 || dehaze != 0 || clarity != 0 || texture != 0 || sharpen > 0 || grain > 0;
  }
};

}  // namespace dapple
