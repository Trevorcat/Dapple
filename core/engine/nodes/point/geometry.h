#pragma once
// Geometry block of the point family (docs/engine/pipeline-architecture.md section 50): the frame's output pixel -> picture -> source mapping.
// Resolved once per frame by point::resolveGeometry (framing: rotate, straighten + ratio crop, flip; then the region of a preview) and read by
// every node that maps pixels: the CPU head (point/cpu), the GPU point / film-input / local-film-input recorders, the local adjustments and the
// vignette (through FrameFacts::geo). Plain data; the per-pixel mapping is mapToSource below.
//
// Mapping: output pixel centre (ox0 + (x + .5) * stepX, oy0 + (y + .5) * stepY) is a position in the whole (geometry-transformed) picture of
// fullW x fullH; it is centred, flipped, straightened (cs, sn), turned `rot` quarter turns into the source of srcW x srcH.

namespace dapple::engine::point {

struct Geometry {
  int srcW = 0, srcH = 0;      // source the frame reads
  int outW = 0, outH = 0;      // frame size (region and output long edge applied)
  bool useGeo = false;         // false: output pixel (x, y) is source pixel (x, y)
  bool exact = false;          // pure index mapping, no resampling (else Catmull-Rom)
  bool region = false;         // a region of the picture (RenderRegion::active)
  int rot = 0;                 // quarter turns
  bool flip = false;
  float cs = 1.f, sn = 0.f, fw = 0.f, fh = 0.f;
  float ox0 = 0.f, oy0 = 0.f, stepX = 1.f, stepY = 1.f, fullW = 0.f, fullH = 0.f;
  // size of an output pixel in source pixels (the film's spatial effects are defined in film-frame micrometres)
  float pitch() const { return region ? (stepX > stepY ? stepX : stepY) : 1.f; }
};

// Source position (continuous, pixel i spans [i, i+1)) of the continuous picture position (px, py).
inline void mapToSource(const Geometry& g, float px, float py, float& sx, float& sy) {
  float u = px - g.fullW * 0.5f;
  const float v = py - g.fullH * 0.5f;
  if (g.flip) u = -u;
  const float fx = u * g.cs + v * g.sn + g.fw * 0.5f;
  const float fy = -u * g.sn + v * g.cs + g.fh * 0.5f;
  switch (g.rot) {
    case 0: sx = fx; sy = fy; break;
    case 1: sx = fy; sy = float(g.srcH) - fx; break;
    case 2: sx = float(g.srcW) - fx; sy = float(g.srcH) - fy; break;
    default: sx = float(g.srcW) - fy; sy = fx; break;
  }
}

// Source position of the centre of output pixel (x, y).
inline void mapPixel(const Geometry& g, int x, int y, float& sx, float& sy) {
  mapToSource(g, g.ox0 + (float(x) + 0.5f) * g.stepX, g.oy0 + (float(y) + 0.5f) * g.stepY, sx, sy);
}

}  // namespace dapple::engine::point
