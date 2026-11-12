#pragma once
// Frame builder of the GPU executor: turns the CPU reference's FramePlan into the node chain
//   point (fused Geometry+Gain+ToneColour+Vignette) -> NRLuma -> NRChroma -> Dehaze -> Detail (+ Grain/Pack8 epilogue on the last pass)
// by calling the node record functions in nodes/*/gpu.cpp. This is the fixed-order stand-in for the Planner's stage list: when
// Plan{stages, groups} lands, its groups call the same record functions with the same Epilogue slot.
#include <string>

#include "engine/nodes/gpu_common.h"

namespace dapple::engine::gpu {

enum class Mode {
  Whole,          // one frame: analysis from the frame itself, writes RGBA8
  AnalyzeDehaze,  // tile sweep 1: point + noise reduction, then average the interior into GlobalRes::color
  AnalyzeClarity, // tile sweep 2: ... + dehaze, then average (Y, Y^2) into GlobalRes::yy
  Final,          // tile sweep 3: everything, RGBA8
};

struct FrameJob {
  const FramePlan* plan = nullptr;
  float resizeSharpen = 0.f;
  Mode mode = Mode::Whole;
  WRect interior;  // pixels of the picture this job produces / analyses
  WRect work;      // interior + halo (clamped to the picture): the size of the working images
  SourceBind src;
  LutBind lut;
  GlobalRes* glob = nullptr;  // required by Final when dehaze/clarity are on, and by the Analyze modes
};

// Appends the passes of one job. `out8` receives the handle of the RGBA8 image (Whole / Final).
bool buildFrame(GpuRecorder& r, const FrameJob& j, H* out8);

// Tiled export: after sweep 1 (dehaze = true: GlobalRes::color -> dc + airlight) / sweep 2 (false: yy -> ab).
bool buildGlobalAnalysis(GpuRecorder& r, const FrameJob& j, bool dehaze);

}  // namespace dapple::engine::gpu
