#pragma once
// FrameFacts: what a frame decides that the node families read when they resolve their parameter blocks
// (docs/engine/pipeline-architecture.md section 50). Producer: the point family (point::facts over its resolved blocks, nodes/point/blocks.h).
#include "engine/nodes/point/geometry.h"

namespace dapple::engine {

struct FrameFacts {
  point::Geometry geo;             // output pixel -> picture -> source mapping (the point family's Geometry block)
  float spatialScale = 1.f;        // radii scale of the neighbourhood stages (full-picture long edge / 2048), resolved
  int quality = 2;                 // SpatialQuality: 0 off, 1 interactive, 2 full
  bool noise = true;               // interactive frames: noise reduction runs
  bool keepGrain = false;          // interactive frames: grain runs (a grain-family slider is being dragged, 14 Q8)
  float lookGrain = 0.f;           // grain the look adds (slider units; 0 when a film at full strength brings its own)
  float resizeSharpen = 0.f;       // GPU export of a downscaled picture: output sharpening added to the Detail stage
  bool sceneReferred = true;       // the source is scene-referred (RAW); display-referred sources have in-camera NR
};

}  // namespace dapple::engine
