#pragma once
// FrameFacts: what a frame's geometry and quality decide that the node families read when they resolve their parameter blocks
// (docs/engine/pipeline-architecture.md section 50). Producer today: the point family that is not migrated yet
// (FrameKernels::frameFacts() on the CPU, frameFacts(FramePlan, FastOptions) on the GPU path, both in core/src/frame_kernels.h);
// after the point family migrates, the Geometry block.

namespace dapple::engine {

struct FrameFacts {
  // output pixel (x, y) -> position in the whole (geometry-transformed) picture: (ox0 + (x + .5) * stepX, oy0 + (y + .5) * stepY)
  float ox0 = 0.f, oy0 = 0.f, stepX = 1.f, stepY = 1.f;
  float fullW = 0.f, fullH = 0.f;  // size of the whole picture in that space
  float spatialScale = 1.f;        // radii scale of the neighbourhood stages (full-picture long edge / 2048), resolved
  int quality = 2;                 // SpatialQuality: 0 off, 1 interactive, 2 full
  bool noise = true;               // interactive frames: noise reduction runs
  bool keepGrain = false;          // interactive frames: grain runs (a grain-family slider is being dragged, 14 Q8)
  float lookGrain = 0.f;           // grain the look adds (slider units; 0 when a film at full strength brings its own)
  float resizeSharpen = 0.f;       // GPU export of a downscaled picture: output sharpening added to the Detail stage (FramePlan::resizeSharpen)
  bool sceneReferred = true;       // the source is scene-referred (RAW); display-referred sources have in-camera NR
};

}  // namespace dapple::engine
