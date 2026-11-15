#pragma once
// What a frame request asks of the point family besides the edit: spatial quality, the region of the picture, film options
// (docs/engine/pipeline-architecture.md section 50). Set by the C layer / video session / tests, read by point::resolveFrame and FrameFacts.
#include <cstdint>

namespace dapple {

enum class SpatialQuality {
  Off,          // no neighbourhood stages
  Interactive,  // everything but grain; noise reduction only when `noise` is set
  Full,
};

// Which part of the (geometry-transformed) picture to render and at what size.
struct RenderRegion {
  float x0 = 0.f, y0 = 0.f, x1 = 1.f, y1 = 1.f;  // normalised, in the output space of the whole picture
  int outLongEdge = 0;                              // long edge of the region image (0 = natural scale)
  bool active() const { return outLongEdge > 0 || x0 > 0.f || y0 > 0.f || x1 < 1.f || y1 < 1.f; }
};

struct FastOptions {
  SpatialQuality spatial = SpatialQuality::Full;
  bool noise = true;  // Interactive only: also run noise reduction
  RenderRegion region;
  // Film (engine nodes): the interactive tier keeps the film grain while a grain-family slider is dragged (14 Q8); the
  // grain of a frame that previews a larger render is scale-aware against `fullLongEdge`; `frameIndex` seeds video grain.
  bool keepGrain = false;
  int fullLongEdge = 0;
  std::uint32_t frameIndex = 0;
  // Film, interactive frames only: a print table that is not cached yet is baked coarsely (17^3 instead of 33^3) while a table slider moves; the
  // frame after the drag bakes the exact one (docs/engine/pipeline-architecture.md section 26).
  bool coarseFilm = false;
};

}  // namespace dapple
